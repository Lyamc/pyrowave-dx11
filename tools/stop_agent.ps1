# tools\stop_agent.ps1 - ask the driver to stop.
#   default: create logs\STOP (the driver kills the running opencode session within ~10 s and exits; uncommitted work stays in the tree)
#   -AfterSession: create logs\STOP_AFTER (finish the current session and its acceptance run, then exit)
param([switch]$AfterSession)
$logs = Join-Path (Split-Path -Parent $PSScriptRoot) 'logs'
$f = if ($AfterSession) { 'STOP_AFTER' } else { 'STOP' }
New-Item -ItemType File -Force (Join-Path $logs $f) | Out-Null
Write-Host "created logs\$f"
