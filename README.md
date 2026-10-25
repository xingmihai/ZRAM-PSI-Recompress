# ZRAM-PSI-Recompress

基于 **PSI 事件驱动** 的 zram huge 页重压缩守护，替代定时轮询。

## 为什么不用定时轮询

实测同一 huge 区间（42000~44000）下的两次触发：

| 场景 | huge | 压掉 |
|---|---|---|
| 刚压完 5 分钟 | 43880 | **1727** |
| 隔了 35 分钟 | 40910 | **13953** |

**绝对值几乎一样，结果差 8 倍。** 差别不在 huge 有多大，而在距上次压缩多久——刚压完剩下的全是压不动的硬骨头。

定时轮询无法区分这两种状态，所以在残留区反复空压。实测 17 次触发中有 6 次只压掉 705~2974 页，合计仅省 20.72 MiB，却占了 35% 的触发次数。

## 本方案怎么做

1. **PSI 事件驱动** — `poll("/proc/pressure/memory")`，内存回收产生 stall 时内核主动唤醒；空闲时进程完全阻塞，零唤醒零 CPU
2. **裁决仍读 mm_stat** — PSI 只报 stall 时间，不报 huge 页数，唤醒后读第 8 字段决定压不压
3. **自适应滞回** — 刚压完 90 分钟内，阈值 = 上次残留 + 8000 页（而非固定值），避开残留硬骨头
4. **限流** — 两次检查最小间隔 60 秒，防 PSI 风暴
5. **定时兜底** — PSI 不可用（open/write/poll 失败）自动降级为 1800 秒轮询，功能不丢失
6. **运行时配置** — 支持 `psi.conf` 配置文件，无需重新编译即可调整所有参数

## 参数配置

### 方式一：配置文件（推荐）

在模块目录创建 `psi.conf`（刷入后路径：`/data/adb/modules/ZRAM-PSI-Recompress/psi.conf`）：

```ini
# 常态触发阈值（huge 页数）
THRESH_NORMAL=40000

# 滞回持续时间（秒）
HYSTERESIS_SEC=5400

# 滞回余量（页数）
HYST_MARGIN=8000

# 两次检查最小间隔（秒）
MIN_GAP_SEC=60

# 兜底轮询间隔（毫秒）
IDLE_TIMEOUT_MS=1800000

# PSI 触发条件
PSI_TRIGGER=some 15000 1000000

# 日志轮转
LOG_MAX_LINES=600
LOG_KEEP_LINES=300

# skip 日志降频
SKIP_LOG_EVERY=5
```

修改后重启模块生效。

### 方式二：编译时默认值

在 `src/psi_recomp.c` 中修改 `g_*` 全局变量的初始值。

## 构建

推送到 `main` 分支即触发 GitHub Actions，用 Android NDK r27c 交叉编译 arm64 动态二进制（兼容 Android 15/16 的 16KB page size），产出可刷的 `ZRAM-PSI-Recompress.zip`。

## 安装

Magisk / KernelSU 刷入 zip 即可。模块本身**不携带任何 .ko** —— zram 设备与 lz4kd/zstdn/zstdn_o 算法由内核（Oplus 机型为 `oplus_bsp_hybridswap_zram` + `oplus_bsp_zstdn`）自带。

## 验证

```bash
# 守护是否活着（通过 PID 文件）
cat /data/adb/modules/ZRAM-PSI-Recompress/psi_recomp.pid
ps -A | grep psi_recomp

# PSI 是否武装成功
head -20 /data/adb/modules/ZRAM-PSI-Recompress/psi.log

# 开机配置（含两级重压缩注册结果）
cat /data/adb/modules/ZRAM-PSI-Recompress/zram_module.log
```

日志示例：

```
[2025-10-24 15:30:52] === psi_recomp start (pid=1156, thresh=40000, hyst=5400s/+8000, gap=60s) ===
[2025-10-24 15:30:52] PSI armed: some 15000 1000000
[2025-10-24 16:02:11] evt=psi huge=46890 > 40000 -> recompress
[2025-10-24 16:02:11]   done: 46890 -> 31204 (-15686, -33.5%)
[2025-10-24 18:30:00] stats: total_comp=10, total_saved=145230 pages (567.3 MiB), peak=52341
```


## WebUI（KernelSU）

v1.2.0 起支持 KernelSU 模块 WebUI，可在 KernelSU 管理器中直接：

- 查看守护进程运行状态（PID、huge 页数、PSI 状态）
- 查看压缩统计（总检查次数、重压缩次数、累计节省内存）
- 可视化编辑 `psi.conf` 配置参数
- 实时查看日志（psi.log / zram_module.log / recompress.log）
- 快捷操作：立即触发重压缩、重启守护进程、测试 PSI

> WebUI 需要 KernelSU 管理器 v0.9.0+，Magisk 用户无影响。

## 说明

- 需要内核开启 `CONFIG_PSI`（`/proc/pressure/memory` 存在即可）
- 若缺失自动降级为定时轮询，不会失效
- `service.sh` 内含纯 shell 兜底循环，helper 缺失/启动失败时接管
- 守护进程自动设置 `oom_score_adj=-800` 和 `nice=10`，降低被系统杀掉和抢占前台 CPU 的概率
- v1.2.0 起支持 `psi.conf` 运行时配置，无需重新编译即可调参

## 更新日志

### v1.2.0
- 新增 KernelSU 模块 WebUI（可视化状态监控、配置编辑、日志查看、快捷操作）

### v1.1.0
- 新增 `psi.conf` 运行时配置文件支持
- 修复 `sleep()` 被信号中断后提前醒来的问题
- 修复 `read_huge()` 解析健壮性（改用 `getline` + `strtok_r`）
- 优化 `log_rotate()` 性能
- 添加 OOM 保护（`oom_score_adj=-800`）和低优先级调度（`nice=10`）
- 添加累计压缩统计（每 10 次压缩输出一次）
- 守护进程自动写 PID 文件，方便外部检测
- 统一版本号为 v1.1.0
