-- Paladin: a small example class for SlormAPI 2.0.0+ (docs/CLASSES.md has every option).
-- Copy it into <The Slormancer>\mods\lua\. In the hub the Paladin stands right of the Knight's spot: talk to it and
-- say Yes to play it. It fights with the Knight's skills (its base kit) and has its own level, gear, skill ranks and
-- save data, its own line in the hub, one skill renamed, an Ancestral Legacy and an achievement of its own.
-- For a class with skills of its own, its own tree, mechanic, reapers and story, see examples/evil_knight.

slorm.mod{
  name = "Paladin",
  version = "1.0",
  author = "thothas",
  description = "An example class: the Knight's skills, its own name, progress, legacy and achievement",
}
local S = slorm

local C = S.class{
  id = "paladin", name = "Paladin", title = "The Paladin",
  base = 0,                                            -- 0 the Knight, 1 the Huntress, 2 the Mage
  talk = { "The light needs a sword.", "Shall I take over?" },   -- its line in the hub (pages)
  talk_as = "self",                                    -- said by the Paladin itself
  skills = { [3] = { name = "Holy Swing" } },          -- row 3 is the Knight's first active skill
  -- its own legacy in the tree's first realm (the game's legacy there is hidden while it is played)
  legacies = {
    { id = "devotion", realm = 0, name = "Devotion", stat = "the_max_health_percent", value = 0, per_rank = 2,
      value_type = "%", max = 5, description = "You have +@ £." },   -- @ = its value now, £ = the stat's name
  },
  -- after the Knight's 23 (counted on its own data), one of its own: its steps, "$" = the next one
  achievements = {
    { id = "smite", name = "Smiter", description = "Kill $ Enemies as the Paladin.", icon = 0, steps = { 10, 100, 1000 } },
  },
}

S.on("enemy_killed", function()
  if S.get("current_hero") == C.hero then C.achievement_add("smite") end
end)
print("Paladin is hero " .. C.hero .. "; its legacy Devotion is row " .. tostring(C.legacy.devotion))
