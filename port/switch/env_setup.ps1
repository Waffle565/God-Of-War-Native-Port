# Windows side of the Switch build environment setup (docs/switch-env-setup.md).
# Installs a fresh WSL2 Ubuntu 24.04 named "Ubuntu" (the name the docs and scripts use), then runs env_setup.sh in it.
#   powershell -ExecutionPolicy Bypass -File port\switch\env_setup.ps1
# If an "Ubuntu" distro is registered but its ext4.vhdx is gone, it is unregistered first (nothing left to lose).
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$names = (wsl -l -q) -replace "`0", '' | Where-Object { $_.Trim() }
if ($names -contains 'Ubuntu') {
    $ok = $false
    Get-ChildItem HKCU:\Software\Microsoft\Windows\CurrentVersion\Lxss | ForEach-Object {
        if ($_.GetValue('DistributionName') -eq 'Ubuntu') { $ok = Test-Path (Join-Path $_.GetValue('BasePath') 'ext4.vhdx') }
    }
    if ($ok) { Write-Host 'Ubuntu already registered and its disk exists: reusing it.' }
    else { wsl --unregister Ubuntu }
}
if (-not ((wsl -l -q) -replace "`0", '' | Where-Object { $_.Trim() -eq 'Ubuntu' })) {
    wsl --update
    wsl --install -d Ubuntu-24.04 --name Ubuntu --no-launch
}
$sh = (wsl -d Ubuntu -u root -- wslpath -a ($here -replace '\\', '/')).Trim()
wsl -d Ubuntu -u root -- bash "$sh/env_setup.sh"
