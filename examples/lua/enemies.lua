-- Two custom enemies (SlormAPI 2.1.0+, the guide: docs/ENEMIES.md). Copy this file into <The Slormancer>\mods\lua\ and
-- start a Battlefield run.
--   * the Bone Brute: the Skeleton Soldier, bigger and tinted, tougher, with better loot; any enemy a run places
--     can be one (1 in 12), and a quarter of the Battlefield's boss floors have one as their boss;
--   * the Hex Archer: on the Skeleton Archer's body, with its own attack - a fan of arrows, then a blast where the hero
--     stood, in turn; a quarter of a run's archers are one.
slorm.mod{ name = "Enemies Example", version = "1.0.0", author = "SlormAPI", description = "Two custom enemies" }
local S = slorm

S.enemy{
  id = "bone_brute", base = "Soldier",
  name = { en = "Bone Brute", fr = "Brute d'Os", de = "Knochenbrecher" },
  stats = { health = 2.5, damage = 1.5, speed = 0.8 },   -- x the Soldier's at the same level
  scale = 1.35, color = "#ff8060",                       -- bigger, tinted
  loot = "tactical",                                     -- drops as the game's "tactical" enemies do
  spawn = { chance = 1 / 12, replaces = "any", boss = 0.25 },
  on_death = function(e) S.log("a Bone Brute fell (level " .. tostring(e.level) .. ")") end,
}

local turns = {}
S.enemy{
  id = "hex_archer", base = "Archer",
  name = "Hex Archer",
  stats = { health = 1.5 },
  color = "#b070ff",
  spawn = { chance = 0.25, replaces = { "Archer" } },
  attack = function(e)                                   -- instead of the Archer's shot, each time it attacks
    local me = e.enemy["$inst"]
    turns[me] = (turns[me] or 0) + 1
    if turns[me] % 2 == 1 or not e.tx then
      S.enemy_projectile(e, { n = 5, spread = 40, speed = 5 })
    else
      local x, y = e.tx, e.ty                            -- where the hero is now: a mark, then the blast
      S.anim(x, y, "spr_aoe_360", { speed = 0.5, scale = 0.6 })
      S.after(0.6, function()
        S.enemy_area(e, { x = x, y = y, mask = "spr_aoe_360", scale = 0.6 })   -- (nothing once the archer is gone)
      end)
    end
  end,
  on_death = function(e) turns[e.enemy["$inst"]] = nil end,
}
