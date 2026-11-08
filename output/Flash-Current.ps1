param([string]$Serial)
# 米 8 没有 vendor_boot，也没有槽位：旧版这里刷 vendor_boot_a 并 set_active，在
# dipper 上两条命令都不成立。现在统一走 tools/Flash-Dipper.ps1 的四分区预检与刷写，
# 本脚本只做转发，避免出现两套互相矛盾的刷机逻辑。
. (Join-Path $PSScriptRoot 'Common.ps1')
$flasher = Join-Path (Split-Path -Parent $PSScriptRoot) 'tools/Flash-Dipper.ps1'
if (-not (Test-Path -LiteralPath $flasher)) { throw "Missing flasher: $flasher" }
$arguments = @{ Mode = 'Flash' }
if ($Serial) { $arguments.Serial = $Serial }
Write-Host "Delegating to $flasher (Mode=Flash). Back up boot/system/vendor/cust first."
& $flasher @arguments
