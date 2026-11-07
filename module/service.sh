MODDIR=${0%/*}
CONFIG_FILE="$MODDIR/config.prop"
LOG="$MODDIR/zram_module.log"

if [ ! -f "$CONFIG_FILE" ]; then
  echo "config.prop not found! Exiting."
  exit 1
fi

# 安全读取配置：不再 `. config.prop`（source 等于以 root 执行文件内容，
# 一旦 WebUI 或其他途径写入恶意值就会在每次开机时被执行）。
# 改为逐项取值并按白名单校验，非法值一律视为未设置。
get_cfg() {
  sed -n "s/^$1=//p" "$CONFIG_FILE" | head -n 1 | tr -d '\r' | sed 's/[[:space:]]*$//'
}
valid_name() { case "$1" in ''|*[!A-Za-z0-9_.-]*) return 1;; esac; return 0; }
valid_num()  { case "$1" in ''|*[!0-9]*) return 1;; esac; return 0; }
# zram 设备名：zram + 1~3 位数字；与 C 守护、WebUI 的校验一致
valid_zdev() { case "$1" in zram[0-9]|zram[0-9][0-9]|zram[0-9][0-9][0-9]) return 0;; esac; return 1; }
# PID 是否确实是 psi_recomp（PID 文件可能残留，PID 也可能被复用，不能只靠 kill -0）
# 只比对 argv[0] 的 basename：整条命令行匹配会把 `cat psi_recomp.pid` 之类的无关进程误判为守护。
# 纯 shell 参数展开（${x##*/}），不依赖 basename / local，兼容 /system/bin/sh；变量加前缀避免冲突。
is_psi_pid() {
  [ -n "$1" ] && [ -r "/proc/$1/cmdline" ] || return 1
  _psi_a0=$(tr '\0' '\n' < "/proc/$1/cmdline" 2>/dev/null | sed -n '1p')
  [ "${_psi_a0##*/}" = "psi_recomp" ]
}

ZRAM_ALGO=$(get_cfg ZRAM_ALGO)
ZRAM_SIZE=$(get_cfg ZRAM_SIZE)
RECOMP_ALGO1=$(get_cfg RECOMP_ALGO1)
RECOMP_ALGO2=$(get_cfg RECOMP_ALGO2)
PSI_THRESH_NORMAL=$(get_cfg PSI_THRESH_NORMAL)
PSI_IDLE_SEC=$(get_cfg PSI_IDLE_SEC)
ZRAM_DEV=$(get_cfg ZRAM_DEV)

valid_name "$RECOMP_ALGO1" || RECOMP_ALGO1=""
valid_name "$RECOMP_ALGO2" || RECOMP_ALGO2=""
valid_num "$PSI_THRESH_NORMAL" || PSI_THRESH_NORMAL=""
valid_num "$PSI_IDLE_SEC" || PSI_IDLE_SEC=""
valid_zdev "$ZRAM_DEV" || ZRAM_DEV="zram0"

if ! valid_name "$ZRAM_ALGO" || ! valid_num "$ZRAM_SIZE"; then
  echo "ZRAM_ALGO or ZRAM_SIZE is missing/invalid! Exiting."
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

if [ ! -e "/dev/block/${ZRAM_DEV}" ]; then
  echo "${ZRAM_DEV} not found, failed to initialize swap" >> "$LOG"
  exit 1
fi

sleep 5
echo '1' > /sys/block/${ZRAM_DEV}/reset
echo '0' > /sys/block/${ZRAM_DEV}/disksize
echo '8' > /sys/block/${ZRAM_DEV}/max_comp_streams
echo "${ZRAM_ALGO}" > /sys/block/${ZRAM_DEV}/comp_algorithm

# 两级重压缩：一级压不动的页交给二级
# 算法名从 config.prop 读取（WebUI 可改），缺省回退到 zstdn / zstdn_o
RECOMP_ALGO1="${RECOMP_ALGO1:-zstdn}"
RECOMP_ALGO2="${RECOMP_ALGO2:-zstdn_o}"
echo "algo=${RECOMP_ALGO1} priority=1" > /sys/block/${ZRAM_DEV}/recomp_algorithm
echo "recomp p1(${RECOMP_ALGO1}) rc=$?" >> "$LOG"
echo "algo=${RECOMP_ALGO2} priority=2" > /sys/block/${ZRAM_DEV}/recomp_algorithm
echo "recomp p2(${RECOMP_ALGO2}) rc=$?" >> "$LOG"

echo "${ZRAM_SIZE}" > /sys/block/${ZRAM_DEV}/disksize
mkswap "/dev/block/${ZRAM_DEV}" > /dev/null 2>&1
swapon "/dev/block/${ZRAM_DEV}" > /dev/null 2>&1
echo "${ZRAM_DEV} active: ${ZRAM_ALGO} / ${ZRAM_SIZE}"

echo "--- recomp_algorithm ---" >> "$LOG"
cat /sys/block/${ZRAM_DEV}/recomp_algorithm >> "$LOG" 2>&1
echo "--- comp_algorithm ---" >> "$LOG"
cat /sys/block/${ZRAM_DEV}/comp_algorithm >> "$LOG" 2>&1

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

  # 不再 rm -f PID 文件：旧进程若仍持有旧 inode 的 flock，删掉后新进程会拿到
  # 新 inode 的锁，单实例保护失效且旧进程退出时还会 unlink 新进程的 PID 文件。
  # 残留但无人持有的 PID 文件由守护自己 open(O_CREAT) 同一 inode、靠 flock 复用。
  OLD_PID=$(cat "$MODDIR/psi_recomp.pid" 2>/dev/null)
  if is_psi_pid "$OLD_PID"; then
    echo "psi_recomp already running (pid=$OLD_PID), skip start" >> "$LOG"
    DAEMON_PID=1
  else
    : > "$MODDIR/psi.log"

    # 配置文件不存在时守护会静默使用默认值；ZRAM_DEV 以第 3 个参数传入（唯一来源 config.prop）
    PSI_CONF="$MODDIR/psi.conf"
    [ -f "$PSI_CONF" ] && echo "using config: $PSI_CONF" >> "$LOG"
    setsid "$MODDIR/psi_recomp" "$MODDIR/psi.log" "$PSI_CONF" "$ZRAM_DEV" &

    HELPER_PID=$!
    sleep 3

    # 双重检测：日志有 start 且 PID 文件里的进程确实是 psi_recomp
    PID_ALIVE=""
    PID_IN_FILE=$(cat "$MODDIR/psi_recomp.pid" 2>/dev/null)
    if is_psi_pid "$PID_IN_FILE"; then
      PID_ALIVE="$PID_IN_FILE"
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
      # 清理可能残留的进程（只杀确认是 psi_recomp 的；也不再删 PID 文件）
      [ -n "$HELPER_PID" ] && kill "$HELPER_PID" 2>/dev/null
      is_psi_pid "$PID_IN_FILE" && kill "$PID_IN_FILE" 2>/dev/null
      DAEMON_PID=""
    fi
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
  huge=$(awk '{print $8}' /sys/block/${ZRAM_DEV}/mm_stat 2>/dev/null)
  case "$huge" in ''|*[!0-9]*) huge=0;; esac
  ts=$(date '+%F %H:%M:%S')
  if [ "$huge" -gt "$THRESH" ]; then
    echo "=== $ts ===" >> "$RLOG"
    echo "BEFORE: $(cat /sys/block/${ZRAM_DEV}/mm_stat)" >> "$RLOG"
    echo "type=huge" > /sys/block/${ZRAM_DEV}/recompress
    echo "AFTER : $(cat /sys/block/${ZRAM_DEV}/mm_stat)" >> "$RLOG"
  else
    echo "$ts huge=$huge skip(<$THRESH)" >> "$RLOG"
  fi
  echo "$ts huge=$huge" > "$ST"
  tail -n 300 "$RLOG" > "$RLOG.tmp" && mv "$RLOG.tmp" "$RLOG"
  sleep "$IDLE"
done
) &
