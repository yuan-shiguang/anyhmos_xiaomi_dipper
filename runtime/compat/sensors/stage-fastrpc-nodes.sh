#!/busybox sh
set -eu
B=/busybox

stage_node()
{
    name=$1
    mode=$2
    target=/hmos-root/dev/$name
    # 从已创建的内核设备读取 dev_t，不猜测主次设备号
    IFS=: read -r major minor < "/sys/class/fastrpc/$name/dev"
    case "$major:$minor" in
        *[!0-9:]*|:*|*:) return 2 ;;
    esac
    if [ -e "$target" ] || [ -L "$target" ]; then
        [ ! -L "$target" ] && [ -c "$target" ] || return 2
        actual=$($B stat -c '%t:%T' "$target")
        expected=$(printf '%x:%x' "$major" "$minor")
        [ "$actual" = "$expected" ] || return 2
    else
        $B mknod -m "$mode" "$target" c "$major" "$minor"
    fi
    # 沿用原 dipper ueventd 的 system 所有权和读写权限
    $B chown 1000:1000 "$target"
    $B chmod "$mode" "$target"
    echo "FASTRPC_NODE name=$name dev=$major:$minor mode=$mode owner=1000:1000"
}

stage_node adsprpc-smd 0664
stage_node adsprpc-smd-secure 0644
