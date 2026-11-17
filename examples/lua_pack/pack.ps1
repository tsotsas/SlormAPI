# Zips a Lua pack for Nexus / Vortex: <id>-<version>.zip with mods/lua/<id>/lua-pack.json, its .lua files and its
# art and sounds.
#   pack.cmd [folder]        (default: my_mod; a folder here or a full path)
# Checks lua-pack.json against the rules The Slormancer's Vortex extension installs by, so the zip installs as is.
# That extension installs a pack's .lua files only, so each image and sound (.png .gif .jpg .jpeg .ogg .wav) goes in
# as a data file "<file>.lua" (return "<the file in base64>"): SlormAPI 2.1.0+ reads art/x.png from art/x.png.lua when
# the file itself is not there, so the mod's code stays the same.
param([string]$Folder = 'my_mod')
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem

$dir = (Resolve-Path $(if ([IO.Path]::IsPathRooted($Folder)) { $Folder } else { Join-Path $PSScriptRoot $Folder })).Path.TrimEnd('\')
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

$dataExt = '.png', '.gif', '.jpg', '.jpeg', '.ogg', '.wav'
$files = @(Get-ChildItem $dir -Recurse -File | Where-Object { $_.Extension -notin '.zip', '.dll', '.asi', '.exe' })
# (a data file left next to its file is made again from the file)
$files = @($files | Where-Object { -not ($_.Name -match '^(.+\.(png|gif|jpg|jpeg|ogg|wav))\.lua$' -and (Test-Path -LiteralPath (Join-Path $_.DirectoryName $Matches[1]) -PathType Leaf)) })
$data = @($files | Where-Object { $dataExt -contains $_.Extension.ToLower() })
$count = @($files | Where-Object { $_.Extension -eq '.lua' }).Count + $data.Count + 1
if ($count -gt 256) { $problems += "Vortex installs up to 256 files of a pack (lua-pack.json, .lua and data files): this one has $count" }
if ($data -and $req.'slorm.slormapi' -match $ver -and [version]$req.'slorm.slormapi' -lt [version]'2.1.0') {
    $problems += 'its art / sounds go in as data files, which SlormAPI reads from 2.1.0: requires "slorm.slormapi": "2.1.0"'
}
if ($problems) { $problems | ForEach-Object { Write-Host "lua-pack.json: $_" -ForegroundColor Red }; exit 1 }

$zip = Join-Path $PSScriptRoot "$($m.id)-$($m.version).zip"
if (Test-Path $zip) { Remove-Item $zip }
$utf8 = New-Object System.Text.UTF8Encoding $false
$archive = [System.IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    foreach ($f in $files) {
        $rel = $f.FullName.Substring($dir.Length + 1).Replace('\', '/')
        if ($data -contains $f) {
            $w = New-Object System.IO.StreamWriter($archive.CreateEntry("mods/lua/$($m.id)/$rel.lua").Open(), $utf8)
            try {
                $w.Write("-- SlormAPI data file: $rel in base64 (made by pack.ps1; SlormAPI reads it as that file)`n")
                $w.Write('return "' + [Convert]::ToBase64String([IO.File]::ReadAllBytes($f.FullName)) + "`"`n")
            } finally { $w.Dispose() }
            $rel += '.lua'
        } else {
            [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, "mods/lua/$($m.id)/$rel") | Out-Null
        }
        Write-Host "  mods/lua/$($m.id)/$rel"
    }
} finally { $archive.Dispose() }
$other = @($files | Where-Object { $_.Extension -ne '.lua' -and $dataExt -notcontains $_.Extension.ToLower() -and $_.Name -ne 'lua-pack.json' -and $_.Name -ne 'README.md' })
if ($other) {
    Write-Host "Vortex installs a pack's .lua files only (art and sounds go in as data files): these $($other.Count) file(s) will be missing for Vortex users -" -ForegroundColor Yellow
    $other | ForEach-Object { Write-Host "  $($_.FullName.Substring($dir.Length + 1))" -ForegroundColor Yellow }
}
$note = if ($data) { " ($($data.Count) art / sound file(s) as .lua data files)" } else { '' }
Write-Host "$zip$note - upload it to Nexus, or drag it onto Vortex's Mods page" -ForegroundColor Green
