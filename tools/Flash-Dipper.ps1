[CmdletBinding()]
param(
    [ValidateSet('Verify','Inspect','Flash')][string]$Mode = 'Verify',
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Serial,
    [string]$PackageRoot
)
$ErrorActionPreference = 'Stop'
if (!$PackageRoot) { $PackageRoot = [IO.Path]::GetDirectoryName($MyInvocation.MyCommand.Path) }
$PackageRoot = (Resolve-Path -LiteralPath $PackageRoot).Path
$Fastboot = Join-Path $PackageRoot 'bin\fastboot.exe'
$ManifestPath = Join-Path $PackageRoot 'PACKAGE.json'
if (!(Test-Path -LiteralPath $ManifestPath)) { throw 'PACKAGE.json is missing.' }
$Manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
if ($Manifest.kind -ne 'dipper-experimental' -or $Manifest.device_boot_tested -isnot [bool]) {
    throw 'Unexpected package identity or validation status.'
}
# 米 8 是单槽静态分区：只有这四个 Android 分区需要写，没有 super/vendor_boot/init_boot
$RequiredImages = @('boot.img','system.img','vendor.img','cust.img')
$OptionalImages = @('userdata.img','recovery.img')
$FlashOrder = @('boot','system','vendor','cust','userdata')
foreach ($Name in $RequiredImages) {
    $Records = @($Manifest.files | Where-Object { $_.path -eq "images/$Name" })
    if ($Records.Count -ne 1) { throw "Missing or duplicate artifact: $Name" }
}
Write-Host 'Verifying package sizes and SHA-256 checksums...'
# 刷写前逐项核对文件大小和哈希
foreach ($Record in $Manifest.files) {
    if ($Record.path -match '(^[\\/]|:|(^|[\\/])\.\.([\\/]|$))') { throw 'Invalid manifest path.' }
    $Path = Join-Path $PackageRoot $Record.path
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing file: $($Record.path)" }
    if ((Get-Item -LiteralPath $Path).Length -ne [long]$Record.bytes) { throw "Size mismatch: $($Record.path)" }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Record.sha256) {
        throw "SHA-256 mismatch: $($Record.path)"
    }
    Write-Host "Verified $($Record.path)"
}
Write-Host 'Experimental HarmonyOS NEXT port for Xiaomi dipper (Mi 8).'
if ($Manifest.device_boot_tested) {
    Write-Host 'A device boot was validated; see reports/device-validation.json for scope.'
} else {
    Write-Host 'Successful device boot has NOT been validated for this image.'
}
Write-Host ('Not validated: ' + (@($Manifest.not_validated) -join ', '))
if ($Mode -eq 'Verify') { Write-Host 'Offline verification passed. No device command was sent.'; return }

$LogDir = Join-Path $PackageRoot 'logs'
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$LogPath = Join-Path $LogDir ('fastboot-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.txt')
# 同时读取标准输出和错误流，避免原生命令阻塞
function Invoke-Fastboot([string[]]$Arguments) {
    foreach ($Argument in $Arguments) {
        if ($Argument.Contains('"') -or $Argument.EndsWith('\')) { throw 'Unsupported native argument.' }
    }
    $Start = New-Object System.Diagnostics.ProcessStartInfo
    $Start.FileName = $Fastboot
    $Start.Arguments = (($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' ')
    $Start.UseShellExecute = $false
    $Start.CreateNoWindow = $true
    $Start.RedirectStandardOutput = $true
    $Start.RedirectStandardError = $true
    $Process = New-Object System.Diagnostics.Process
    $Process.StartInfo = $Start
    try {
        [void]$Process.Start()
        $Stdout = $Process.StandardOutput.ReadToEndAsync()
        $Stderr = $Process.StandardError.ReadToEndAsync()
        $Timeout = 30000
        if ($Arguments -contains 'flash') { $Timeout = 1800000 }
        if (!$Process.WaitForExit($Timeout)) {
            $Process.Kill()
            throw 'fastboot timed out. Check the USB connection before retrying.'
        }
        $Text = $Stdout.Result + "`n" + $Stderr.Result
        Add-Content -LiteralPath $LogPath -Value ("fastboot " + ($Arguments -join ' ') + "`n" + $Text)
        if ($Process.ExitCode -ne 0) { throw "fastboot failed ($($Process.ExitCode)):`n$Text" }
        return $Text
    } finally { $Process.Dispose() }
}
$Devices = Invoke-Fastboot @('devices')
$Found = @([regex]::Matches($Devices, '(?m)^([a-zA-Z0-9_-]+)\s+fastboot\s*$') | ForEach-Object { $_.Groups[1].Value })
if ($Serial) {
    if ($Found -notcontains $Serial) { throw 'Requested Fastboot serial is absent.' }
} else {
    if ($Found.Count -ne 1) { throw 'Connect exactly one Fastboot device, or specify -Serial.' }
    $Serial = $Found[0]
}
function Get-FastbootVar([string]$Name) {
    $Result = Invoke-Fastboot @('-s',$Serial,'getvar',$Name)
    $Match = [regex]::Match($Result, '(?m)^(?:\(bootloader\)\s*)?' + [regex]::Escape($Name) + ':\s*(\S+)\s*$')
    if (!$Match.Success) { throw "Cannot read Fastboot variable: $Name" }
    return $Match.Groups[1].Value
}
function Get-FastbootVarOptional([string]$Name) {
    $Result = Invoke-Fastboot @('-s',$Serial,'getvar',$Name)
    $Match = [regex]::Match($Result, '(?m)^(?:\(bootloader\)\s*)?' + [regex]::Escape($Name) + ':\s*(\S+)\s*$')
    if (!$Match.Success) { return '' }
    return $Match.Groups[1].Value
}
function Convert-HexSize([string]$Value) {
    if ($Value -notmatch '^0x[0-9a-fA-F]+$') { throw "Invalid partition size: $Value" }
    return [Convert]::ToInt64($Value.Substring(2),16)
}
# 核对设备型号与解锁状态
if ((Get-FastbootVar 'product') -ne 'dipper') { throw 'Only Xiaomi dipper is supported.' }
if ((Get-FastbootVar 'unlocked') -ne 'yes') { throw 'Bootloader must already be unlocked.' }
if ((Get-FastbootVar 'is-userspace') -ne 'no') { throw 'Use bootloader Fastboot, not fastbootd.' }
# 米 8 的 Android 分区没有 current-slot：不加 _a 后缀，也不调用 set_active
$Slot = Get-FastbootVarOptional 'current-slot'
$Slotted = $Slot -in @('a','b')
$Suffix = if ($Slotted) { '_a' } else { '' }
if ($Slotted) { throw 'dipper has single-slot Android partitions; a slotted bootloader is unexpected.' }
# 逐分区核对容量：镜像必须装得进设备上的实际分区
$Targets = @()
foreach ($Name in $FlashOrder) {
    $ArtifactPath = "images/$Name.img"
    $Artifact = $Manifest.files | Where-Object { $_.path -eq $ArtifactPath }
    if (!$Artifact) {
        if ($RequiredImages -contains "$Name.img") { throw "Missing or duplicate artifact: $Name.img" }
        Write-Host "Package has no $Name.img; $Name will be left untouched."
        continue
    }
    $Capacity = Get-FastbootVarOptional "partition-size:${Name}$Suffix"
    if (!$Capacity) {
        if ($RequiredImages -contains "$Name.img") { throw "Device has no ${Name}$Suffix partition." }
        Write-Host "Device has no ${Name}$Suffix; $Name.img will be skipped."
        continue
    }
    $Bytes = Convert-HexSize $Capacity
    if ($Bytes -lt [long]$Artifact.bytes) { throw "Partition too small: ${Name}$Suffix" }
    Write-Host ("{0,-9} capacity {1,12:N0} bytes  image {2,12:N0} bytes" -f $Name, $Bytes, [long]$Artifact.bytes)
    $Targets += $Name
}
Write-Host "Read-only preflight passed for dipper $Serial. Target is single slot."
if ($Mode -eq 'Inspect') { Write-Host 'No partition was changed.'; return }
Write-Host 'Flashing will DELETE ALL USER DATA and replace the installed system.' -ForegroundColor Yellow
Write-Host 'Keep the original MIUI fastboot package for recovery. This port may fail to boot.'
$Confirmation = Read-Host 'Type ERASE DIPPER DATA to flash this experimental package'
if ($Confirmation -cne 'ERASE DIPPER DATA') { throw 'Confirmation did not match. No write was performed.' }
foreach ($Name in $Targets) {
    if ($Name -in @('system','vendor','cust')) {
        Write-Host "Flashing $Name (large image; please wait)..."
    } else {
        Write-Host "Flashing $Name..."
    }
    Write-Host (Invoke-Fastboot @('-s',$Serial,'flash',"$Name$Suffix",(Join-Path $PackageRoot "images\$Name.img")))
}
# 米 8 没有 metadata 分区，也没有 super；不要照搬其它机型的 erase metadata
Write-Host 'All writes succeeded; rebooting.'
Write-Host (Invoke-Fastboot @('-s',$Serial,'reboot'))
Write-Host "Flash log: $LogPath"
