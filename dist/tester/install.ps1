# omd2mp tester installer. Run through install.bat / uninstall.bat (no Python needed).
#   install.ps1 [-Game "<folder>"] [-Uninstall | -CollectLogs]
# Only ever writes/removes version.dll and omd2mp.ini next to OrcsMustDie2.exe.
# Refuses to overwrite a version.dll that is not omd2mp's.
param([string]$Game = "", [switch]$Uninstall, [switch]$CollectLogs)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$GameStamp = 0x51DD91A9          # PE TimeDateStamp of the supported game build
$Marker = "omd2mp loader"

function Fail($msg) { Write-Host ""; Write-Host "ERROR: $msg" -ForegroundColor Red; exit 1 }

function PeStamp($exe) {
    $b = New-Object byte[] 1024
    $f = [IO.File]::OpenRead($exe)
    try { [void]$f.Read($b, 0, 1024) } finally { $f.Close() }
    $e = [BitConverter]::ToInt32($b, 0x3C)
    if ($e -lt 0 -or $e -gt 1016 -or $b[$e] -ne 0x50 -or $b[$e + 1] -ne 0x45) { return $null }
    return [BitConverter]::ToUInt32($b, $e + 8)
}

function IsOurs($dll) {
    $text = [Text.Encoding]::GetEncoding(28591).GetString([IO.File]::ReadAllBytes($dll))
    return $text.Contains($Marker)
}

function SteamLibraries {
    $libs = @()
    foreach ($key in "HKCU:\Software\Valve\Steam", "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam") {
        try {
            $p = (Get-ItemProperty $key -ErrorAction Stop)
            foreach ($v in $p.SteamPath, $p.InstallPath) { if ($v) { $libs += ($v -replace "/", "\") } }
        } catch {}
    }
    $libs += "C:\Program Files (x86)\Steam"
    foreach ($root in @($libs)) {
        $vdf = Join-Path $root "steamapps\libraryfolders.vdf"
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libs += ($m.Groups[1].Value -replace "\\\\", "\")
            }
        }
    }
    return $libs | Select-Object -Unique
}

# Accepts the Steam game folder or the folder holding the exe. The real game is
# build\game\OrcsMustDie2.exe; build\release\OrcsMustDie2.exe is only a launcher.
function GameDirFrom($path) {
    foreach ($d in $path, (Join-Path $path "build\game")) {
        $exe = Join-Path $d "OrcsMustDie2.exe"
        if (Test-Path $exe) {
            $st = PeStamp $exe
            if ($st -eq $GameStamp) { return $d }
            Write-Host ("Skipping {0}: build stamp {1:X}, want {2:X}" -f $exe, $st, $GameStamp)
        }
    }
    return $null
}

if ($Game) {
    $dir = GameDirFrom $Game
    if (-not $dir) { Fail "Orcs Must Die! 2 (supported Steam build) not found in $Game" }
} else {
    $dir = $null
    foreach ($lib in SteamLibraries) {
        $dir = GameDirFrom (Join-Path $lib "steamapps\common\Orcs Must Die 2")
        if ($dir) { break }
    }
    if (-not $dir) {
        Fail ("Could not find Orcs Must Die! 2. In Steam: right-click the game > Manage > Browse local files, " +
              "copy that folder path, then run:`n  install.bat ""<that folder>""")
    }
}
Write-Host "Game folder: $dir"

$dll = Join-Path $dir "version.dll"
$ini = Join-Path $dir "omd2mp.ini"

if ($CollectLogs) {
    $files = @(Get-ChildItem $dir -Filter "omd2mp*" | Where-Object { $_.Name -ne "omd2mp.ini.bak" -and $_.Extension -ne ".dll" })
    if ($files.Count -eq 0) { Fail "No omd2mp.log in $dir - was the game started with omd2mp installed?" }
    $out = Join-Path ([Environment]::GetFolderPath("Desktop")) ("omd2mp-logs-{0:yyyyMMdd-HHmmss}.zip" -f (Get-Date))
    Compress-Archive -Path ($files | ForEach-Object { $_.FullName }) -DestinationPath $out -Force
    Write-Host "Saved $out - send this file to the omd2mp developer." -ForegroundColor Green
    exit 0
}
if ((Test-Path $dll) -and -not (IsOurs $dll)) {
    Fail "$dll exists and is not omd2mp's (another mod?). Remove it yourself if you want to install omd2mp."
}
if (Get-Process -Name OrcsMustDie2 -ErrorAction SilentlyContinue) { Fail "Close Orcs Must Die! 2 first." }

if ($Uninstall) {
    foreach ($f in $dll, $ini) { if (Test-Path $f) { Remove-Item $f -Force; Write-Host "removed $f" } }
    Write-Host "omd2mp uninstalled. The game is back to vanilla." -ForegroundColor Green
    exit 0
}

if (Test-Path $ini) {
    Copy-Item $ini "$ini.bak" -Force
    Write-Host "backed up your old omd2mp.ini to omd2mp.ini.bak"
}
Copy-Item (Join-Path $here "files\version.dll") $dll -Force
Copy-Item (Join-Path $here "files\omd2mp.ini") $ini -Force
Write-Host ""
Write-Host "omd2mp installed: $dll" -ForegroundColor Green
Write-Host "Start the game from Steam as usual. Everyone in the party needs this same version."
