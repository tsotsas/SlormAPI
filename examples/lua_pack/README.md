# A Lua mod, ready for Vortex

1. **Name it.** Rename `my_mod` and set `id` (a-z 0-9 _ . -), `name`, `author` and `description` in
   `lua-pack.json`: the Mods page and Options > Mods show them.
2. **Write it.** Copy the folder into `<The Slormancer>\mods\lua\`: it runs at once and reloads every time you save.
   Errors show on the Mods page and in the log. Every function: [API.md](../../docs/API.md#lua-reference).
3. **Pack it.** Run `pack.cmd <folder>` (a folder next to it, or a full path; double-click it for `my_mod`): it
   checks `lua-pack.json` and makes `<id>-<version>.zip`. Upload that to Nexus; players drag it onto Vortex's Mods
   page and Vortex installs it.

Raise `version` for each release, and `requires["slorm.slormapi"]` when you use something from a newer SlormAPI.

**A pack with art or sounds** (a custom class's PNG, GIF, JPG, OGG or WAV files): Vortex installs a pack's `.lua`
files only, so `pack.cmd` puts each of them into the zip as a data file `<file>.lua`, which SlormAPI 2.1.0+ reads as
that file. Your code stays the same (`slorm.sprite("art/idle.png")`); the pack requires `"slorm.slormapi": "2.1.0"`
(`pack.cmd` says so). Vortex takes up to 256 files per pack.
