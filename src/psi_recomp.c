/*
 * psi_recomp - zram huge 页重压缩守护进程（PSI 事件驱动 + 定时兜底 + 滞回）
 *
 * 背景：
 *   纯定时轮询无法区分「刚压完的残留」与「新涌入的可压页」。实测同一 huge
 *   区间(42000~44000)下，刚压完 5 分钟只压得动 1727 页，隔 35 分钟却能压掉
 *   13953 页 —— 差 8 倍。所以触发时机比触发频率更关键。
 *
 * 设计：
 *   1. 用 /proc/pressure/memory 的 poll() 做事件驱动：内存回收产生 stall 时
 *      内核主动唤醒，空闲时进程完全阻塞，零唤醒、零 CPU。
 *   2. PSI 只报 stall 时间，不报 huge 页数，故唤醒后仍读 mm_stat 第 8 字段裁决。
 *   3. 滞回：刚压完的一段时间内提高门槛，避免啃残留硬骨头。
 *      门槛 = 上次压缩后的残留 + HYST_MARGIN（自适应，而非固定值）。
 *   4. 限流：两次检查最小间隔 MIN_GAP_SEC，防 PSI 风暴。
 *   5. 兜底：即使 PSI 完全不可用（open/write/poll 失败），自动降级为纯定时
 *      轮询，行为与定时方案一致，功能不丢失。
 *   6. 支持运行时配置文件：/data/adb/modules/ZRAM-PSI-Recompress/psi.conf
 *      可在不重新编译的情况下调整所有阈值参数。
 *
 * 用法: psi_recomp <logpath> [configpath] [zram_dev]
 *   zram_dev 缺省/非法时回退 zram0；唯一来源是 module/config.prop 的 ZRAM_DEV，
 *   由 service.sh / WebUI 读取校验后以第 3 个参数传入。
 *
 * v1.3.6 变更（第二轮 review 修复）：
 *   - PSI 降级后的重试间隔钳制为 ≤PSI_RETRY_SEC：原先降级分支睡满
 *     IDLE_TIMEOUT_MS(1800s)，重试条件要 30 分钟才求值一次。定时检查点
 *     仍按 IDLE_TIMEOUT_MS 走，提前醒来只为重试 PSI。
 *   - 去掉 psi_ever 门槛：开机时 PSI 未就绪（首次 arm 失败）也会定期重试。
 *   - ZRAM_DEV 改由第 3 个命令行参数传入（来源 config.prop），不再读 psi.conf，
 *     与 service.sh / WebUI 统一为单一来源。
 *   - 滞回去掉 HYSTERESIS_SEC 时间窗：门槛 = 上次压缩残留 + HYST_MARGIN，
 *     一直生效到下次压缩。HYSTERESIS_SEC 改为「绝对上限兜底」的最小间隔：
 *     huge > THRESH_NORMAL×1.5 且距上次压缩 ≥ HYSTERESIS_SEC 时无视滞回强制压一次。
 *   - oom_score_adj 恢复 -800。
 *   - 抢锁时「inode 不一致」分支补 500ms 退避，LOCK_WAIT_TRIES 才真正对应 10 秒。
 *
 * v1.3.5 变更（代码审查修复）：
 *   - 计时改用 CLOCK_BOOTTIME：原先用 time()（墙钟），改系统时间/NTP 校时
 *     会让滞回窗口与限流失效；BOOTTIME 不受校时影响且休眠期间也计时。
 *     psi_stats 里的 last_comp 仍输出墙钟时间戳，供 WebUI 显示。
 *   - 单实例保护：PID 文件加 flock 并校验 inode，旧进程尚未退出时新进程
 *     最多等 10 秒，仍拿不到锁则退出，不再出现两个守护进程同时重压缩，
 *     也不再有「旧进程退出时删掉新进程 PID 文件」的竞态。
 *   - 限流由「丢弃唤醒」改为「延后到间隔期满再检查」：原先被限流的 PSI
 *     唤醒直接丢掉，之后若压力消失就要等 30 分钟兜底才会再检查。
 *     psi_throttled 现在表示被延后的次数。
 *   - 修复重压缩后 mm_stat 重读失败（返回 -1）时 saved = huge + 1，
 *     累计节省被高估、last_residual 不更新的问题。
 *   - 节省量改用 mm_stat 第 3 字段 mem_used_total 的降幅统计
 *     （新增 saved_mem_mib）。原 total_saved_mib = 页数/256 只是「不再是
 *     huge 的页数 × 4KB」，并非真实省下的内存，仅保留页数 total_saved。
 *   - read_huge/配置解析全部检查 errno 与 endptr，非法值忽略并写日志，
 *     不再静默当成 0。
 *   - PSI_TRIGGER 增加格式与范围校验（some|full，窗口 0.5~10s，
 *     stall ≤ 窗口），非法值回退默认，避免写入内核失败后永久降级。
 *   - 启动后若 PSI 中途失败降级为轮询，每 10 分钟自动重试武装。
 *   - 新增 ZRAM_DEV 配置项（默认 zram0），不再写死设备名。
 *   - psi_stats 改为写临时文件再 rename（原子替换），WebUI 不会读到半截。
 *   - 日志轮转改为写临时文件再 rename，中途被杀不会丢日志。
 *   - oom_score_adj 由 -800 调为 -500；信号改用 sigaction；去掉多余 setsid。
 *   - 修复 %d 打印 long 的格式告警。
 *
 * v1.2.4 变更：
 *   - 移除 WebUI 中重复的「当前 huge」：v1.2.2 新增统计格时未检查已有字段，
 *     导致「运行状态」(实时读 mm_stat) 与「压缩统计」(psi_stats 快照)
 *     各显示一个，数值还可能不一致。保留实时版本，
 *     统计卡片该格改为展示「距上次压缩」(利用已有的 last_comp 字段)。
 *
 * v1.2.3 变更：
 *   - 修复 psi_events 计数错位：原实现在 poll() 返回处就 n_psi++，
 *     位于限流闸门之前，被 MIN_GAP_SEC 丢弃的唤醒也被计入，
 *     导致 psi_events > checks（实测 23/12 = 192%）。
 *     改为仅在实际执行检查后才计数，并新增 psi_throttled
 *     单独统计被丢弃的唤醒次数（可用于判断 PSI 触发是否过敏感）。
 *
 * v1.2.2 变更：
 *   - 修复 WebUI 统计恒为 0：原实现只解析日志里的 stats/exit 行，
 *     而 stats 每 10 次压缩才输出、exit 仅在进程退出时输出，
 *     运行中的守护进程两者皆无，故重压缩次数/累计节省/峰值全显示 0。
 *     改为每次检查后写 psi_stats 状态文件，WebUI 直接读取，
 *     不依赖日志解析，也不受日志轮转影响。
 *   - 状态文件额外提供 psi_events、当前 huge、last_comp、pid
 *
 * v1.2.1 变更：
 *   - 修复限流忙循环：PSI 持续触发时 continue 直接回到循环顶部，
 *     poll() 立即返回形成 arm/poll/continue 空转，每轮还开关 /proc 文件，
 *     持续 g_min_gap_sec 秒满负荷。改为睡掉剩余时间。
 *   - 修复 IDLE_TIMEOUT_MS 无下限：配置 < 1000 时 /1000 得 0 导致
 *     sleep(0) 满核；超 INT_MAX 时转 int 溢出为负导致 poll 立即返回。
 *     新增 clamp_config() 对全部 13 个参数做安全钳制。
 *   - 修复 poll() 出错分支 fd 泄漏（原实现置 psi_ok=0 后不再 close）
 *   - 修复 log_rotate() 中 freopen 失败后 fclose 可能 double-close
 *   - WebUI 数值输入改为滑块，范围与 clamp_config 一一对应
 *
 * v1.2.0 变更：
 *   - 新增 KernelSU 模块 WebUI 支持
 *
 * v1.1.0 变更：
 *   - 支持运行时配置文件（psi.conf），无需重新编译即可调参
 *   - 修复 sleep() 被信号中断后提前醒来的问题（改用 nanosleep 循环）
 *   - 修复 read_huge() 的 fscanf 解析健壮性（改用 getline + strtok_r）
 *   - 优化 log_rotate() 使用 getline() 替代逐字符 fgetc
 *   - 添加 OOM 保护（oom_score_adj = -800）
 *   - 添加累计压缩统计（每 10 次压缩输出一次统计）
 *   - 守护进程自行写 PID 文件，方便外部检测和管理
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/stat.h>

#define PSI_PATH  "/proc/pressure/memory"

/* ---------------- 默认参数（可被配置文件覆盖） ---------------- */
static long g_thresh_normal   = 40000;          /* 常态触发阈值（huge 页数） */
static long g_idle_timeout_ms = 1800 * 1000;    /* 兜底：无事件时最多等 30 分钟 */
static long g_min_gap_sec     = 60;             /* 两次检查最小间隔，防 PSI 风暴 */
static long g_hysteresis_sec  = 5400;           /* 滞回持续 90 分钟 = 3 个检查周期 */
static long g_hyst_margin     = 8000;           /* 残留之上再新增这么多页才值得再压 */
static long g_log_max_lines   = 600;            /* 超过此行数触发轮转 */
static long g_log_keep_lines  = 300;            /* 轮转后保留的行数 */
static long g_skip_log_every  = 5;              /* 连续 skip 时每 N 次记一行 */
static char g_psi_trigger[64] = "some 15000 1000000";  /* 1s 窗口内 stall > 15ms 触发 */
static char g_zram_dev[16]    = "zram0";        /* zram 设备名 */

#define PSI_RETRY_SEC   600     /* PSI 降级后每隔多久重试武装一次 */
#define LOCK_WAIT_TRIES 20      /* 等旧实例退出：20 × 500ms = 10s */

/* ---------------- 全局状态 ---------------- */
static volatile sig_atomic_t g_running = 1;
static FILE *g_log = NULL;
static const char *g_logpath = "/dev/null";
static const char *g_pidpath = NULL;
static const char *g_statspath = NULL;   /* 实时统计状态文件，供 WebUI 读取 */
static int g_pidfd = -1;                 /* 持有 flock 的 PID 文件 fd */
static unsigned long g_lines = 0;
static unsigned long g_total_saved_pages = 0;      /* 累计减少的 huge 页数 */
static unsigned long long g_total_saved_bytes = 0; /* 累计 mem_used_total 降幅 */

static char g_mm_stat_path[64];
static char g_recomp_path[64];
static char g_cfg_warn[512];             /* 配置解析告警，日志打开后统一输出 */

static void on_sig(int s) { (void)s; g_running = 0; }

/* 单调时钟（含休眠时间），不受系统改时间影响 */
static long mono_sec(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_BOOTTIME, &ts) != 0 &&
        clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (long)ts.tv_sec;
}

static void now_str(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm tm_;
    localtime_r(&t, &tm_);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm_);
}

static void cfg_warn(const char *fmt, ...)
{
    size_t used = strlen(g_cfg_warn);
    if (used >= sizeof(g_cfg_warn) - 1) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_cfg_warn + used, sizeof(g_cfg_warn) - used, fmt, ap);
    va_end(ap);
}

/* 可中断的完整睡眠：收到 SIGTERM 立即退出，其他信号继续睡剩余时间 */
static void sleep_full(long seconds)
{
    struct timespec req;
    req.tv_sec = seconds;
    req.tv_nsec = 0;
    while (g_running && nanosleep(&req, &req) < 0 && errno == EINTR) {
        /* 循环继续，nanosleep 会自动更新 req 为剩余时间 */
    }
}

static void sleep_ms(long ms)
{
    struct timespec req;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (ms % 1000) * 1000000L;
    while (nanosleep(&req, &req) < 0 && errno == EINTR) { }
}

/*
 * 单实例锁：对 PID 文件加 flock(LOCK_EX|LOCK_NB)。
 *   - 拿不到锁说明旧实例还活着（可能正阻塞在重压缩的 sysfs write 里），
 *     最多等 10 秒，让 WebUI「重启守护进程」时旧进程有时间退出。
 *   - 拿到锁后比对 fd 与路径的 inode：旧进程退出时会 unlink PID 文件，
 *     若我们锁到的是已被删除的旧 inode，必须重开，否则两个实例各锁各的。
 * 成功返回 0，失败返回 -1。
 */
static int acquire_instance_lock(void)
{
    if (!g_pidpath) return 0;   /* 无法确定 PID 路径则不做保护 */

    for (int i = 0; i < LOCK_WAIT_TRIES; i++) {
        int fd = open(g_pidpath, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0) return 0;   /* 目录不可写等：不因此拒绝启动 */

        if (flock(fd, LOCK_EX | LOCK_NB) == 0) {
            struct stat a, b;
            if (fstat(fd, &a) == 0 && stat(g_pidpath, &b) == 0 &&
                a.st_ino == b.st_ino && a.st_dev == b.st_dev) {
                g_pidfd = fd;
                if (ftruncate(fd, 0) == 0) {
                    char buf[32];
                    int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
                    if (n > 0) {
                        ssize_t w = write(fd, buf, (size_t)n);
                        (void)w;
                    }
                }
                return 0;
            }
            /* inode 不一致：锁到了已被删除的旧文件，退避后重开 */
            close(fd);
            sleep_ms(500);
            continue;
        }
        close(fd);
        sleep_ms(500);
    }
    return -1;
}

static void release_instance_lock(void)
{
    if (g_pidfd < 0) return;
    if (g_pidpath) unlink(g_pidpath);   /* 仍持锁时删除，旧 inode 由重开逻辑兜住 */
    close(g_pidfd);
    g_pidfd = -1;
}

/*
 * 写实时统计状态文件（先写临时文件再 rename，WebUI 不会读到半截）。
 * last_comp_mono：上次压缩的单调时间（<0 表示从未）；输出时换算为墙钟时间戳。
 */
static void write_stats_file(unsigned long n_check, unsigned long n_psi,
                             unsigned long n_throttled,
                             unsigned long n_comp, long peak, long huge,
                             long last_comp_mono)
{
    if (!g_statspath) return;

    char tmp[560];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_statspath);
    FILE *f = fopen(tmp, "w");
    if (!f) return;

    time_t wall = time(NULL);
    long last_comp_wall = 0;
    if (last_comp_mono >= 0) {
        long ago = mono_sec() - last_comp_mono;
        if (ago < 0) ago = 0;
        last_comp_wall = (long)wall - ago;
    }

    fprintf(f, "checks=%lu\n", n_check);
    fprintf(f, "psi_events=%lu\n", n_psi);
    fprintf(f, "psi_throttled=%lu\n", n_throttled);
    fprintf(f, "recompress=%lu\n", n_comp);
    fprintf(f, "peak=%ld\n", peak);
    fprintf(f, "total_saved=%lu\n", g_total_saved_pages);
    fprintf(f, "saved_mem_mib=%.1f\n", (double)g_total_saved_bytes / 1048576.0);
    fprintf(f, "huge=%ld\n", huge);
    fprintf(f, "last_comp=%ld\n", last_comp_wall);
    fprintf(f, "updated=%ld\n", (long)wall);
    fprintf(f, "pid=%d\n", (int)getpid());
    if (fclose(f) == 0) {
        if (rename(tmp, g_statspath) != 0) unlink(tmp);
    } else {
        unlink(tmp);
    }
}

/* 提高 OOM 优先级，降低被 LMK 杀掉的概率 */
static void protect_from_oom(void)
{
    int fd = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t w = write(fd, "-800\n", 5);
        (void)w;
        close(fd);
    }
}

/* 设置为低优先级调度，避免抢占前台应用 CPU */
static void set_low_priority(void)
{
    int old_nice = nice(10);
    (void)old_nice;
}

/* ---------------- 日志轮转（临时文件 + rename，中途被杀不丢日志） ---------------- */
static void log_rotate(unsigned long keep)
{
    if (!g_log) return;
    fflush(g_log);

    FILE *r = fopen(g_logpath, "r");
    if (!r) return;

    char *line = NULL;
    size_t linecap = 0;

    /* 第一遍：统计行数 */
    unsigned long total = 0;
    while (getline(&line, &linecap, r) > 0) total++;
    fclose(r);
    if (total <= keep) { free(line); g_lines = total; return; }

    /* 第二遍：跳过前 skip 行，其余写入临时文件 */
    r = fopen(g_logpath, "r");
    if (!r) { free(line); return; }

    char tmp[560];
    snprintf(tmp, sizeof(tmp), "%s.tmp", g_logpath);
    FILE *w = fopen(tmp, "w");
    if (!w) { free(line); fclose(r); return; }

    unsigned long skip = total - keep;
    unsigned long i = 0;
    int ok = 1;
    while (getline(&line, &linecap, r) > 0) {
        if (i++ < skip) continue;
        if (fputs(line, w) == EOF) { ok = 0; break; }
    }
    free(line);
    fclose(r);
    if (fclose(w) != 0) ok = 0;

    if (!ok || rename(tmp, g_logpath) != 0) {
        unlink(tmp);
        return;
    }

    /* rename 后旧 g_log 仍指向被替换的旧 inode，必须重开 */
    fclose(g_log);
    g_log = fopen(g_logpath, "a");
    g_lines = keep;
}

static void plog(const char *fmt, ...)
{
    if (!g_log) return;
    char ts[32];
    now_str(ts, sizeof(ts));
    fprintf(g_log, "[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);

    if (++g_lines >= (unsigned long)g_log_max_lines)
        log_rotate((unsigned long)g_log_keep_lines);
}

/* ---------------- 配置文件解析 ---------------- */
static void trim(char *s)
{
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                       s[len-1] == '\n' || s[len-1] == '\r'))
        s[--len] = '\0';
}

/* 严格解析十进制 long：必须整串都是数字，且不溢出 */
static int parse_long(const char *s, long *out)
{
    if (!s || !*s) return -1;
    char *end = NULL;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') return -1;
    *out = v;
    return 0;
}

/* PSI trigger 格式：<some|full> <stall_us> <window_us>，内核要求窗口 0.5~10s */
static int psi_trigger_valid(const char *s)
{
    char type[8];
    long stall = 0, win = 0;
    int n = 0;
    if (sscanf(s, "%7s %ld %ld %n", type, &stall, &win, &n) != 3) return 0;
    if (s[n] != '\0') return 0;
    if (strcmp(type, "some") != 0 && strcmp(type, "full") != 0) return 0;
    if (win < 500000 || win > 10000000) return 0;
    if (stall <= 0 || stall > win) return 0;
    return 1;
}

/* zram 设备名：zram + 1~3 位数字（与 service.sh / WebUI 的校验一致） */
static int zram_dev_valid(const char *s)
{
    if (strncmp(s, "zram", 4) != 0) return 0;
    size_t n = strlen(s + 4);
    if (n < 1 || n > 3) return 0;
    for (const char *p = s + 4; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    return 1;
}

static void clamp_config(void);

static void set_long(const char *key, const char *val, long *dst)
{
    long v;
    if (parse_long(val, &v) == 0) *dst = v;
    else cfg_warn("[%s=%.32s 非法，已忽略] ", key, val);
}

static void load_config(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;  /* 配置文件不存在，用默认值 */

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim(key);
        trim(val);

        if (!strcmp(key, "THRESH_NORMAL"))
            set_long(key, val, &g_thresh_normal);
        else if (!strcmp(key, "IDLE_TIMEOUT_MS"))
            set_long(key, val, &g_idle_timeout_ms);
        else if (!strcmp(key, "MIN_GAP_SEC"))
            set_long(key, val, &g_min_gap_sec);
        else if (!strcmp(key, "HYSTERESIS_SEC"))
            set_long(key, val, &g_hysteresis_sec);
        else if (!strcmp(key, "HYST_MARGIN"))
            set_long(key, val, &g_hyst_margin);
        else if (!strcmp(key, "LOG_MAX_LINES"))
            set_long(key, val, &g_log_max_lines);
        else if (!strcmp(key, "LOG_KEEP_LINES"))
            set_long(key, val, &g_log_keep_lines);
        else if (!strcmp(key, "SKIP_LOG_EVERY"))
            set_long(key, val, &g_skip_log_every);
        else if (!strcmp(key, "PSI_TRIGGER")) {
            if (strlen(val) < sizeof(g_psi_trigger) && psi_trigger_valid(val))
                snprintf(g_psi_trigger, sizeof(g_psi_trigger), "%s", val);
            else
                cfg_warn("[PSI_TRIGGER 非法，沿用 \"%s\"] ", g_psi_trigger);
        }
    }
    fclose(f);
    clamp_config();
}

/*
 * 参数钳制：配置文件来自用户可编辑的 psi.conf / WebUI，
 * 必须限制到安全区间，否则：
 *   - IDLE_TIMEOUT_MS < 1000 时 /1000 得 0 -> sleep(0) 满核忙循环
 *   - IDLE_TIMEOUT_MS 超 INT_MAX 时转 int 溢出为负 -> poll 立即返回忙循环
 *   - MIN_GAP_SEC=0 时限流失效
 */
static void clamp_config(void)
{
    if (g_idle_timeout_ms < 1000)     g_idle_timeout_ms = 1000;
    if (g_idle_timeout_ms > 3600000)  g_idle_timeout_ms = 3600000;
    if (g_min_gap_sec < 1)            g_min_gap_sec = 1;
    if (g_min_gap_sec > 3600)         g_min_gap_sec = 3600;
    if (g_thresh_normal < 1000)       g_thresh_normal = 1000;
    if (g_thresh_normal > 200000)     g_thresh_normal = 200000;
    if (g_hysteresis_sec < 0)         g_hysteresis_sec = 0;
    if (g_hysteresis_sec > 86400)     g_hysteresis_sec = 86400;
    if (g_hyst_margin < 0)            g_hyst_margin = 0;
    if (g_hyst_margin > 200000)       g_hyst_margin = 200000;
    if (g_log_max_lines < 50)         g_log_max_lines = 50;
    if (g_log_max_lines > 100000)     g_log_max_lines = 100000;
    if (g_log_keep_lines < 10)        g_log_keep_lines = 10;
    if (g_log_keep_lines >= g_log_max_lines) g_log_keep_lines = g_log_max_lines / 2;
    if (g_skip_log_every < 1)         g_skip_log_every = 1;
    if (g_skip_log_every > 100)       g_skip_log_every = 100;
}

/* ---------------- mm_stat 读取 ---------------- */
/*
 * mm_stat 字段（内核文档）：
 *   0 orig_data_size  1 compr_data_size  2 mem_used_total  3 mem_limit
 *   4 mem_used_max    5 same_pages       6 pages_compacted 7 huge_pages
 * 返回 0 成功；-1 打开/读取失败（errno 有效）；-2 字段不足或无法解析。
 * mem_used 可为 NULL。
 */
static int read_mm_stat(long *huge, long long *mem_used)
{
    FILE *f = fopen(g_mm_stat_path, "r");
    if (!f) return -1;

    char buf[512];
    if (!fgets(buf, sizeof(buf), f)) { int e = errno; fclose(f); errno = e; return -1; }
    fclose(f);

    char *saveptr = NULL;
    char *tok = strtok_r(buf, " \t\n", &saveptr);
    int idx = 0;
    int got_huge = 0, got_mem = 0;
    long hv = 0;
    long long mv = 0;

    while (tok && idx <= 7) {
        if (idx == 2 || idx == 7) {
            char *end = NULL;
            errno = 0;
            long long v = strtoll(tok, &end, 10);
            if (errno != 0 || end == tok || *end != '\0') return -2;
            if (idx == 2) { mv = v; got_mem = 1; }
            else          { if (v < 0 || v > LONG_MAX) return -2; hv = (long)v; got_huge = 1; }
        }
        idx++;
        tok = strtok_r(NULL, " \t\n", &saveptr);
    }
    if (!got_huge) return -2;
    *huge = hv;
    if (mem_used) *mem_used = got_mem ? mv : -1;
    return 0;
}

static const char *mm_err_str(int rc)
{
    return rc == -2 ? "mm_stat 字段不足或无法解析" : strerror(errno);
}

static int do_recompress(void)
{
    int fd = open(g_recomp_path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    const char *cmd = "type=huge";
    ssize_t w = write(fd, cmd, strlen(cmd));
    int err = errno;
    close(fd);
    if (w != (ssize_t)strlen(cmd)) { errno = err; return -1; }
    return 0;
}

/* 用全新 fd 武装 PSI 触发器；失败返回 -1（errno 有效） */
static int psi_arm(void)
{
    int fd = open(PSI_PATH, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    if (write(fd, g_psi_trigger, strlen(g_psi_trigger)) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

/* ---------------- 主函数 ---------------- */
int main(int argc, char **argv)
{
    const char *logpath = (argc > 1) ? argv[1] : "/dev/null";
    const char *configpath = (argc > 2) ? argv[2] : NULL;
    const char *zdev_arg   = (argc > 3) ? argv[3] : NULL;

    /* 先加载配置文件（如果有的话） */
    if (configpath && *configpath) load_config(configpath);

    /* zram 设备名：唯一来源 config.prop，经第 3 个参数传入；非法值回退 zram0 */
    if (zdev_arg && *zdev_arg) {
        if (zram_dev_valid(zdev_arg))
            snprintf(g_zram_dev, sizeof(g_zram_dev), "%s", zdev_arg);
        else
            cfg_warn("[zram_dev 参数非法，回退 %s] ", g_zram_dev);
    }

    snprintf(g_mm_stat_path, sizeof(g_mm_stat_path), "/sys/block/%s/mm_stat", g_zram_dev);
    snprintf(g_recomp_path,  sizeof(g_recomp_path),  "/sys/block/%s/recompress", g_zram_dev);

    g_logpath = logpath;
    g_log = fopen(logpath, "a");

    /* PID 文件路径：日志路径同目录下的 psi_recomp.pid */
    static char pidpath[512];
    snprintf(pidpath, sizeof(pidpath), "%s", logpath);
    char *slash = strrchr(pidpath, '/');
    if (slash && (size_t)(slash - pidpath) + 1 + sizeof("psi_recomp.pid") <= sizeof(pidpath)) {
        strcpy(slash + 1, "psi_recomp.pid");
        g_pidpath = pidpath;
    }

    /* 统计文件路径：与日志同目录下的 psi_stats */
    static char statpath[512];
    snprintf(statpath, sizeof(statpath), "%s", logpath);
    char *slash2 = strrchr(statpath, '/');
    if (slash2 && (size_t)(slash2 - statpath) + 1 + sizeof("psi_stats") <= sizeof(statpath)) {
        strcpy(slash2 + 1, "psi_stats");
        g_statspath = statpath;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sig;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    signal(SIGHUP, SIG_IGN);

    if (acquire_instance_lock() != 0) {
        plog("another psi_recomp instance holds the lock -> exit");
        if (g_log) fclose(g_log);
        return 1;
    }

    protect_from_oom();
    set_low_priority();
    write_stats_file(0, 0, 0, 0, 0, -1, -1);   /* 初始占位，避免 WebUI 读到空文件 */

    plog("=== psi_recomp start (pid=%d, dev=%s, thresh=%ld, hyst=%lds/+%ld, gap=%lds) ===",
         (int)getpid(), g_zram_dev, g_thresh_normal, g_hysteresis_sec,
         g_hyst_margin, g_min_gap_sec);
    if (g_cfg_warn[0]) plog("config warning: %s", g_cfg_warn);

    long last_check = -1;       /* 单调时间；<0 表示尚未检查 */
    long last_comp  = -1;       /* 单调时间；<0 表示从未压缩 */
    long last_residual = 0;
    unsigned long skip_streak = 0;
    long peak = 0;
    unsigned long n_check = 0, n_comp = 0, n_psi = 0, n_throttled = 0;

    /* 先探测一次：PSI 是否可用，不可用则全程走定时 */
    int psi_ok = 0;
    int psi_warned = 0;         /* 降级期间只记一次失败日志，重试失败不刷屏 */
    long idle_start = -1;       /* 降级轮询：本轮定时检查的起点（<0 表示未开始） */
    long psi_retry_at = mono_sec();
    {
        int probe = psi_arm();
        if (probe >= 0) {
            psi_ok = 1;
            plog("PSI armed: %s", g_psi_trigger);
            close(probe);
        } else {
            /* 开机时 PSI 可能尚未就绪：先降级，后面每 PSI_RETRY_SEC 重试 */
            plog("PSI unavailable (%s) -> fallback to timer (retry every %ds)",
                 strerror(errno), PSI_RETRY_SEC);
            psi_warned = 1;
        }
    }

    while (g_running) {
        int fired = 0;
        int psi = -1;

        /* 降级后定期重试（含启动时即失败的情况），避免永久丢失事件驱动 */
        if (!psi_ok && (mono_sec() - psi_retry_at) >= PSI_RETRY_SEC) {
            psi_ok = 1;
            psi_retry_at = mono_sec();
        }

        if (psi_ok) {
            /*
             * 每轮都用全新 fd 武装：PSI 触发是边沿触发且绑定到 fd，
             * 在旧 fd 上直接 write 重设会返回 EBUSY（实测 v1.0.3 表现），
             * 换 fd 可彻底避开残留状态。
             */
            psi = psi_arm();
            if (psi < 0) {
                if (!psi_warned) {
                    plog("PSI arm failed (%s) -> fallback to timer", strerror(errno));
                    psi_warned = 1;
                }
                psi_ok = 0;
                psi_retry_at = mono_sec();
            } else {
                if (psi_warned) {
                    plog("PSI re-armed: %s", g_psi_trigger);
                    psi_warned = 0;
                }
                struct pollfd pfd;
                pfd.fd = psi;
                pfd.events = POLLPRI;
                pfd.revents = 0;

                int r = poll(&pfd, 1, (int)g_idle_timeout_ms);
                if (r < 0) {
                    if (errno == EINTR) { close(psi); continue; }
                    /* 先关 fd 再置 psi_ok=0，否则下方 close 条件不成立会泄漏 */
                    plog("poll error: %s -> fallback to timer", strerror(errno));
                    close(psi); psi = -1;
                    psi_ok = 0;
                    psi_warned = 1;
                    psi_retry_at = mono_sec();
                } else if (r > 0) {
                    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                        /* fd 失效：不能当作有效事件，也不能再 poll 否则空转 */
                        plog("poll revents=0x%x -> fallback to timer", pfd.revents);
                        close(psi); psi = -1;
                        psi_ok = 0;
                        psi_warned = 1;
                        psi_retry_at = mono_sec();
                    } else {
                        fired = 1;
                    }
                }
            }
        }

        if (psi >= 0) { close(psi); psi = -1; }
        if (!psi_ok && !fired) {
            /*
             * 降级轮询：把单次睡眠钳制到 PSI_RETRY_SEC，保证每 ≤600s 回到循环顶
             * 求值一次重试条件；否则默认要睡满 IDLE_TIMEOUT_MS(1800s) 才有机会重试。
             * 定时检查点仍按 IDLE_TIMEOUT_MS 走：未到点就直接回循环顶，不做检查。
             */
            long idle_s = g_idle_timeout_ms / 1000;
            if (idle_start < 0) idle_start = mono_sec();
            long remain = idle_s - (mono_sec() - idle_start);   /* 距定时检查点 */
            if (remain < 1) remain = 1;
            long chunk = remain > PSI_RETRY_SEC ? PSI_RETRY_SEC : remain;
            sleep_full(chunk);
            if (!g_running) break;
            if ((mono_sec() - idle_start) < idle_s) continue;
        }
        idle_start = -1;

        long now = mono_sec();
        /*
         * 限流：距上次检查不足 MIN_GAP_SEC 时，睡掉剩余时间后再检查。
         * 旧实现直接 continue 回到循环顶部：压力持续时 poll() 立即返回形成
         * 忙循环；v1.2.1 改成睡眠后，唤醒仍被丢弃，压力一消失就要等兜底超时。
         * 现在是「延后」而不是「丢弃」。
         */
        if (last_check >= 0 && (now - last_check) < g_min_gap_sec) {
            long wait = g_min_gap_sec - (now - last_check);
            if (wait > g_min_gap_sec) wait = g_min_gap_sec;   /* 防时钟异常 */
            if (fired) n_throttled++;
            if (wait > 0) sleep_full(wait);
            if (!g_running) break;
            now = mono_sec();
        }
        last_check = now;
        n_check++;
        if (fired) n_psi++;

        long huge = 0;
        long long mem_before = -1;
        int rc = read_mm_stat(&huge, &mem_before);
        if (rc != 0) {
            plog("read mm_stat failed: %s", mm_err_str(rc));
            continue;
        }
        if (huge > peak) peak = huge;

        /* 每次检查后立即落盘，WebUI 读到的是实时值 */
        write_stats_file(n_check, n_psi, n_throttled, n_comp, peak, huge, last_comp);

        /*
         * 滞回：门槛 = 上次压缩后的残留 + margin，一直生效到下次压缩。
         * 基准每次压缩后重置为新的残留，不会累加，没有门槛螺旋。
         * （原先 5400s 后解除滞回，门槛回落到 40000 而地板已 ~47000，
         *   必然触发且只剩压不动的残留，实测白干两次。）
         *
         * 绝对上限兜底：huge > THRESH_NORMAL×1.5 且距上次压缩 ≥ HYSTERESIS_SEC
         * 时无视滞回强制压一次，防止极端情况下永不触发；
         * 最小间隔防止地板本身已高于上限时每次检查都空压。
         */
        int in_hyst = (last_comp >= 0) && (last_residual > 0);
        int forced = 0;
        long th = g_thresh_normal;
        if (in_hyst) {
            long cand = last_residual + g_hyst_margin;
            if (cand > th) th = cand;
            if (huge > g_thresh_normal + g_thresh_normal / 2 &&
                (now - last_comp) >= g_hysteresis_sec) {
                th = g_thresh_normal;
                forced = 1;
            }
        }
        const char *tag = forced ? "(cap)" : (in_hyst ? "(hyst)" : "");

        if (huge > th) {
            plog("evt=%s huge=%ld > %ld%s -> recompress",
                 fired ? "psi" : "idle", huge, th, tag);
            skip_streak = 0;
            if (do_recompress() == 0) {
                long after = -1;
                long long mem_after = -1;
                int rc2 = read_mm_stat(&after, &mem_after);

                last_comp = mono_sec();
                n_comp++;

                if (rc2 == 0) {
                    long saved = huge - after;
                    if (saved > 0) g_total_saved_pages += (unsigned long)saved;
                    if (mem_before >= 0 && mem_after >= 0 && mem_before > mem_after)
                        g_total_saved_bytes += (unsigned long long)(mem_before - mem_after);
                    plog("  done: %ld -> %ld (%+ld pages, %.1f%%), mem_used %.1f -> %.1f MiB",
                         huge, after, -saved,
                         huge > 0 ? (double)saved / huge * 100 : 0.0,
                         mem_before >= 0 ? (double)mem_before / 1048576.0 : -1.0,
                         mem_after  >= 0 ? (double)mem_after  / 1048576.0 : -1.0);
                    if (after > 0) last_residual = after;
                    write_stats_file(n_check, n_psi, n_throttled, n_comp, peak, after, last_comp);
                } else {
                    /* 压缩已执行，但无法得知残留：不统计节省，滞回沿用旧残留 */
                    plog("  done, but re-read mm_stat failed: %s", mm_err_str(rc2));
                    write_stats_file(n_check, n_psi, n_throttled, n_comp, peak, huge, last_comp);
                }

                /* 每 10 次压缩输出一次累计统计 */
                if (n_comp % 10 == 0) {
                    plog("stats: total_comp=%lu, huge_reduced=%lu pages, mem_saved=%.1f MiB, peak=%ld",
                         n_comp, g_total_saved_pages,
                         (double)g_total_saved_bytes / 1048576.0, peak);
                }
            } else {
                plog("  recompress FAILED: %s", strerror(errno));
            }
        } else {
            skip_streak++;
            /* skip_streak 先自增，最小为 1：(x-1)%N==0 命中第 1、N+1、2N+1…次；
             * N=1 恒真（每次都写），N≥2 与原 x%N==1 完全一致。 */
            if ((skip_streak - 1) % (unsigned long)g_skip_log_every == 0) {
                plog("evt=%s huge=%ld <= %ld%s skip x%lu (peak=%ld)",
                     fired ? "psi" : "idle", huge, th, tag,
                     skip_streak, peak);
            }
        }
    }

    plog("=== exit: checks=%lu psi_events=%lu throttled=%lu recompress=%lu peak=%ld huge_reduced=%lu pages mem_saved=%.1f MiB ===",
         n_check, n_psi, n_throttled, n_comp, peak,
         g_total_saved_pages, (double)g_total_saved_bytes / 1048576.0);
    if (g_log) fclose(g_log);
    release_instance_lock();
    return 0;
}
