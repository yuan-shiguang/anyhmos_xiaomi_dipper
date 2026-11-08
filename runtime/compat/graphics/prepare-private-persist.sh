#!/bin/sh
set -eu
B=/busybox
R=/hmos-root
U=/dev/block/by-name/userdata
C=/hmos-init-debug.cfg
N=/dipper-compat-v118/graphics/dipper/hmos-init-private-persist.cfg
BOOTSTRAP_SHA=@BOOTSTRAP_SHA@
BASELINE_SHA=@BASELINE_SHA@
CANDIDATE_SHA=@CANDIDATE_SHA@
P=$R/data/service/el1/startup/parameters
Q=$R/data/service/el1/public/startup/parameters
stage=
stage_inode=
cfg_inode=
old_empty=0
old_mode=755

# 失败时保留原启动配置和持久参数
fail() {
 echo "HMOS_PRIVATE_PERSIST_ERROR: $*" >&2
 exit 1
}

digest() {
 result=$($B sha256sum "$1") || return 1
 echo "${result%% *}"
}

# 拒绝符号链接及非普通文件
regular() {
 [ ! -L "$1" ] && [ -f "$1" ] &&
 [ "$($B readlink -f "$1")" = "$1" ] &&
 [ "$($B stat -c '%u:%g' "$1")" = 0:0 ]
}

present() {
 [ -e "$1" ] || [ -L "$1" ]
}

directory() {
 [ ! -L "$1" ] && [ -d "$1" ] &&
 [ "$($B readlink -f "$1")" = "$1" ] &&
 [ "$($B stat -c '%u:%g' "$1")" = 0:0 ]
}

# 仅信任完整发布的原始备份标记
prepared() {
 directory "$Q/original-public" &&
 [ "$($B stat -c '%a' "$Q/original-public")" = 700 ] || return 1
 manifest=$Q/original-public/SHA256SUMS
 marker=$Q/original-public/PREPARED
 regular "$manifest" && regular "$marker" || return 1
 expected=$($B printf 'HMOS_PRIVATE_PERSIST_MIRROR_V1\n%s\n' "$(digest "$manifest")")
 [ "$($B cat "$marker")" = "$expected" ] || return 1
 while read -r hash file; do
  case "$file" in public_persist_parameters|tmp_public_persist_parameters|persist_parameters) ;; *) return 1;; esac
  case "$hash" in *[!0-9a-f]*|'') return 1;; esac
  [ "${#hash}" = 64 ] || return 1
  regular "$Q/original-public/$file" || return 1
  [ "$(digest "$Q/original-public/$file")" = "$hash" ] || return 1
 done < "$manifest"
}

# 提交前失败时退回本次发布，保留原始备份
finish() {
 status=$?
 trap - EXIT HUP INT TERM
 if [ -n "$cfg_inode" ] && regular "$C" &&
  [ "$($B stat -c '%d:%i' "$C")" = "$cfg_inode" ] &&
  [ "$(digest "$C")" = "$CANDIDATE_SHA" ]; then
  echo HMOS_PRIVATE_PERSIST_COMMITTED || true
  exit 0
 fi
 if [ -n "$stage_inode" ] && present "$Q"; then
  if ! directory "$Q" || [ "$($B stat -c '%d:%i' "$Q")" != "$stage_inode" ] ||
   ! prepared || present "$stage" || ! $B mv -T "$Q" "$stage"; then
   echo HMOS_PRIVATE_PERSIST_ROLLBACK_FAILED >&2
   exit 2
  fi
  [ "$($B stat -c '%d:%i' "$stage")" = "$stage_inode" ] || exit 2
 fi
 if [ "$old_empty" = 1 ] && ! present "$Q"; then
  $B mkdir -m "$old_mode" "$Q" || exit 2
 fi
 echo HMOS_PRIVATE_PERSIST_BASELINE_RETAINED >&2
 exit "$status"
}
trap finish EXIT
trap 'exit 128' HUP INT TERM

# 仅在已核对的冷启动和实际用户数据分区上执行
[ "$($B id -u)" = 0 ] || fail uid
[ "$(digest /proc/1/exe)" = "$BOOTSTRAP_SHA" ] || fail warm-init
$B test -b "$U" || fail userdata-device
real=$($B readlink -f "$U") || fail userdata-path
name=${real##*/}
$B grep -qx PARTNAME=userdata "/sys/class/block/$name/uevent" || fail userdata-name
nums=$($B cat "/sys/class/block/$name/dev") || fail userdata-number
$B awk -v dev="$nums" -v target="$R/data" \
 '$3==dev && $5==target && $6 ~ /(^|,)rw(,|$)/ {for(i=7;i<=NF;i++) if($i=="-" && $(i+1)=="ext4") found=1} END {exit !found}' \
 /proc/self/mountinfo || fail userdata-mount
regular "$C" && [ "$(digest "$C")" = "$BASELINE_SHA" ] || fail baseline-cfg
regular "$N" && [ "$(digest "$N")" = "$CANDIDATE_SHA" ] || fail candidate-cfg

# 配置的复制和校验均在持久目录发布前完成
next=$($B mktemp /hmos-init-private-persist-XXXXXX) || fail cfg-stage
$B cp "$N" "$next" || fail cfg-copy
$B chmod 0644 "$next" || fail cfg-mode
[ "$(digest "$next")" = "$CANDIDATE_SHA" ] || fail cfg-hash
cfg_inode=$($B stat -c '%d:%i' "$next") || fail cfg-inode

# 逐级检查边界，缺失目录沿用原生创建模式
for path in "$R" "$R/data" "$R/data/service" "$R/data/service/el1" \
 "$R/data/service/el1/startup" "$P" "$R/data/service/el1/public"; do
 directory "$path" || fail path-boundary
done
for path in "$R/data/service/el1/public/startup" "$Q"; do
 if present "$path"; then
  directory "$path" || fail private-directory
 fi
done
if ! present "$R/data/service/el1/public/startup"; then
 $B mkdir -m 0755 "$R/data/service/el1/public/startup" || fail private-parent
fi
directory "$R/data/service/el1/public/startup" || fail private-parent-boundary
[ "$($B stat -c '%a' "$R/data/service/el1/public/startup")" = 755 ] || fail private-parent-mode

# 任一原生私有文件已存在时，禁止重新播种
has_private=0
for file in private_persist_parameters tmp_private_persist_parameters persist_parameters; do
 if present "$Q/$file"; then
  regular "$Q/$file" || fail existing-private-file
  has_private=1
 fi
done
if [ "$has_private" = 1 ] || present "$Q/original-public"; then
 prepared || fail unprepared-private-group
else
 if present "$Q"; then
  directory "$Q" || fail private-directory
  [ -z "$($B ls -A "$Q")" ] || fail nonempty-private-directory
 fi
 stage=$($B mktemp -d "$R/data/service/el1/public/startup/.private-persist-XXXXXX") || fail stage
 $B chmod 0755 "$stage" || fail stage-mode
 $B mkdir -m 0700 "$stage/original-public" || fail backup-directory
 $B touch "$stage/original-public/SHA256SUMS" || fail manifest
 bytes=0

 # 保留完整原始字节及新旧文件顺序，不重新分类参数
 for pair in public_persist_parameters:private_persist_parameters \
  tmp_public_persist_parameters:tmp_private_persist_parameters \
  persist_parameters:persist_parameters; do
  source_name=${pair%:*}
  target_name=${pair#*:}
  source=$P/$source_name
  if present "$source"; then
   regular "$source" || fail source-file
   case "$($B stat -c '%a' "$source")" in 600|640|644) ;; *) fail source-mode;; esac
   identity=$($B stat -c '%d:%i:%s:%Y:%Z:%u:%g:%a' "$source") || fail source-stat
   hash=$(digest "$source") || fail source-hash
   size=$($B stat -c '%s' "$source") || fail source-size
   bytes=$((bytes+size))
   $B cp -p "$source" "$stage/original-public/$source_name" || fail backup-copy
   $B cp -p "$source" "$stage/$target_name" || fail private-copy
   for copy in "$stage/original-public/$source_name" "$stage/$target_name"; do
    regular "$copy" || fail copy-type
    [ "$(digest "$copy")" = "$hash" ] || fail copy-hash
    [ "$($B stat -c '%s:%Y:%u:%g:%a' "$copy")" = "$($B stat -c '%s:%Y:%u:%g:%a' "$source")" ] || fail copy-metadata
    [ "$($B stat -c '%d:%i' "$copy")" != "$($B stat -c '%d:%i' "$source")" ] || fail copy-inode
   done
   [ "$($B stat -c '%d:%i:%s:%Y:%Z:%u:%g:%a' "$source")" = "$identity" ] || fail source-changed
   [ "$(digest "$source")" = "$hash" ] || fail source-changed-hash
   echo "$hash  $source_name" >> "$stage/original-public/SHA256SUMS"
  fi
 done
 [ "$bytes" -gt 0 ] || fail empty-public-group
 $B printf 'HMOS_PRIVATE_PERSIST_MIRROR_V1\n%s\n' \
  "$(digest "$stage/original-public/SHA256SUMS")" > "$stage/original-public/PREPARED"
 $B chmod 0400 "$stage/original-public/SHA256SUMS" "$stage/original-public/PREPARED" || fail marker-mode
 $B sync || fail stage-sync
 stage_inode=$($B stat -c '%d:%i' "$stage") || fail stage-inode
 if present "$Q"; then
  directory "$Q" && [ -z "$($B ls -A "$Q")" ] || fail target-changed
  old_mode=$($B stat -c '%a' "$Q") || fail old-directory-mode
  old_empty=1
  $B rmdir "$Q" || fail empty-target
 fi
 # 一次发布整组镜像，部分复制不会启用加载命令
 $B mv -T "$stage" "$Q" || fail publish
 [ "$($B stat -c '%d:%i' "$Q")" = "$stage_inode" ] || fail publish-inode
 $B sync || fail publish-sync
fi

# 最后原子启用加载命令，之前失败始终沿用原配置
[ "$(digest "$C")" = "$BASELINE_SHA" ] || fail cfg-changed
$B mv -T "$next" "$C" || fail cfg-publish
echo "HMOS_PRIVATE_PERSIST_PREPARED cfg_inode=$cfg_inode" || true
