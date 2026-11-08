set -e
B=/busybox
R=/hmos-root
# 米 8（dipper）触控器件依据见 compat/touch/README.md：
#   ST FTS  fts@49（compatible "st,fts"，I2C 0x49，IRQ 31 / RST 32，fw st_fts_e1.ftb）
#   Atmel   atmel_mxt_ts_640u@4a（mXT640U，I2C 0x4a）
# 两者都用 input_mt_init_slots(..., INPUT_MT_DIRECT) 走标准 MT-B，坐标 X 0..1080、Y 0..2160。
# 官方 4.9 内核把驱动编进内核（CONFIG_TOUCHSCREEN_ST_FTS_V521=y / CONFIG_TOUCHSCREEN_ATMEL_MXT=y），
# 开机即注册 evdev；若换成模块化内核，则从已暂存的模块目录补加载。
M=$R/compat/modules
for d in $R/vendor_dlkm/lib/modules $R/vendor/lib/modules; do
  if [ -d "$d" ]; then M="$M $d"; fi
done

touch_present() {
  for dir in /sys/class/input/event*; do
    [ -e "$dir/device/name" ] || continue
    n=$($B cat "$dir/device/name" 2>/dev/null) || continue
    case "$n" in
      fts*|*mxt*|*atmel*) return 0 ;;
    esac
  done
  return 1
}

if ! touch_present; then
  for ko in fts.ko atmel_mxt_ts.ko; do
    for d in $M; do
      [ -f "$d/$ko" ] || continue
      echo "FTS_TOUCH_MODULE_LOAD=$d/$ko"
      $B insmod "$d/$ko" || echo "FTS_TOUCH_MODULE_FAILED=$ko"
    done
  done
fi

# 把内核已注册的输入节点映射进 $R/dev/input，并交给多模输入服务读取
$B mkdir -p "$R/dev/input"
found=0
for dir in /sys/class/input/event*; do
  [ -e "$dir/device/name" ] || continue
  name=$($B cat "$dir/device/name" 2>/dev/null) || continue
  case "$name" in
    fts*|*mxt*|*atmel*) ;;
    *) continue ;;
  esac
  node=${dir##*/}
  dev=$($B cat "$dir/dev" 2>/dev/null) || continue
  major=${dev%:*}
  minor=${dev#*:}
  case "$major$minor" in
    ''|*[!0-9]*) continue ;;
  esac
  target="$R/dev/input/$node"
  if [ -e "$target" ]; then $B rm -f "$target"; fi
  $B mknod "$target" c "$major" "$minor"
  $B chmod 0660 "$target"
  # 多模输入服务以 6696 读取输入节点；该 uid 不存在时退化为 0666，避免触摸整体不可用
  if ! $B chown 6696:6696 "$target" 2>/dev/null; then $B chmod 0666 "$target"; fi
  echo "FTS_TOUCH_NODE_READY name=$name node=/dev/input/$node dev=$major:$minor"
  found=1
done
if [ "$found" -eq 0 ]; then
  echo "FTS_TOUCH_NODE_MISSING"
fi
echo DIPPER_STOCK_FTS_TOUCH_PUBLISHED_VENDOR_UNTOUCHED
