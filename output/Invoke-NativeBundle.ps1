param(
    [ValidateSet('List','Dump','Install')][string]$Action='List',
    [ValidatePattern('^[A-Za-z0-9_.]{1,159}$')][string]$Name
)
$ErrorActionPreference='Stop'
if ($Action -ne 'List' -and -not $Name) { throw 'Dump/Install requires -Name.' }
if ($Action -eq 'Install' -and $Name -notin @('ABCreadingHm','CapCutHm','DouyinHm','IqiyiHm','KuaiShouHm','MeituxiuxiuHm','MindmasterHm','WPSHm','XiaohongshuHm','XiaoyuanAIHm','XunleiHm','ZhimuyunHm','ZuoyebangHm','ZwcadHm')) { throw 'Only the reviewed preload directories are allowed.' }
$request=$Action.ToLowerInvariant()
if ($Action -ne 'List') { $request+=' '+$Name }
$script=@'
set -e
B=/busybox
R=/hmos-root
$B test -x $R/compat/bin/native_bundle_cli
state=$($B chroot $R /system/bin/param get startup.service.ctl.dipper_bundle_cli 2>/dev/null | $B tr -d '[:space:]' || true)
if [ "$state" = 2 ] || [ "$state" = 1 ]; then echo BUNDLE_CLIENT_ALREADY_RUNNING; exit 24; fi
printf '%s\n' '__REQUEST__' > $R/data/local/tmp/dipper-bundle-request.txt
$B chmod 0644 $R/data/local/tmp/dipper-bundle-request.txt
$B chroot $R /system/bin/param set dipper.debug.bundle.cli true
$B sleep 1
$B cat $R/data/local/tmp/dipper-bundle-last.log
$B chroot $R /system/bin/param get startup.service.ctl.dipper_bundle_cli
'@
& (Join-Path $PSScriptRoot 'Invoke-ProbeDiagnostic.ps1') -Script $script.Replace('__REQUEST__',$request) -Seconds 4 -Lines 70
