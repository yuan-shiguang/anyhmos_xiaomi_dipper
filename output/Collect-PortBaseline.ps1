param([Parameter(Mandatory=$true)][ValidatePattern('^COM[0-9]+$')][string]$Port)
$ErrorActionPreference = 'Stop'
$script = [IO.File]::ReadAllText((Join-Path $PSScriptRoot '../tools/collect_port_baseline.sh'))
$result = (& (Join-Path $PSScriptRoot 'Invoke-ProbeCommand.ps1') -Port $Port -Script $script -Seconds 20) | ConvertFrom-Json
$log = Get-Content -LiteralPath $result.Log -Raw -Encoding UTF8
# 仅保存包含完整边界标记的采集结果
$body = [regex]::Match($log, '(?ms)^DIPPER_PORT_BASELINE_BEGIN\r?\n.*?^DIPPER_PORT_BASELINE_END\r?$')
if (-not $result.Completed -or -not $body.Success -or $log -notmatch ('CMD END id=' + $result.Id + '\b[^\r\n]*exit=0 signal=0')) {
    throw ('Baseline collection incomplete; inspect local log: ' + $result.Log)
}
$destination = [IO.Path]::ChangeExtension($result.Log, '.baseline.txt')
[IO.File]::WriteAllText($destination, $body.Value + "`n", [Text.UTF8Encoding]::new($false))
Write-Host $body.Value
Write-Host ('Saved local baseline: ' + $destination)
