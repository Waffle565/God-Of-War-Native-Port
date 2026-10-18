# Builds the shipped PC folder of the port (docs/build-and-run.md "PC release folder"):
#   <Dest>\God of War.exe   launcher (launcher.cpp/.rc: settings window, icon, version info)
#   <Dest>\bin\             GodOfWar.exe (the Release ps2EntryRunner, with its own icon/version info) + DLLs
#   <Dest>\data\            the extracted USA disc from disc-usa\ (hard links on the same volume)
#   <Dest>\data\lang\       language overlays (tools/lang_pack.py) when the European disc is extracted in disc\
#   <Dest>\saves\           GodOfWar.sav (the player's saves; first pack: a copy of disc-usa\play.mcd)
#   <Dest>\README.txt       controls, saves, folders
# Usage: powershell -ExecutionPolicy Bypass -File tools\pc_release\make_pc_folder.ps1 [-Dest <folder>]
# Re-running replaces the program and relinks only changed data files; saves\ and settings.ini are never overwritten.
# The runner needs the build configured with -DPS2X_RUNNER_ICON=<repo>/tools/pc_release/icon.ico (icon in the window).
param([string]$Dest = (Join-Path $PSScriptRoot '..\..\dist\God of War (PC)'))
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$release = Join-Path $root 'ext\PS2Recomp-kl\out\build-kl\ps2xRuntime\Release'
$disc = Join-Path $root 'disc-usa'
if (-not (Test-Path "$release\ps2EntryRunner.exe")) { throw "Release runner not built: $release" }
if (-not (Test-Path "$disc\SCUS_973.99")) { throw "No extracted disc in $disc (docs/disc.md)" }

# launcher, built in a temp folder with the MSVC Build Tools
$work = Join-Path $env:TEMP 'gow_launcher_build'
New-Item -ItemType Directory -Force $work | Out-Null
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul && cd /d `"$PSScriptRoot`" && rc /nologo /fo `"$work\launcher.res`" launcher.rc && cd /d `"$work`" && cl /nologo /std:c++17 /O2 /EHsc /DUNICODE /D_UNICODE `"$PSScriptRoot\launcher.cpp`" launcher.res /Fe:launcher.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib"
if ($LASTEXITCODE -ne 0) { throw 'launcher build failed' }

New-Item -ItemType Directory -Force $Dest, "$Dest\bin", "$Dest\data", "$Dest\saves" | Out-Null
$Dest = (Resolve-Path $Dest).Path

# one-time move from the first layout (game\, disc\ with disc\play.mcd): keep the saves, drop the rest
if ((Test-Path "$Dest\disc\play.mcd") -and -not (Test-Path "$Dest\saves\GodOfWar.sav")) {
    Move-Item "$Dest\disc\play.mcd" "$Dest\saves\GodOfWar.sav"
}
foreach ($old in @('game', 'disc')) {
    if (Test-Path "$Dest\$old") {
        if (Test-Path "$Dest\$old\play.mcd") { throw "$Dest\$old\play.mcd still there (saves): move it by hand" }
        Remove-Item -Recurse -Force "$Dest\$old"
    }
}

Copy-Item -Force (Join-Path $work 'launcher.exe') (Join-Path $Dest 'God of War.exe')
Copy-Item -Force (Join-Path $PSScriptRoot 'README.txt') (Join-Path $Dest 'README.txt')

# program: the runner (as GodOfWar.exe) and its DLLs only (no pdb/lib or test output from the build folder)
robocopy $release "$Dest\bin" *.dll /PURGE /XF GodOfWar.exe /NJH /NJS /NP /NFL /NDL | Out-Null
if ($LASTEXITCODE -ge 8) { throw "robocopy bin failed ($LASTEXITCODE)" }
Copy-Item -Force "$release\ps2EntryRunner.exe" "$Dest\bin\GodOfWar.exe"

# data: the extracted disc without the test memory cards (Mcd001.ps2, play.mcd, mc0/, mc1/). Hard links on the same
# volume (no second 8 GB copy; the game never writes disc files), copies elsewhere.
$sameVolume = [IO.Path]::GetPathRoot($Dest) -eq [IO.Path]::GetPathRoot($disc)
Get-ChildItem -Recurse -File -Force $disc | Where-Object {
    $_.Name -notin @('Mcd001.ps2', 'play.mcd') -and $_.FullName -notmatch '[\\/](mc0|mc1|lang)[\\/]'
} | ForEach-Object {
    $target = Join-Path "$Dest\data" $_.FullName.Substring($disc.Length + 1)
    New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
    $old = Get-Item -Force -LiteralPath $target -ErrorAction SilentlyContinue
    if ($old -and $old.Length -eq $_.Length -and $old.LastWriteTime -eq $_.LastWriteTime) { return }
    if ($old) { Remove-Item -Force -LiteralPath $target }
    if ($sameVolume) { New-Item -ItemType HardLink -Path $target -Target $_.FullName | Out-Null }
    else { Copy-Item -LiteralPath $_.FullName $target }
}

# languages: French, German, Italian and Spanish text and voices from the European disc, the Japanese dub from the
# Japanese one (docs/settings.md); without them the launcher offers English only
$eu = Join-Path $root 'disc'
$jpn = Join-Path $root 'disc-jpn'
$langArgs = @()
if (Test-Path "$eu\SCES_531.33") { $langArgs += $eu } else { Write-Host "no European disc in ${eu}: English text" }
if (Test-Path "$jpn\SLPM_670.10") { $langArgs += @('--ja', $jpn) } else { Write-Host "no Japanese disc in ${jpn}: no Japanese voices" }
if ($langArgs.Count -gt 0) {
    python -I (Join-Path $root 'tools\lang_pack.py') "$Dest\data" @langArgs
    if ($LASTEXITCODE -ne 0) { throw 'lang_pack.py failed' }
}

# saves: the player's file is never replaced; a new folder starts from the repo's card (formatted, with the saves)
if (-not (Test-Path "$Dest\saves\GodOfWar.sav")) {
    $card = if (Test-Path "$disc\play.mcd") { "$disc\play.mcd" } elseif (Test-Path "$root\ref\saves\Mcd002.ps2") { "$root\ref\saves\Mcd002.ps2" }
    if ($card) { Copy-Item $card "$Dest\saves\GodOfWar.sav" }
}
$size = (Get-ChildItem -Recurse -File $Dest | Measure-Object Length -Sum).Sum / 1GB
Write-Host ("done: {0} ({1:N1} GB) - start 'God of War.exe'" -f $Dest, $size)
