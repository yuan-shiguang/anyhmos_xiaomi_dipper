param([string]$Serial='YOUR_DEVICE_SERIAL', [string]$Port='COM21')
. (Join-Path $PSScriptRoot 'Common.ps1')
$restored = $false
$sent = $false
for ($attempt=0; $attempt -lt 25; $attempt++) {
    $adbState = Invoke-DeviceTool $Adb @('devices')
    if ($adbState.Text -match ('(?m)^' + [regex]::Escape($Serial) + '\s+recovery\s*$')) {
        Write-Host 'Recovery ADB is ready.'
        return
    }
    $fbState = Invoke-DeviceTool $Fastboot @('devices')
    if ($fbState.Text -match ('(?m)^' + [regex]::Escape($Serial) + '\s+fastboot\s*$')) {
        if ($restored) { throw 'Still in fastboot after the original three-image restore. Enter the known-working TWRP manually.' }
        Write-Host 'Recovery request landed in fastboot. Restoring all three original device boot images, as requested by the user.'
        & (Join-Path $PSScriptRoot '03-Restore-BootChain.ps1') -Serial $Serial
        $restored = $true
        Require-Success (Invoke-DeviceTool $Fastboot @('-s',$Serial,'reboot','recovery'))
        Start-Sleep -Seconds 2
        continue
    }
    if (-not $sent -and [System.IO.Ports.SerialPort]::GetPortNames() -contains $Port) {
        & (Join-Path $PSScriptRoot 'Send-ProbeCommand.ps1') -Port $Port -Command reboot-recovery -Seconds 2
        $sent = $true
    }
    Start-Sleep -Seconds 2
}
throw 'Recovery ADB has not appeared. Check the phone and USB connection; no further images were flashed.'
