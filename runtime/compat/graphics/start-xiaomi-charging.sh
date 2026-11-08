#!/busybox sh
set -eu

A=/compat/audio-runtime
P=/compat/audio-pal-v1
U=/compat/audio-supplement-v1
C=/graphics/xiaomi-charging

# 仅为充电进程装载安卓依赖和属性适配
export LD_LIBRARY_PATH="$C:$A/lib64:$U/lib64:$P/lib64"
export LD_PRELOAD="$C/libdipper_batterysecret_process.so"

# 认证过程日志不写入终端或日志文件
exec "$A/bin/linker64" "$C/batterysecret" >/dev/null 2>&1
