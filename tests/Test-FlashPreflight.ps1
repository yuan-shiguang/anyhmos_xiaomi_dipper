# 使用模拟传输验证刷写前检查，不访问实际设备
$ErrorActionPreference = 'Stop'
$Repo = Split-Path $PSScriptRoot -Parent
$Fixture = Join-Path $Repo ('work\flash-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $Fixture 'images') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $Fixture 'bin') -Force | Out-Null
# 米 8 只有 boot/system/vendor/cust 需要写；userdata 由兼容层自己格式化，不随包刷写
$Files = @()
foreach ($Name in @('boot.img','system.img','vendor.img','cust.img')) {
    $Path = Join-Path $Fixture "images\$Name"
    [IO.File]::WriteAllText($Path,"fixture-$Name")
    $Files += @{path="images/$Name"; bytes=(Get-Item $Path).Length; sha256=(Get-FileHash $Path).Hash.ToLowerInvariant()}
}
$Manifest = @{kind='dipper-experimental';device_boot_tested=$false;files=$Files}
$Manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $Fixture 'PACKAGE.json') -Encoding UTF8
$Source = [IO.File]::ReadAllText((Join-Path $Repo 'tools\Flash-Dipper.ps1'))
$Tokens=$ParseErrors=$null
$Ast=[System.Management.Automation.Language.Parser]::ParseInput($Source,[ref]$Tokens,[ref]$ParseErrors)
if ($ParseErrors.Count) { throw 'Production script does not parse.' }
$Function=$Ast.Find({param($Node) $Node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $Node.Name -eq 'Invoke-Fastboot'},$true)
$Mock=@'
function Invoke-Fastboot([string[]]$Arguments) {
 Add-Content -LiteralPath (Join-Path $PackageRoot 'calls.txt') -Value ($Arguments -join ' ')
 if ($Arguments[0] -eq 'devices') { return "fixture_serial`tfastboot`n" }
 if ($Arguments -contains 'flash' -or $Arguments -contains 'erase' -or $Arguments -contains 'reboot') { throw 'WRITE ATTEMPT IN READONLY TEST' }
 $Name=$Arguments[-1]
 $Value=switch ($Name) {
  'product' { if($env:HMOS_FLASH_TEST -eq 'wrong-product'){'other'}else{'dipper'} }
  'unlocked' {'yes'}
  'is-userspace' {'no'}
  # dipper 没有 current-slot：返回空值，脚本必须走单槽分支
  'current-slot' {''}
  'partition-size:cust' { if($env:HMOS_FLASH_TEST -eq 'wrong-capacity'){'0x4'}else{'0x34000000'} }
  default {'0x6000000'}
 }
 return "(bootloader) ${Name}: $Value`nFinished. Total time: 0.001s"
}
'@
$Source=$Source.Substring(0,$Function.Extent.StartOffset)+$Mock+$Source.Substring($Function.Extent.EndOffset)
$TestScript=Join-Path $Fixture 'Mock-Flash.ps1'
# 保留 BOM，使 Windows PowerShell 正确读取中文注释
[IO.File]::WriteAllText($TestScript,$Source,[Text.UTF8Encoding]::new($true))
$Passed=@()
foreach ($Case in @('valid','wrong-product','wrong-capacity','corrupt-image')) {
    $env:HMOS_FLASH_TEST=$Case
    if($Case -eq 'corrupt-image'){[IO.File]::AppendAllText((Join-Path $Fixture 'images\boot.img'),'x')}
    $Info=New-Object System.Diagnostics.ProcessStartInfo
    $Info.FileName='powershell.exe'
    # 必须先关掉 ShellExecute，EnvironmentVariables 才会实例化；
    # 取不到时直接跳过，预检语义不依赖这一步
    $Info.UseShellExecute=$false
    $EnvBlock=$Info.EnvironmentVariables
    if ($EnvBlock) { $EnvBlock.Remove('PSModulePath') }
    $Info.Arguments='-NoProfile -ExecutionPolicy Bypass -File "'+$TestScript+'" -Mode Inspect'
    $Info.CreateNoWindow=$true;$Info.RedirectStandardOutput=$true;$Info.RedirectStandardError=$true
    $Process=[Diagnostics.Process]::Start($Info)
    $Out=$Process.StandardOutput.ReadToEndAsync();$Err=$Process.StandardError.ReadToEndAsync()
    $Process.WaitForExit()
    $Success=$Process.ExitCode -eq 0
    [IO.File]::WriteAllText((Join-Path $Fixture "$Case.txt"),($Out.Result+$Err.Result))
    $Process.Dispose()
    if($Success -ne ($Case -eq 'valid')){throw "Unexpected preflight result: $Case; see $Fixture"}
    $Passed+=$Case
}
if((Get-Content (Join-Path $Fixture 'calls.txt') -Raw) -match '\b(flash|erase|reboot|set_active)\b'){throw 'Read-only preflight attempted a write.'}
# 预检不得去读 super/vendor_boot/init_boot 这些米 8 上不存在的分区
if((Get-Content (Join-Path $Fixture 'calls.txt') -Raw) -match 'partition-size:(super|vendor_boot|init_boot)'){
    throw 'Preflight probed a partition that dipper does not have.'
}
Remove-Item Env:HMOS_FLASH_TEST
@{passed=$Passed;device_opened=$false;fixture=$Fixture}|ConvertTo-Json -Compress
