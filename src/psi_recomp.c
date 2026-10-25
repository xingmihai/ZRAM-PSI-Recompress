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
 *   4. 限流：两次检查最小间隔 60 秒，防 PSI 风暴。
 *   5. 兜底：即使 PSI 完全不可用（open/write/poll 失败），自动降级为纯定时
 *      轮询，行为与定时方案一致，功能不丢失。
 *   6. 支持运行时配置文件：/data/adb/modules/ZRAM-PSI-Recompress/psi.conf
 *      可在不重新编译的情况下调整所有阈值参数。
 *
 * 用法: psi_recomp <logpath> [configpath]
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
#include <sys/stat.h>
#include <sched.h>

#define PSI_PATH  "/proc/pressure/memory"
#define MM_STAT   "/sys/block/zram0/mm_stat"
#define RECOMP    "/sys/block/zram0/recompress"

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

#define SKIP_LOG_EVERY_DEFAULT  5

/* ---------------- 全局状态 ---------------- */
static volatile sig_atomic_t g_running = 1;
static FILE *g_log = NULL;
static const char *g_logpath = "/dev/null";
static const char *g_pidpath = NULL;
static unsigned long g_lines = 0;
static unsigned long g_total_saved_pages = 0;   /* 累计节省的 huge 页数 */

static void on_sig(int s) { (void)s; g_running = 0; }

static void now_str(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm tm_;
    localtime_r(&t, &tm_);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm_);
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

/* 写 PID 文件，方便外部检测守护进程是否存活 */
static void write_pid_file(void)
{
    if (!g_pidpath) return;
    FILE *f = fopen(g_pidpath, "w");
    if (f) {
        fprintf(f, "%d\n", getpid());
        fclose(f);
    }
}

static void remove_pid_file(void)
{
    if (g_pidpath) unlink(g_pidpath);
}

/* 提高 OOM 优先级，降低被 LMK 杀掉的概率 */
static void protect_from_oom(void)
{
    int fd = open("/proc/self/oom_score_adj", O_WRONLY);
    if (fd >= 0) {
        write(fd, "-800", 4);
        close(fd);
    }
}

/* 设置为低优先级调度，避免抢占前台应用 CPU */
static void set_low_priority(void)
{
    /* nice 值提高到 10（后台任务级别） */
    nice(10);
}

/* ---------------- 日志轮转（getline 版，O(n) 但常数更小） ---------------- */
static void log_rotate(unsigned long keep)
{
    if (!g_log) return;
    fflush(g_log);

    FILE *r = fopen(g_logpath, "r");
    if (!r) return;

    /* 第一遍：统计行数 */
    unsigned long total = 0;
    int c;
    while ((c = fgetc(r)) != EOF) if (c == '\n') total++;
    if (total <= keep) { fclose(r); return; }
    fclose(r);

    /* 第二遍：用 getline 读取，保留最后 keep 行 */
    r = fopen(g_logpath, "r");
    if (!r) return;

    unsigned long skip = total - keep;
    char *line = NULL;
    size_t linecap = 0;
    ssize_t len;

    /* 跳过前 skip 行 */
    for (unsigned long i = 0; i < skip; i++) {
        if (getline(&line, &linecap, r) < 0) break;
    }

    /* 读取剩余行到内存 */
    char *buf = NULL;
    size_t cap = 0, blen = 0;
    while ((len = getline(&line, &linecap, r)) > 0) {
        if (blen + (size_t)len + 1 > cap) {
            cap = (blen + (size_t)len + 1) * 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); free(line); fclose(r); return; }
            buf = nb;
        }
        memcpy(buf + blen, line, (size_t)len);
        blen += (size_t)len;
    }
    free(line);
    fclose(r);
    if (!buf) return;

    FILE *w = fopen(g_logpath, "w");
    if (!w) { free(buf); return; }
    fwrite(buf, 1, blen, w);
    fclose(w);
    free(buf);

    if (freopen(g_logpath, "a", g_log) == NULL) {
        fclose(g_log);
        g_log = fopen(g_logpath, "a");
        if (!g_log) return;
    }
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
            g_thresh_normal = strtol(val, NULL, 10);
        else if (!strcmp(key, "IDLE_TIMEOUT_MS"))
            g_idle_timeout_ms = strtol(val, NULL, 10);
        else if (!strcmp(key, "MIN_GAP_SEC"))
            g_min_gap_sec = strtol(val, NULL, 10);
        else if (!strcmp(key, "HYSTERESIS_SEC"))
            g_hysteresis_sec = strtol(val, NULL, 10);
        else if (!strcmp(key, "HYST_MARGIN"))
            g_hyst_margin = strtol(val, NULL, 10);
        else if (!strcmp(key, "LOG_MAX_LINES"))
            g_log_max_lines = strtol(val, NULL, 10);
        else if (!strcmp(key, "LOG_KEEP_LINES"))
            g_log_keep_lines = strtol(val, NULL, 10);
        else if (!strcmp(key, "SKIP_LOG_EVERY"))
            g_skip_log_every = strtol(val, NULL, 10);
        else if (!strcmp(key, "PSI_TRIGGER")) {
            strncpy(g_psi_trigger, val, sizeof(g_psi_trigger) - 1);
            g_psi_trigger[sizeof(g_psi_trigger) - 1] = '\0';
        }
    }
    fclose(f);
}

/* ---------------- mm_stat 读取（健壮版） ---------------- */
static long read_huge(void)
{
    FILE *f = fopen(MM_STAT, "r");
    if (!f) return -1;

    char buf[512];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    fclose(f);

    /*
     * mm_stat 字段（内核文档）：
     *   orig_data_size compr_data_size mem_used_total mem_limit
     *   mem_used_max same_pages pages_compacted huge_pages
     * 第 8 个字段（index 7）= huge_pages
     */
    char *saveptr = NULL;
    char *tok = strtok_r(buf, " \t\n", &saveptr);
    int idx = 0;
    long val = -1;

    while (tok) {
        if (idx == 7) {
            val = strtol(tok, NULL, 10);
            break;
        }
        idx++;
        tok = strtok_r(NULL, " \t\n", &saveptr);
    }
    return val;
}

static int do_recompress(void)
{
    int fd = open(RECOMP, O_WRONLY | O_CLOEXEC);
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

    /* 先加载配置文件（如果有的话） */
    if (configpath) load_config(configpath);

    g_logpath = logpath;
    g_log = fopen(logpath, "a");

    /* PID 文件路径：日志路径同目录下的 psi_recomp.pid */
    static char pidpath[512];
    snprintf(pidpath, sizeof(pidpath), "%s", logpath);
    char *slash = strrchr(pidpath, '/');
    if (slash) {
        strcpy(slash + 1, "psi_recomp.pid");
        g_pidpath = pidpath;
    }

    signal(SIGTERM, on_sig);
    signal(SIGINT,  on_sig);
    signal(SIGHUP,  SIG_IGN);

    /* 自行脱离会话/进程组 */
    if (setsid() < 0 && errno != EPERM) {
        /* 已是进程组组长时 setsid 会失败，可忽略 */
    }

    protect_from_oom();
    set_low_priority();
    write_pid_file();

    plog("=== psi_recomp start (pid=%d, thresh=%ld, hyst=%ds/+%ld, gap=%lds) ===",
         getpid(), g_thresh_normal, g_hysteresis_sec, g_hyst_margin, g_min_gap_sec);

    time_t last_check = 0;
    time_t last_comp  = 0;
    long last_residual = 0;
    unsigned long skip_streak = 0;
    long peak = 0;
    unsigned long n_check = 0, n_comp = 0, n_psi = 0;

    /* 先探测一次：PSI 是否可用，不可用则全程走定时 */
    int psi_ok = 0;
    {
        int probe = psi_arm();
        if (probe >= 0) {
            psi_ok = 1;
            plog("PSI armed: %s", g_psi_trigger);
            close(probe);
        } else {
            plog("PSI unavailable (%s) -> fallback to timer", strerror(errno));
        }
    }

    while (g_running) {
        int fired = 0;
        int psi = -1;

        if (psi_ok) {
            /*
             * 每轮都用全新 fd 武装：PSI 触发是边沿触发且绑定到 fd，
             * 在旧 fd 上直接 write 重设会返回 EBUSY（实测 v1.0.3 表现），
             * 换 fd 可彻底避开残留状态。
             */
            psi = psi_arm();
            if (psi < 0) {
                plog("PSI arm failed (%s) -> fallback to timer", strerror(errno));
                psi_ok = 0;
            } else {
                struct pollfd pfd;
                pfd.fd = psi;
                pfd.events = POLLPRI;
                pfd.revents = 0;

                int r = poll(&pfd, 1, (int)g_idle_timeout_ms);
                if (r < 0) {
                    if (errno == EINTR) { close(psi); continue; }
                    plog("poll error: %s -> fallback to timer", strerror(errno));
                    psi_ok = 0;
                } else if (r > 0) {
                    fired = 1;
                    n_psi++;
                }
            }
        }

        if (psi_ok && psi >= 0) { close(psi); psi = -1; }
        if (!psi_ok && !fired) sleep_full(g_idle_timeout_ms / 1000);

        time_t now = time(NULL);
        /* 限流 */
        if (last_check && (now - last_check) < g_min_gap_sec) continue;
        last_check = now;
        n_check++;

        long huge = read_huge();
        if (huge < 0) {
            plog("read mm_stat failed: %s", strerror(errno));
            continue;
        }
        if (huge > peak) peak = huge;

        /* 滞回：门槛 = 上次残留 + margin */
        int in_hyst = (last_comp != 0) && ((now - last_comp) < g_hysteresis_sec);
        long th = g_thresh_normal;
        if (in_hyst && last_residual > 0) {
            long cand = last_residual + g_hyst_margin;
            if (cand > th) th = cand;
        }

        if (huge > th) {
            plog("evt=%s huge=%ld > %ld%s -> recompress",
                 fired ? "psi" : "idle", huge, th, in_hyst ? "(hyst)" : "");
            skip_streak = 0;
            if (do_recompress() == 0) {
                long after = read_huge();
                long saved = huge - after;
                g_total_saved_pages += (saved > 0) ? saved : 0;
                plog("  done: %ld -> %ld (-%ld, -%.1f%%)",
                     huge, after, saved,
                     huge > 0 ? (double)saved / huge * 100 : 0.0);
                last_comp = now;
                if (after > 0) last_residual = after;
                n_comp++;

                /* 每 10 次压缩输出一次累计统计 */
                if (n_comp % 10 == 0) {
                    plog("stats: total_comp=%lu, total_saved=%lu pages (%.1f MiB), peak=%ld",
                         n_comp, g_total_saved_pages,
                         g_total_saved_pages * 4.0 / 1024.0, peak);
                }
            } else {
                plog("  recompress FAILED: %s", strerror(errno));
            }
        } else {
            skip_streak++;
            if (skip_streak % (unsigned long)g_skip_log_every == 1) {
                plog("evt=%s huge=%ld <= %ld%s skip x%lu (peak=%ld)",
                     fired ? "psi" : "idle", huge, th, in_hyst ? "(hyst)" : "",
                     skip_streak, peak);
            }
        }
    }

    plog("=== exit: checks=%lu psi_events=%lu recompress=%lu peak=%ld total_saved=%lu pages (%.1f MiB) ===",
         n_check, n_psi, n_comp, peak,
         g_total_saved_pages, g_total_saved_pages * 4.0 / 1024.0);
    if (g_log) fclose(g_log);
    remove_pid_file();
    return 0;
}
