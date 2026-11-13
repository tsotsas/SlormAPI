# Zips a Lua pack for Nexus / Vortex: <id>-<version>.zip with mods/lua/<id>/lua-pack.json, its .lua files and its
# other files (art, sounds).
#   pack.cmd [folder]        (default: my_mod)
# Checks lua-pack.json against the rules The Slormancer's Vortex extension installs by, so the zip installs as is.
# That extension installs a pack's .lua files only: a pack with other files (a class's art or sounds) is installed by
# hand - the zip unzipped into the game folder - and this says so.
param([string]$Folder = 'my_mod')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$dir = (Resolve-Path (Join-Path $PSScriptRoot $Folder)).Path
$m = Get-Content (Join-Path $dir 'lua-pack.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$problems = @()
$allowed = '$schema', 'schema_version', 'type', 'id', 'name', 'version', 'author', 'description', 'entry', 'minimum_runtime', 'requires'
foreach ($k in $m.PSObject.Properties.Name) { if ($allowed -notcontains $k) { $problems += "unknown field '$k'" } }
if ($m.schema_version -ne 1 -or $m.type -ne 'slorm-lua') { $problems += 'needs "schema_version": 1 and "type": "slorm-lua"' }
if ($m.id -cnotmatch '^[a-z0-9_.-]{1,64}$' -or $m.id -match '[.]$') { $problems += 'id: 1-64 of a-z 0-9 _ . - (lower case, not ending in .)' }
if (-not $m.name) { $problems += 'name is missing' }
$ver = '^(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})$'
if ($m.version -notmatch $ver) { $problems += 'version: x.y.z, e.g. 1.0.0' }
$entry = if ($m.entry) { $m.entry } else { 'main.lua' }
if ($entry -notmatch '\.lua$' -or $entry -match '[\\:]' -or $entry -match '(^|/)\.\.?(/|$)') { $problems += 'entry: a .lua file in the pack folder' }
elseif (-not (Test-Path (Join-Path $dir $entry))) { $problems += "entry '$entry' not found" }
$req = $m.requires
if (-not $req -or -not $req.'slorm.slormapi') { $problems += 'requires: { "slorm.slormapi": "<minimum SlormAPI version>" }' }
else { foreach ($p in $req.PSObject.Properties) { if ($p.Value -notmatch $ver) { $problems += "requires.$($p.Name): x.y.z" } } }
if ($problems) { $problems | ForEach-Object { Write-Host "lua-pack.json: $_" -ForegroundColor Red }; exit 1 }

$files = @(Get-ChildItem $dir -Recurse -File | Where-Object { $_.Extension -notin '.zip', '.dll', '.asi', '.exe' })
$other = @($files | Where-Object { $_.Extension -ne '.lua' -and $_.Name -ne 'lua-pack.json' -and $_.Name -ne 'README.md' })
$zip = Join-Path $PSScriptRoot "$($m.id)-$($m.version).zip"
if (Test-Path $zip) { Remove-Item $zip }
$archive = [System.IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($dir.Length + 1).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, "mods/lua/$($m.id)/$rel") | Out-Null
        Write-Host "  mods/lua/$($m.id)/$rel"
    }
} finally { $archive.Dispose() }
if ($other) {
    Write-Host "$zip - $($other.Count) file(s) besides .lua (art, sounds): Vortex installs a pack's .lua files only." -ForegroundColor Yellow
    Write-Host "Tell players to unzip it into the game folder (The Slormancer\mods\lua\$($m.id)\) instead." -ForegroundColor Yellow
} else {
    Write-Host "$zip - upload it to Nexus, or drag it onto Vortex's Mods page" -ForegroundColor Green
}
