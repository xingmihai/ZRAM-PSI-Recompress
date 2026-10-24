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
if [ -x "$MODDIR/psi_recomp" ]; then
  echo "starting psi_recomp daemon" >> "$LOG"
  "$MODDIR/psi_recomp" "$MODDIR/psi.log" &
  DAEMON_PID=$!
  sleep 3
  if kill -0 "$DAEMON_PID" 2>/dev/null; then
    echo "psi_recomp running (pid=$DAEMON_PID)" >> "$LOG"
    exit 0
  fi
  echo "psi_recomp died, fallback to shell loop" >> "$LOG"
fi

# 兜底：纯 shell 定时循环（无 PSI、无滞回，行为保守但可靠）
(
RLOG="$MODDIR/recompress.log"
ST="$MODDIR/status"
: > "$RLOG"
THRESH=40000
IDLE=1800
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
