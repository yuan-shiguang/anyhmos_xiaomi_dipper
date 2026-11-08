param([string]$Serial)
# 恢复 00-Backup-Recovery.ps1 备份的四个静态分区。米 8 没有 current-slot，
# 也不存在 init_boot/vendor_boot/super，所以这里只按无后缀分区名写回。
. (Join-Path $PSScriptRoot 'Common.ps1')
$device = Select-Device 'fastboot' $Serial
if ((Get-FastbootVar $device 'product') -ne 'dipper') { throw 'Unexpected product.' }
$folder = Get-BackupFolder $device 'single'
$backup = Get-Content -LiteralPath (Join-Path $folder 'backup-manifest.json') -Raw | ConvertFrom-Json
if ($backup.Serial -ne $device -or $backup.SlotModel -ne 'single' -or $backup.Files.Count -ne 4) { throw 'Device backup manifest does not match.' }
foreach ($f in $backup.Files) {
    if ($f.Partition -notin @('boot', 'system', 'vendor', 'cust') -or $f.File -ne "$($f.Partition).img") { throw 'Invalid backup entry.' }
    Assert-FileHash (Join-Path $folder $f.File) $f.Sha256
}
foreach ($f in $backup.Files) {
    Require-Success (Invoke-DeviceTool $Fastboot @('-s', $device, 'flash', $f.Partition, (Join-Path $folder $f.File)))
}
Write-Host 'Original boot/system/vendor/cust restored. Nothing else on the device was touched.'
