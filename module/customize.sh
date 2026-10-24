# 自动获取模块名称与路径
MODNAME="$(basename "$MODPATH")"

ui_print "-------------"
ui_print "  _____           _     ____ "
ui_print " |  ___|   _ _ __| |   / ___|"
ui_print " | |_ | | | | '__| |  | |    "
ui_print " |  _|| |_| | |  | |__| |___ "
ui_print " |_|   \__,_|_|  |_____\____|"
ui_print "      FurLC ZRAM Module      "
ui_print "        (Lite / 精简版)      "
ui_print "-------------"

ui_print ">> 精简版：不携带任何 .ko 模块"
ui_print ">> zram0 与 lz4kd/zstdn/zstdn_o 均由内核自带"
ui_print ">> 本模块仅负责参数配置与重压缩调度"

# 清理旧模块残留的 zram 目录（原 zram.ko / zsmalloc.ko 在部分机型上永远无法加载）
OLD_ZRAM_DIR="/data/adb/modules/$MODNAME/zram"
if [ -d "$OLD_ZRAM_DIR" ]; then
  ui_print ">> 检测到旧版 zram 目录，移除中..."
  rm -rf "$OLD_ZRAM_DIR"
  ui_print ">> 已清理旧 .ko 残留 ✅"
fi

ui_print ">> 安装完成"
