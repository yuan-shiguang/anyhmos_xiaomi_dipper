param([Parameter(Mandatory=$true)][ValidatePattern('^v[1-9][0-9]*$')][string]$Version)
$script=@'
B=/busybox
R=/hmos-root
L=$R/data/local/tmp/__VERSION__-live.txt
$B cat /proc/sys/kernel/random/boot_id /proc/uptime
$B ps -o pid,user,args | $B grep -E '20020020|appspawn' | $B head -5
$B grep -E 'DIPPER_OPTIONAL_SA|com.ohos.sceneboard.*(Error message:|RunPage:|initSceneBoard ok|NotifyAppFault:)' "$L" | $B tail -14
f=$($B grep 'com.ohos.sceneboard/AppDfr:.*NotifyAppFault:' "$L" | $B tail -1 | $B sed -n 's/.*appfreezeInfo:\([^ ,]*\).*/\1/p')
case "$f" in
 freeze_[0-9_]*.txt)
  src=$R/data/app/el2/100/log/com.ohos.sceneboard/watchdog/freeze/$f
  $B head -26 "$src"
  dst=$R/data/local/tmp/__VERSION__-$f
  [ -e "$dst" ] || $B cp "$src" "$dst"
  ;;
esac
'@
& (Join-Path $PSScriptRoot 'Invoke-ProbeDiagnostic.ps1') -Script ($script.Replace('__VERSION__',$Version)) -Seconds 6 -Lines 50
