#!/busybox sh
set -eu
set -o pipefail
umask 077
B=/busybox
R=/hmos-root
P=/graphics/dipper
D=$R/data/local/tmp/dipper-hiview-events-v1
launcher_sha=@HIVIEW_LAUNCHER_SHA256@
assets_sha=@HIVIEW_ASSETS_SHA256@


check_directory() {
 [ -d "$1" ] && [ ! -L "$1" ] || return 1
 [ "$($B readlink -f "$1")" = "$1" ] || return 1
 [ "$($B stat -c '%u' "$1")" = "$2" ] || return 1
 case "$($B stat -c '%a' "$1")" in *[2367]) return 1;; esac
}


check_launcher() {
 target=$R/compat/bin/hiview_namespace_launcher
 [ -f "$target" ] && [ ! -L "$target" ] || return 1
 [ "$($B stat -c '%u:%g:%a:%h' "$target")" = 0:0:755:1 ] || return 1
 [ "$($B readlink -f "$target")" = "$target" ] || return 1
 echo "$launcher_sha  $target" | $B sha256sum -c -
}


check_request() {
 [ -f "$1" ] && [ ! -L "$1" ] || return 1
 [ "$($B stat -c '%u:%g:%a:%h' "$1")" = 0:0:600:1 ] || return 1
 [ "$($B readlink -f "$1")" = "$1" ]
}


# 仅安装已核入口，保留既有文件和所有运行目录
install_payload() {
 check_directory "$R/compat" 0
 [ "$($B stat -c '%u:%g:%a' "$R/compat")" = 0:0:755 ]
 if [ ! -e "$R/compat/bin" ] && [ ! -L "$R/compat/bin" ]; then
  $B mkdir -m 0755 "$R/compat/bin"
 fi
 check_directory "$R/compat/bin" 0
 [ "$($B stat -c '%u:%g:%a' "$R/compat/bin")" = 0:0:755 ]
 echo "$launcher_sha  $P/hiview_namespace_launcher" | $B sha256sum -c -
 echo "$assets_sha  $P/hiview-events-assets.tar.gz" | $B sha256sum -c -
 target=$R/compat/bin/hiview_namespace_launcher
 if [ -e "$target" ] || [ -L "$target" ]; then
  check_launcher
 else
  temporary=$R/compat/bin/.hiview_namespace_launcher.tmp
  [ ! -e "$temporary" ] && [ ! -L "$temporary" ] || return 1
  $B cp "$P/hiview_namespace_launcher" "$temporary"
  $B chown 0:0 "$temporary"
  $B chmod 0755 "$temporary"
  echo "$launcher_sha  $temporary" | $B sha256sum -c -
  $B mv -T "$temporary" "$target"
  check_launcher
 fi
 if [ ! -e "$D" ] && [ ! -L "$D" ]; then
  $B mkdir -m 0755 "$D"
  $B chown 0:0 "$D"
 fi
 check_directory "$D" 0
 [ "$($B stat -c '%u:%g:%a' "$D")" = 0:0:755 ]
}


[ "$#" = 1 ] || exit 61
nonce=$1
case "$nonce" in ''|*[!0-9a-f]*) exit 61;; esac
[ "${#nonce}" -le 32 ] || exit 61
old_request_sha=
if [ -e "$D" ] || [ -L "$D" ]; then
 check_directory "$D" 0
 [ "$($B stat -c '%u:%g:%a' "$D")" = 0:0:755 ]
 if [ -e "$D/request" ] || [ -L "$D/request" ]; then
  check_request "$D/request"
  old_request_sha=$($B sha256sum "$D/request")
  old_request_sha=${old_request_sha%% *}
 fi
fi
trap 'result=$?; trap - EXIT; if [ "$result" != 0 ] && [ -n "$old_request_sha" ]; then
 if echo "$old_request_sha  $D/request" | $B sha256sum -c -; then
  echo HIVIEW_EVENTS_OLD_REQUEST_RETAINED >&2
 fi
fi; exit "$result"' EXIT
# 仅在显式请求通过校验后安装入口并准备隔离目录
install_payload
run=$D/run-$nonce
[ ! -e "$run" ] && [ ! -L "$run" ] || exit 61

# 新建独立输出树，不覆盖旧日志或已有请求
$B mkdir -m 0700 "$run"
$B gzip -dc "$P/hiview-events-assets.tar.gz" | $B tar -xf - -C "$run"
cd "$run"
$B sha256sum -c <<'HIVIEW_ASSET_HASHES'
@HIVIEW_ASSET_HASHES@
HIVIEW_ASSET_HASHES
$B mkdir -m 0700 "$run/data-log" "$run/log" "$run/system-hiview"
$B find "$run" -type f | while IFS= read -r file; do
 $B chmod 0400 "$file" || exit 61
 [ "$($B stat -c '%a' "$file")" = 400 ] || exit 61
done
$B find "$run" -type d | while IFS= read -r directory; do
 $B chmod 0750 "$directory" || exit 61
 [ "$($B stat -c '%a' "$directory")" = 750 ] || exit 61
done
$B chmod 0700 "$run"
$B chown -R 1201:1000 "$run"
check_directory "$run" 1201
[ "$($B stat -c '%u:%g:%a' "$run")" = 1201:1000:700 ]
identities=
[ -r /proc/self/mountinfo ] || exit 61
for directory in "$run" "$run/config-tree" "$run/data-log" "$run/log" "$run/system-hiview"; do
 check_directory "$directory" 1201
 identity=$($B stat -c '%d:%i' "$directory")
 case " $identities " in *" $identity "*) exit 61;; esac
 identities="$identities $identity"
 mounted=$($B awk -v path="$directory" \
  '$5 == path {found=1} END {print found+0}' /proc/self/mountinfo)
 [ "$mounted" = 0 ] || exit 61
done
target=$R/data/system/hiview
if [ ! -e "$target" ] && [ ! -L "$target" ]; then
 $B mkdir -m 0700 "$target"
 $B chown 1201:1000 "$target"
fi
check_directory "$target" 1201
check_directory "$R/data/log" 1000
check_directory "$R/log" 0

# 完整准备后原子发布请求，服务仍由独立参数开关启动
request=$D/request-$nonce.tmp
[ ! -e "$request" ] && [ ! -L "$request" ] || exit 61
printf '%s\n' "$nonce" > "$request"
$B chmod 0600 "$request"
$B chown 0:0 "$request"
check_request "$request"
request_sha=$(printf '%s\n' "$nonce" | $B sha256sum)
request_sha=${request_sha%% *}
echo "$request_sha  $request" | $B sha256sum -c -
$B mv -T "$request" "$D/request" || :
check_request "$D/request"
echo "$request_sha  $D/request" | $B sha256sum -c -
trap - EXIT
echo HIVIEW_EVENTS_PREPARED run=$run
