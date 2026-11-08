param([string]$Serial)
# 米 8 是单槽静态分区：没有 _a/_b 后缀，也没有 super。这里备份本仓库会写的四个分区。
# system/vendor/cust 合计数 GiB，dd + pull 会比较慢，属于预期行为。
. (Join-Path $PSScriptRoot 'Common.ps1')
$device = Select-Device 'adb' $Serial
$id = Invoke-DeviceTool $Adb @('-s', $device, 'shell', 'id')
if ($id.Code -ne 0 -or $id.Text -notmatch 'uid=0') { throw 'Run this from a root-enabled Recovery with ADB.' }
$dest = Get-BackupFolder $device 'single'
if (Test-Path -LiteralPath $dest) { throw "Backup folder already exists; preserve it before creating another: $dest" }
New-Item -ItemType Directory -Path $dest -Force | Out-Null
$files = @()
foreach ($name in @('boot', 'system', 'vendor', 'cust')) {
    $tmp = "/tmp/hmos-backup-$name.img"
    $r = Invoke-DeviceTool $Adb @('-s', $device, 'shell', "dd if=/dev/block/by-name/$name of=$tmp bs=1048576")
    Require-Success $r
    $local = Join-Path $dest "$name.img"
    Require-Success (Invoke-DeviceTool $Adb @('-s', $device, 'pull', $tmp, $local))
    $length = (Get-Item -LiteralPath $local).Length
    if ($length -lt 4096) { throw "Backup is too small: $local" }
    $files += [pscustomobject]@{ Partition = $name; File = "$name.img"; Bytes = $length; Sha256 = (Get-FileHash -LiteralPath $local -Algorithm SHA256).Hash }
    Require-Success (Invoke-DeviceTool $Adb @('-s', $device, 'shell', "rm -f $tmp"))
}
[pscustomobject]@{ Serial = $device; SlotModel = 'single'; Created = (Get-Date).ToString('o'); Files = $files } | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $dest 'backup-manifest.json') -Encoding UTF8
Write-Host "Backup complete: $dest. Review the matching image and backup hashes before any flash operation."
