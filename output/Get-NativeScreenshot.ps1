param([Parameter(Mandatory=$true)][ValidatePattern('^[a-zA-Z0-9-]{3,80}$')][string]$Name)
$ErrorActionPreference='Stop'
$dest=Join-Path $PSScriptRoot ('screenshots/'+$Name+'.jpeg')
if(Test-Path -LiteralPath $dest){throw 'Screenshot destination exists'}
$script=@'
set -e
B=/busybox
R=/hmos-root
P=$R/data/local/tmp/dipper-capture/display0.jpeg
if [ -e "$P" ]; then $B mv "$P" "$R/data/local/tmp/__NAME__-previous.jpeg"; fi
$B chroot $R /system/bin/param set dipper.debug.start.capture true
i=0
while [ "$i" -lt 12 ]; do
 $B sleep 1
 state=$($B chroot $R /system/bin/param get startup.service.ctl.dipper_snapshot)
 if [ "$state" -eq 5 ] 2>/dev/null && $B test -s "$P"; then
  $B cp "$P" "$R/data/local/tmp/__NAME__.jpeg"
  echo FRESH_SYSTEM_SCREENSHOT=1
  exit 0
 fi
 i=$((i+1))
done
echo FRESH_SYSTEM_SCREENSHOT=0
exit 23
'@
$r=(& (Join-Path $PSScriptRoot 'Invoke-ProbeCommand.ps1') -Script $script.Replace('__NAME__',$Name) -Seconds 16)|ConvertFrom-Json
$log=Get-Content -LiteralPath $r.Log -Raw -Encoding UTF8
if(-not $r.Completed -or $log -notmatch ('CMD END id='+$r.Id+'\b[^\r\n]*exit=0 signal=0')){throw ('Capture failed; inspect '+$r.Log)}
& (Join-Path $PSScriptRoot 'Receive-ProbeArtifact.ps1') -Source ('/hmos-root/data/local/tmp/'+$Name+'.jpeg') -Destination $dest
