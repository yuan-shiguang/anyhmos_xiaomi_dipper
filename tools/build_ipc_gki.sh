#!/bin/bash
set -euo pipefail

repo=$(realpath "${1:?source checkout required}")
work=$(mktemp -d "${RUNNER_TEMP:-/tmp}/dipper-ipc-build.XXXXXX")
kernel_commit=6f2f96be86b93de6ed58a2c46b456fb49734c382
clang_commit=9f759dee5cdc5f85d076c642a192f6a9232f7058
pahole_commit=c2b7b8c20877d267159ace36119f6340b9d12823
libbpf_commit=94a49850c5ee61ea02dfcbabf48013391e8cecdf
kernel_dir=$work/common
clang_dir=$work/clang-prebuilt
pahole_dir=$work/pahole
pahole_build=$work/pahole-build
headers_dir=$work/official-headers
output=$work/kernel-out
module_dir=$work/module
artifacts=${DIPPER_IPC_ARTIFACT_DIR:-$work/artifacts}
mkdir -p "$kernel_dir" "$clang_dir" "$output" "$artifacts" \
    "$pahole_dir" "$pahole_build" "$headers_dir"
artifacts=$(realpath "$artifacts")
stage=source-fetch
finish() {
    local status=$?
    printf '{"exit_code":%d,"stage":"%s"}\n' "$status" "$stage" \
        > "$artifacts/build-result.json"
    exit "$status"
}
trap finish EXIT
exec > >(tee "$artifacts/build.log") 2>&1

# 固定源码和工具链版本，不依赖远端分支的后续变化
git -C "$kernel_dir" init -q
git -C "$kernel_dir" remote add origin https://android.googlesource.com/kernel/common
git -C "$kernel_dir" fetch --depth=1 origin "$kernel_commit"
git -C "$kernel_dir" checkout -q --detach FETCH_HEAD
test "$(git -C "$kernel_dir" rev-parse HEAD)" = "$kernel_commit"
git -C "$clang_dir" init -q
git -C "$clang_dir" remote add origin \
    https://android.googlesource.com/platform/prebuilts/clang/host/linux-x86
git -C "$clang_dir" sparse-checkout init --cone
git -C "$clang_dir" sparse-checkout set clang-r450784e
git -C "$clang_dir" fetch --filter=blob:none --depth=1 origin "$clang_commit"
git -C "$clang_dir" checkout -q --detach FETCH_HEAD
test "$(git -C "$clang_dir" rev-parse HEAD)" = "$clang_commit"
export PATH="$clang_dir/clang-r450784e/bin:$PATH"
clang --version | tee "$artifacts/clang-version.txt"
git -C "$kernel_dir" rev-parse HEAD > "$artifacts/kernel-revision.txt"
git -C "$clang_dir" rev-parse HEAD > "$artifacts/clang-revision.txt"
git -C "$repo" rev-parse HEAD > "$artifacts/source-revision.txt"

stage=official-inputs
ci=https://ci.android.com/builds/submitted/13729987/kernel_aarch64/latest/raw
curl --fail --location --retry 3 "$ci/vmlinux.symvers" -o "$artifacts/vmlinux.symvers"
curl --fail --location --retry 3 "$ci/kernel-headers.tar.gz" -o "$artifacts/kernel-headers.tar.gz"
printf '%s  %s\n' \
    bd974d513f43962cef02f7e8fe731df9c72c6911c3b9739324dbcf9583614c54 \
    "$artifacts/vmlinux.symvers" \
    090310ee2208e53767ff7164d8e22288029937aa42eb0a52372548abb1129e15 \
    "$artifacts/kernel-headers.tar.gz" | sha256sum -c -
tar -xzf "$artifacts/kernel-headers.tar.gz" -C "$headers_dir"
release=$(sed -n 's/^#define UTS_RELEASE "\([^"]*\)"$/\1/p' \
    "$headers_dir/kernel-headers/include/generated/utsrelease.h")
test "$release" = 5.15.178-android13-8-00021-g6f2f96be86b9-ab13729987

# 使用同版 pahole，保持 BTF 构建探测一致
stage=host-tools
git -C "$pahole_dir" init -q
git -C "$pahole_dir" remote add origin \
    https://git.kernel.org/pub/scm/devel/pahole/pahole.git
git -C "$pahole_dir" fetch --depth=1 origin "$pahole_commit"
git -C "$pahole_dir" checkout -q --detach FETCH_HEAD
test "$(git -C "$pahole_dir" rev-parse HEAD)" = "$pahole_commit"
# 使用发布版本绑定的 libbpf，保留 BTF 标签接口
git -C "$pahole_dir" submodule update --init --depth=1 lib/bpf
test "$(git -C "$pahole_dir/lib/bpf" rev-parse HEAD)" = "$libbpf_commit"
cmake -S "$pahole_dir" -B "$pahole_build" -DLIBBPF_EMBEDDED=ON \
    -DBUILD_SHARED_LIBS=OFF -D__LIB=lib -DCMAKE_BUILD_TYPE=Release
cmake --build "$pahole_build" --target pahole -j2
pahole=$pahole_build/pahole
"$pahole" --version | tee "$artifacts/pahole-version.txt"
test "$("$pahole" --version)" = v1.23
git -C "$pahole_dir" rev-parse HEAD > "$artifacts/pahole-revision.txt"
git -C "$pahole_dir/lib/bpf" rev-parse HEAD > "$artifacts/libbpf-revision.txt"
"$kernel_dir/scripts/cc-can-link.sh" clang --target=aarch64-linux-gnu
"$kernel_dir/scripts/cc-can-link.sh" clang --target=aarch64-linux-gnu -static

# 使用同版生成配置和符号版本，编译结果仍需独立加载验证
stage=kernel-prepare
cp "$repo/compat/kernel/kernel.config" "$output/.config"
cp "$repo/compat/kernel/kernel.config" "$artifacts/config-source"
options=(ARCH=arm64 LLVM=1 LLVM_IAS=1 KERNELRELEASE="$release"
    PAHOLE="$pahole")
make -C "$kernel_dir" O="$output" "${options[@]}" olddefconfig
python3 - "$repo/compat/kernel/kernel.config" "$output/.config" <<'PY'
import pathlib
import sys


def values(path):
    return dict(
        line.split("=", 1)
        for line in pathlib.Path(path).read_text().splitlines()
        if line.startswith("CONFIG_") and "=" in line
    )


expected, actual = map(values, sys.argv[1:])
differences = {
    name: (expected.get(name), actual.get(name))
    for name in expected.keys() | actual.keys()
    if expected.get(name) != actual.get(name)
}
if differences:
    raise SystemExit("Kernel configuration changed: " + repr(differences))
PY
cp "$output/.config" "$artifacts/config-prepared"
mkdir -p "$output/include/generated" "$output/arch/arm64/include/generated"
# 复用已验证的符号裁剪结果，避免依赖原构建机路径
cp -a "$headers_dir/kernel-headers/include/generated/." "$output/include/generated/"
cp -a "$headers_dir/kernel-headers/arch/arm64/include/generated/." \
    "$output/arch/arm64/include/generated/"
make -C "$kernel_dir" O="$output" "${options[@]}" -j2 modules_prepare
cp "$artifacts/vmlinux.symvers" "$output/Module.symvers"
cp -a "$repo/compat/ipc" "$module_dir"
python3 - "$module_dir" "$artifacts" "$repo/tools/build_ipc_gki.sh" \
    "$pahole" <<'PY'
import hashlib
import json
import pathlib
import sys

module, artifacts, script, pahole = map(pathlib.Path, sys.argv[1:])
inputs = [
    path for path in sorted(module.iterdir())
    if path.suffix in {".c", ".h"} or path.name == "Makefile"
]
inputs.extend([artifacts / "config-source", artifacts / "vmlinux.symvers",
               artifacts / "kernel-headers.tar.gz", script, pahole])
manifest = {
    "files": [{"name": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
              for path in inputs],
    "runtime_validated": False,
    "default_oneway_sender_pid": False,
}
(artifacts / "input-hashes.json").write_text(json.dumps(manifest, indent=2) + "\n")
PY
stage=module-build
make -C "$kernel_dir" O="$output" "${options[@]}" M="$module_dir" -j2 modules
module=$module_dir/dipper_hmos_ipc.ko
test -s "$module"
stage=module-audit
llvm-readelf -h -S -s -r "$module" > "$artifacts/module-elf.txt"
llvm-nm --defined-only "$module" > "$artifacts/module-symbols.txt"
grep -Eq '[[:space:]]__cfi_check$' "$artifacts/module-symbols.txt"
modinfo "$module" > "$artifacts/module-info.txt"
read -r vermagic < <(modinfo -F vermagic "$module")
test "$vermagic" = \
    "$release SMP preempt mod_unload modversions aarch64"
modprobe --dump-modversions "$module" > "$artifacts/module-versions.txt"
python3 - "$artifacts/vmlinux.symvers" "$artifacts/module-versions.txt" <<'PY'
import pathlib
import sys

expected = {
    fields[1]: int(fields[0], 16)
    for line in pathlib.Path(sys.argv[1]).read_text().splitlines()
    if (fields := line.split())
}
for line in pathlib.Path(sys.argv[2]).read_text().splitlines():
    crc, name = line.split()
    if expected.get(name) != int(crc, 16):
        raise SystemExit("Module symbol version mismatch: " + name)
PY
cp "$module" "$artifacts/dipper_hmos_ipc.ko"
sha256sum "$artifacts/dipper_hmos_ipc.ko" > "$artifacts/module-sha256.txt"
cat "$artifacts/module-sha256.txt"
stage=complete
printf 'ARTIFACT_DIRECTORY=%s\n' "$artifacts"
