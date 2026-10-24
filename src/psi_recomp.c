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
 *   3. 滞回：刚压完 30 分钟内把阈值从 40000 提到 52000，避免在残留区反复空压
 *      （实测残留 >40000 的比例达 47%，这是此前 5 分钟复检失效的根因）。
 *   4. 限流：两次检查最小间隔 60 秒，防 PSI 风暴。
 *   5. 兜底：即使 PSI 完全不可用（open/write/poll 失败），自动降级为纯定时
 *      1800 秒轮询，行为与定时方案一致，功能不丢失。
 *
 * 用法: psi_recomp <logpath>
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

#define PSI_PATH  "/proc/pressure/memory"
#define MM_STAT   "/sys/block/zram0/mm_stat"
#define RECOMP    "/sys/block/zram0/recompress"

/* ---------------- 可调参数 ---------------- */
#define THRESH_NORMAL   40000   /* 常态触发阈值（huge 页数） */
#define IDLE_TIMEOUT_MS (1800 * 1000)  /* 兜底：无事件时最多等 30 分钟 */
#define MIN_GAP_SEC     60      /* 两次检查最小间隔，防 PSI 风暴 */
#define PSI_TRIGGER     "some 15000 1000000"  /* 1s 窗口内 stall > 15ms 触发 */

/*
 * 滞回：刚压完的一段时间内提高门槛，避免啃残留硬骨头。
 *
 * 注意：HYSTERESIS_SEC 必须显著大于 IDLE_TIMEOUT_MS/1000。
 * 二者相等时，下一次检查恰好落在窗口边界上（实测 dt=1800~1802s），
 * (dt < HYSTERESIS_SEC) 恒为 false，滞回从不生效 —— 这是 v1.0.3 的实际表现。
 */
#define HYSTERESIS_SEC  5400    /* 滞回持续 90 分钟 = 3 个检查周期 */

/*
 * 滞回门槛不再用固定值，而是「上次压缩后的残留 + 余量」。
 * 原因：实测压缩后残留会从 31839 一路涨到 55269（地板抬升 74%），
 * 固定的 52000 很快就被地板超过，形同虚设。
 * 以残留为基准才能真正表达「等新料攒够再压」。
 */
#define HYST_MARGIN     8000    /* 残留之上再新增这么多页才值得再压 */

/*
 * 日志轮转：
 * 实测 PSI 模式下约 631 行/天（其中 85% 是 skip 行），
 * 若不轮转会无限增长，最终占满 /data/adb/modules 所在分区。
 * 超过 LOG_MAX_LINES 时保留最近 LOG_KEEP_LINES 行。
 */
#define LOG_MAX_LINES   600     /* 超过此行数触发轮转 */
#define LOG_KEEP_LINES  300     /* 轮转后保留的行数 */

/*
 * skip 行降频：连续 skip 时每 SKIP_LOG_EVERY 次才记一行，
 * 中间用计数汇总。避免日志被无信息的 skip 淹没。
 */
#define SKIP_LOG_EVERY  5

static volatile sig_atomic_t g_running = 1;
static FILE *g_log = NULL;
static const char *g_logpath = "/dev/null";
static unsigned long g_lines = 0;   /* 本次启动写入的行数（轮转后重新计数） */

static void on_sig(int s) { (void)s; g_running = 0; }

static void now_str(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm tm_;
    localtime_r(&t, &tm_);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm_);
}

/* 保留文件末尾 keep 行：读入内存后截断重写，避免依赖外部命令 */
static void log_rotate(unsigned long keep)
{
    if (!g_log) return;
    fflush(g_log);

    FILE *r = fopen(g_logpath, "r");
    if (!r) return;

    /* 先统计总行数 */
    unsigned long total = 0;
    int c;
    while ((c = fgetc(r)) != EOF) if (c == '\n') total++;
    if (total <= keep) { fclose(r); return; }

    unsigned long start = total - keep;   /* 丢弃前 start 行 */
    rewind(r);
    unsigned long seen = 0;
    char *buf = NULL;
    size_t cap = 0, len = 0;

    while ((c = fgetc(r)) != EOF) {
        if (seen >= start) {
            if (len + 1 >= cap) {
                cap = cap ? cap * 2 : 8192;
                char *nb = realloc(buf, cap);
                if (!nb) { free(buf); fclose(r); return; }
                buf = nb;
            }
            buf[len++] = (char)c;
        }
        if (c == '\n') seen++;
    }
    fclose(r);
    if (!buf) return;

    FILE *w = fopen(g_logpath, "w");
    if (!w) { free(buf); return; }
    fwrite(buf, 1, len, w);
    fclose(w);
    free(buf);

    /* 重新以追加方式打开，保持 plog 可用 */
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

    if (++g_lines >= LOG_MAX_LINES) log_rotate(LOG_KEEP_LINES);
}

/* 读 mm_stat 第 8 个字段 = huge 页数；失败返回 -1 */
static long read_huge(void)
{
    FILE *f = fopen(MM_STAT, "r");
    if (!f) return -1;
    long v[9];
    for (int i = 0; i < 9; i++) {
        if (fscanf(f, "%ld", &v[i]) != 1) { fclose(f); return -1; }
    }
    fclose(f);
    return v[7];
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
    if (write(fd, PSI_TRIGGER, strlen(PSI_TRIGGER)) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    return fd;
}

int main(int argc, char **argv)
{
    const char *logpath = (argc > 1) ? argv[1] : "/dev/null";
    g_logpath = logpath;
    g_log = fopen(logpath, "a");

    signal(SIGTERM, on_sig);
    signal(SIGINT,  on_sig);
    signal(SIGHUP,  SIG_IGN);

    /* 自行脱离会话/进程组：父进程(service.sh)退出或被 ksud 清理时不受牵连 */
    if (setsid() < 0 && errno != EPERM) {
        /* 已是进程组组长时 setsid 会失败，可忽略 */
    }

    plog("=== psi_recomp start (pid=%d, thresh=%ld, hyst=%ds/+%ld) ===",
         getpid(), THRESH_NORMAL, HYSTERESIS_SEC, HYST_MARGIN);

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
            plog("PSI armed: %s", PSI_TRIGGER);
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

                int r = poll(&pfd, 1, IDLE_TIMEOUT_MS);
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
        if (!psi_ok && !fired) sleep(IDLE_TIMEOUT_MS / 1000);

        time_t now = time(NULL);
        /* 限流：PSI 可能高频触发，两次实际检查至少间隔 MIN_GAP_SEC */
        if (last_check && (now - last_check) < MIN_GAP_SEC) continue;
        last_check = now;
        n_check++;

        long huge = read_huge();
        if (huge < 0) {
            plog("read mm_stat failed: %s", strerror(errno));
            continue;
        }
        if (huge > peak) peak = huge;

        /*
         * 滞回：刚压完的一段时间内提高门槛，避免啃残留硬骨头。
         * 门槛 = 上次压缩后的残留 + HYST_MARGIN，而非固定值 ——
         * 实测残留会从 31839 涨到 55269，固定值很快被地板淹没。
         */
        int in_hyst = (last_comp != 0) && ((now - last_comp) < HYSTERESIS_SEC);
        long th = THRESH_NORMAL;
        if (in_hyst && last_residual > 0) {
            long cand = last_residual + HYST_MARGIN;
            if (cand > th) th = cand;
        }

        if (huge > th) {
            plog("evt=%s huge=%ld > %ld%s -> recompress",
                 fired ? "psi" : "idle", huge, th, in_hyst ? "(hyst)" : "");
            skip_streak = 0;
            if (do_recompress() == 0) {
                long after = read_huge();
                plog("  done: %ld -> %ld (-%ld, -%.1f%%)",
                     huge, after, huge - after,
                     huge > 0 ? (double)(huge - after) / huge * 100 : 0.0);
                last_comp = now;
                if (after > 0) last_residual = after;
                n_comp++;
            } else {
                plog("  recompress FAILED: %s", strerror(errno));
            }
        } else {
            /*
             * skip 行占日志 85%，连续 skip 时降频记录：
             * 每 SKIP_LOG_EVERY 次才写一行，中间静默。
             * 这样既不丢趋势，也不会淹没真正的压缩记录。
             */
            skip_streak++;
            if (skip_streak % SKIP_LOG_EVERY == 1) {
                plog("evt=%s huge=%ld <= %ld%s skip x%lu (peak=%ld)",
                     fired ? "psi" : "idle", huge, th, in_hyst ? "(hyst)" : "",
                     skip_streak, peak);
            }
        }
    }

    plog("=== exit: checks=%lu psi_events=%lu recompress=%lu peak=%ld ===",
         n_check, n_psi, n_comp, peak);
    if (g_log) fclose(g_log);
    /* psi fd 每轮在循环内关闭，此处无需处理 */
    return 0;
}
