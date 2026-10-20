# A Lua mod, ready for Vortex

1. **Name it.** Rename `my_mod` and set `id` (a-z 0-9 _ . -), `name`, `author` and `description` in
   `lua-pack.json`: the Mods page and Options > Mods show them.
2. **Write it.** Copy the folder into `<The Slormancer>\mods\lua\`: it runs at once and reloads every time you save.
   Errors show on the Mods page and in the log. Every function: [API.md](../../docs/API.md#lua-reference).
3. **Pack it.** Run `pack.cmd <folder>` (or double-click it for `my_mod`): it checks `lua-pack.json` and makes
   `<id>-<version>.zip`. Upload that to Nexus; players drag it onto Vortex's Mods page and Vortex installs it.

Raise `version` for each release, and `requires["slorm.slormapi"]` when you use something from a newer SlormAPI.
