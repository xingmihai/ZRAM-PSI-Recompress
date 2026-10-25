MODDIR=${0%/*}
CONFIG_FILE="$MODDIR/config.prop"
LOG="$MODDIR/zram_module.log"

if [ ! -f "$CONFIG_FILE" ]; then
  echo "config.prop not found! Exiting."
  exit 1
fi

. "$CONFIG_FILE"

if [ -z "$ZRAM_ALGO" ] || [ -z "$ZRAM_SIZE" ]; then
  echo "ZRAM_ALGO or ZRAM_SIZE is not set! Exiting."
  exit 1
fi

: > "$LOG"
echo "=== $(date '+%F %H:%M:%S') boot ===" >> "$LOG"

sleep 30

# 重置现有 zram 设备
# zram0 与 lz4kd/zstdn/zstdn_o 均由内核自带（Oplus 机型为 oplus_bsp_hybridswap_zram
# + oplus_bsp_zstdn），本模块不携带也不加载任何 .ko
for dev in /dev/block/zram*; do
  if [ -e "$dev" ]; then
    echo "Disabling $dev"
    swapoff "$dev" 2>/dev/null
    echo 1 > "/sys/block/$(basename "$dev")/reset"
  fi
done

if [ ! -e /dev/block/zram0 ]; then
  echo "zram0 not found, failed to initialize swap" >> "$LOG"
  exit 1
fi

sleep 5
echo '1' > /sys/block/zram0/reset
echo '0' > /sys/block/zram0/disksize
echo '8' > /sys/block/zram0/max_comp_streams
echo "${ZRAM_ALGO}" > /sys/block/zram0/comp_algorithm

# 两级重压缩：zstdn 压不动的页交给 zstdn_o
echo "algo=zstdn priority=1" > /sys/block/zram0/recomp_algorithm
echo "recomp p1(zstdn) rc=$?" >> "$LOG"
echo "algo=zstdn_o priority=2" > /sys/block/zram0/recomp_algorithm
echo "recomp p2(zstdn_o) rc=$?" >> "$LOG"

echo "${ZRAM_SIZE}" > /sys/block/zram0/disksize
mkswap /dev/block/zram0 > /dev/null 2>&1
swapon /dev/block/zram0 > /dev/null 2>&1
echo "zram0 active: ${ZRAM_ALGO} / ${ZRAM_SIZE}"

echo "--- recomp_algorithm ---" >> "$LOG"
cat /sys/block/zram0/recomp_algorithm >> "$LOG" 2>&1
echo "--- comp_algorithm ---" >> "$LOG"
cat /sys/block/zram0/comp_algorithm >> "$LOG" 2>&1

# ---------------- 启动重压缩守护 ----------------
# 优先使用 PSI 事件驱动 helper（空闲零唤醒、突发秒级响应、带滞回）
# helper 内部已含定时兜底：PSI 不可用时自动降级为 1800 秒轮询
# 若 helper 缺失或启动失败，则退回纯 shell 定时循环，保证功能不丢
#
# 注意：zip 打包/下载/解压链路会丢失可执行位（实测刷入后全部变成 644），
# 故此处先自行 chmod，不能只依赖 [ -x ] 判定，否则会静默回退到 shell 循环
if [ -f "$MODDIR/psi_recomp" ]; then
  chmod 0755 "$MODDIR/psi_recomp" 2>/dev/null
  echo "starting psi_recomp daemon (mode=$(stat -c '%a' "$MODDIR/psi_recomp" 2>/dev/null))" >> "$LOG"

  # 清理旧 PID 文件和日志
  rm -f "$MODDIR/psi_recomp.pid"
  : > "$MODDIR/psi.log"

  # 构建可选配置文件路径
  PSI_CONF="$MODDIR/psi.conf"
  if [ -f "$PSI_CONF" ]; then
    echo "using config: $PSI_CONF" >> "$LOG"
    setsid "$MODDIR/psi_recomp" "$MODDIR/psi.log" "$PSI_CONF" &
  else
    setsid "$MODDIR/psi_recomp" "$MODDIR/psi.log" &
  fi

  HELPER_PID=$!
  sleep 3

  # 双重检测：日志有 start 且 PID 文件已写入
  PID_ALIVE=""
  if [ -f "$MODDIR/psi_recomp.pid" ]; then
    PID_IN_FILE=$(cat "$MODDIR/psi_recomp.pid" 2>/dev/null)
    if [ -n "$PID_IN_FILE" ] && kill -0 "$PID_IN_FILE" 2>/dev/null; then
      PID_ALIVE="$PID_IN_FILE"
    fi
  fi

  # 如果 PID 文件检测失败，回退到 $! 检测（setsid 可能 fork，$! 未必准确）
  if [ -z "$PID_ALIVE" ] && kill -0 "$HELPER_PID" 2>/dev/null; then
    PID_ALIVE="$HELPER_PID"
  fi

  if grep -q "psi_recomp start" "$MODDIR/psi.log" 2>/dev/null && [ -n "$PID_ALIVE" ]; then
    echo "psi_recomp running (pid=$PID_ALIVE)" >> "$LOG"
    DAEMON_PID=1
  else
    echo "psi_recomp failed to start, falling back to shell loop" >> "$LOG"
    echo "  psi.log: $(cat "$MODDIR/psi.log" 2>/dev/null)" >> "$LOG"
    # 清理可能残留的进程
    [ -n "$HELPER_PID" ] && kill "$HELPER_PID" 2>/dev/null
    [ -n "$PID_IN_FILE" ] && kill "$PID_IN_FILE" 2>/dev/null
    rm -f "$MODDIR/psi_recomp.pid"
    DAEMON_PID=""
  fi
fi

# 兜底：纯 shell 定时循环（无 PSI、无滞回，行为保守但可靠）
# helper 已运行时不再启动，避免两套逻辑同时重压缩
if [ -n "$DAEMON_PID" ]; then
  echo "psi mode active, shell loop skipped" >> "$LOG"
  exit 0
fi

(
RLOG="$MODDIR/recompress.log"
ST="$MODDIR/status"
: > "$RLOG"
THRESH=${PSI_THRESH_NORMAL:-40000}
IDLE=${PSI_IDLE_SEC:-1800}
while true; do
  huge=$(awk '{print $8}' /sys/block/zram0/mm_stat)
  ts=$(date '+%F %H:%M:%S')
  if [ "$huge" -gt "$THRESH" ]; then
    echo "=== $ts ===" >> "$RLOG"
    echo "BEFORE: $(cat /sys/block/zram0/mm_stat)" >> "$RLOG"
    echo "type=huge" > /sys/block/zram0/recompress
    echo "AFTER : $(cat /sys/block/zram0/mm_stat)" >> "$RLOG"
  else
    echo "$ts huge=$huge skip(<$THRESH)" >> "$RLOG"
  fi
  echo "$ts huge=$huge" > "$ST"
  tail -n 300 "$RLOG" > "$RLOG.tmp" && mv "$RLOG.tmp" "$RLOG"
  sleep "$IDLE"
done
) &
