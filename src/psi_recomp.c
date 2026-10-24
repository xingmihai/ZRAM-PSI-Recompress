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
#define THRESH_AFTER    52000   /* 刚压完时的滞回阈值 */
#define HYSTERESIS_SEC  1800    /* 滞回持续时间：30 分钟 */
#define IDLE_TIMEOUT_MS (1800 * 1000)  /* 兜底：无事件时最多等 30 分钟 */
#define MIN_GAP_SEC     60      /* 两次检查最小间隔，防 PSI 风暴 */
#define PSI_TRIGGER     "some 15000 1000000"  /* 1s 窗口内 stall > 15ms 触发 */

static volatile sig_atomic_t g_running = 1;
static FILE *g_log = NULL;

static void on_sig(int s) { (void)s; g_running = 0; }

static void now_str(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm tm_;
    localtime_r(&t, &tm_);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm_);
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

int main(int argc, char **argv)
{
    const char *logpath = (argc > 1) ? argv[1] : "/dev/null";
    g_log = fopen(logpath, "a");

    signal(SIGTERM, on_sig);
    signal(SIGINT,  on_sig);
    signal(SIGHUP,  SIG_IGN);

    /* 自行脱离会话/进程组：父进程(service.sh)退出或被 ksud 清理时不受牵连 */
    if (setsid() < 0 && errno != EPERM) {
        /* 已是进程组组长时 setsid 会失败，可忽略 */
    }

    plog("=== psi_recomp start (pid=%d, thresh=%d/%d, hyst=%ds) ===",
         getpid(), THRESH_NORMAL, THRESH_AFTER, HYSTERESIS_SEC);

    int psi = open(PSI_PATH, O_RDWR | O_CLOEXEC);
    int psi_ok = 0;
    if (psi >= 0) {
        if (write(psi, PSI_TRIGGER, strlen(PSI_TRIGGER)) >= 0) {
            psi_ok = 1;
            plog("PSI armed: %s", PSI_TRIGGER);
        } else {
            plog("PSI trigger write failed: %s -> fallback to timer", strerror(errno));
        }
    } else {
        plog("PSI open failed (%s) -> fallback to timer", strerror(errno));
    }

    time_t last_check = 0;
    time_t last_comp  = 0;
    long peak = 0;
    unsigned long n_check = 0, n_comp = 0, n_psi = 0;

    while (g_running) {
        int fired = 0;

        if (psi_ok) {
            struct pollfd pfd;
            pfd.fd = psi;
            pfd.events = POLLPRI;
            pfd.revents = 0;

            int r = poll(&pfd, 1, IDLE_TIMEOUT_MS);
            if (r < 0) {
                if (errno == EINTR) continue;
                plog("poll error: %s -> fallback to timer", strerror(errno));
                psi_ok = 0;
            } else if (r > 0) {
                fired = 1;
                n_psi++;
                /* 读走事件内容，然后重新 arm（PSI 为边沿触发） */
                char buf[256];
                lseek(psi, 0, SEEK_SET);
                ssize_t n = read(psi, buf, sizeof(buf) - 1);
                if (n > 0) buf[n] = '\0';
                lseek(psi, 0, SEEK_SET);
                if (write(psi, PSI_TRIGGER, strlen(PSI_TRIGGER)) < 0) {
                    plog("re-arm failed: %s -> fallback to timer", strerror(errno));
                    psi_ok = 0;
                }
            }
        } else {
            sleep(IDLE_TIMEOUT_MS / 1000);
        }

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

        /* 滞回：刚压完的一段时间内提高门槛，避免啃残留硬骨头 */
        int in_hyst = (last_comp != 0) && ((now - last_comp) < HYSTERESIS_SEC);
        long th = in_hyst ? THRESH_AFTER : THRESH_NORMAL;

        if (huge > th) {
            plog("evt=%s huge=%ld > %ld%s -> recompress",
                 fired ? "psi" : "idle", huge, th, in_hyst ? "(hyst)" : "");
            if (do_recompress() == 0) {
                long after = read_huge();
                plog("  done: %ld -> %ld (-%ld, -%.1f%%)",
                     huge, after, huge - after,
                     huge > 0 ? (double)(huge - after) / huge * 100 : 0.0);
                last_comp = now;
                n_comp++;
            } else {
                plog("  recompress FAILED: %s", strerror(errno));
            }
        } else {
            plog("evt=%s huge=%ld <= %ld%s skip (peak=%ld)",
                 fired ? "psi" : "idle", huge, th, in_hyst ? "(hyst)" : "", peak);
        }
    }

    plog("=== exit: checks=%lu psi_events=%lu recompress=%lu peak=%ld ===",
         n_check, n_psi, n_comp, peak);
    if (g_log) fclose(g_log);
    if (psi >= 0) close(psi);
    return 0;
}
