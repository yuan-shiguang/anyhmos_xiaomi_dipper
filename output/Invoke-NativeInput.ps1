param([Parameter(Mandatory=$true)][ValidatePattern('^[-A-Za-z0-9. ]{3,160}$')][string]$Arguments)
$ErrorActionPreference='Stop'
$script=@'
set -e
B=/busybox
R=/hmos-root
$B test -f $R/compat/bin/uinput-original
gate=dipper.debug.input.dispatch
if ! $B test -x $R/compat/bin/native_input_dispatch; then
 $B grep -q ' /hmos-root/system/bin/uinput ' /proc/mounts
 gate=dipper.debug.input.swipe
fi
printf '%s\n' '__ARGS__' > $R/data/local/tmp/dipper-input.args
$B chmod 0644 $R/data/local/tmp/dipper-input.args
$B touch $R/data/local/tmp/dipper-input-last.log
$B chown 2000:2000 $R/data/local/tmp/dipper-input-last.log
$B chmod 0600 $R/data/local/tmp/dipper-input-last.log
$B chroot $R /system/bin/param set "$gate" true
$B sleep 2
$B cat $R/data/local/tmp/dipper-input-last.log
'@
& (Join-Path $PSScriptRoot 'Invoke-ProbeDiagnostic.ps1') -Script $script.Replace('__ARGS__',$Arguments) -Seconds 6 -Lines 18
