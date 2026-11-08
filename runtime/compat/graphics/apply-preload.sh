#!/bin/sh
set -eu
B=/busybox
R=/hmos-root
CFG=$R/system/etc/cust/baselayer.cfg
ROOT=/preload/WKI-W10/all/cn
EXPECTED=0b9d034abf4841c60542d9a5daf4f7c9145c09652ef932ccc905371b24ac336c
echo "$EXPECTED  $CFG" | $B sha256sum -c -
$B test -r "$R$ROOT/etc/app/install_list.json"
$B test -d "$R/preload/app"
$B mkdir -p "$R/compat/etc/cust"
$B cp "$CFG" "$R/compat/etc/cust/baselayer.cfg"
# Actual mounted donor directory, independent of absent Huawei OEM parameters.
printf '\n# dipper compatibility: mounted preload application policy\n%s\n' "$ROOT" >> "$R/compat/etc/cust/baselayer.cfg"
$B chmod 0644 "$R/compat/etc/cust/baselayer.cfg"
$B mount --bind "$R/compat/etc/cust/baselayer.cfg" "$CFG"
echo HMOS_PRELOAD_POLICY_ROOT_ADDED_$ROOT

$B cp /graphics/native_bundle_cli "$R/compat/bin/native_bundle_cli"
$B chmod 0755 "$R/compat/bin/native_bundle_cli"
