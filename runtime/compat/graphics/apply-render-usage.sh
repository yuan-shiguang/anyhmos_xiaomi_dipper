set -e
B=/busybox
R=/hmos-root
p=$R/compat/lib64/librender_service_base.z.so
echo "d423413ec1fc58f40c5b04846b00530a4d82d0929eee769ede3bf5c605e5a77d  $p" | $B sha256sum -c -
echo 6QKAUg== | $B base64 -d | $B dd of="$p" bs=1 seek=9162080 conv=notrunc
echo "7f4fdf74cc0897ed322af94835a76fa3f5dd8fa82f44f75f5ab47cfa7a91f57b  $p" | $B sha256sum -c -
echo DIPPER_RGB_RENDERABLE_USAGE_INSTALLED
/busybox sh /graphics/apply-rs-commit-sync-v1.sh
