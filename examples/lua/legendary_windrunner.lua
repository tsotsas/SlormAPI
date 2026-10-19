-- Windrunner: a custom legendary boots for The Slormancer, as a Lua mod for SlormAPI.
-- Put this file into <The Slormancer>\mods\lua\. Windrunner then drops like the game's own legendaries.
--
--   +40% Movement Speed (multiplier), scaled by the item's legendary roll like the game's legendaries
--   critical strikes: +25% Attack Speed for 3 seconds (a buff in the game's buff bar)
--   kills restore 3% of your Maximum Life
--   every hit: counted in this mod (see on_hit) - your own Lua logic goes there
--
-- Keep the id ("windrunner") the same in later versions: saved items find their legendary by it.

slorm.mod{ name = "Windrunner", version = "1.0", author = "thothas", description = "A custom legendary boots" }

local hits = 0

slorm.legendary{
  id = "windrunner",
  name = "Windrunner",
  slot = "boot",                                   -- helm body shoulder bracer glove boot ring amulet belt cape
  description = "+@% Movement Speed. Critical strikes grant +@% Attack Speed for 3 seconds. Kills restore @% of your Maximum Life.",
  stats = { the_speed_global_mult = 40 },          -- applied by the game itself while it is worn
  values = { 25, 3 },                              -- more numbers for the description's @
  icon = 84,                                       -- the icon of one of the game's legendaries (84: Sandals of Speed)
  effects = {
    { trigger = "crit", cooldown = 0.5,
      buff = { stats = { attack_speed_percent = 25 }, duration = 3, name = "Windrunner Haste", icon = "slow" } },
    { trigger = "kill", heal = { life_percent = 3 } },
  },
  on_hit = function(e, roll)                      -- only while Windrunner is worn; roll = its 75-100 legendary roll
    hits = hits + 1
  end,
}
