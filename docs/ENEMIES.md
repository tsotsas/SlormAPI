# SlormAPI 2.1.0 — custom enemies

A mod can add **enemies** of its own. Each one is built on one of the game's enemies (its **base**): it has the base's
body, AI and attacks, with its own name, stats, size, colour, art and loot, and, if you want, its own attack written in
Lua. Runs place it among the game's enemies, or your code places one where it wants. The commands and events are also
listed in [API.md](API.md#custom-enemies).

- [A first enemy](#a-first-enemy)
- [Options](#options)
- [Bases](#bases)
- [Where it appears](#where-it-appears)
- [As the Battlefield's boss](#as-the-battlefields-boss)
- [Its own attack](#its-own-attack)
- [Placing one yourself](#placing-one-yourself)
- [Commands and events](#commands-and-events)
- [Limits](#limits)

The example is [examples/lua/enemies.lua](../examples/lua/enemies.lua): a bigger, tougher Soldier that can turn up
anywhere, and an enemy on the Archer's body with its own attack.

---

## A first enemy

```lua
slorm.mod{ name = "Bone Brute", version = "1.0.0", author = "you" }
local S = slorm

local brute = S.enemy{
  id = "bone_brute", base = "Soldier",
  name = { en = "Bone Brute", fr = "Brute d'Os" },
  stats = { health = 2.5, damage = 1.5, speed = 0.8 },   -- x the Soldier's
  scale = 1.35, color = "#ff8060",
  loot = "tactical",
  spawn = { chance = 1 / 12, replaces = "any" },         -- 1 in 12 of the enemies a run places
  on_death = function(e) S.log("a Bone Brute fell") end,
}
```

Saved in `mods/lua/` (or as a pack), it is live at once: the next run you start has Bone Brutes in it.

## Options

| option | |
|---|---|
| `id` | required; its key is `"<owner>:<id>"` (from Lua: `"lua:<file or pack>:<id>"`) |
| `base` | required: a game enemy — its name in the game's enemy table (`"Soldier"`, `"Zombie Peasant"`), its object (`"obj_soldier"`) or its number in that table. See [Bases](#bases) |
| `name` | a text, or one per language (`{ en = ..., fr = ... }`); `enemy_info` and `enemies` give it |
| `stats` | multipliers of its base's stats at the same level (1 = the base's): `health`, `damage` (basic and elemental), `speed`, `res_phy`, `res_mag`, `cooldown` (above 1 = slower attacks), `attack_range`, `precision`, `dodge`, `crit_chance`, `knockback_weight`, `knockback_force` |
| `scale` | its size (1 = the base's; up to 10) |
| `color` | a tint: `"#rrggbb"` or `{r, g, b}` |
| `art` | `{ [game sprite] = its sprite }`: wherever its base draws that sprite (its walk, cast and precast poses, its death, also as an elite), its own. Its sprite: a file of your mod's folder (`"brute_walk.png"`, framed like the game sprite it replaces, or `{ file, frames, xorig, yorig }`), a `slorm.sprite` number, or a game sprite's name. Some enemies have more than one look (a Soldier walks as `spr_soldier_walk` or `spr_soldier_walk_2`): give each |
| `loot` | its loot profile, as the game's enemies have: `trash`, `low`, `good`, `high`, `tactical`, `elite`, `boss` (another word is refused) |
| `spawn` | `{ chance = 0-1, replaces = "any" }` or `replaces = { game enemies }`: see [Where it appears](#where-it-appears); `boss = 0-1`: its chance to be a boss floor's boss, see [As the Battlefield's boss](#as-the-battlefields-boss). Without `spawn`, it appears only where you place it |
| `attack = function(e)` | Lua: its own attack instead of its base's. See [Its own attack](#its-own-attack) |
| `on_spawn = function(e)` / `on_death = function(e)` | Lua: one of its enemies placed (`enemy_spawned`) / killed by the hero (`enemy_killed`) |

`slorm.enemy` returns `r` with `r.id` (its key) and `r.spawn(x, y, level?)`.

Its level is the run's (or the one you give `spawn`): the game computes its base's stats for that level, then its
multipliers apply.

## Bases

Any of the game's enemies, by the name in its enemy table: Soldier, Archer, Colossus, Swordman, Wizard, Corrupted
Pudding, Necromancer, Rogue, Zombie Peasant, Burning Pudding, Hex Shroud, Plague Doctor, Bat, Spider, Soul, Prisoner,
Executioner, Student, Witch, Tutor, Imp, Unstable Imp, Shepherd, Pudding Hermit, Thief, Domestic, Jester, Paladin,
Spectrum, Obelisk, Scarecrow. The friendly ones (the escort, the friendly Colossus, Archer and Wizard) and the bosses
are not bases.

Underneath it stays its base: the same object (`$object` is the base's), the same AI and moves. Code that asks "is
this a Soldier?" sees a Soldier; `enemy_info` or the variable `slormapi_enemy` tells which custom enemy it is.

## Where it appears

With `spawn`, it replaces some of the enemies a **Battlefield run** (a run on the game's generated maps) places: its
areas' squads and doors, its wanderers, and the enemies of the map streamed in around the camera.
Each enemy the run places is tried against the rules in the order mods registered them; the first that hits replaces
it (once). `replaces = "any"` takes any enemy; a list takes only those (`{ "Soldier", "Rogue" }`). `chance = 1` and
`"any"`: every enemy of the run is yours.

A story mission's or a preset map's enemies and friendly ones stay the game's; a boss floor's boss follows its own
rule (`spawn.boss`, below). When the mod goes (a Lua file
removed or switched off), new runs have none; the ones alive fight on as their base.

## As the Battlefield's boss

A Battlefield run counts its floors down to a **boss floor** ("Defeat the Boss"). Its boss is one of the game's enemies
made a champion: elite, bigger, much more health, its own adds around it, elite loot, and a name on the game's elite
bar. With `spawn = { boss = 0-1 }` your enemy has that chance to be it:

```lua
S.enemy{ id = "lord_of_bones", base = "Colossus", name = "Lord of Bones", stats = { health = 1.5 }, color = "#a0ffa0",
  spawn = { boss = 0.5 } }                               -- half of the boss floors: the Lord of Bones
```

The game then makes it the floor's champion exactly as it would its own boss (its stats are yours times the game's
champion scaling), and its `name` becomes the boss's name. `enemy_spawned` has `boss = true`. When the hero kills it,
it drops the boss's loot and the floor goes on as after the game's boss; `enemy_killed` carries its `id`. Several mods with boss rules: tried in the
order they registered; the first that hits is the boss. A Warlord run's boss stays the game's.

`boss` and `chance` are separate: an enemy can be only a boss (`spawn = { boss = 1 }`), only among a run's enemies, or
both.

## Its own attack

With `attack`, its base's attack does not run for it. When its base would strike (its attack's moment, after its
precast animation; a base that attacks over several frames, like the Soul's beam: once per attack), your function
gets `e`:

| field | |
|---|---|
| `enemy` | the enemy `{$inst}` |
| `id` | its key |
| `x`, `y`, `facing` | where it is, which way it faces (1 / -1) |
| `target`, `tx`, `ty`, `dir` | its target (the hero) `{$inst}`, where it is, the direction to it in degrees |
| `element`, `level` | the element its base would use (`"none"`, ...), its level |

The pieces hit with the enemy's own damage (its multiplied stats), on the hero's side ("bad"):

| piece | |
|---|---|
| `slorm.enemy_projectile(e, o)` | `o.n` projectiles (1) toward its target (or `o.dir` degrees), fanned over `o.spread` (30); `o.speed` (6), `o.sprite` (`"spr_archer_arrow"`), `o.sound` (`"bow_fire"`, a game sound name or `slorm.sound`'s), `o.element` |
| `slorm.enemy_area(e, o)` | a hitbox: `o.mask` (`"spr_aoe_210"`) scaled by `o.scale` (0.5), in front of it toward its target, or at `o.x`, `o.y`; `o.impact` (`"spr_impact_melee_base"`), `o.sound` (`"small_sword_impact"`), `o.element`, `o.dir` |

`slorm.anim(x, y, sprite, o)` shows an animation (a warning mark), and `slorm.after(seconds, fn)` a later piece. Once
the enemy is gone (killed meanwhile) a piece does nothing and returns nil.

```lua
local turns = {}
S.enemy{
  id = "hex_archer", base = "Archer", name = "Hex Archer", color = "#b070ff",
  spawn = { chance = 0.25, replaces = { "Archer" } },
  attack = function(e)
    local me = e.enemy["$inst"]
    turns[me] = (turns[me] or 0) + 1
    if turns[me] % 2 == 1 or not e.tx then
      S.enemy_projectile(e, { n = 5, spread = 40 })        -- a fan of arrows
    else
      local x, y = e.tx, e.ty                              -- a mark where the hero is, a blast 0.6 s later
      S.anim(x, y, "spr_aoe_360", { speed = 0.5, scale = 0.6 })
      S.after(0.6, function() S.enemy_area(e, { x = x, y = y, mask = "spr_aoe_360", scale = 0.6 }) end)
    end
  end,
}
```

## Placing one yourself

`r.spawn(x, y, level?)` (or the command `spawn_enemy {enemy, x, y, level, aggro}`) places one of your enemies now,
through the game's own `instance_create`, so it is made as the run's enemies are. It goes for the hero at once
(`aggro`, true by default). `level` (1-1000) sets its level; without it, the run's. Returns its `{$inst}`.

Enemies do not fight the hero inside the run's starting circle: place them away from it.

## Commands and events

For C++ mods and tools (Lua: `slorm.enemy` sends `register_enemy`):

| command | |
|---|---|
| `register_enemy` | `owner`, `id`, `base`, `name`, `stats`, `scale`, `color`, `art` (`{game sprite name: sprite number}`), `loot`, `spawn`, `attack` *b* (its attack comes as `enemy_attack`) → `{id: key, base: its object}`. Registering the same id again replaces it |
| `unregister_enemy` | `owner`, `id` (none: all of the owner's) → `true` |
| `spawn_enemy` | `enemy` (key), `x`, `y`, `level`, `aggro` *b* [true] → `{$inst}` |
| `enemy_info` | `inst` → `{id, name}` of a custom enemy, `null` for a game one |
| `enemies` | → `[{id, owner, name, base, chance, boss}]` |

| event | data |
|---|---|
| `enemy_spawned` | `enemy` {$inst, $object}, `id`, `x`, `y`, `level`, `boss` (true for a boss floor's boss) — one of yours placed (by a run or `spawn_enemy`) |
| `enemy_attack` | as `e` above — one of yours with its own attack strikes |
| `enemy_killed` / `boss_killed` | as before, plus `id` when it was a custom enemy |

## Limits

- **Bosses**: a boss floor's boss, yes (above); a Warlord's, the story's bosses (Alarin, Palstrang, Adam, Agad, the
  Siege Leader, the Keeper) and their fights, no - they are not bases either.
- **Names**: the game shows no names for its common enemies, so neither for yours (`enemy_info` has it). Elites get
  the game's random elite names; as a boss floor's boss, its own.
- **Experience** follows its base and level.
- **Story missions and preset maps** keep their own enemies (see [Where it appears](#where-it-appears)).
- A custom enemy's look on screen comes from its base's sprites, its `art`, `scale` and `color`; the game's shaders
  (elite glow, hit flash) apply as to its base.
