# Zips a Lua pack for Nexus / Vortex: <id>-<version>.zip with mods/lua/<id>/lua-pack.json and its .lua files.
#   pack.cmd [folder]        (default: my_mod)
# Checks lua-pack.json against the rules The Slormancer's Vortex extension installs by, so the zip installs as is.
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

$files = @(Get-ChildItem $dir -Recurse -File -Filter *.lua) + @(Get-Item (Join-Path $dir 'lua-pack.json'))
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
Write-Host "$zip - upload it to Nexus, or drag it onto Vortex's Mods page" -ForegroundColor Green
