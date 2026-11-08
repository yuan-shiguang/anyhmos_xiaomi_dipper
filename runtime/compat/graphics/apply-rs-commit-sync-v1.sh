#!/bin/sh
set -eu
B=/busybox
p=/hmos-root/compat/lib64/librender_service_base.z.so
echo "7f4fdf74cc0897ed322af94835a76fa3f5dd8fa82f44f75f5ab47cfa7a91f57b  $p" | $B sha256sum -c -
$B cp "$p" "$p.rs-sync-new"
echo AQCAUg== | $B base64 -d | $B dd of="$p.rs-sync-new" bs=1 seek=8957364 conv=notrunc
echo "528f0700081e17e4cd9a278bd98f426fae7925817d765ec9fbec2a32c37b123f  $p.rs-sync-new" | $B sha256sum -c -
$B chmod 0644 "$p.rs-sync-new"
$B mv "$p.rs-sync-new" "$p"
echo DIPPER_RS_COMMIT_SYNC_INSTALLED
