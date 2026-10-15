# tools\run_agent.ps1 - start the autonomous task driver (tools\run_agent.py) detached and hidden.
#   powershell -ExecutionPolicy Bypass -File tools\run_agent.ps1            start (resumes from logs\state.json)
#   powershell -ExecutionPolicy Bypass -File tools\run_agent.ps1 -Status    show logs\STATUS.md
# Stop: tools\stop_agent.ps1 (creates logs\STOP), or New-Item logs\STOP
param([switch]$Status, [switch]$Foreground)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$logs = Join-Path $repo 'logs'
$py = 'C:\Users\Lyam\AppData\Local\Programs\Python\Python313\python.exe'
New-Item -ItemType Directory -Force $logs | Out-Null
if ($Status) { Get-Content (Join-Path $logs 'STATUS.md'); exit 0 }
$pidFile = Join-Path $logs 'driver.pid'
if (Test-Path $pidFile) {
    $old = Get-Content $pidFile | Select-Object -First 1
    $p = Get-Process -Id $old -ErrorAction SilentlyContinue
    if ($p -and $p.ProcessName -like 'python*') { Write-Host "driver already running (PID $old)"; exit 1 }
}
Remove-Item (Join-Path $logs 'STOP'), (Join-Path $logs 'STOP_AFTER') -ErrorAction SilentlyContinue
$script = Join-Path $repo 'tools\run_agent.py'
if ($Foreground) { & $py -u $script; exit $LASTEXITCODE }
$p = Start-Process -FilePath $py -ArgumentList @('-u', "`"$script`"") -WorkingDirectory $repo -WindowStyle Hidden `
    -RedirectStandardOutput (Join-Path $logs 'driver.stdout.txt') -RedirectStandardError (Join-Path $logs 'driver.stderr.txt') -PassThru
Write-Host "driver started: PID $($p.Id). Status: logs\STATUS.md  Stop: tools\stop_agent.ps1"
