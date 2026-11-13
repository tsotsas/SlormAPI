# Evil Knight — an example class

A complete custom class for SlormAPI 2.0.0+, as a Lua pack. It is the reference for [docs/CLASSES.md](../../docs/CLASSES.md):
nearly every option a class has is used here, in one file (`main.lua`).

**Play it:** copy this folder into `<The Slormancer>\mods\lua\` (as `mods\lua\evil_knight\`), start the game, and talk
to the Evil Knight in the hub (right of the Knight's spot): Yes plays it. Copy it by hand: Vortex installs a pack's
`.lua` files only, so its art would be left out.

| what | where in `main.lua` |
|---|---|
| its look: the Knight's sprites inverted (`art`, 31 game sprites from `art/`) and a skin "Knight's Memory" (the Knight as he was) | `ART_NAMES`, `LOOK`, `MEMORY`, `skins` |
| its own kit (`kit = "own"`): 3 supports and 8 skills with their own code | `skills = {...}` in `S.class`, the `skills[n] = { cast = ..., hit = ... }` handlers |
| its upgrade tree: 128 entries (stat passives, damage / cooldown effects, upgrades its code reads) | `entries`, the helpers `stat`, `more`, `faster`, `upgrade` |
| its class, mechanic and specialization texts (Dread Lord, Night Stalker, Black Bulwark) | `texts` |
| its mechanic, Malice: 1 per hit, at 10 Unleashed Malice, shown on the HUD | `on_hit_malice`, `C.meter` |
| its Ancestral Legacy tree: 152 legacies in the game's 140 realms (stat legacies, 13 keystones, 2 auras, 2 actives) | `REALMS`, `SCHOOLS`, `KEYSTONES`, `ACTIVABLES`, `legacies` |
| its achievements: the Knight's 23 renamed, and 4 of its own | `achievements` |
| its own versions of two reapers (Malice-Drinker) | `reapers` |
| 4 legendaries that drop for it only | `S.legendary{ ..., hero = C.hero }` |
| 4 story missions in its own story area, The Black Crypt, with lines before and after | `missions`, `area` |
| its hub line, said by itself | `talk`, `talk_as` |
| its damage as elemental damage (basic and elemental damage swapped after each stats refresh) | `S.on_stats` at the end |

The art files are the game's own Knight sprites with their colours inverted. A class of yours uses its own files: any
PNG strip named in `art` replaces that game sprite (see [CLASSES.md — Art](../../docs/CLASSES.md#art)).
