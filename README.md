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
3. **滞回** — 刚压完 30 分钟内阈值从 40000 提到 52000，避开残留硬骨头（实测残留 >40000 的比例达 47%）
4. **限流** — 两次检查最小间隔 60 秒，防 PSI 风暴
5. **定时兜底** — PSI 不可用（open/write/poll 失败）自动降级为 1800 秒轮询，功能不丢失

## 参数

在 `src/psi_recomp.c` 顶部调整：

| 宏 | 默认 | 含义 |
|---|---|---|
| `THRESH_NORMAL` | 40000 | 常态触发阈值（huge 页数） |
| `THRESH_AFTER` | 52000 | 刚压完时的滞回阈值 |
| `HYSTERESIS_SEC` | 1800 | 滞回持续时间（秒） |
| `IDLE_TIMEOUT_MS` | 1800000 | 兜底轮询间隔（毫秒） |
| `MIN_GAP_SEC` | 60 | 两次检查最小间隔 |
| `PSI_TRIGGER` | `some 15000 1000000` | 1 秒窗口内 stall > 15ms 触发 |

## 构建

推送到 `main` 分支即触发 GitHub Actions，用 Android NDK r27c 交叉编译 arm64 静态二进制，产出可刷的 `ZRAM-PSI-Recompress.zip`。

## 安装

Magisk / KernelSU 刷入 zip 即可。模块本身**不携带任何 .ko** —— zram 设备与 lz4kd/zstdn/zstdn_o 算法由内核（Oplus 机型为 `oplus_bsp_hybridswap_zram` + `oplus_bsp_zstdn`）自带。

## 验证

```
# 守护是否活着
ps -A | grep psi_recomp

# PSI 是否武装成功
head -20 /data/adb/modules/ZRAM-PSI-Recompress/psi.log

# 开机配置（含两级重压缩注册结果）
cat /data/adb/modules/ZRAM-PSI-Recompress/zram_module.log
```

日志示例：

```
[10-24 15:30:52] === psi_recomp start (pid=1156, thresh=40000/52000, hyst=1800s) ===
[10-24 15:30:52] PSI armed: some 15000 1000000
[10-24 16:02:11] evt=psi huge=46890 > 40000 -> recompress
[10-24 16:02:11]   done: 46890 -> 31204 (-15686, -33.5%)
```

## 说明

- 需要内核开启 `CONFIG_PSI`（`/proc/pressure/memory` 存在即可）
- 若缺失自动降级为定时轮询，不会失效
- `service.sh` 内含纯 shell 兜底循环，helper 缺失/启动失败时接管
