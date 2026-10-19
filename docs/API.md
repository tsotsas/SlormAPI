# SlormAPI 1.0.0 — API reference

Everything a mod can do through SlormAPI: every command with its fields and results, the events, draw programs and
widgets, Lua mods, settings, data files, and the game facts you need to use them.

- [How it works](#how-it-works)
- [Making a mod](#making-a-mod) — [C++](#a-c-mod) · [Lua](#a-lua-mod) · [external tool (dev server)](#an-external-tool-the-dev-server)
- [Values, paths and roots](#values-paths-and-roots)
- [Commands](#commands)
  - [Game state](#game-state): `ping` `get` `keys` `set` `call` `instances` `objects` `scripts`
  - [Hooks](#hooks): `hook` `unhook` `hooks` `hooklog`
  - [Drawing and UI](#drawing-and-ui): `ui_add` `ui_remove` `ui_refresh` `overlay`
  - [Events](#events-commands): `subscribe` `unsubscribe` `events` `event_types`
  - [Mods](#mods): `register_mod` `mods` `mods_page` `mod_enable`
  - [Settings](#settings): `register_settings` `settings` `set_setting`
  - [Data files](#data-files): `mod_data` `mod_log`
  - [Game knowledge](#game-knowledge): `item_info` `stat_info` `stat_list` `hero_stats` `slot_info` `legendary_info` `skill_info`
  - [Other](#other): `batch` `perf` `runner` `routines` `routine_index`
- [Events](#events)
- [Draw programs](#draw-programs)
- [Widgets (UI kit)](#widgets-ui-kit)
- [Lua reference](#lua-reference)
- [Items](#items)
- [Game cheat sheet](#game-cheat-sheet)
- [Rules of thumb](#rules-of-thumb)

---

## How it works

SlormAPI is an [Aurie](https://github.com/AurieFramework/Aurie) module built on
[YYToolkit](https://github.com/AurieFramework/YYToolkit) 5. The Slormancer is a GameMaker game compiled with YYC: its
logic is native code, but its variables, instances, scripts and built-in functions are reachable at run time, and
that is what SlormAPI exposes.

Every request is **one JSON object with a `cmd`**; every response is

```json
{"ok": true, "result": ...}          {"ok": false, "error": "what went wrong"}
```

The same commands are available in three ways:

| from | how | owner |
|---|---|---|
| a C++ Aurie module | `SlormAPIInterface::Request(json)` (+ `Subscribe` for events) | whatever you pass as `"owner"` |
| a Lua mod | `slorm.request{...}` and the helpers on top of it | forced to `"lua:<file name>"` |
| an external tool | newline-delimited JSON on `127.0.0.1:47950` (off by default) | whatever you pass |

Everything runs **on the game thread**. Requests from C++ and Lua run immediately; TCP requests are processed between
the game's object events (never inside a hooked script call). Events are delivered once per frame.

**Owners.** Many commands take `"owner"`: your mod's name. It keeps mods apart — each owner has its own hook layer
on a script, its own captured-call log, its own event queue, its own UI programs and its own data file. Use the
**same name** as in `register_mod`, so the Mods page can attribute problems (e.g. a script your mod could not hook
after a game update) to your mod. Without `owner` you share the `""` owner with development tools.

---

## Making a mod

### A C++ mod

An Aurie module (a 64-bit DLL) in `mods/aurie/`. You need the Aurie / YYToolkit 5 SDK headers (in SlormAPI's
`third_party/YYToolkit`; BuildFilter's repository shows a complete project) and SlormAPI's one header,
[`source/SlormAPI_Interface.hpp`](../source/SlormAPI_Interface.hpp):

```cpp
struct SlormAPIInterface : public Aurie::AurieInterfaceBase
{
    virtual const char* Request(const char* RequestJson) = 0;     // -> {"ok":..., "result"/"error":...}
    virtual uint64_t Subscribe(const char* Event, SlormEventCallback Callback, void* User) = 0;   // 0 = unknown event
    virtual void Unsubscribe(uint64_t SubscriptionId) = 0;
};
using SlormEventCallback = void (*)(const char* Event, const char* DataJson, void* User);
```

A minimal mod:

```cpp
#include <YYToolkit/YYTK_Shared.hpp>
#include "SlormAPI_Interface.hpp"
using namespace Aurie;
using namespace YYTK;

static YYTKInterface* g_Yytk = nullptr;
static SlormAPIInterface* g_Api = nullptr;

static void OnKill(const char* Event, const char* DataJson, void* User)
{
    // DataJson: {"enemy": {"$inst": ..., "$object": ...}, "x": ..., "y": ..., "level": ..., "profile": "elite"}
}

// runs for every object event; we use the UI object's Step (once per frame, also in menus)
static void OnObjectEvent(FWCodeEvent& Context)
{
    CCode* code = std::get<2>(Context.Arguments());
    if (!code || !code->GetName() || strcmp(code->GetName(), "gml_Object_obj_ui_next_gen_Step_0") != 0) return;
    if (!g_Api)
    {
        // modules load in any order: ask for SlormAPI from a game callback, not in ModuleInitialize
        AurieInterfaceBase* base = nullptr;
        if (!AurieSuccess(ObGetInterface(SLORMAPI_INTERFACE_NAME, base)) || !base) return;
        g_Api = static_cast<SlormAPIInterface*>(base);
        g_Api->Request(R"({"cmd":"register_mod","name":"MyMod","version":"1.0.0","author":"me",
            "description":"What it does","requires":{"SlormAPI":"1.0.0"},"game":"1.0.2c"})");
        g_Api->Subscribe("enemy_killed", OnKill, nullptr);
    }
    // ... per-frame work: g_Api->Request(...)
}

EXPORTED AurieStatus ModuleInitialize(IN AurieModule* Module, IN const fs::path& ModulePath)
{
    g_Yytk = YYTK::GetInterface();
    if (!g_Yytk) return AURIE_MODULE_DEPENDENCY_NOT_RESOLVED;
    return g_Yytk->CreateCallback(Module, EVENT_OBJECT_CALL, reinterpret_cast<PVOID>(OnObjectEvent), 0);
}

EXPORTED AurieStatus ModuleUnload(IN AurieModule* Module, IN const fs::path& ModulePath)
{
    if (g_Yytk) g_Yytk->RemoveCallback(Module, reinterpret_cast<PVOID>(OnObjectEvent));
    return AURIE_SUCCESS;
}
```

- Build: Visual Studio 2022, x64 Release DLL, C++20. Copy BuildFilter's `.vcxproj` as a starting point.
- The string `Request` returns stays valid until your next `Request`; copy what you keep. `DataJson` in an event
  callback is valid only during the call.
- A callback that crashes is unsubscribed (and logged) instead of taking the game down.
- YYToolkit refuses two callbacks that are byte-identical functions; the linker's identical-code folding (`/OPT:ICF`)
  can make two of yours identical. Keep their bodies different or turn ICF off.
- Use `"owner": "MyMod"` on your hooks, UI, events and data (the C++ side has no forced owner).

### A Lua mod

One text file in `<The Slormancer>/mods/lua/`, e.g. `my_mod.lua`. SlormAPI loads it while the game runs, reloads it
2 seconds after you save it, and lists it on the Mods page (switching it off renames it to `my_mod.lua.disabled` and
unloads it at once).

```lua
slorm.mod{ name = "Kill Counter", version = "1.0", author = "me", description = "Counts your kills" }
local kills = 0
slorm.on("enemy_killed", function(e)
  kills = kills + 1
  slorm.ui("main", { slorm.text(1300, 120, "Kills: " .. kills, "ft_standard_16") })
end)
```

See [Lua reference](#lua-reference) for every function and the sandbox rules. `examples/lua/kill_counter.lua` is a
small complete mod; [BuildFilter](https://github.com/tsotsas/BuildFilter)'s `lua/BuildFilter.lua` is a big one
(hooks, draw programs, clicks, item conversion), written next to its C++ twin.

### An external tool (the dev server)

For tools, tests and reverse engineering. Off for players; it starts when the environment variable
`SLORMAPI_SERVER=1` (or `SLORMAPI_PORT=<port>`) is set, or an empty file `SlormAPI.server` is next to the DLL.

- `127.0.0.1:47950` (loopback only), one JSON request per line, one JSON response per line, in order.
- A request's `"id"` is copied into its response.
- Connections that start like an HTTP request are dropped (web pages cannot reach it); a line over 16 MB ends the
  connection.
- While it is on, any program on the PC can control the game.

```python
import json, socket
s = socket.create_connection(("127.0.0.1", 47950)); f = s.makefile("rw")
f.write(json.dumps({"cmd": "get", "path": "gold"}) + "\n"); f.flush()
print(json.loads(f.readline()))      # {'result': 749312, 'ok': True}
```

---

## Values, paths and roots

**JSON → game.** Numbers, strings, booleans, `null` (undefined) and arrays as you expect; objects become structs,
except:

| JSON | becomes |
|---|---|
| `{"$inst": 100856}` | that instance (fails if it does not exist or is deactivated) |
| `{"$asset": "spr_menu_grey_button"}` | the asset's index (sprite, object, script, font, room, sound...) |

**Game → JSON.** Instances, arrays and structs are expanded to `depth` levels (default 1); below that:

| JSON | is |
|---|---|
| `{"$inst": id, "$object": "obj_loot", "$x": .., "$y": .., "$bbox": [l, r, t, b]}` | an instance (with its variables when expanded) |
| `{"$array": 12}` | an array of 12 entries, not expanded |
| `{"$struct": 5}` | a struct with 5 members, not expanded |
| `{"$method": "..."}` | a method / script reference |
| `{"$ptr": "0x..."}`, `{"$ref": "...", "id": n}` | pointers, references that are not instances |
| `"$truncated"` / `"$truncated": true` | the 500 000-value budget per response ran out |

**Paths** walk from a root: `"a.b[3].c"`, or the same as a list `["a", "b", 3, "c"]`. Names are variables (instance,
global or struct members) or engine built-ins (`x`, `y`, `depth`, `object_index`, `visible`, `room`,
`instance_count`, ...). Reading never creates variables.

**Roots** (`"root"`, and `"self"` for `call`):

| root | means |
|---|---|
| omitted / `"global"` | the global instance (`global.*`) |
| `100856` | the instance with that id |
| `{"$inst": 100856}` | the same |
| `"obj_parent_hero"` | the first instance of that object |

---

## Commands

Field types: *s* string, *n* number, *b* boolean, *a* array, *o* object. Defaults in brackets.

### Game state

#### `ping`
→ `{version, frame, code_events, wndproc_calls, uptime_s, room, fps_real, event_listeners: {callbacks: {event: count},
owners: [..]}, game_version, last_session: {crashed, during_startup}}`. `game_version` is `global.version` (e.g.
`"1.0.2c"`); `last_session.crashed` is true when the previous game session did not end normally.

#### `get`
| field | |
|---|---|
| `path` *s/a* | what to read (empty = the root itself) |
| `root` | see [roots](#values-paths-and-roots) |
| `depth` *n* [1] | how far arrays / structs / instances are expanded |

→ the value. `{"cmd":"get","path":"inventory[0][0]","depth":3}` = the current Knight's helmet (item array).

#### `keys`
`path`, `root` → the member names of a struct or instance (instance variables, not built-ins).

#### `set`
| field | |
|---|---|
| `path` *s/a* | required |
| `value` | JSON → game as above |
| `root` | |

→ `true`. Array entries are written **in place** (the game's own array changes; `set` past the end grows it). A
missing variable is created. Built-ins (`x`, `visible`, ...) work too.

#### `call`
| field | |
|---|---|
| `fn` *s* | a game script (`scr_...`, with or without `gml_Script_`) or an engine function (`instance_destroy`, `array_length`, ...) |
| `args` *a* [] | JSON → game |
| `self` | the instance it runs as (a [root](#values-paths-and-roots); default the global instance) |
| `depth` *n* [1] | of the result |

→ the return value. Scripts that use `self` / `other` need the right `self` (e.g. `scr_create_loot` runs as the hero).
A GML runtime error inside the call comes back as an error, but can leave the game in a bad state: test unknown
scripts in a throwaway game.

#### `instances`
| field | |
|---|---|
| `object` | an object name, index, or `"all"` (children count: `obj_parent_hero` finds the hero) |
| `vars` *a* | variables or built-ins to add to each (not expanded: depth 0) |

→ `[{"$inst", "$object", "$x", "$y", "$bbox", <vars>...}, ...]`. Deactivated instances are not listed.

#### `objects` / `scripts`
→ every object name / every script name (the index in the list is the asset index).

### Hooks

A hook puts your **layer** on a game script: log its calls, change its result, skip it, change its arguments, set
variables after it, or run a draw program inside it. Up to 256 different scripts can be hooked at once (layers of
several mods on one script share a slot).

#### `hook`
| field | |
|---|---|
| `owner` *s* [""] | your layer; hooking the same script again **replaces** your layer only |
| `script` *s* / `scripts` *a* | script name(s), with or without `gml_Script_` |
| `capture` *b* [true] | log the calls (read them with `hooklog`) |
| `capture_args` *a* | log only calls whose arguments match this pattern (see below) |
| `depth` *n* [1] | expansion of logged arguments / results |
| `stack` *b* [false] | log the GML call stack too (slow) |
| `self_vars` *a* | variables of `self` to log with each call |
| `overrides` *a* | `[{args, value, skip, set_args}]`, below |
| `after` *o* | a [draw program](#draw-programs) run after every call (also after a skipped one), e.g. inside a menu bake |
| `post_set` *o* | `{var: value}` set on `self` after each call |
| `post_set_count` *n* [-1] | how many calls `post_set` applies to (-1 = always) |
| `ttl_ms` *n* | the layer's overrides / `post_set` / `after` stop applying unless you hook again within this time |

→ `{"gml_Script_x": slot}` per script, or `"error: ..."` for a script that could not be hooked (that error also shows
on the Mods page for the owner).

**Patterns** (`capture_args`, `overrides[].args`): one entry per argument, compared from the first: `null` = anything,
`"abc*"` = a string starting with abc, other strings / numbers / booleans = equal. Fewer entries than arguments = the
rest match anything.

**Overrides** (the first one whose `args` match is used):
- `{"args": [...], "value": v}` — the script runs, then its result is replaced by `v`.
- `{"args": [...], "skip": true, "value": v}` — the script does **not** run; the call returns `v`.
- `{"args": [...], "set_args": {"2": "new text"}}` — the script runs with those arguments replaced (keys are argument
  indices as strings).

Across mods: skip / `set_args` overrides are checked layer by layer and the first match of any layer wins; result
overrides, `post_set` and `after` programs then run for every layer in order.

#### `unhook`
`owner`, `script` (none = all of your hooks), `all` *b* (every owner's layers — a dev tool; refused for Lua) →
the scripts it removed layers from. A script without layers is un-hooked.

#### `hooks`
→ `[{slot, script, calls, layers: [{owner, capture, depth, overrides, after?, after_error?, expired?}]}]`.
`after_error` is set (and the program dropped) when a layer's draw program failed.

#### `hooklog`
| field | |
|---|---|
| `owner` *s* [""] | whose log |
| `max` *n* [10000] | at most this many entries, oldest first |
| `consume` *b* [true] | remove the returned entries |

→ `{entries: [{seq, script, t_us, depth, self, args, result, us, stack?}], remaining, dropped}`. `self` is an
instance stub (+ `vars` from `self_vars`); `us` is the call's duration. A mod's log keeps the newest 5000 entries.

**Click pattern** (what the UI kit does for you, and how BuildFilter does its own): give your clickable `mba` entries
the script `scr_blacksmith_open_filter_from_inventory` with an argument string of your own (`"mymod:open"`), and hook
that script with `{"overrides": [{"args": ["mymod:*"], "skip": true}], "capture_args": ["mymod:*"]}`: the game calls
it on a click, your override stops it, and the click lands in your `hooklog`.

### Drawing and UI

#### `ui_add`
| field | |
|---|---|
| `owner` *s* | |
| `menu` *s* | a game menu: `main`, `inventory`, `settings`, `skills`, `elements`, `profile`, ... (the program runs after `scr_ui_menus_<menu>`, the game's bake of that menu) |
| `program` *o* | a [draw program](#draw-programs); [widgets](#widgets-ui-kit) work here |

→ `true`. Replaces your earlier program for that menu and redraws the menu. The program runs on every bake of the
menu (the game re-bakes on clicks and when asked), so it can read live values.

#### `ui_remove`
`owner`, `menu` → removes your program and your widgets' clickable entries from that menu.

#### `ui_refresh`
→ `true`; asks the open menu to re-bake (when your program reads game variables that changed).

#### `overlay`
A single global draw program run after one object event every frame — a development tool (one overlay for everybody;
not available to Lua). Fields: `event` [`gml_Object_obj_ui_next_gen_Draw_64`], `vars` / `when` / `calls` (a
program; `calls: []` clears it), `owner`, `ttl_ms`. → `{calls, event, frames_drawn, error?}`. Mods should use
`ui_add` or a hook's `after`.

### Events commands

#### `subscribe`
`owner`, `events` *a* (names, `"*"` = all) → the owner's event list. Events are queued for the owner (up to 2000;
older ones drop) until read with `events`. C++ mods can use `Subscribe` instead (a callback per event).

#### `unsubscribe`
`owner`, `events` *a* (none = all, and the queue is dropped) → `true`.

#### `events`
`owner`, `max` *n* [1000] → `{events: [{seq, event, t_us, data}], remaining, dropped}` (returned events are removed).

#### `event_types`
→ every event name. See [Events](#events).

### Mods

#### `register_mod`
| field | |
|---|---|
| `name` *s* | required; your mod's name (also use it as `owner`) |
| `version` *s* | shown on the Mods page |
| `author`, `description` *s* | shown on the Mods page |
| `file` *s* [name + ".dll"] | your DLL in `mods/aurie` (ties the row to the file and its on/off switch) |
| `requires` *o* | `{"SlormAPI": "1.0.0", "OtherMod": "2.1"}`: minimum versions; unmet ones show as problems |
| `game` *s* | the game version you made it for (`"1.0.2c"`); a different game version shows a note |

→ `true`. Registering again updates your row.

#### `mods`
→ the Mods page's rows: SlormAPI, the DLLs of `mods/aurie` (registered or not, switched off ones too), Lua mods, then
the loader parts. Each: `{name, version, author, description, file, kind: "mod" | "lua" | "loader", loaded, enabled,
locked, pending, problems: [..], note?, managed?, game?}`. `pending` = switched this session, applies at the next
start; `managed` = deployed by a mod manager (Vortex hardlink: switched there, not here).

#### `mods_page`
`enabled` *b* (turn SlormAPI's MODS button and page off / on), `open` *b* → `{enabled, open}`. Not for Lua.

#### `mod_enable`
`file` *s* (`"X.dll"` or `"x.lua"`), `enabled` *b* → `true`. DLLs are renamed to / from `X.dll.disabled` (applies
at the next start); Lua mods switch at once. Refused for SlormAPI, the loader and Vortex-managed files. Not for Lua.

### Settings

Declare your settings once; SlormAPI shows them in **Options > Mods** (a tab per mod, drawn like the game's own
settings), validates and saves them, and tells you when the player changes one.

#### `register_settings`
| field | |
|---|---|
| `mod` *s* | your mod's name (the tab's title) |
| `settings` *a* | `[{key, label, help, type, default, options, min, max, step}]` |

Types: `"bool"` (a switch, default false), `"choice"` (one of `options`, any JSON values; default the first),
`"number"` (`min` [0] .. `max` [100] by `step` [1]; shown as a selector). → the current values `{key: value}`:
the saved ones that are still valid, else the defaults. Saved in `mods/aurie/SlormAPI/settings/<mod>.json`.

```json
{"cmd": "register_settings", "mod": "MyMod", "settings": [
  {"key": "enabled", "label": "Show the overlay", "type": "bool", "default": true},
  {"key": "hotkey", "label": "Hotkey", "help": "While the game has the focus", "type": "choice",
   "options": ["Off", "F5", "F6", "F7"], "default": "Off"},
  {"key": "size", "label": "Size", "type": "number", "min": 50, "max": 200, "step": 10, "default": 100}]}
```

#### `settings`
`mod` → `{key: value}`.

#### `set_setting`
`mod`, `key`, `value` → the stored value (numbers are clamped and snapped to the step; a bad value is an error).
Emits `setting_changed`. Lua mods can change only their own.

### Data files

For anything that is not a setting (a mod's rules, counters, layouts). Files are in `mods/aurie/SlormAPI/data/`,
named after your `owner` (Lua: forced, `lua_<file name>`), else `mod`.

#### `mod_data`
`mod`, `value` → saves `value` (any JSON, up to 4 MB; written whole or not at all) and returns `true`. Without
`value` → the saved value, or `null`.

#### `mod_log`
`mod`, `line` *s* → appends `HH:MM:SS line` to `<name>.log` (a line up to 4096 characters; past 2 MB the file moves
to `<name>.log.old`).

### Game knowledge

Read from the game's own data files (`dat_*.json` next to the exe), so names follow game updates and nothing from
the game is redistributed. Loaded in the background at start.

#### `item_info`
One of: `item` (an [item array](#items)), `inst` (an `obj_loot` instance: id or `{"$inst"}`), `equipped` (0-10, the
current hero's slot). →

```json
{"kind": "equipment", "rarity": "epic", "slot": "glove", "slot_name": "Gloves", "level": 100,
 "legendary": {"id": 52, "name": "...", "roll": 88} | null,
 "stats": [{"tier": "E", "stat": "crit_chance_percent", "name": "Critical Strike Chance", "roll": 40, "value": 2, "pure": false}],
 "extras": [{"type": "skill_mastery" | "attribute" | "reaper_affinity", "index": 3, "value": 1, "name": "..."}]}
```

`value` is the stat's real value on that item level (what the game shows).

#### `stat_info`
`stat` *s* (+ `slot` *s*, `roll` *n*, `level` *n* [100]) → `{stat, name, score, epic_slots: [..], label?, value?}`.
`epic_slots`: slots where it can roll as a gold (epic) line; `label`: its name on that slot ("Max Life %" when the
slot has a flat and a % version); `value`: the value of `roll` on an item of `level`.

#### `slot_info`
`slot` *s* (`helm` `body` `shoulder` `bracer` `glove` `boot` `ring` `amulet` `belt` `cape`), `hero` *n* [current]
→ `{slot, name, hero, epic_stats: [{stat, name}], legendaries: [{id, name}]}` — what can roll there as gold lines,
and the lootable legendaries for that slot and hero, both sorted by name.

#### `legendary_info`
`id` *n* → `{id, name, slot, hero, lootable, description, values, types}`. `hero`: -1 any, 0-2 a hero, 99 legacy,
999 reaper. `description` is the game's text: `@` = a value (from `values`), `#` = a line break.

#### `stat_list`
→ every stat input of the game (176): `[{stat, name, unit, item_affix}]`. `unit`: `flat`, `percent`,
`multiplier_percent` (the `*_global_mult` multipliers) or `seconds`; `item_affix`: one of the 92 stats items can roll.
`stat_info` returns the same `unit` / `item_affix` for one stat.

#### `hero_stats`
→ the hero's current final stats, as the game computed them from items, skills, buffs and everything else:
`{hero, instance, life, max_life, mana, max_mana, shield, max_shield, stats: {crit_chance: 51.3, cooldown_reduction:
45.6, attack_speed: ..., the_max_health: ..., ...}}` (`stats` = the hero's `final_*` variables, without the prefix;
81 in 1.0.2c). Fails when there is no hero (e.g. in the main menu).

#### `skill_info`
`hero` *n* [current], `id` *n* → `{hero, id, name}`.

### Other

#### `batch`
`cmds` *a* → `[{ok, result | error}, ...]`, one per command, all in the same frame (reads are consistent). Lua mods
cannot nest batches.

#### `perf`
`enable` *b*, `reset` *b* → SlormAPI's own cost per frame since the last reset: `{enabled, frames,
callbacks_us_per_frame, ticks_us_per_frame, modspage_us_per_frame, events_us_per_frame, lua_us_per_frame,
detours_us_per_frame, object_events_per_frame, hooked_calls_per_frame}`. Off unless enabled. Not for Lua.

#### `runner` / `routines` / `routine_index`
Reverse-engineering helpers: RVAs of the runner's helper functions; `names` → RVAs of built-in functions; `names` →
their function-table indices. Not for Lua.

---

## Events

Delivered once per frame on the game thread, right after they happened. Script events install their hook only while
someone listens; watchers compare state each frame (or a few times a second).

| event | data | source |
|---|---|---|
| `enemy_killed` | `enemy` {$inst, $object}, `x`, `y`, `level`, `profile` (`trash` / `good` / `elite` / `boss`) | `scr_hero_stats_on_kill` |
| `boss_killed` | as `enemy_killed` | a kill with profile `boss` |
| `hero_hit` | `damage`, `attacker` {$inst, $object}, `life`, `max_life`, `shield`, `max_shield` (Light Shield after the hit) | `scr_hero_proc_when_hit` |
| `hit_dealt` | `target` {$inst, $object}, `skill` (id, or a name such as `"overdrive"`), `strike` (`normal` / `critical` / `ancestral`) | every hero hit (`scr_hero_proc_when_crit_brutal`) |
| `damage_dealt` | `target`, `damage` (the health it lost, after its shields) | `scr_damage_shield` (hits, procs and other damage to enemies) |
| `status_applied` | `status` (the game's key, e.g. `fire_burn`, `ice_chill`, `knight_bleed`, `reaper_enfeeble`), `name` ("Burn"), `target`, `duration` (-1 = until removed), `stacks` (after it) | `scr_buff_list_add` on an enemy |
| `status_removed` | `status`, `name`, `target`, `reason` (`ended`, or `target_gone` when the enemy died / despawned) | a status seen applied whose stacks are gone (checked 4×/s) |
| `cooldown_started` | `skill`, `name`, `hero`, `cooldown` (the game's cooldown value) | `scr_new_attack_state_start_cooldown` |
| `stats_refreshed` | `hero` (at most once per frame; often several times a second in combat) | the hero's `scr_stats_refresh` |
| `hero_died` | — (once per death) | `scr_hero_dead` |
| `item_picked_up` | `items` (what was picked up), `picked` (the script's result) | `scr_pick_up_item` |
| `skill_cast` | `skill` (id), `name`, `hero` | `scr_hero_proc_when_any_cast` |
| `run_started` / `run_ended` | — | `scr_run_start` / `scr_run_end` |
| `loot_dropped` | `loot` (the `obj_loot` stub), `item` ([item array](#items)), `info` (as `item_info`) | new `obj_loot` instances |
| `gold_changed` | `gold`, `delta` | `global.gold` |
| `level_up` | `level`, `previous`, `hero` (0 Knight, 1 Huntress, 2 Mage) | `global.level[hero]` |
| `item_equipped` | `slot` (key), `index` (0-10), `hero`, `item`, `previous`, `info` (also when a slot is emptied) | the hero's 11 slots, 4×/s |
| `room_changed` | `room`, `previous` (room names) | the current room |
| `menu_changed` | `menu`, `previous` (`obj_ui_next_gen.menu_show`) | the open menu |
| `setting_changed` | `mod`, `key`, `value` | `set_setting` / Options > Mods |
| `ui_clicked` | `owner`, `id`, `action` | a click on a [widget](#widgets-ui-kit) |

What already exists when you start listening is not reported (e.g. loot already on the ground).

---

## Draw programs

Used by `ui_add`, a hook's `after` and `overlay`. A program runs at draw / bake time, so its values are live:

```json
{"vars": [["bx", {"$op": "??", "args": [{"$op": "mba", "args": ["Filter Active", 2]}, 1537]}]],
 "when": {"$op": "==", "args": [{"$self": "menu_show"}, "inventory"]},
 "calls": [
   {"fn": "draw_sprite", "args": [{"$asset": "spr_menu_grey_button"}, 0, {"$v": "bx"}, 904]},
   {"fn": "scr_draw_text", "args": [{"$op": "+", "args": [{"$v": "bx"}, 84]}, 934, "Hello", 1, 1, "ft_standard_12", 1, 1, 0, 1, "white", "black", 1, false],
    "when": {"$global": "gold"}}]}
```

- `vars`: `[name, expression]`, evaluated in order. `when`: the whole program runs only if true. Each call: `fn`
  (an engine function, a game script, or a pseudo call below), `args` (expressions), optional `when`.
- `self` is the hooked call's / the event's instance (in menus: `obj_ui_next_gen`).

**Expressions**: JSON literals (with `$asset` / `$inst`), arrays of expressions, and

| expression | value |
|---|---|
| `{"$v": name}` | a program variable |
| `{"$self": var}` | a variable of self |
| `{"$global": var}` | a global variable |
| `{"$arg": i}` | argument i of the hooked call (e.g. a panel's x / y) |
| `{"$fn": name, "args": [...]}` | a call's result |
| `{"$op": op, "args": [...]}` | `+` (also joins strings) `-` `*` `/` `==` `!=` `<` `<=` `>` `>=` `and` `or` `not` `?` (cond, a, b) `??` (first defined) `def` (is defined) `in_rect` (px, py, x, y, w, h) `mba` (name, index: a field of the first `self.mba` entry with that name) |

**Pseudo calls** on `self.mba`, the game's list of clickable UI elements (each `[name, sprite, x, y, w, h, text,
script, argument, tooltip]`; the game draws hover frames, plays sounds and on a click calls `script(argument)`):

| call | does |
|---|---|
| `$mba_push [entry]` | adds an entry (replaces one with the same name and position) |
| `$mba_set [name, index, value]` | sets field `index` of the entries called `name` |
| `$mba_remove [index, value]` | removes the entries whose field `index` equals `value` |
| `$mba_remove_in [x1, y1, x2, y2]` | removes the entries placed inside that rectangle |
| `$set [var, value]` | sets a variable on self |
| `$button` `$radio` `$selector` `$text` | [widgets](#widgets-ui-kit) |

A program that fails (bad function, bad arguments) is dropped and its error is shown by `hooks` / `overlay`.

**Text**: the game's `scr_draw_text` takes fonts by **name**:
`[x, y, text, 1, 1, font, halign, valign, 0, 1, color, outline, 1, false]` — fonts such as `ft_standard_11`,
`ft_standard_12`, `ft_standard_14`, `ft_standard_16`, `ft_standard_13_b`, `ft_standard_22_b`, colors by name (`white`, `grey`, `black`, `item_highlight`, `pure`, ...), halign 0 left /
1 center / 2 right. Use `0, 1` for arguments 9-10: with `1, 0` the game defers the text and it may never be drawn.
`{text}` in the string with a 15th argument (a color name) marks words in that color.

---

## Widgets (UI kit)

The game's own controls in one call each, inside any draw program (usually `ui_add`). Clicks arrive as the event
`ui_clicked` `{owner, id, action}` (owner = the program's owner).

| widget | args | actions |
|---|---|---|
| `$button` | `id, x, y, text, sprite?, tooltip?` (sprite default: `spr_menu_thin_grey_button`; size = the sprite's) | `click` |
| `$radio` | `id, x, y, label, on, tooltip?` (the game's diamond switch) | `toggle` |
| `$selector` | `id, x, y, label, value, tooltip?` (label above, ◀ value ▶ box, as in the game's settings) | `prev`, `next` |
| `$text` | `x, y, text, font? [ft_standard_12], color? [white], halign? [0]` | — |

Widgets you stop drawing disappear (each run replaces your previous entries); the tooltip is the game's own. Change
state → call `ui_add` again with the new program (or `ui_refresh` if the program reads game variables).

---

## Lua reference

Lua 5.5, one state per file, run on the game thread. `slorm.request` plus helpers:

| function | does |
|---|---|
| `slorm.mod{name, version, author, description}` | your row on the Mods page (call it first) |
| `slorm.request{cmd = ..., ...}` | any command above → its `result` (an error raises a Lua error) |
| `slorm.on(event, fn)` → id / `slorm.off(id)` | events: `fn(data, event)` |
| `slorm.every(seconds, fn)` / `slorm.after(seconds, fn)` → id / `slorm.cancel(id)` | timers (at most once per frame and every 10 ms; `every(0, fn)` = about every frame) |
| `slorm.get(path, root?, depth? [2])` / `slorm.set(path, value, root?)` | variables |
| `slorm.call(fn, ...)` / `slorm.call_on(self, fn, ...)` | scripts and engine functions (result depth 2) |
| `slorm.instances(object, vars?)` | instances |
| `slorm.inst(id)` / `slorm.asset(name)` | `{"$inst": id}` / `{"$asset": name}` |
| `slorm.null` | JSON null where Lua has no nil, e.g. "any" in patterns: `{ slorm.null, "abc*" }` |
| `slorm.item_info(item)` / `slorm.slot_info(slot, hero?)` / `slorm.stat_info(stat, slot?, roll?, level?)` | game knowledge |
| `slorm.hero_stats()` / `slorm.stat_list()` | the hero's final stats / every stat with its unit |
| `slorm.settings{ {key=..., ...}, ... }` → values / `slorm.setting(key)` | your settings (kept up to date) |
| `slorm.save_data(table)` / `slorm.load_data()` | your data file |
| `slorm.log_file(...)` | a line in your log file |
| `slorm.ui(menu, calls, when?)` / `slorm.ui_remove(menu)` | your program in a menu |
| `slorm.text(x, y, text, font?, color?, halign?)` | a `$text` call |
| `slorm.button(id, x, y, text, tooltip?, sprite?)` / `slorm.radio(id, x, y, label, on, tooltip?)` / `slorm.selector(id, x, y, label, value, tooltip?)` | widget calls |
| `slorm.on_click(function(id, action) ... end)` | clicks on your widgets |
| `slorm.clock()` | seconds since the game started |
| `slorm.log(...)` / `print(...)` | a line in Aurie's log |
| `slorm.display_name()` | your mod's name (as on the Mods page) |
| `slorm.name` / `slorm.owner` / `slorm.version` | the file name / your owner (`lua:<file name>`) / SlormAPI's version |

Tables become JSON: keys 1..n only (or empty) → an array, else an object. JSON `null` inside arrays becomes `nil` in
Lua (a hole) — use your own markers ("" / -1) in data you save.

**The sandbox** (a Lua mod is a text file, but it still must not be able to harm the PC):
- Libraries: base, coroutine, math, string, table, utf8. No io, os, package, debug; `load` takes text only;
  `string.dump`, `dofile`, `loadfile` are gone.
- Engine functions that reach outside the game are refused in `call` and in draw programs: files (`file_*`,
  `directory_*`, `ini_*`, `buffer_save` / `buffer_load`, `screen_save`, ...), network (`http_*`, `network_*`, `url_*`,
  `steam_*`), the OS (`execute_*`, `environment_*`, `os_*`, `external_*`, `shell_*`), dialogs, the clipboard, ...
- Commands not available: `mod_enable`, `mods_page`, `perf`, `overlay`, `runner`, `routines`, `routine_index`;
  nested batches. `set_setting` only for your own mod. Your `owner` is always `lua:<file name>` and `unhook`'s `all`
  is ignored, so a Lua mod only ever touches its own hooks, UI, events and data.
- Each call may run 200 ms and each mod may use 64 MB; past that it stops with an error.
- Errors are shown on the Mods page ("Lua error: ...") and in Aurie's log; fix the file and save to reload.

---

## Items

Items (drops' `obj_loot.ar`, inventory entries, `loot_dropped.item`, save files) are arrays of arrays:

```json
[["epic", "glove", 100, "equipment", ...],
 ["E", "crit_chance_percent", "40", "0", "0"],
 ["M", "the_max_health_percent", "61", "0", "0"],
 ["L", 52, 88],
 ["MA", 3, 1]]
```

- **Header** `[0]`: rarity (`normal` `magic` `rare` `epic` `legendary` `nether`), slot (`helm` `body` `shoulder`
  `bracer` `glove` `boot` `ring` `amulet` `belt` `cape`; `weapon`), item level, kind (`equipment`, `weapon` /
  `reaper`, or a material / other pickup). For weapons / reapers field 4 is the reaper's id. Leave the other fields
  as they are.
- **Stat lines** `[tier, stat, roll, pure, ...]`: tier `N` / `M` / `R` / `E` (epic = **gold**, the hard ones to
  reforge) / `D`; the stat's id (e.g. `crit_chance_percent`, `the_max_health_add`); the roll (1-40 for gold lines;
  numbers may come as strings); `pure` > 0 = a pure stat.
- **Legendary** `["L", id, roll]` (roll 75-100). **Extras**: `["MA", skill id, n]` +n skill mastery,
  `["AT", attribute 0-7, n]` +n attribute points (Toughness, Savagery, Fury, Determination, Zeal, Willpower,
  Dexterity, Bravery), `["RP", reapersmith, n]` +n affinity.

`item_info` turns any of these into names and real values; `stat_info` with `roll` and `level` gives one value.

---

## Game cheat sheet

Version 1.0.2c. Names are the game's own.

**Globals** (`get` / `set` without a root):

| | |
|---|---|
| `current_hero` | 0 Knight, 1 Huntress, 2 Mage |
| `gold`, `level[hero]`, `version` | gold, the heroes' levels, the game version |
| `inventory[hero][0..10]` | equipped items: helm, body, shoulder, bracer, glove, boot, ring, ring, amulet, belt, cape |
| `loot_filter[hero]` | the Filter-o-tron: `[enabled, [5 rarity rows × 7], convert (1 fragments, 2 goldus)]` |
| `skill_equipped[hero][skill]` | 2-4 = the skill is equipped |

**Objects**:

| | |
|---|---|
| `obj_ui_next_gen` | all menus: `menu_show` (`none`, `main`, `inventory`, `settings`, ...), `menu_citizen` (`blacksmith` = Friedrich's panel), `mba` (clickable elements), `rebake_menu` (set true to redraw), `main_panel` (0 Welcome, 1 New Game, 2 Load Game), `settings_panel` (0 Gameplay, 1 Settings, 2 Keybinds, 3 Codex) |
| `obj_parent_hero` | the hero: `actual_state` (`"dead"`), `the_health`, `final_the_max_health`, `x`, `y` |
| `obj_parent_enemy_NL` | enemies: `enemy_level`, `the_health`, `final_the_max_health` |
| `obj_loot` | a drop: `ar` (the item), `loot_name` (its label; set `baked` false after changing it), `visible` |
| `obj_gold_coin` | gold: `value`; set `pick_up_animation` true to make it fly to the hero |

**Scripts** (call with the right `self`):

| | |
|---|---|
| `scr_ui_menus_<menu>` | a menu's bake (hook it, or use `ui_add`) |
| `scr_draw_text` | text with the game's fonts ([arguments](#draw-programs)) |
| `scr_create_loot(level, profile)` | self = hero: drops loot as if an enemy of that profile died there |
| `scr_pick_up_item(item, true, false, true)` | self = hero: puts an item into the inventory (false when full) |
| `scr_blacksmith_filter_mla(item)` | self = hero: the Filter-o-tron's conversion of one item with the current `loot_filter` (0 = converted) |
| `scr_save_load_save(slot)` / `scr_run_start()` | self = `obj_ui_next_gen`: load a save / start a run |
| `scr_ui_settings_change_panel(n)` | switch the Options tab |

Rooms: `rm_procedural` while in a run. GUI coordinates are 1920×1080.

---

## Rules of thumb

- Use your mod's name as `owner` everywhere, and register it with `requires` and `game`.
- Do work from events instead of every frame; when you poll, batch the reads (`batch`) and do it a few times a second.
- Never keep instance ids across rooms (instances are recreated); look them up again (`instances`).
- Hooks run inside the game's own code: keep `after` programs small, and don't call scripts with side effects from
  them unless you mean to.
- An error in a game script you call can leave the game in a bad state: try new calls in a test game.
- The game's `mba` list only changes when a menu is baked: after changing what your program draws, `ui_add` again or
  `ui_refresh`.
- Test with the dev server: everything a mod does can be tried live from a Python prompt first.
