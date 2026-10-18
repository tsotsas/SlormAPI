"""Build the player installer zip: SlormAPI-Installer-<version>.zip, or with mods added, e.g. BuildFilter-Installer-<version>.zip

    python installer/build.py [--slormapi bin/SlormAPI.dll] [--loader <folder>] [--out dist]
    python installer/build.py --mod <path to BuildFilter.dll>          # -> BuildFilter-Installer-<version>.zip

--loader is a folder with the official AurieCore.dll + AuriePatcher.exe (Aurie v2.0.2) and YYToolkit.dll
(YYToolkit v5.0.0c) from their GitHub releases; their SHA-256 is checked. The zip holds Install.bat, installer.ps1,
payload/ (the loader files and the mods), licenses/ and README.txt.
"""
import argparse, hashlib, os, re, shutil, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
OFFICIAL = {   # GitHub's published SHA-256 of the official release files
    'AurieCore.dll': '18e3a1de980f487a6b3858b673d2030e96984dd96de3a047b43a263a5ba829ae',
    'AuriePatcher.exe': '4d3aec439dbba5209ad48fb0a40d6324406247fe678541df9031f5b89363d536',
    'YYToolkit.dll': 'ae7809f136f9222e5f49393d7ce7c3ad4c375c171308e2414660946d4ba0378a',
}
KNOWN = {      # mods the installer knows (installer.ps1 removes these with "Remove the mods")
    'BuildFilter.dll': ('a loot filter for your build (inventory: Build Filter next to the Filter-o-tron)',
                        'https://github.com/tsotsas/BuildFilter',
                        'BuildFilter starts switched off: inventory > Build Filter > Gear to pick\n     your stats, then turn it on in Rules.'),
}
README = """{title} {v}
{rule}

Installs, updates, repairs or removes these mods for The Slormancer (Steam, Windows):
{mods}  - Aurie Core 2.0.2 + YYToolkit 5.0.0c, the mod loader (only if you do not have one yet)

How to use
  1. Close the game.
  2. Double-click Install.bat and choose 1 (Install / update).
     It finds the game in your Steam libraries (or asks for the folder). If the game folder needs administrator
     rights, Windows asks once.
  3. Start the game from Steam as usual. {start}

What it changes (only in the game folder, never your saves)
  - "The Slormancer.exe" gets Aurie's small loader. The original is kept in mods\\slormancer-mods-backup\\.
  - It adds mods\\native\\AurieCore.dll, mods\\aurie\\YYToolkit.dll and {files},
    and writes mods\\slormancer-mods.json (what it added, so removing only removes that).
  - Already using Slorm Loader (Vortex) or another version.dll mod loader? Then the game exe is left alone and only
    the mods are added: that loader starts them.

Repair (keeps your mods and settings)
  After a game update or Steam > Verify files, the mods stop loading. Run Install.bat and choose 2 (Repair the
  loader only): it puts the loader back and touches nothing else. Moving the game to another folder or drive is
  fine - the loader is found relative to the game's folder at every launch.

Remove
  Install.bat > 3 removes the mods (keeps the loader).
  Install.bat > 4 removes everything it added and puts the original game exe back.

Windows or your antivirus may warn about unsigned files. None of these files are digitally signed - the official
Aurie and YYToolkit releases are not signed by their authors either. The loader files here are byte-identical to the
official releases (the installer checks this before using them):
  AurieCore.dll     Aurie v2.0.2       SHA-256 {AurieCore.dll}
  AuriePatcher.exe  Aurie v2.0.2       SHA-256 {AuriePatcher.exe}
  YYToolkit.dll     YYToolkit v5.0.0c  SHA-256 {YYToolkit.dll}
  (compare: https://github.com/AurieFramework/Aurie/releases/tag/v2.0.2 ,
            https://github.com/AurieFramework/YYToolkit/releases/tag/v5.0.0c)
The source code is public under AGPL-3.0:
  SlormAPI     https://github.com/tsotsas/SlormAPI (the installer is in installer/)
{links}  Aurie        https://github.com/AurieFramework/Aurie
  YYToolkit    https://github.com/AurieFramework/YYToolkit
Licenses are in the licenses folder.
"""


def sha256(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--slormapi', default=os.path.join(REPO, 'bin', 'SlormAPI.dll'))
    ap.add_argument('--mod', action='append', default=[], help='another mod DLL to install with SlormAPI (repeatable)')
    ap.add_argument('--loader', default=os.path.join(HERE, 'loader'),
                    help='folder with the official AurieCore.dll, AuriePatcher.exe, YYToolkit.dll')
    ap.add_argument('--out', default=os.path.join(REPO, 'dist'))
    ap.add_argument('--name', help='zip name without version (default: <first mod>-Installer, or SlormAPI-Installer)')
    a = ap.parse_args()

    version = re.search(r"\$Version = '([^']+)'", open(os.path.join(HERE, 'installer.ps1'), encoding='utf-8').read()).group(1)
    mods = [os.path.basename(m) for m in a.mod]
    for m in mods:
        if m not in KNOWN:
            raise SystemExit(f'{m}: add it to KNOWN here and to $AllMods in installer.ps1 first')
    title = (a.name or (mods[0][:-4] + '-Installer' if mods else 'SlormAPI-Installer'))
    name = f'{title}-{version}'
    out = os.path.join(a.out, name)
    payload = {'SlormAPI.dll': a.slormapi}
    payload.update({os.path.basename(m): m for m in a.mod})
    for f, digest in OFFICIAL.items():
        p = os.path.join(a.loader, f)
        if not os.path.isfile(p) or sha256(p) != digest:
            raise SystemExit(f'{p}: missing or not the official release file')
        payload[f] = p
    for f, p in payload.items():
        if not os.path.isfile(p):
            raise SystemExit(f'missing {p}')

    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(os.path.join(out, 'payload'))
    os.makedirs(os.path.join(out, 'licenses'))
    for f in ('Install.bat', 'installer.ps1'):
        shutil.copy2(os.path.join(HERE, f), out)
    for f, p in payload.items():
        shutil.copy2(p, os.path.join(out, 'payload', f))
    lic = os.path.join(REPO, 'LICENSE')                  # AGPL-3.0, the license of Aurie, YYToolkit and these mods
    for f in ['Aurie', 'YYToolkit', 'SlormAPI'] + [m[:-4] for m in mods]:
        shutil.copy2(lic, os.path.join(out, 'licenses', f + '-LICENSE.txt'))
    lines = ''.join(f'  - {m[:-4]} {version}  {KNOWN[m][0]}\n' for m in mods)
    lines += f'  - SlormAPI {version}     the base mod for other mods and Lua mods (MODS page, Mods tab in Options)\n'
    start = KNOWN[mods[0]][2] if mods else 'The MODS button is next to DISCORD in the main menu.'
    heading = title.replace('-Installer', ' installer')
    text = (README.replace('{title}', heading).replace('{rule}', '=' * (len(heading) + len(version) + 1))
            .replace('{mods}', lines).replace('{start}', start)
            .replace('{files}', ' and '.join(['SlormAPI.dll'] + mods))
            .replace('{links}', ''.join(f'  {m[:-4]:<12} {KNOWN[m][1]}\n' for m in mods))
            .replace('{v}', version).replace('{AurieCore.dll}', OFFICIAL['AurieCore.dll'])
            .replace('{AuriePatcher.exe}', OFFICIAL['AuriePatcher.exe']).replace('{YYToolkit.dll}', OFFICIAL['YYToolkit.dll']))
    with open(os.path.join(out, 'README.txt'), 'w', encoding='utf-8', newline='\r\n') as fh:
        fh.write(text)
    zpath = out + '.zip'
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(out):
            for f in files:
                p = os.path.join(root, f)
                z.write(p, os.path.join(name, os.path.relpath(p, out)))
    print(f'{zpath}  ({os.path.getsize(zpath) // 1024} KB)')


if __name__ == '__main__':
    main()
