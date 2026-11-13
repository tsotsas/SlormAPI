# SlormAPI 2.0.0 — custom classes and hero content

A mod can add a **class**: a playable hero of its own next to the Knight, the Huntress and the Mage. It can also give
the **game's three heroes** content of their own (art, skins, reapers, sounds, achievements, missions, legacies).
This page covers both. The commands and events are listed in [API.md](API.md); this page explains how to use them.

- [What a class is](#what-a-class-is)
- [A first class](#a-first-class)
- [Options](#options) — [identity](#identity) · [kit and skills](#kit-and-skills) · [entries](#entries-the-upgrade-tree) ·
  [texts](#texts-class-mechanic-and-specialization-rows) · [art](#art) · [skins](#skins) · [sounds](#sounds) ·
  [reapers](#reapers) · [legacies](#legacies-the-ancestral-legacy-tree) · [achievements](#achievements) ·
  [missions](#story-missions) · [area](#a-story-area)
- [Skill code](#skill-code): `class_cast`, `class_hit`, the attack pieces, a cast's late effects
- [The game's heroes](#the-games-heroes-register_hero): `register_hero` / `slorm.hero`
- [A blank base](#a-blank-base-base--none)
- [Lua: what `slorm.class` returns](#lua-what-slormclass-returns)
- [Commands and events](#commands-and-events)
- [Images and sounds from files](#images-and-sounds-from-files)
- [Texts and languages](#texts-and-languages)
- [Limits](#limits)

The full example is [examples/evil_knight](../examples/evil_knight): a class on the Knight with its own skills and code,
upgrade tree, specializations, mechanic, legacy tree, achievements, reapers, legendaries, skins, story missions and story
area. A small one is [examples/lua/paladin.lua](../examples/lua/paladin.lua).

---

## What a class is

A class is a real hero of its own (hero number 3, 4, ...), built on a **base hero**: the Knight (0), the Huntress (1),
the Mage (2), or [nothing](#a-blank-base-base--none). The base hero gives its body, its skill table's layout, its look
and, unless the class has its own kit, its skills. Everything a hero has per save is the class's own: level, xp,
gear, reapers, skill ranks, attributes, legacies, runes, statistics.

In the game:
- **The hub**: the class's citizen stands next to its base hero's spot. Talk to it and say Yes to play the class
  (its line: `talk`). While the class is played, its citizen is gone and its base hero's is back.
- **A new game**: the intro's hero picker has a list on the hero's name with the three heroes and every class.
- **The Profile**: each hero card and each hero achievement category has a list to show any hero or class.
- **The Character menu**: a list on the name bar picks one of its [skins](#skins).
- **Saves**: the game's own save stays a normal three-hero save. The classes' data is kept per save slot in
  `mods/aurie/SlormAPI/data/classes.json`. A save played as a class loads as that class again. A save loaded without
  the class's mod plays its base hero; the class's data stays in that file for when the mod is back.
- **Steam**: a class never unlocks Steam achievements; the game's own achievement checks wait while a class is played.

Classes come from mods: `slorm.class{...}` in Lua, or the `register_class` command from C++ or a tool. A class's
**key** is `"<owner>:<id>"` (from Lua: `"lua:<file>:<id>"`). Its hero number depends on the order mods load, so keep
the key (or read `C.hero`) rather than a fixed number.

## A first class

```lua
slorm.mod{ name = "Paladin", version = "1.0", author = "you" }
local S = slorm

local C = S.class{
  id = "paladin", name = "Paladin", title = "The Paladin",
  base = 0,                                           -- the Knight: its skills, its body, its look
  talk = { "The light needs a sword.", "Shall I take over?" }, talk_as = "self",
  skills = { [3] = { name = "Holy Swing" } },         -- one of the Knight's skills renamed
  legacies = {
    { id = "devotion", realm = 0, name = "Devotion", stat = "the_max_health_percent", value = 0, per_rank = 2,
      value_type = "%", max = 5, description = "You have +@ £." },
  },
  achievements = {
    { id = "smite", name = "Smiter", description = "Kill $ Enemies as the Paladin.", icon = 0, steps = { 10, 100, 1000 } },
  },
}

S.on("enemy_killed", function()
  if S.get("current_hero") == C.hero then C.achievement_add("smite") end
end)
```

Put it in `mods/lua/` and it loads at once (and again each time you save it). The same file is
[examples/lua/paladin.lua](../examples/lua/paladin.lua). As a Lua pack, its `lua-pack.json` requires SlormAPI 2.0.0:
`"requires": { "slorm.slormapi": "2.0.0" }`.

**Art and sound files and Vortex.** A class's files sit in its pack's folder, next to its script. Vortex installs a
pack's `.lua` files only, so a pack with art or sounds is installed by hand: its zip unzipped into the game folder
(`mods/lua/<id>/`). A class without files of its own (like the Paladin) installs with Vortex as any Lua pack.

A class registers again when its mod loads again: same owner and id, same class, its data kept. Registering it again
with other options rebuilds what changed, also while it is played.

---

## Options

Every option of `register_class` / `slorm.class`. Only `id` is needed (and `owner` outside Lua). Lua takes the same
fields; where Lua gives a function or a file name, the command takes data (see each option).

### Identity

| option | value | default |
|---|---|---|
| `id` | its id within its mod | — |
| `name` | its name ("Evil Knight") | the id |
| `title` | its name in menus ("The Evil Knight"), shown where the game shows "The Mighty Knight" | the name |
| `base` | `0` Knight, `1` Huntress, `2` Mage, or `"none"` ([a blank base](#a-blank-base-base--none)) | `0` |
| `weapon` | its reapers' noun ("Axe"); in reaper names `$` is replaced by it | its base hero's |
| `attribute` | the attribute (0-7) a new hero of it starts with a point in | its base hero's |
| `talk` | its citizen's line in the hub: `"page/page"` or a list of pages | "Shall we let the \<name\> take over?" |
| `talk_as` | `"self"`: the line is said by the class itself, with its name and portrait | said by Bryan |
| `hub_x`, `hub_y` | its citizen's spot in the hub | next to its base hero's spot |

### Kit and skills

| option | value |
|---|---|
| `kit` | `"own"`: its skills are its own code (see [Skill code](#skill-code)); left out: its base hero's skills, as the game plays them |
| `skills` | `{ [row] = {fields} }`: its skill rows 0-10 (0-2 supports, 3-10 active skills) |
| `entries` | its upgrade tree ([entries](#entries-the-upgrade-tree)); with kit `"own"` |
| `texts` | the texts of its class, mechanic and specialization rows ([texts](#texts-class-mechanic-and-specialization-rows)); with kit `"own"` |

A skill row's fields: `name`, `description`, and with kit `"own"`:

| field | the game's column | for |
|---|---|---|
| `genre` | GENRE | `"melee"`, `"projectile"`, `"aoe"`, `"aoe,totem"`, `"movement"`, `"special"`, ... (reapers and legendaries trigger on genres) |
| `dmg_type` | DMG_TYPE | `"phy"` or `"mag"` |
| `stat` | DESC_VALUE | what its value is: `"physical_damage"`, `"elemental_damage"`, or a stat key |
| `value`, `per_level` | DESC_VALUE_BASE, DESC_VALUE_PER_LVL | its value and its growth per mastery level (`@` in the description) |
| `value_type` | DESC_VALUE_TYPE | `"%"` for a percentage |
| `cost`, `cost_type` | COST, COST_TYPE | mana cost; `"mana"`, `"life"`, ... |
| `cooldown` | COOLDOWN | in the game's units (the Knight's Mighty Swing: 40) |
| `icon` | — | its icon: a frame of its base hero's skill icons, `"x.png"` (Lua), or `{sprite = n, frame = k}` |
| `precast`, `cast_time`, `precast_speed`, `cast_speed`, `pose`, `knockback`, `auto_cd`, `extra` | the game's columns of those names | as in the game's rows |

Any other column of the game's skill table can be given by its name (`dat_cla` names, e.g. `UPGRADE_NUMBER`). A
value list `{ a, b }` becomes the table's `"a|b"`. With the base kit only `name`, `description` and the game's column
names apply.

In descriptions, `@` is the value and `£` the stat's name, as in the game's own texts; `#` breaks the line.

### Entries (the upgrade tree)

With kit `"own"`, `entries` is the list of its upgrades and passives. They come after its base hero's rows (its table
keeps its base hero's length: game code looks up some rows by number on any hero).

| field | value | default |
|---|---|---|
| `id` | its name in your code (`C.entry[id]`, `C.rank(id)`) | — |
| `skill` | the skill it belongs to (0-10) | 3 |
| `tier` | its tier (1-5 for active skills, 1-8 for supports) | 1 |
| `kind` | `"upgrade"` or `"passive"` | `"upgrade"` |
| `name`, `description` | its texts (`@` its value now, `£` its stat's name) | — |
| `stat`, `value`, `per_rank`, `value_type` | its value(s): one stat key and value each, or lists | — |
| `max` | its highest rank | the template's |
| `hero_stat` | `true`: its stat(s) apply to the hero while taken (a passive buff) | false |
| `effects` | `{ {row, field, index, percent, add} }`: per rank, change a cell of its skill rows (e.g. its skill's `value` +8 %, its `cooldown` -5 %) | — |

Its place in the tier follows the list's order; the mastery it needs is set the game's way. An entry with neither
`hero_stat` nor `effects` does nothing by itself: your skill code reads its rank (`C.rank(id)`).

### Texts (class, mechanic and specialization rows)

`texts = { class = {...}, mechanics = {...}, mechanic = {...}, spec = {...} }`: a list of `{name, description}` per
kind, in the order of its base hero's rows of that kind (the Knight has 2 `class`, 2 `mechanics`, 2 `mechanic` and 3
`spec` rows). A specialization's description is the game's format, `"flavour|tagline|A/B/C|{Row}/{Row}"`, where
`{Name}` is replaced by the number of the row named Name (its key mechanics).

### Art

`art = { [game sprite] = its sprite }`: wherever the game draws that sprite for the class, its own is drawn instead.
The game sprite is named (`"spr_npc_knight_idle"`); its sprite is a sprite number (from `sprite_file` / `slorm.sprite`)
or, in Lua, a file in the mod's folder (`"art/idle.png"`, cut and placed like the game sprite it replaces; or
`{file = "...", frames = n, xorig = x, yorig = y}`).

What it covers:
- the body: `spr_npc_<base>_idle`, `_walk`, `_pointing`, `_arm`, the armless poses, `spr_<base>_death`, the cast poses
  `spr_<base>_cast_pose_*` (on the hero and on its citizen);
- the skills menu and HUD: `spr_class_<base>`, `spr_supports_<base>`, `spr_skills_<base>_los`, `spr_skills_<base>`
  (skill icons), `spr_specialization_<base>`, its banners `spr_<base>_banner_*`, its mechanic's widget `spr_<base>_ui_*`;
- reapers: `spr_reapers_<swords|bows|staffs>` and `_corrupted` (in hand and in menus, primordial too);
- one frame: `spr_class_icon_small` / `_mid` (its class icon), `spr_ui_dialogue_citizen` (its portrait in dialogues);
- `spr_elements` (its legacy tree's icons), `spr_achievement_back` (its achievement category's icon);
- any other sprite: put in place of the game's while the class is played (effects, projectiles, the story map, ...).

Images are horizontal strips of equal frames. The Knight's body poses: idle 4 frames of 32×36 (origin 16,18), walk 8
of 32×40 (16,22), death 8 of 50×40 (26,20), cast poses 3 / 4 / 1 frames of 32×40 (16,22). The Huntress and the Mage
differ a little; a file given for a game sprite takes that sprite's frame size and origin.

Without `art` a class looks like its base hero.

### Skins

`skins = { {id, name, art} }`: looks the player picks in the Character menu (a list on the name bar). A skin's `art`
goes over the class's own (a game sprite given as itself: the game's). `skin_name` is the name of its own look
(default "Default"). The pick is kept in its save data.

### Sounds

`sounds = { [game sound name] = its sound | false | true }`: the hero's own sounds by the game's names
(`"large_sword"`, `"footstep_dirt"`, `"dodge"`, `"whirlwind"`, ...). Its sound plays instead (at the game's sound
effect volume), `false` plays nothing, `true` the game's (for a [blank base](#a-blank-base-base--none)). In Lua a
file name loads the file (`.ogg` or `.wav`). Only the hero's sounds change: its swings, steps, hurt and death, its
dodges, the effects it makes or that follow it. Enemies, loot and the UI keep theirs.

### Reapers

`reapers = { [ref] = {...} }` (a command takes a list `[{ref, ...}]`): the class's own version of a game reaper
(0-119) while it is played. Every hero owns its own copy of the 120 reapers, so the save keeps the game's numbers.

| field | value |
|---|---|
| `name`, `description`, `lore` | its texts; `$` in the name is its weapon noun |
| `stats` | `{ {stat, value, per_level, percent} }`: its stats (× its level where `per_level`) |
| `values` | `{ {value, percent} }`: the numbers of its effect's text (`@`) |
| `damage` | `{min, max, min_level, max_level}` |
| `multiplier`, `max_level`, `evolve_in` (a ref or `"none"`), `lootable` (false: not), `blacksmith` | as the game's |
| `art`, `primordial_art` | its frame in its reaper strips (a sprite of one frame; Lua: a file) |
| `code` | `"own"`: the game's scripted effect of that reaper is off for the class (your code does its effect) |
| `on_hit`, `on_kill`, `on_boss_kill`, `on_hero_hit`, `on_cast`, `on_status`, `on_damage`, `on_cooldown` | Lua: `fn(event)` while its effect is on (worn, or evolved from) |

New `stats` or `values` mean `code = "own"` (the game's code reads only its own values). Its stats apply as a passive
buff while it is worn. Lua: `C.reaper()` → `{ref, primordial, level}` worn, `C.reaper_active(ref)`.

### Legacies (the Ancestral Legacy tree)

`legacies = { {...} }`: its own Ancestral Legacies, in the game's tree. The tree has 140 realms in fixed places
(realm 0-139); each shows its first one or two legacies, by its size. A class's legacy in a realm hides the game's
there; `legacy_base = "none"` hides every game legacy (only its own are shown).

| field | value |
|---|---|
| `id` | its name in your code (`C.legacy[id]`, `C.legacy_rank(id)`) |
| `realm` | 0-139 |
| `kind` | `"stat"` (its stats apply by rank), `"upgrade"` (your code reads its rank), `"aura"` or `"active"` (on the skill bar) |
| `name`, `description` | its texts (`@` value, `£` stat name) |
| `stat`, `value`, `per_rank`, `value_type` | its stat(s): value + per_rank × rank; lists for several |
| `max` | its highest rank |
| `icon` | its icon (Lua: a file; default: its realm's game legacy's) |
| `cost`, `cost_type` | an aura: `"mana_lock"` / `"life_lock"`; an active: `"mana"`, `"life"`, `"life_percent"` |
| `cooldown`, `duration` | an active's, in seconds |
| `buff_icon` | the game buff whose icon its buff shows (`"neutral_aura"`, ...) |
| `use` | Lua: `fn(e)` when it is used (`e.on`: an aura toggled on or off) |

Auras toggle a buff with their stats and lock mana or life; actives pay their cost, give their stats for `duration`
and start their cooldown. Both raise `legacy_used`. Their ranks are the hero's, saved with it.

### Achievements

A class has its own achievement category in the Profile: its base hero's 23 achievements, counted on its own data
(`achievements_base = "none"` leaves them out), then its own.

| field | value |
|---|---|
| `id` | its id (not `base_*`) |
| `name`, `description` | its texts; `$` in the description is its next step |
| `steps` | up to 5 values, e.g. `{10, 100, 1000}` |
| `icon` | a frame of the game's achievement icons |
| `stat` | a player statistic to count (e.g. `"Total Enemies"`) instead of your own value |

`{base = <row>, name, description, icon}` renames one of its base hero's achievements. A new step shows the game's
"New Achievement Completed" notice and raises `class_achievement`. Lua: `C.achievement(id)` its value,
`C.achievement(id, value)`, `C.achievement_add(id, n)`; `C.achievement("base_<row>")` reads a mirrored one.

### Story missions

`missions = { {...} }`: its own missions in the game's story pages, played while it is played.

| field | value |
|---|---|
| `id` | its id (`C.mission[id]`, `C.mission_runs(id)`) |
| `name`, `description` | its texts |
| `map` | a game mission (0-52) whose map, act, objective, waves and boss it plays |
| `layout`, `breach_layout`, `act`, `objective` | instead of `map`: a map in the game's encoding, its act (0-4) and objective (`enemies`, `breach`, `breaches`, `waves`, `bonus_breakables`, `leave`, `boss`, `exit`, `lever`, `escort`, `named`, `emblem`) |
| `after` | one of its mission ids or a game mission number, or a list (any one done opens it); none: open at once |
| `level`, `cataclysms` | as the game's missions |
| `x`, `y`, `number` | its node on the act's page and its numeral (defaults: next to its map's node, after the act's last) |
| `intro`, `outro` | lines said in the map once it starts / back in the hub after it (Lua: as `slorm.say`'s lines) |
| `area`, `room` | `true`: in its [story area](#a-story-area); `room`: its room picture's frame there |
| `start`, `done` | Lua: `fn(e)` when it starts / when a run of it is done (`e.runs`) |

They raise `mission_started` and `mission_done`. The class hero's runs are saved with it.

### A story area

`area = {name, map, rooms, entrance, hub_x, hub_y, act}`: an act page of its own, entered from its own gate in the
hub while it is played. `map` is the page's map (592×556), `rooms` its rooms (a frame per mission), `entrance` the
gate's look, `act` the act page it takes the place of (0-4). Missions with `area = true` are on it.

---

## Skill code

With kit `"own"` the game casts its skills as usual (cost, cooldown, pose, timing), then hands the cast to the mod:

- **`class_cast`** `{class, skill, hero, x, y, precast_x, precast_y, facing, forced, element, dmg_type}` — at the cast
  moment (delivered with that frame's events). `x`, `y` is the hero, `precast_x`, `precast_y` the aim.
- **`class_hit`** `{class, skill, target, primary, hero}` — after each hit of its skills (the game's damage done:
  `primary` is the skill's main hit).

In Lua, `skills[row].cast(e)` and `skills[row].hit(e)` are those, for the class's own skills. A hit handler must not
make new hits of the same skill (each hit raises `class_hit` again): statuses, buffs and movement only.

The attack pieces (Lua) make the game's own attacks for a cast `e`; options in `o`, sprites by name or number:

| function | makes |
|---|---|
| `slorm.melee(e, o)` | a swing toward the aim: `o.anim`, `o.anim2`, `o.mask`, `o.scale`, `o.facing`, `o.anim_speed`, `o.sound` |
| `slorm.projectile(e, o)` | `o.n` projectiles toward the aim (or `o.dir` degrees): `o.sprite`, `o.speed`, `o.spread`, `o.sound` |
| `slorm.area(e, o)` | a hitbox at `o.x`, `o.y` (default the aim): `o.mask`, `o.scale`, `o.impact`, `o.sound` |
| `slorm.anim(x, y, sprite, o)` | an effect animation: `o.speed`, `o.scale`, `o.depth`, `o.follow` (an instance) |
| `slorm.knock(target, force, by)` | pushes `target` away from `by` (a negative force pulls it) |
| `slorm.debuff(target, key, seconds, value)` | a game status on an enemy: `"slow"`, `"daze"`, `"blind"`, `"stun"`, `"root"`, `"elemental_broken"`, ... |

Each hit's damage is the game's, from the skill's row (its value, damage type, genre). Buffs: `slorm.buff{...}`.
Anything else is the game's scripts and functions (`slorm.call`, `slorm.call_on`).

**A cast's late effects.** Timers (`slorm.after`, `slorm.every`) run on after the run is left. Once the cast's hero is
gone (its run left, the hero switched), a request that names it does nothing: `slorm.get` / `set` / `call_on` on it
give `nil` (the log says so once per mod), and the attack pieces do nothing. What does not name it — an animation at
a place, a sound, a buff without `target` (that is the hero now) — check first:

```lua
S.after(0.6, function()
  if not S.exists(e.hero) then return end      -- its run was left meanwhile
  S.area(e, { x = x, y = y })
  S.anim(x, y, "spr_burst_darkness")
end)
```

Over TCP such a request returns its error with `"hero_gone": true`.

**Ranks and stats.** `C.rank(id)` an entry's rank, `C.legacy_rank(id)` a legacy's. `slorm.on_stats(fn)` runs `fn(e)`
right after each refresh of the hero's stats, inside the game's (before any hit uses them): change its final stats
there (`e.hero`, `e.hero_number`). The Evil Knight swaps its basic and elemental damage there.

**Its mechanic on the HUD.** `C.meter(value, max, lit)` draws its base hero's mechanic widget (the Knight's gems, the
Huntress's faces, the Mage's emblems) where the game draws it, in battle: `value` of `max` lit, all lit while `lit`.
`C.meter(nil)` hides it. The game draws the mechanic widget for its three heroes only, so a class shows none until it
calls `C.meter`.

---

## The game's heroes: `register_hero`

The Knight, the Huntress and the Mage can get the same content options while they are played: `art`, `skins`,
`skin_name`, `reapers`, `sounds`, `achievements`, `missions`, `legacies`.

```lua
local K = slorm.hero{ hero = "knight",          -- or 0; "huntress" / 1, "mage" / 2
  skins = { { id = "gold", name = "Gilded", art = { spr_npc_knight_idle = "gold_idle.png" } } },
  legacies = { { id = "oath", realm = 3, name = "Oath", stat = "res_phy_percent", value = 0, per_rank = 2,
                 value_type = "%", max = 5, description = "You have +@ £." } },
}
```

- The result has the same functions as a class's (`K.legacy`, `K.legacy_rank`, `K.achievement`,
  `K.achievement_add`, `K.mission`, `K.mission_runs`, `K.reaper`, `K.reaper_active`) and its key `"hero:<n>"`.
- Its achievements come after the game's in that hero's category, never on Steam.
- Its legacies take the realms they name; the game's legacy there is hidden while that hero is played.
- **Several mods** can give the same hero content. Each mod's part is kept, and the hero's content is built from all of
  them in load order:
  - art, sounds and a reaper's fields: the later mod's (a log line says so);
  - skins, achievements, missions and legacies: added up. A skin, achievement or mission id given again replaces the
    earlier one (a log line); legacies are kept per mod, so two mods may use the same legacy id.
- `unregister_hero {owner[, hero]}` takes a mod's part out. A Lua mod's parts go when it is switched off or removed.
- Not for a game hero (refused): `name`, `title`, `base`, `kit`, `skills`, `entries`, `texts`, `weapon`,
  `attribute`, `talk`, `talk_as`, `area`, `legacy_base`, `achievements_base`, renaming its game achievements.

A game hero's legacy rows move when another mod's part changes. Use `K.legacy[id]` (asked again after each change),
not a stored number. The `legacy_used` / `mission_*` events carry `owner` and `index` (its place in its mod's list).

## A blank base: `base = "none"`

`register_class{base = "none", kit = "own", weapon = "...", ...}`: a class with nothing of a game hero's.

- **Its own skills.** `kit = "own"` and `weapon` are required.
- **Nothing inherited.** None of the Knight's texts in its rows, no mirrored achievements, none of the game's legacies,
  and starting attribute 0.
- **Invisible without art.** Every sprite it gives no `art` for is drawn as nothing; the log lists them. That covers
  the body, the skills menu, spec art, banners, reapers, skill icons, its widget, class icon, portrait and category icon.
- **Silent without sounds.** Its hero plays none of the game's sounds by name, only the ones its `sounds` gives (`true`
  for the game's). The log names each sound it skips, once.
- **The Knight is its frame.** It is used only for the game's plumbing: table length, hero switches and the body object.

## Lua: what `slorm.class` returns

`local C = slorm.class{...}` gives the register answer plus functions:

| field / function | |
|---|---|
| `C.hero`, `C.key`, `C.citizen`, `C.tag` | its hero number, key, citizen and speaker tag (`slorm.say{ {C.tag, "..."} }`) |
| `C.rows`, `C.effects`, `C.passives` | (kit `"own"`) its table's length, entry effects, passive entries |
| `C.legacy_first`, `C.mission_first`, `C.achievement_first` | first rows of its legacies / missions / achievements now |
| `C.entry[id]`, `C.rank(id)` | an entry's row / its rank now (0: not taken) |
| `C.meter(value, max, lit)` | its HUD widget |
| `C.legacy[id]`, `C.legacy_rank(id)` | a legacy's row / its rank (0 unless it is played) |
| `C.achievement(id[, value])`, `C.achievement_add(id[, n])` | its achievements' values |
| `C.mission[id]`, `C.mission_runs(id)` | a mission's number in the story table / its runs |
| `C.reaper()`, `C.reaper_active(ref)` | the reaper worn / whether a reaper's effect is on |

Its handlers (`skills[row].cast` / `.hit`, a legacy's `use`, a reaper's `on_*`, a mission's `start` / `done`) run
inside a protected call: an error is logged once and the handler keeps running next time.

Other Lua functions for classes: `slorm.say(lines, on_yes)` (a dialogue in the game's box; `on_yes`: its last page
asks Yes / No), `slorm.sprite(file, o)`, `slorm.sound(file)`, `slorm.play(sound, o)`, `slorm.exists(inst)`,
`slorm.on_stats(fn)`. See [API.md](API.md#lua-reference).

## Commands and events

Commands (fields in [API.md](API.md#classes-and-hero-content)): `register_class`, `register_hero`,
`unregister_hero`, `class_rank`, `legacy_rank`, `content_row`, `class_meter`, `class_achievement`, `class_reaper`,
`class_mission`, `say`, `sprite_file`, `sound_file`, `sound_play`, `switch_hero` (not for Lua).

Events: `class_cast`, `class_hit`, `legacy_used`, `class_achievement`, `mission_started`, `mission_done`,
`dialogue_choice` ([API.md](API.md#events)).

## Images and sounds from files

- `sprite_file {file, frames, xorig, yorig, like}` / `slorm.sprite(file, o)`: a PNG, GIF or JPG strip, its frames side
  by side (`frames`; else as many of `like`'s width as fit; else 1), its origin `xorig` / `yorig` (else `like`'s, else
  its middle). Made once per file version.
- `sound_file {file}` / `slorm.sound(file)`: `.ogg` (streamed) or `.wav` (PCM, 8 or 16 bit, mono or stereo).
- `sound_play {sound, gain, pitch, loop}` / `slorm.play(sound, o)`: plays it at the game's sound effect volume × gain.
- Paths are relative to the Lua mod's folder (for other callers, to `mods/`) and must stay inside `mods/`.

## Texts and languages

Every text (names, titles, descriptions, lines, achievements, reapers, missions) is one text or one per language:
`{ en = "Dread", fr = "Effroi", de = "Schrecken" }`. Missing languages show the English one. The game's languages:
`en`, `fr`, `ch` (`zh`), `tw`, `jp` (`ja`), `de`, `es`, `ru`, `kr` (`ko`), `it`, `pt`, `po` (`pl`), `tur` (`tr`).

Dialogue text (`talk`, `say`, mission lines) uses the game's format: pages split by `/`, `#` a line break,
`[tag]` a speaker by citizen tag (`[hero]` the hero played). Lua's `slorm.say` takes a text or a list of pages
(`"text"` or `{speaker, "text"}`).

## Limits

- **Base hero's mechanics.** Four of the 120 reapers name a base hero's mechanic or skill (Big Fury, Time and Space,
  the two Great Necromancy). With an own kit their effect has nothing to work on.
- **Game version.** Made and checked on The Slormancer 1.0.2c. A few parts depend on that build's code and check it
  first; on another build they stay off and the log says so (e.g. the reaper tables for heroes 5+ in menus).
- **Classes and saves.** The game's own save never holds a class, so other tools see a three-hero save.
