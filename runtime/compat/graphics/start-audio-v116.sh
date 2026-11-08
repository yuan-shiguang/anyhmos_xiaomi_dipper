#!/busybox sh
# 米 8（dipper）音频启动：sdm845 + WCD934x（Tavil）声卡 sdm845-tavil-snd-card。
# 依据 4.9 官方内核树：dipper_defconfig 里 CONFIG_SND_SOC_SDM845=y、CONFIG_SND_SOC_QDSP6V2=y、
# CONFIG_SND_SOC_MSM_QDSP6V2_INTF=y、CONFIG_SLIMBUS_MSM_NGD=y、CONFIG_SND_SOC_TAS2557=y，
# 没有任何 =m 音频模块，声卡由内核在 ADSP 起来后自己注册，
# 因此不再加载 sm8550 的 wcd938x/lpass_cdc/gpr 模块。
set -eu
B=/busybox
R=/hmos-root
D=$R/data/local/tmp
S=/compat/audio/scripts
P=/compat/audio-pal-v1
stage=check
trap 'rc=$?; echo "AUDIO_START_EXIT=$rc STAGE=$stage"' EXIT
[ -f /dipper-audio-v1/SHA256SUMS ] || exit 21
boot_id=$($B cat /proc/sys/kernel/random/boot_id)
[ "$($B head -1 "$D/v116-wifi-startup.log")" = "$boot_id" ] || exit 23

card_ready() { $B grep -q 'sdm845' /proc/asound/cards 2>/dev/null; }

stage=card
if ! card_ready; then
    # sdm845 的 ADSP 用 boot_adsp 触发；内核已自行启动时该节点可能不存在
    if [ -w /sys/kernel/boot_adsp/boot ]; then
        echo 1 > /sys/kernel/boot_adsp/boot
    fi
    i=0
    while ! card_ready; do
        [ "$i" -lt 100 ] || break
        i=$((i+1)); $B sleep 0.1
    done
fi
if card_ready; then
    echo AUDIO_CARD_READY_SDM845=1
else
    echo AUDIO_CARD_ABSENT_SDM845=1
    $B cat /proc/asound/cards 2>/dev/null || true
fi
# 音频卡检测结束（成功或缺失都继续），传感器在此之后异步启动
AUDIO_CARD_CHECKED=1

if card_ready; then
    stage=nodes
    $B sh "$S/stage-audio-nodes.sh"
    if ! $B sh "$S/stage-audio-supplement.sh"; then
        echo AUDIO_SUPPLEMENT_UNAVAILABLE=1
    fi
    # 后端二选一：sm8550 级平台有 /dev/agm，走华为 PAL/AGM；
    # sdm845 没有 AGM，走本仓库的 tinyalsa 后端，直接对接内核 PCM。
    stage=backend
    [ -z "$($B grep dipper.pal.pcm /proc/net/unix)" ]
    A=/compat/audio-runtime
    U=/compat/audio-supplement-v1
    if [ -e /dev/agm ] || [ -e "$R/dev/agm" ]; then
        echo AUDIO_BACKEND=pal-agm
        LD_LIBRARY_PATH=$A/lib64:$U/lib64:$P/lib64 LD_PRELOAD=$P/libdipper_pal_lifecycle_clean.so:$P/libdipper_audio_paths.so:$P/libdipper_pal_server_v1c.so:$A/lib64/libagm.so $B setsid $A/bin/linker64 $A/bin/agmplay -h > "$D/v116-pal-server.txt" 2>&1 </dev/null &
    elif [ -x "$R/compat/bin/dipper-tinyalsa-server" ]; then
        echo AUDIO_BACKEND=tinyalsa
        # 后端跑在 hmos-root 里（需要 musl 动态链接器），/proc 已由 bootstrap 绑定，
        # 只有 /dev/snd 需要单独映射，否则后端看不到 PCM 与控制节点。
        # 不假定声卡编号：只要目录为空就整目录绑定。
        if [ -z "$($B ls "$R/dev/snd" 2>/dev/null)" ]; then
            $B mkdir -p "$R/dev/snd"
            if ! $B mount --bind /dev/snd "$R/dev/snd"; then
                echo AUDIO_SND_BIND_FAILED=1
            fi
        fi
        echo "AUDIO_SND_NODES=$($B ls "$R/dev/snd" 2>/dev/null | $B wc -l)"
        $B setsid $B chroot "$R" /compat/bin/dipper-tinyalsa-server > "$D/v116-pal-server.txt" 2>&1 </dev/null &
    else
        echo AUDIO_BACKEND_ABSENT_NO_TINYALSA=1
        exit 0
    fi
    p=$!; echo "$p" > "$D/v116-pal-server.pid"
    i=0
    while ! $B grep -q '^PAL_SERVER_READY=1 ' "$D/v116-pal-server.txt"; do
        [ -d /proc/$p ] && [ "$i" -lt 100 ] || exit 26
        i=$((i+1)); $B sleep 0.1
    done
fi

stage=native
$B chroot "$R" /system/bin/param set dipper.debug.start.audio true
stage=complete
