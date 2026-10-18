# SlormAPI installer: Aurie + YYToolkit (the mod loader) + SlormAPI (+ the mods in its payload) for The Slormancer.
# It installs the mods that are in its payload folder: SlormAPI alone (SlormAPI-Installer), or SlormAPI + BuildFilter
# (BuildFilter-Installer).
#
#   Install.bat                       menu
#   installer.ps1 -Action install|update|repair|remove-mods|remove-all|status [-GamePath "<folder>"] [-Yes]
#   (repair = only the loader again - after a Steam update or file check - mods and settings stay)
#
# What it changes in the game folder (nothing else, never your saves):
#   The Slormancer.exe        gets Aurie's small loader (AuriePatcher); the original is kept in
#                             mods\slormancer-mods-backup\ and put back by "remove everything"
#   mods\native\AurieCore.dll, mods\aurie\YYToolkit.dll   the loader (only if you do not already have them)
#   mods\aurie\SlormAPI.dll, mods\aurie\BuildFilter.dll    the mods
#   mods\slormancer-mods.json  what this installer added (so removal only removes that)
#
# A game that already loads Aurie another way (Slorm Loader / Vortex: version.dll + plugins\AurieCore.asi, or another
# version.dll loader) keeps that loader: the exe is not patched and only the mods are installed (two loaders would
# start Aurie twice).
param(
    [ValidateSet('menu', 'install', 'update', 'repair', 'remove-mods', 'remove-all', 'status')][string]$Action = 'menu',
    [string]$GamePath = '',
    [switch]$Yes
)
$ErrorActionPreference = 'Stop'
$Version = '1.0.0'
$AppId = '1104280'
$ExeName = 'The Slormancer.exe'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Payload = Join-Path $Here 'payload'
$AllMods = @('SlormAPI.dll', 'BuildFilter.dll')        # removal takes both: BuildFilter does not run without SlormAPI
$Mods = @($AllMods | Where-Object { Test-Path (Join-Path $Payload $_) })   # installed: the ones in the payload
$Loader = @{ 'AurieCore.dll' = 'native'; 'YYToolkit.dll' = 'aurie' }
# the official release files (GitHub's published SHA-256): Aurie v2.0.2, YYToolkit v5.0.0c
$Official = @{
    'AurieCore.dll'    = '18E3A1DE980F487A6B3858B673D2030E96984DD96DE3A047B43A263A5BA829AE'
    'AuriePatcher.exe' = '4D3AEC439DBBA5209AD48FB0A40D6324406247FE678541DF9031F5B89363D536'
    'YYToolkit.dll'    = 'AE7809F136F9222E5F49393D7CE7C3AD4C375C171308E2414660946D4BA0378A'
}

function Say([string]$Text, [string]$Color = 'Gray') { Write-Host $Text -ForegroundColor $Color }

function Find-Game {
    # Steam: its install path from the registry, every library from libraryfolders.vdf, the app's manifest
    $steam = $null
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try {
            $p = Get-ItemProperty -Path $key -ErrorAction Stop
            if ($p.SteamPath) { $steam = $p.SteamPath; break }
            if ($p.InstallPath) { $steam = $p.InstallPath; break }
        } catch {}
    }
    if (-not $steam) { return $null }
    $libraries = @($steam)
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
            $libraries += $m.Groups[1].Value -replace '\\\\', '\'
        }
    }
    foreach ($lib in ($libraries | Select-Object -Unique)) {
        $acf = Join-Path $lib "steamapps\appmanifest_$AppId.acf"
        $dir = 'The Slormancer'
        if (Test-Path $acf) {
            $m = [regex]::Match((Get-Content $acf -Raw), '"installdir"\s+"([^"]+)"')
            if ($m.Success) { $dir = $m.Groups[1].Value }
        }
        $game = Join-Path $lib "steamapps\common\$dir"
        if (Test-Path (Join-Path $game $ExeName)) { return (Resolve-Path $game).Path }
    }
    return $null
}

function Ask-Folder {
    Add-Type -AssemblyName System.Windows.Forms
    $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
    $dlg.Description = "Select The Slormancer's game folder (the one with $ExeName)"
    if ($dlg.ShowDialog() -eq 'OK') { return $dlg.SelectedPath }
    return $null
}

function Get-Sha256([string]$Path) { (Get-FileHash -Algorithm SHA256 -Path $Path).Hash }

function Test-Patched([string]$Exe) {
    # AuriePatcher adds a PE section named ".aurie"
    $bytes = [System.IO.File]::ReadAllBytes($Exe)
    $pe = [BitConverter]::ToInt32($bytes, 0x3C)
    $sections = [BitConverter]::ToUInt16($bytes, $pe + 6)
    $optSize = [BitConverter]::ToUInt16($bytes, $pe + 20)
    $first = $pe + 24 + $optSize
    for ($i = 0; $i -lt $sections; $i++) {
        $name = [System.Text.Encoding]::ASCII.GetString($bytes, $first + 40 * $i, 8).TrimEnd([char]0)
        if ($name -eq '.aurie') { return $true }
    }
    return $false
}

function Get-OtherLoader([string]$Game) {
    if (Test-Path (Join-Path $Game 'plugins\AurieCore.asi')) { return 'Slorm Loader (version.dll + plugins\AurieCore.asi)' }
    if (Test-Path (Join-Path $Game 'version.dll')) { return 'a version.dll mod loader' }
    return $null
}

function Read-State([string]$Game) {
    $f = Join-Path $Game 'mods\slormancer-mods.json'
    if (Test-Path $f) { return Get-Content $f -Raw | ConvertFrom-Json }
    return $null
}

function Write-State([string]$Game, $State) {
    $State | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 (Join-Path $Game 'mods\slormancer-mods.json')
}

function Assert-NotRunning {
    if (Get-Process -Name 'The Slormancer' -ErrorAction SilentlyContinue) {
        throw 'The Slormancer is running. Close the game first.'
    }
}

function Assert-Writable([string]$Game) {
    $probe = Join-Path $Game '.slormancer-mods-write-test'
    try { [IO.File]::WriteAllText($probe, 'x'); Remove-Item $probe -Force; return $true } catch { return $false }
}

function Invoke-Patcher([string]$Game, [string]$What) {
    # The loader path is written into the exe RELATIVE (mods\native\AurieCore.dll): Windows resolves it against the
    # game's own folder at every launch, so moving the game (another drive, Steam "Move install folder") keeps working.
    $exe = Join-Path $Game $ExeName
    Push-Location $Game
    try { $out = & (Join-Path $Payload 'AuriePatcher.exe') $exe 'mods\native\AurieCore.dll' $What 2>&1 }
    finally { Pop-Location }
    if ($LASTEXITCODE -ne 0) { throw "AuriePatcher $What failed ($LASTEXITCODE): $($out -join ' ')" }
}

function Show-Status([string]$Game) {
    $exe = Join-Path $Game $ExeName
    Say "Game folder: $Game"
    $other = Get-OtherLoader $Game
    if ($other) { Say "  mod loader: $other" }
    Say ("  loader in the game exe: " + $(if (Test-Patched $exe) { 'yes' } else { 'no' }))
    foreach ($f in $Loader.Keys) {
        Say ("  $f".PadRight(24) + $(if (Test-Path (Join-Path $Game "mods\$($Loader[$f])\$f")) { 'installed' } else { '-' }))
    }
    foreach ($f in $AllMods) {
        Say ("  $f".PadRight(24) + $(if (Test-Path (Join-Path $Game "mods\aurie\$f")) { 'installed' } else { '-' }))
    }
    $s = Read-State $Game
    if ($s) { Say "  installed by this tool: version $($s.version) on $($s.date)" }
}

function Assert-Official {
    foreach ($f in $Official.Keys) {
        if ((Get-Sha256 (Join-Path $Payload $f)) -ne $Official[$f]) {
            throw "payload\$f is not the official release file (checksum mismatch). Download the installer again."
        }
    }
    Say 'Loader files match the official Aurie / YYToolkit releases' 'DarkGray'
}

function New-State {
    [pscustomobject]@{ version = $Version; date = ''; added = @(); exe_sha256 = ''; patched_by_us = $false }
}

# Our hook out of the game exe: the backed-up original back (byte-identical), else the patcher's own removal
function Restore-Exe([string]$Game, $State) {
    $exe = Join-Path $Game $ExeName
    $backup = Join-Path $Game "mods\slormancer-mods-backup\$ExeName.original"
    if ((Test-Patched $exe) -and $State -and $State.patched_by_us) {
        if ($State.exe_sha256 -and (Test-Path $backup) -and ((Get-Sha256 $backup) -eq $State.exe_sha256)) {
            Copy-Item $backup $exe -Force
            Say "Restored the original $ExeName" 'DarkGray'
        } else {
            Invoke-Patcher $Game 'remove'
            Say "Removed the loader from $ExeName" 'DarkGray'
        }
    }
}

# Another loader starts Aurie: our exe hook and our mods\native\AurieCore.dll must go, or Aurie would start twice
function Use-OtherLoader([string]$Game, $State, [string]$Other) {
    Say "This game loads mods through $Other - using it, no second loader." 'DarkGray'
    Restore-Exe $Game $State
    $State.patched_by_us = $false
    $core = 'mods\native\AurieCore.dll'
    if (@($State.added) -contains $core) {
        $p = Join-Path $Game $core
        if (Test-Path $p) { Remove-Item $p -Force; Say "Removed $core (the other loader has its own)" 'DarkGray' }
        $State.added = @($State.added | Where-Object { $_ -ne $core })
    }
    if (Test-Patched (Join-Path $Game $ExeName)) {
        Say "Warning: $ExeName also has an Aurie loader that this installer did not add, so Aurie may start twice." 'Yellow'
        Say "         Steam > The Slormancer > Properties > Installed Files > Verify restores the original exe." 'Yellow'
    }
}

function Install-All([string]$Game) {
    Assert-NotRunning
    Assert-Official
    $exe = Join-Path $Game $ExeName
    $backupDir = Join-Path $Game 'mods\slormancer-mods-backup'
    $state = Read-State $Game
    if (-not $state) { $state = New-State }
    $other = Get-OtherLoader $Game
    if ($other) {
        New-Item -ItemType Directory -Force -Path (Join-Path $Game 'mods\aurie') | Out-Null
        Use-OtherLoader $Game $state $other
        $added = [System.Collections.ArrayList]@($state.added | Where-Object { $_ })
        $tk = 'mods\aurie\YYToolkit.dll'                   # the other loader brings its own; only if it is missing
        if (-not (Test-Path (Join-Path $Game $tk))) {
            Copy-Item (Join-Path $Payload 'YYToolkit.dll') (Join-Path $Game $tk)
            if (-not $added.Contains($tk)) { [void]$added.Add($tk) }
            Say 'Installed YYToolkit.dll' 'DarkGray'
        }
        Install-Mods $Game $state $added
        return
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $Game 'mods\native'), (Join-Path $Game 'mods\aurie'), $backupDir | Out-Null

    # 1. keep the untouched game exe (only while it is unpatched: after a Steam update this is the new original).
    #    An exe that already had the loader (other Aurie mods) is not ours to un-patch later.
    if (-not (Test-Patched $exe)) {
        Copy-Item $exe (Join-Path $backupDir "$ExeName.original") -Force
        $state.exe_sha256 = Get-Sha256 $exe
        $state.patched_by_us = $true
        Say "Backed up the original $ExeName" 'DarkGray'
    }
    # 2. the loader, unless you already have one
    $added = [System.Collections.ArrayList]@($state.added | Where-Object { $_ })
    foreach ($f in $Loader.Keys) {
        $dst = Join-Path $Game "mods\$($Loader[$f])\$f"
        if (-not (Test-Path $dst)) {
            Copy-Item (Join-Path $Payload $f) $dst
            if (-not $added.Contains("mods\$($Loader[$f])\$f")) { [void]$added.Add("mods\$($Loader[$f])\$f") }
            Say "Installed $f" 'DarkGray'
        }
    }
    # 3. the loader hook in the game exe (re-applied after Steam updates or a file check)
    Invoke-Patcher $Game 'install'
    if (-not (Test-Patched $exe)) { throw 'The game exe was not patched.' }
    Say 'Aurie loader is in the game exe' 'DarkGray'
    Install-Mods $Game $state $added
}

function Install-Mods([string]$Game, $state, $added) {
    # the mods (always replaced: this is also the update)
    foreach ($f in $Mods) {
        Copy-Item (Join-Path $Payload $f) (Join-Path $Game "mods\aurie\$f") -Force
        if (-not $added.Contains("mods\aurie\$f")) { [void]$added.Add("mods\aurie\$f") }
        Say "Installed $f" 'DarkGray'
    }
    $state.added = @($added)
    $state.version = $Version
    $state.date = (Get-Date).ToString('yyyy-MM-dd HH:mm')
    Write-State $Game $state
    if ($Mods -contains 'BuildFilter.dll') {
        Say "Done. Start The Slormancer from Steam; in the inventory you will find Build Filter next to the Filter-o-tron." 'Green'
    } else {
        Say "Done. Start The Slormancer from Steam; the MODS button is next to DISCORD in the main menu." 'Green'
    }
    if (-not (Get-OtherLoader $Game)) {
        Say 'If Steam updates the game or you verify its files: run this installer again and choose 2 (Repair).' 'Green'
    }
}

function Repair-Loader([string]$Game) {
    # Only the Aurie loader: the loader files and the hook in the game exe. Mods, their settings and anything else in
    # mods\ stay as they are. Fixes: Steam updated / verified the game (the new exe has no hook), and turns an older
    # hook with a full path into the relative one.
    Assert-NotRunning
    Assert-Official
    $exe = Join-Path $Game $ExeName
    $backupDir = Join-Path $Game 'mods\slormancer-mods-backup'
    $state = Read-State $Game
    if (-not $state) { $state = New-State }
    $other = Get-OtherLoader $Game
    if ($other) {
        Use-OtherLoader $Game $state $other
        if (Test-Path (Join-Path $Game 'mods\slormancer-mods.json')) { Write-State $Game $state }
        Say "Nothing to repair here: $other starts the mods, and it does not need the game exe changed." 'Green'
        return
    }
    New-Item -ItemType Directory -Force -Path (Join-Path $Game 'mods\native'), (Join-Path $Game 'mods\aurie'), $backupDir | Out-Null
    if (-not (Test-Patched $exe)) {
        Copy-Item $exe (Join-Path $backupDir "$ExeName.original") -Force      # a Steam update: the new original
        $state.exe_sha256 = Get-Sha256 $exe
        $state.patched_by_us = $true
        Say "Backed up the original $ExeName" 'DarkGray'
    }
    $added = [System.Collections.ArrayList]@($state.added | Where-Object { $_ })
    foreach ($f in $Loader.Keys) {
        $rel = "mods\$($Loader[$f])\$f"
        $dst = Join-Path $Game $rel
        # ours (or missing): put the official file back; a loader that was there before us is left alone
        if (-not (Test-Path $dst) -or $added.Contains($rel)) {
            Copy-Item (Join-Path $Payload $f) $dst -Force
            if (-not $added.Contains($rel)) { [void]$added.Add($rel) }
            Say "Loader file $f in place" 'DarkGray'
        }
    }
    Invoke-Patcher $Game 'install'
    if (-not (Test-Patched $exe)) { throw 'The game exe was not patched.' }
    $state.added = @($added)
    if (-not $state.date) { $state.date = (Get-Date).ToString('yyyy-MM-dd HH:mm') }
    Write-State $Game $state
    Say 'The loader is repaired. Your mods and their settings were not touched.' 'Green'
}

function Remove-Mods([string]$Game) {
    Assert-NotRunning
    foreach ($f in $AllMods) {
        $p = Join-Path $Game "mods\aurie\$f"
        if (Test-Path $p) { Remove-Item $p -Force; Say "Removed $f" 'DarkGray' }
    }
    $state = Read-State $Game
    if ($state) {
        $state.added = @($state.added | Where-Object { $_ -notlike 'mods\aurie\SlormAPI.dll' -and $_ -notlike 'mods\aurie\BuildFilter.dll' })
        Write-State $Game $state
    }
    Say 'The mods are removed (the Aurie loader stays; settings in mods\aurie\BuildFilter and mods\aurie\SlormAPI and Lua mods in mods\lua stay).' 'Green'
}

function Remove-All([string]$Game) {
    Assert-NotRunning
    Remove-Mods $Game
    $exe = Join-Path $Game $ExeName
    $state = Read-State $Game
    Restore-Exe $Game $state
    $other = Get-OtherLoader $Game
    if ($state) {
        foreach ($rel in $state.added) {
            if ($other -and $rel -eq 'mods\aurie\YYToolkit.dll') { continue }   # the other loader needs it
            $p = Join-Path $Game $rel
            if (Test-Path $p) { Remove-Item $p -Force; Say "Removed $rel" 'DarkGray' }
        }
        Remove-Item (Join-Path $Game 'mods\slormancer-mods.json') -Force
    }
    if ($state -and $state.exe_sha256 -and ((Get-Sha256 $exe) -ne $state.exe_sha256)) {
        Say "Note: $ExeName differs from the backed-up original (a Steam update?). Steam > Properties > Installed Files > Verify restores it." 'Yellow'
    } elseif (-not (Test-Patched $exe)) {
        # the original is back: drop our backup and any folders left empty
        Remove-Item (Join-Path $Game 'mods\slormancer-mods-backup') -Recurse -Force -ErrorAction SilentlyContinue
        # the mods' own data folders go only when empty (settings and Lua mods you added stay)
        foreach ($d in @('mods\aurie\BuildFilter', 'mods\aurie\SlormAPI\settings', 'mods\aurie\SlormAPI', 'mods\lua',
                         'mods\aurie', 'mods\native', 'mods')) {
            $p = Join-Path $Game $d
            if ((Test-Path $p) -and -not (Get-ChildItem $p -Force)) { Remove-Item $p -Force }
        }
    }
    Say 'Everything this installer added is removed.' 'Green'
}

# ---- main -----------------------------------------------------------------------------------------------------------
try {
    $Title = if ($Mods -contains 'BuildFilter.dll') { 'BuildFilter installer' } else { 'SlormAPI installer' }
    Say "$Title $Version  (Aurie loader + $(($Mods | ForEach-Object { $_ -replace '\.dll$', '' }) -join ' + '))" 'Cyan'
    if (-not $GamePath) { $GamePath = Find-Game }
    if (-not $GamePath) {
        Say 'The Slormancer was not found in your Steam libraries. Please pick its folder.' 'Yellow'
        $GamePath = Ask-Folder
    }
    if (-not $GamePath -or -not (Test-Path (Join-Path $GamePath $ExeName))) { throw "No $ExeName in '$GamePath'." }
    $GamePath = (Resolve-Path $GamePath).Path
    if ($Action -ne 'status' -and -not (Assert-Writable $GamePath)) {
        # Steam folders under Program Files need administrator rights: ask once and run again
        Say 'The game folder needs administrator rights; asking Windows...' 'Yellow'
        $argsList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$($MyInvocation.MyCommand.Path)`"",
                      '-Action', $Action, '-GamePath', "`"$GamePath`"")
        if ($Yes) { $argsList += '-Yes' }
        Start-Process powershell -Verb RunAs -ArgumentList $argsList
        exit 0
    }
    if ($Action -eq 'menu') {
        Show-Status $GamePath
        Say ''
        Say '  1  Install / update'
        Say '  2  Repair the loader only (after a game update or file check; keeps mods and settings)'
        Say '  3  Remove the mods (keep the Aurie loader)'
        Say '  4  Remove everything (mods + loader, original game exe back)'
        Say '  5  Status'
        Say '  Q  Quit'
        $choice = Read-Host 'Choose'
        $Action = @{ '1' = 'install'; '2' = 'repair'; '3' = 'remove-mods'; '4' = 'remove-all'; '5' = 'status' }[$choice.Trim().ToUpper()]
        if (-not $Action) { exit 0 }
    }
    switch ($Action) {
        'install' { Install-All $GamePath }
        'update' { Install-All $GamePath }
        'repair' { Repair-Loader $GamePath }
        'remove-mods' { Remove-Mods $GamePath }
        'remove-all' { Remove-All $GamePath }
        'status' { Show-Status $GamePath }
    }
    $code = 0
} catch {
    Say "Error: $($_.Exception.Message)" 'Red'
    $code = 1
}
if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }
exit $code
