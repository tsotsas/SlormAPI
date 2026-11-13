-- Evil Knight: an example class for SlormAPI's custom classes (SlormAPI 2.0.0+, docs/CLASSES.md). In the hub it stands
-- right of the Knight's spot; talk to it and say Yes to play it. Its own kit (kit = "own"): 3 supports and 8 skills with
-- their own code, its own upgrade tree and specializations, and its own mechanic, Malice; its own Ancestral Legacy tree,
-- achievements, reapers, legendaries, skins, story missions and story area. It looks like the Knight in inverted
-- colours (its art: the Knight's sprites inverted, PNGs in art/), and its damage is elemental (its weapon's damage
-- counts as elemental damage: its basic and elemental damage swapped below).
local S = slorm
local function art(name) return S.sprite("art/" .. name .. ".png", { like = name }) end   -- (frames, origin: the game's)

local SWING, BLOW = art("spr_the_mighty_swing"), art("spr_the_mighty_swing_blow_1")
local BLADE, LANCE = art("spr_gladius"), art("spr_training_lance")

-- its look: each of these game sprites drawn as its inverted copy while it is played
-- ART:BEGIN (art/<name>.png per game sprite: the Knight's, inverted)
local ART_NAMES = {
  "spr_class_knight", "spr_knight_banner_damage", "spr_knight_banner_trigger", "spr_knight_cast_pose_dist",
  "spr_knight_cast_pose_melee", "spr_knight_cast_pose_move", "spr_knight_cast_pose_move_armed", "spr_knight_cast_pose_support",
  "spr_knight_cast_whirlwind_A", "spr_knight_cast_whirlwind_B", "spr_knight_death", "spr_npc_knight_arm",
  "spr_npc_knight_idle", "spr_npc_knight_idle_armless", "spr_npc_knight_pointing", "spr_npc_knight_walk",
  "spr_npc_knight_walk_armless", "spr_skills_knight_los", "spr_specialization_knight", "spr_supports_knight",
  "spr_reapers_swords", "spr_reapers_swords_corrupted", "spr_skills_knight", "spr_knight_ui_back",
  "spr_knight_ui_gem", "spr_knight_ui_ring", "spr_elements", "spr_class_icon_small",
  "spr_class_icon_mid", "spr_ui_dialogue_citizen", "spr_achievement_back",
}
-- ART:END
local LOOK, MEMORY = {}, {}
for _, name in ipairs(ART_NAMES) do
  LOOK[name] = art(name)
  MEMORY[name] = name                         -- "Knight's Memory": each one as the game's (the Knight it was)
end
local BURST, IMPACT = "spr_darkness_explosion", "spr_impact_melee_darkness"

-- texts: @ = the value (base + per rank x rank), £ = its stat's name; value_type "%" adds a %
local function pct(key) return key:find("percent") and "%" or "" end   -- flat stats (Life on Hit, ...) show no %
local function stat(id, skill, tier, name, key, per, max, desc)
  return { id = id, skill = skill, tier = tier, kind = "passive", name = name, stat = key, value = 0, per_rank = per,
    value_type = pct(key), max = max or 5, hero_stat = true, description = desc or "You have +@ £." }
end
local function more(id, skill, tier, name, per, desc)          -- +per% of the skill's damage per rank
  return { id = id, skill = skill, tier = tier, name = name, stat = "increased_damage", value = 0, per_rank = per,
    value_type = "%", max = 5, description = desc,
    effects = { { field = "value", percent = per }, { field = "per_level", percent = per } } }
end
local function faster(id, skill, tier, name, per, desc)        -- -per% cooldown per rank
  return { id = id, skill = skill, tier = tier, name = name, stat = "cooldown", value = 0, per_rank = per,
    value_type = "%", max = 5, description = desc, effects = { { field = "cooldown", percent = -per } } }
end
local function upgrade(id, skill, tier, name, value, per, max, desc)   -- read by the skill's code (C.rank(id))
  return { id = id, skill = skill, tier = tier, name = name, stat = "chance", value = value, per_rank = per,
    value_type = "%", max = max, description = desc }
end

local entries = {
  -- Wicked Swing (3)
  more("ws_edge", 3, 1, "Cruel Edge", 6, "Wicked Swing deals +@ more Damage."),
  stat("ws_skin", 3, 1, "Shadow Skin", "shadow_resistance_percent", 4),
  upgrade("ws_twice", 3, 2, "Second Cut", 0, 8, 5, "Wicked Swing has a @ chance to strike again."),
  stat("ws_thirst", 3, 2, "Bloodthirst", "health_on_hit_add", 2, 5, "You have +@ Life on Hit."),
  upgrade("ws_reach", 3, 3, "Long Shadow", 0, 10, 5, "Wicked Swing's area is @ larger."),
  stat("ws_crit", 3, 3, "Cruelty", "crit_chance_percent", 1),
  upgrade("ws_daze", 3, 4, "Dread Strike", 0, 6, 5, "Enemies hit by Wicked Swing are Dazed: @ Reduced Attack Speed for 3 seconds."),
  faster("ws_quick", 3, 4, "Restless Blade", 4, "Wicked Swing's Cooldown is reduced by @."),
  upgrade("ws_burst", 3, 5, "Ruinous Swing", 0, 10, 5, "Wicked Swing has a @ chance to end in a Dark Burst at its target."),
  stat("ws_cdmg", 3, 5, "Malicious Edge", "crit_damage_percent", 5),
  -- Hex Bolt (4)
  more("hb_power", 4, 1, "Potent Hex", 7, "Hex Bolt deals +@ more Damage."),
  stat("hb_mana", 4, 1, "Hex Siphon", "mana_on_hit_add", 1, 5, "You have +@ Mana on Hit."),
  upgrade("hb_twin", 4, 2, "Twin Hex", 0, 10, 5, "Hex Bolt has a @ chance to throw a second bolt."),
  faster("hb_quick", 4, 2, "Quick Hex", 5, "Hex Bolt's Cooldown is reduced by @."),
  upgrade("hb_long", 4, 3, "Lingering Hex", 0, 20, 5, "Hex's Broken Elemental Resistance lasts @ longer."),
  stat("hb_pen", 4, 3, "Unravel", "elemental_penetration_percent", 2),
  upgrade("hb_slow", 4, 4, "Heavy Hex", 0, 6, 5, "Enemies hit by Hex Bolt are Slowed by @ for 3 seconds."),
  stat("hb_speed", 4, 4, "Swift Curses", "increased_proj_speed_percent", 6),
  upgrade("hb_blind", 4, 5, "Blinding Hex", 0, 5, 5, "Enemies hit by Hex Bolt are Blinded: they miss @ of their attacks for 3 seconds."),
  stat("hb_elem", 4, 5, "Hexmaster", "elemental_damage_percent", 3),
  -- Shadow Blades (5)
  more("sb_sharp", 5, 1, "Black Steel", 6, "Shadow Blades deal +@ more Damage."),
  stat("sb_pierce", 5, 1, "Through Shadow", "chance_to_pierce_percent", 4),
  upgrade("sb_more", 5, 2, "Fan of Night", 0, 1, 2, "Shadow Blades throws +@ more Blades."),
  faster("sb_quick", 5, 2, "Quick Draw", 5, "Shadow Blades' Cooldown is reduced by @."),
  upgrade("sb_wide", 5, 3, "Scattered Dark", 0, 10, 5, "Shadow Blades spread @ wider."),
  stat("sb_proj", 5, 3, "Volley", "increased_proj_speed_percent", 8),
  upgrade("sb_root", 5, 4, "Pinning Blades", 0, 4, 5, "Shadow Blades have a @ chance to Root enemies for 1 second."),
  stat("sb_fork", 5, 4, "Splitting Shadows", "fork_chance_percent", 3),
  upgrade("sb_again", 5, 5, "Second Volley", 0, 8, 5, "Shadow Blades has a @ chance to throw a second volley."),
  stat("sb_as", 5, 5, "Blade Dance", "attack_speed_percent", 2),
  -- Dark Cadence (6)
  more("dc_power", 6, 1, "Dark Rhythm", 6, "Dark Cadence deals +@ more Damage."),
  stat("dc_as", 6, 1, "Tempo", "attack_speed_percent", 2),
  upgrade("dc_every", 6, 2, "Faster Beat", 0, 1, 1, "Dark Burst comes every 2nd cast instead of every 3rd."),
  stat("dc_leech", 6, 2, "Drinking Shadows", "health_leech_percent", 0.2, 5, "You have +@ £."),
  upgrade("dc_big", 6, 3, "Loud Burst", 0, 12, 5, "Dark Burst's area is @ larger."),
  stat("dc_crit", 6, 3, "Precise Beat", "crit_chance_percent", 1),
  upgrade("dc_stun", 6, 4, "Deafening Burst", 0, 5, 5, "Dark Burst has a @ chance to Stun enemies for 1 second."),
  faster("dc_quick", 6, 4, "Allegro", 5, "Dark Cadence's Cooldown is reduced by @."),
  upgrade("dc_double", 6, 5, "Crescendo", 0, 10, 5, "Dark Burst has a @ chance to burst twice."),
  stat("dc_elem", 6, 5, "Black Symphony", "elemental_damage_percent", 3),
  -- Grasping Chain (7)
  more("gc_power", 7, 1, "Heavy Links", 8, "Grasping Chain deals +@ more Damage."),
  stat("gc_tough", 7, 1, "Iron Grip", "res_phy_percent", 3),
  upgrade("gc_root", 7, 2, "Binding Chain", 0, 20, 5, "Grasping Chain's Root lasts @ longer."),
  faster("gc_quick", 7, 2, "Recoil", 6, "Grasping Chain's Cooldown is reduced by @."),
  upgrade("gc_two", 7, 3, "Double Chain", 0, 1, 1, "Grasping Chain throws two chains."),
  stat("gc_ten", 7, 3, "Unbreakable", "tenacity_percent", 3),
  upgrade("gc_weak", 7, 4, "Exposing Grip", 0, 20, 5, "Pulled enemies have their Elemental Resistance Broken for 2 seconds; that lasts @ longer."),
  stat("gc_life", 7, 4, "Bloodchain", "health_on_hit_add", 3, 5, "You have +@ Life on Hit."),
  upgrade("gc_daze", 7, 5, "Dreadful Pull", 0, 8, 5, "Pulled enemies are Dazed by @ for 3 seconds."),
  stat("gc_res", 7, 5, "Chained Soul", "res_mag_percent", 4),
  -- Void Rift (8)
  more("vr_power", 8, 1, "Deep Void", 8, "Void Rift deals +@ more Damage."),
  stat("vr_aoe", 8, 1, "Wide Darkness", "aoe_increased_size_percent", 3),
  upgrade("vr_fast", 8, 2, "Sudden Rift", 0, 10, 5, "Void Rift collapses @ sooner."),
  faster("vr_quick", 8, 2, "Endless Void", 5, "Void Rift's Cooldown is reduced by @."),
  upgrade("vr_slow", 8, 3, "Gravity Well", 0, 8, 5, "Enemies caught in Void Rift are Slowed by @ for 3 seconds."),
  stat("vr_mana", 8, 3, "Void Mind", "mana_regen_add", 1, 5, "You have +@ Mana Regeneration."),
  upgrade("vr_twice", 8, 4, "Aftershock", 0, 10, 5, "Void Rift has a @ chance to collapse again."),
  stat("vr_aoe2", 8, 4, "Spreading Dark", "aoe_increased_effect_percent", 3),
  upgrade("vr_stun", 8, 5, "Event Horizon", 0, 6, 5, "Void Rift has a @ chance to Stun enemies for 1.5 seconds."),
  stat("vr_elem", 8, 5, "Void Lord", "elemental_damage_percent", 3),
  -- Shadow Storm (9)
  more("ss_power", 9, 1, "Black Gale", 6, "Shadow Storm deals +@ more Damage."),
  stat("ss_speed", 9, 1, "Light Feet", "the_speed_percent", 2),
  upgrade("ss_more", 9, 2, "Endless Storm", 0, 1, 2, "Shadow Storm strikes +@ more times."),
  stat("ss_leech", 9, 2, "Storm Drinker", "health_leech_percent", 0.2, 5, "You have +@ £."),
  upgrade("ss_wide", 9, 3, "Eye of the Storm", 0, 10, 5, "Shadow Storm's strikes are @ larger."),
  faster("ss_quick", 9, 3, "Restless Storm", 5, "Shadow Storm's Cooldown is reduced by @."),
  upgrade("ss_daze", 9, 4, "Disorienting Storm", 0, 6, 5, "Enemies hit by Shadow Storm are Dazed by @ for 2 seconds."),
  stat("ss_dodge", 9, 4, "Shadow Dance", "dodge_percent", 3),
  upgrade("ss_burst", 9, 5, "Storm's End", 0, 1, 1, "Shadow Storm ends in a Dark Burst around you."),
  stat("ss_cdmg", 9, 5, "Tempest", "crit_damage_percent", 5),
  -- The Fallen Lance (10)
  more("fl_power", 10, 1, "Heavier Fall", 8, "The Fallen Lance deals +@ more Damage."),
  stat("fl_pen", 10, 1, "Piercing Doom", "elemental_penetration_percent", 2),
  upgrade("fl_end", 10, 2, "Shattering End", 0, 1, 1, "The Fallen Lance explodes in a Dark Burst at the end of its path."),
  faster("fl_quick", 10, 2, "Lance Again", 5, "The Fallen Lance's Cooldown is reduced by @."),
  upgrade("fl_two", 10, 3, "Twin Lances", 0, 10, 5, "The Fallen Lance has a @ chance to throw a second lance."),
  stat("fl_elite", 10, 3, "Kingslayer", "increased_damage_on_elite_percent", 4),
  upgrade("fl_slow", 10, 4, "Crushing Weight", 0, 8, 5, "Enemies hit by The Fallen Lance are Slowed by @ for 3 seconds."),
  stat("fl_crit", 10, 4, "Doom's Eye", "crit_chance_percent", 1),
  upgrade("fl_big", 10, 5, "Cataclysm", 0, 15, 5, "Shattering End's Dark Burst is @ larger."),
  stat("fl_elem", 10, 5, "Fallen Glory", "elemental_damage_percent", 4),
}

-- supports: 8 tiers, two entries each
local function support(skill, list)
  for tier, pair in ipairs(list) do
    for _, en in ipairs(pair) do en.skill = skill; en.tier = tier; entries[#entries + 1] = en end
  end
end
local function sstat(id, name, key, per, max, desc)
  return { id = id, kind = "passive", name = name, stat = key, value = 0, per_rank = per, value_type = pct(key),
    max = max or 5, hero_stat = true, description = desc or "You have +@ £." }
end
local function supg(id, name, value, per, max, desc)
  return { id = id, name = name, stat = "chance", value = value, per_rank = per, value_type = "%", max = max, description = desc }
end
support(0, {   -- Banner of Dread
  { supg("bd_long", "Lasting Dread", 0, 15, 5, "Banner of Dread lasts @ longer."), sstat("bd_elem", "Dread Lord", "elemental_damage_percent", 2) },
  { supg("bd_slow", "Creeping Fear", 10, 5, 5, "Enemies near Banner of Dread are Slowed by @."), sstat("bd_res", "Unafraid", "res_mag_percent", 3) },
  { supg("bd_wide", "Banner of Ruin", 0, 10, 5, "Banner of Dread's area is @ larger."), sstat("bd_life", "Grim Vigor", "the_max_health_percent", 2) },
  { supg("bd_daze", "Terror", 0, 5, 5, "Banner of Dread Dazes enemies by @."), sstat("bd_pen", "Dread Pierce", "elemental_penetration_percent", 2) },
  { supg("bd_fast", "Faster Dread", 0, 8, 5, "Banner of Dread pulses @ faster."), sstat("bd_crit", "Cruel Banner", "crit_chance_percent", 1) },
  { supg("bd_two", "Two Banners", 0, 1, 1, "Banner of Dread plants a second Banner behind you."), sstat("bd_aoe", "Wide Banner", "aoe_increased_size_percent", 3) },
  { supg("bd_weak", "Breaking Fear", 0, 1, 1, "Enemies near Banner of Dread have their Elemental Resistance Broken."), sstat("bd_ten", "Steadfast", "tenacity_percent", 3) },
  { supg("bd_end", "Final Dread", 0, 1, 1, "Banner of Dread ends in a Dark Burst."), sstat("bd_cdmg", "Dread Harvest", "crit_damage_percent", 5) },
})
support(1, {   -- Shadow Step
  { supg("st_far", "Long Step", 0, 10, 5, "Shadow Step goes @ farther."), sstat("st_speed", "Night Runner", "the_speed_percent", 2) },
  { supg("st_wide", "Big Exit", 0, 10, 5, "Shadow Step's explosion is @ larger."), sstat("st_dodge", "Elusive", "dodge_percent", 3) },
  { supg("st_root", "Shadow Snare", 0, 6, 5, "Shadow Step's explosion has a @ chance to Root enemies for 1.5 seconds."), sstat("st_as", "Quick Hands", "attack_speed_percent", 2) },
  { supg("st_haste", "Afterimage", 10, 5, 5, "After Shadow Step you have +@ Movement Speed for 3 seconds."), sstat("st_crit", "Ambusher", "crit_chance_percent", 1) },
  { supg("st_blind", "Blinding Exit", 0, 6, 5, "Shadow Step's explosion Blinds enemies: @ miss chance for 3 seconds."), sstat("st_leech", "Night Feast", "health_leech_percent", 0.2) },
  { supg("st_twice", "Double Step", 0, 1, 1, "Shadow Step explodes at both ends."), sstat("st_pen", "Assassin", "elemental_penetration_percent", 2) },
  { { id = "st_power", name = "Killing Exit", stat = "increased_damage", value = 0, per_rank = 10, value_type = "%", max = 5,
      description = "Shadow Step's explosion deals +@ more Damage.",
      effects = { { row = 1, field = "value", percent = 10 }, { row = 1, field = "per_level", percent = 10 } } }, sstat("st_cdmg", "Shadow Strike", "crit_damage_percent", 5) },
  { supg("st_free", "Phase Walk", 0, 1, 1, "After Shadow Step you are Invulnerable for 1 second."), sstat("st_elem", "Night Stalker", "elemental_damage_percent", 3) },
})
support(2, {   -- Black Aegis
  { supg("ba_long", "Long Night", 0, 15, 5, "Black Aegis lasts @ longer."), sstat("ba_armor", "Black Plate", "res_phy_percent", 3) },
  { supg("ba_thorns", "Spiked Shadow", 0, 20, 5, "During Black Aegis you have +@ Thorns."), sstat("ba_res", "Warded", "res_mag_percent", 3) },
  { supg("ba_heal", "Dark Mend", 0, 2, 5, "Black Aegis heals you for @ of your Max Life every second."), sstat("ba_life", "Black Heart", "the_max_health_percent", 2) },
  { supg("ba_slow", "Chilling Presence", 0, 8, 5, "During Black Aegis, enemies hitting you are Slowed by @."), sstat("ba_ten", "Immovable", "tenacity_percent", 3) },
  { supg("ba_burst", "Shattering Aegis", 0, 1, 1, "Black Aegis ends in a Dark Burst around you."), sstat("ba_regen", "Endurance", "health_regen_add", 2, 5, "You have +@ Life Regeneration.") },
  { supg("ba_more", "Thicker Shadow", 0, 5, 5, "Black Aegis grants +@ more Armor and Elemental Resistance."), sstat("ba_thorn2", "Barbed", "thorns_percent", 5) },
  { supg("ba_ret", "Retribution", 0, 10, 5, "During Black Aegis you have +@ Retaliation."), sstat("ba_retal", "Spite", "retaliate_percent", 3) },
  { supg("ba_aura", "Aegis of Night", 0, 1, 1, "Black Aegis also gives +20% Elemental Damage."), sstat("ba_reduce", "Black Bulwark", "reduced_damage_from_all_percent", 1) },
})


-- ---- its Ancestral Legacy tree: every realm of the game's tree layout holds its own legacies (legacy_base = "none").
-- Realms by size, colour and tier (the game's layout, 3 characters each: size, colour, tier in hex): colours are its
-- six schools, small and medium realms hold stat legacies (stronger with the tier), the big ones its keystones.
local REALMS = "00100101101102102103103104104100205201205202205203205204205210310311311312312313313314314305400405401405402405403405404400500500500501501501501502502502502503503503503504504504504520821822823824805610610605611611605612612605613613605614614604705700700705701701705702702705703703705704710830810811831811812832812813833813814834814835a35a35a35a35a00900900a00900901901901a01901902902902a02902903903903a03903904904904a049049"
local SCHOOLS = {
  [0] = { "Blood", { { "health_leech_percent", 0.08, "Thirst" }, { "the_max_health_percent", 0.5, "Vigor" },
                     { "health_on_hit_add", 0.8, "Feast" }, { "crit_damage_percent", 1.0, "Cruelty" } } },
  [1] = { "Dread", { { "res_mag_percent", 0.6, "Ward" }, { "res_phy_percent", 0.6, "Plate" },
                     { "tenacity_percent", 0.5, "Resolve" }, { "reduced_damage_from_melee_percent", 0.3, "Bulwark" } } },
  [2] = { "Wrath", { { "attack_speed_percent", 0.3, "Fury" }, { "crit_chance_percent", 0.2, "Precision" },
                     { "cooldown_reduction_percent", 0.25, "Haste" }, { "the_speed_percent", 0.3, "Stride" } } },
  [3] = { "Fallen", { { "elemental_damage_percent", 0.6, "Radiance" }, { "elemental_penetration_percent", 0.3, "Piercing Light" },
                      { "aoe_increased_size_percent", 0.4, "Halo" }, { "mana_regen_add", 0.3, "Grace" } } },
  [4] = { "Shadow", { { "dodge_percent", 0.5, "Veil" }, { "shadow_resistance_percent", 0.8, "Shroud" },
                      { "increased_damage_on_elite_percent", 0.5, "Hunt" }, { "crit_chance_percent", 0.2, "Ambush" } } },
  [5] = { "Void", { { "the_max_mana_percent", 0.5, "Depths" }, { "mana_on_hit_add", 0.3, "Hunger" },
                    { "retaliate_percent", 0.5, "Spite" }, { "thorns_percent", 0.8, "Thorns" } } },
}
local ROMAN = { "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X" }
local function round(x) return math.floor(x * 100 + 0.5) / 100 end
-- keystones on the big realms (kind "upgrade": their effects are in this mod's code; per_rank = the value per rank)
local KEYSTONES = {
  [60] = { "sanguine", "Sanguine Strikes", 3, "Your hits have a @ chance to heal you for 2% of your Max Life." },
  [61] = { "dread_chill", "Chilling Dread", 5, "Your hits have a @ chance to Chill enemies for 2 seconds." },
  [62] = { "frenzy", "Killing Frenzy", 3, "Whenever you kill an enemy, you gain +@ Attack Speed for 3 seconds." },
  [63] = { "halo", "Fallen Halo", 6, "Whenever you are hit, there is a @ chance to Blind the attacker for 3 seconds." },
  [64] = { "umbral", "Umbral Pact", 5, "While below 50% Life, you deal +@ Elemental Damage." },
  [96] = { "crimson", "Crimson Tide", 0.5, "Killing an enemy heals you for @ of your Max Life." },
  [99] = { "terror", "Frozen Terror", 2, "Your hits have a @ chance to Stun enemies for half a second." },
  [102] = { "storm", "Storm of Malice", 10, "Your hits have a @ chance to grant 1 additional Malice." },
  [105] = { "corrupt", "Corrupted Light", 2, "Unleashed Malice also heals you for @ of your Max Life." },
  [108] = { "cloak", "Shadow Cloak", 4, "Whenever you cast a Skill, you gain +@ Dodge for 2 seconds." },
  [110] = { "endless", "Endless Malice", 10, "Unleashed Malice lasts @ longer." },
  [112] = { "gaze", "Abyssal Gaze", 4, "Every second, enemies near you are Slowed by @." },
  [113] = { "herald", "Herald of Ruin", 4, "Killing an enemy has a @ chance to unleash a Dark Burst where it died." },
}
-- activable legacies (on the skill bar, keys 1-4) in four realms that hold two legacies: auras (toggled, a lock on
-- mana / life) and actives (cost and cooldown); stats are value + per_rank x rank, "use" runs when one is used
local ACT_USE = {}                            -- (filled further down, where the skills' helpers are)
local ACTIVABLES = {
  [24] = { id = "dark_pact", kind = "active", name = "Dark Pact", cost = 8, cost_type = "life_percent", cooldown = 15,
    duration = 6, stat = { "elemental_damage_percent", "attack_speed_percent" }, value = { 10, 5 }, per_rank = { 2, 1 },
    value_type = { "%", "%" }, max = 5, buff_icon = "neutral_rite",
    description = "You sacrifice 8% of your Max Life: +@ £ and +@ £ for 6 seconds." },
  [27] = { id = "dread_aura", kind = "aura", name = "Aura of Dread", cost = 15, cost_type = "mana_lock",
    stat = "elemental_damage_percent", value = 5, per_rank = 2, value_type = "%", max = 5, buff_icon = "neutral_aura",
    description = "While this Aura is toggled on, you have +@ £ and enemies near you are Slowed.",
    use = function(e) if ACT_USE.dread then ACT_USE.dread(e) end end },
  [76] = { id = "shadow_rift", kind = "active", name = "Shadow Rift", cost = 60, cost_type = "mana", cooldown = 8,
    stat = "aoe_increased_size_percent", value = 0, per_rank = 10, value_type = "%", max = 5,
    description = "You tear a rift of shadow around you: a Dark Burst, @ larger.",
    use = function(e) if ACT_USE.rift then ACT_USE.rift(e) end end },
  [79] = { id = "night_shroud", kind = "aura", name = "Shroud of Night", cost = 10, cost_type = "life_lock",
    stat = { "dodge_percent", "shadow_resistance_percent" }, value = { 4, 5 }, per_rank = { 1, 2 }, value_type = { "%", "%" },
    max = 5, buff_icon = "darkness_pact", description = "While this Aura is toggled on, you have +@ £ and +@ £." },
}
local legacies = {}
local function stat_legacy(realm, school, k, tier, max)
  local sc = SCHOOLS[school]
  local st = sc[2][(k - 1) % #sc[2] + 1]
  local per = round(st[2] * (1 + tier * 0.25))
  legacies[#legacies + 1] = { realm = realm, kind = "stat", name = sc[1] .. " " .. st[3] .. " " .. ROMAN[tier],
    stat = st[1], value = 0, per_rank = per, value_type = st[1]:find("percent") and "%" or "", max = max,
    description = "You have +@ £." }
end
local seen = {}                               -- (colour, tier) -> realms so far: stats rotate
for r = 0, 139 do
  local size, colour, tier = tonumber(REALMS:sub(r * 3 + 1, r * 3 + 1)), tonumber(REALMS:sub(r * 3 + 2, r * 3 + 2)),
    tonumber(REALMS:sub(r * 3 + 3, r * 3 + 3), 16)
  local key = colour * 100 + tier
  seen[key] = (seen[key] or 0) + 1
  local ks = KEYSTONES[r]
  if ks then
    legacies[#legacies + 1] = { id = ks[1], realm = r, kind = "upgrade", name = ks[2], stat = "chance", value = 0,
      per_rank = ks[3], value_type = "%", max = 5, description = ks[4] }
  end
  if r == 111 then
    legacies[#legacies + 1] = { realm = r, kind = "stat", name = "Void Heart", stat = "increased_damage_on_elite_percent",
      value = 0, per_rank = 3, value_type = "%", max = 5, description = "You have +@ £." }
  elseif r == 114 then
    legacies[#legacies + 1] = { realm = r, kind = "stat", name = "Black Covenant", stat = "elemental_damage_percent",
      value = 0, per_rank = 3, value_type = "%", max = 5, description = "You have +@ £." }
  elseif not ks or size == 2 then
    stat_legacy(r, colour, seen[key], tier, size == 0 and 10 or 15)
  end
  if ACTIVABLES[r] then                       -- these realms have room for two legacies
    local a = ACTIVABLES[r]
    a.realm = r
    legacies[#legacies + 1] = a
  elseif r == 95 or r == 100 or r == 109 then
    stat_legacy(r, colour, seen[key] + 2, tier, 15)
  end
end

-- its achievements: the Knight's 23, counted on its own data, under its own names; then its own
local achievements = {
  { id = "unleash", name = "Malice Unbound", description = "Unleash your Malice $ times.", icon = 13, steps = { 5, 50, 250, 1000, 5000 } },
  { id = "dread_kills", name = "Lord of Dread", description = "Kill $ Enemies while your Aura of Dread is on.", icon = 0,
    steps = { 10, 100, 1000, 10000, 50000 } },
  { id = "rifts", name = "Rift Walker", description = "Open $ Shadow Rifts.", icon = 4, steps = { 5, 50, 250, 1000, 5000 } },
  { id = "saga", name = "The Dark Saga", description = "Complete $ of the Evil Knight's missions.", icon = 13, steps = { 1, 2, 3, 4 } },
}

-- its reapers: its own version of the Gold-Plated pair (61, evolving into 62) - its names, stats and Malice effects
-- (code "own": the game's Gold Armor effect is off for it; its Lua below does theirs)
local reapers = {
  [61] = { name = "Malice-Drinker $", code = "own", lore = "It drinks what the wound gives, and asks for more.",
    stats = { { stat = "elemental_damage_percent", value = 1, per_level = true, percent = true },
              { stat = "health_on_hit_add", value = 2, per_level = true } },
    values = { { value = 25, percent = true } },
    description = "Your Skills' hits have a @ chance to grant 1 additional Malice." },
  [62] = { name = "Gluttonous Malice-Drinker $", code = "own", lore = "Sated, for a moment. Never for long.",
    stats = { { stat = "elemental_damage_percent", value = 30, percent = true } },
    values = { { value = 50, percent = true } },
    description = "Unleashed Malice lasts @ longer." },
}

-- its story: four missions in its own area, The Black Crypt - the Graveyard's blood-red mirror (its map and rooms in
-- red, its gate inverted), entered from its own gate in the hub - each on a Graveyard mission's map (its waves, its boss), in turn
local C                                       -- (the class, registered below)
local area = { name = "The Black Crypt", map = art("spr_menu_story_graveyard_overmap"),
  rooms = art("spr_menu_story_graveyard_maps"), entrance = art("spr_entrance_graveyard_open") }
local missions = {
  { id = "black_vigil", map = 0, area = true, room = 0, name = "The Black Vigil",
    description = "The dead of Adreart stir in their graves.##They will answer to a new master.",
    intro = { "The dead of Adreart stir.", "Let them rise. They will kneel to me." }, outro = "One graveyard. A kingdom to go." },
  { id = "open_wounds", map = 4, area = true, room = 4, after = "black_vigil", name = "Open Wounds",
    description = "Breaches bleed Slorm into the Graveyard.##Close them, and drink what spills.",
    intro = "The world is bleeding. How generous.", outro = "Every wound closed is power taken." },
  { id = "dread_march", map = 5, area = true, room = 5, after = "open_wounds", name = "The Dread March",
    description = "The Graveyard sends wave after wave against the intruder.##Break them all.",
    intro = "Come, then. All of you.", outro = "They broke. They always do." },
  { id = "throne_of_bones", map = 15, area = true, room = 15, after = "dread_march", name = "Throne of Bones",
    description = "The Necromancer believes the dead are his.##Show him otherwise.",
    intro = { "Necromancer. Your servants are mine now.", "And so are you." }, outro = "The throne of bones has a new king." },
}
for _, m in ipairs(missions) do
  m.done = function()                         -- "The Dark Saga": how many of them are done
    local n = 0
    for _, x in ipairs(missions) do if C.mission_runs(x.id) > 0 then n = n + 1 end end
    C.achievement("saga", n)
  end
end
for ref, name in pairs{ [88] = "The Unholy Ascension", [89] = "A Blade Drinks Twice", [90] = "Harvest of Shadows",
    [91] = "The Dark Tithe", [92] = "Shatter the Light", [93] = "The Cursed Armory", [94] = "The Primordial Armory",
    [95] = "Squire of Night", [96] = "Banner of Fear", [97] = "Into the Dark", [98] = "Unbreakable Black",
    [99] = "Wicked Ways", [100] = "Hexed", [101] = "A Thousand Cuts", [102] = "Dark Rhythm", [103] = "Chained Souls",
    [104] = "Rift Keeper", [105] = "Eye of the Storm", [106] = "The Fallen", [107] = "Master of Darkness",
    [108] = "Perfect Corruption", [109] = "Faithless", [110] = "Darkness Devours Darkness" } do
  achievements[#achievements + 1] = { base = ref, name = name }
end

C = S.class{
  id = "evil_knight", name = "Evil Knight", title = "The Evil Knight", base = 0, kit = "own", art = LOOK,
  legacies = legacies, legacy_base = "none",
  talk = { "The light had its turn.", "Shall I take over?" }, talk_as = "self",   -- in the hub: its own pages and portrait
  skin_name = "Fallen",                                                             -- its looks (Character menu): its own,
  skins = { { id = "memory", name = "Knight's Memory", art = MEMORY } },          -- and the Knight it was before its fall
  achievements = achievements,
  missions = missions, area = area,
  reapers = reapers,
  skills = {
    [0] = { name = "Banner of Dread", genre = "aoe,totem", dmg_type = "mag", stat = "elemental_damage", value = 30, per_level = 3,
      cooldown = 600, cost = 60, icon = 0,
      description = "You plant a Banner of Dread at target location for 4 seconds. Every second it deals @ £ to enemies around it and Slows them." },
    [1] = { name = "Shadow Step", genre = "aoe,movement", dmg_type = "mag", stat = "elemental_damage", value = 80, per_level = 6,
      cooldown = 300, cost = 40, icon = 1,
      description = "You dash toward target location through the shadows, then explode, dealing @ £ to enemies around you." },
    [2] = { name = "Black Aegis", genre = "special", dmg_type = "mag", stat = "res_phy_percent", value = 25, per_level = 1,
      value_type = "%", cooldown = 900, cost = 50, icon = 2,
      description = "You wrap yourself in shadow for 4 seconds, gaining +@ Armor and Elemental Resistance." },
    [3] = { name = "Wicked Swing", genre = "melee", dmg_type = "mag", stat = "elemental_damage", value = 55, per_level = 4,
      cooldown = 40, cost = 0, icon = 3,
      description = "A cruel swing of your blade, dealing @ £." },
    [4] = { name = "Hex Bolt", genre = "projectile", dmg_type = "mag", stat = "elemental_damage", value = 45, per_level = 3,
      cooldown = 60, cost = 20, icon = 4,
      description = "You hurl a Hex Bolt dealing @ £. Enemies hit have their Elemental Resistance Broken for 3 seconds." },
    [5] = { name = "Shadow Blades", genre = "projectile", dmg_type = "mag", stat = "elemental_damage", value = 30, per_level = 2,
      cooldown = 80, cost = 34, icon = 5,
      description = "You throw 3 Shadow Blades, each dealing @ £." },
    [6] = { name = "Dark Cadence", genre = "melee", dmg_type = "mag", stat = "elemental_damage", value = 25, per_level = 2,
      cooldown = 20, cost = 0, icon = 6,
      description = "A fast dark strike dealing @ £.#Every 3rd cast unleashes a Dark Burst at its target." },
    [7] = { name = "Grasping Chain", genre = "projectile", dmg_type = "mag", stat = "elemental_damage", value = 60, per_level = 4,
      cooldown = 150, cost = 50, icon = 7,
      description = "You throw a Grasping Chain dealing @ £, pulling enemies hit toward you and Rooting them for 1 second." },
    [8] = { name = "Void Rift", genre = "aoe", dmg_type = "mag", stat = "elemental_damage", value = 120, per_level = 8,
      cooldown = 240, cost = 80, icon = 8,
      description = "You tear open a Void Rift at target location. After a short delay it collapses, dealing @ £ to enemies inside." },
    [9] = { name = "Shadow Storm", genre = "melee", dmg_type = "mag", stat = "elemental_damage", value = 40, per_level = 3,
      cooldown = 180, cost = 70, icon = 9,
      description = "You spin through the shadows, striking 4 times around you, each strike dealing @ £." },
    [10] = { name = "The Fallen Lance", genre = "projectile", dmg_type = "mag", stat = "elemental_damage", value = 150, per_level = 10,
      cooldown = 300, cost = 120, icon = 10,
      description = "You throw The Fallen Lance, piercing every enemy in its path for @ £." },
  },
  entries = entries,
  texts = {
    class = { { name = "Malice", description = "The Evil Knight feeds on Malice: every enemy he hits makes his darkness grow." },
              { name = "Malice 2", description = "Each hit with a Skill grants 1 Malice (up to 10).##At 10 Malice, your Malice is Unleashed: +40% Elemental Damage for 4 seconds." } },
    mechanics = { { name = "Unleashed Malice", description = "+40% Elemental Damage for 4 seconds.##Exclusive to The Evil Knight." },
                  { name = "Dark Burst", description = "An explosion of darkness that deals the Skill's Damage to enemies around it.##Exclusive to The Evil Knight." } },
    mechanic = { { name = "Hex", description = "Hexed enemies have their Elemental Resistance Broken.##Exclusive to The Evil Knight." },
                 { name = "Grasp", description = "Grasped enemies are pulled toward you and Rooted.##Exclusive to The Evil Knight." } },
    spec = {
      { name = "Dread Lord", description = "The Evil Knight plants fear itself on the battlefield. His banners break the will and the resistance of every foe around them.|Fear is a weapon.|Areas/Crowd Control/Elemental Damage|{Dark Burst}/{Hex}" },
      { name = "Night Stalker", description = "The Evil Knight moves through the shadows, appearing where his enemies least expect him and vanishing before they strike back.|Now you see me.|Mobility/Critical Strikes/Burst|{Dark Burst}/{Unleashed Malice}" },
      { name = "Black Bulwark", description = "The Evil Knight wraps himself in living shadow. Every blow against him feeds his darkness and returns as pain.|Hit me. I dare you.|Defense/Thorns/Sustain|{Unleashed Malice}/{Grasp}" },
    },
  },
}
print("Evil Knight is hero " .. tostring(C.hero) .. ", citizen " .. tostring(C.citizen) .. ", " .. tostring(C.rows) .. " rows, "
  .. tostring(C.effects) .. " effects, " .. #legacies .. " legacies")

-- ---- skill code (handlers get the class_cast event: e.hero, e.skill, e.precast_x / _y = the aim, e.x / e.y = the hero)
local rank = C.rank
local function chance(id) return math.random(100) <= rank(id) * (({ ws_twice = 8, ws_burst = 10, hb_twin = 10, sb_root = 4,
  sb_again = 8, dc_stun = 5, dc_double = 10, vr_twice = 10, vr_stun = 6, fl_two = 10, st_root = 6 })[id] or 0) end
local function burst(e, x, y, scale, anim)   -- (anim: its animation's scale, default 1.4 x scale)
  if not x or not S.exists(e.hero) then return end   -- (its hero gone - its run left: none, not in the next room)
  S.area(e, { x = x, y = y, mask = "spr_aoe_360", scale = scale or 0.6, impact = IMPACT })
  S.anim(x, y, BURST, { scale = anim or (scale or 0.6) * 1.4 })
end
local function aim(e) return math.atan(e.precast_y - e.y, e.precast_x - e.x) end

local skills = {}
skills[3] = { cast = function(e)          -- Wicked Swing
  local scale = 1 + rank("ws_reach") * 0.10
  S.melee(e, { anim = SWING, anim2 = BLOW, scale = scale })
  if chance("ws_twice") then S.after(0.15, function() S.melee(e, { anim = SWING, anim2 = BLOW, scale = scale, facing = -(e.facing or 1) }) end) end
  if chance("ws_burst") then burst(e, e.precast_x, e.precast_y) end
end, hit = function(h)
  local d = rank("ws_daze"); if d > 0 then S.debuff(h.target, "daze", 3, d * 6) end
end }
skills[4] = { cast = function(e)          -- Hex Bolt
  S.projectile(e, { sprite = "spr_arcane_missile", speed = 6 })
  if chance("hb_twin") then S.after(0.12, function() S.projectile(e, { sprite = "spr_arcane_missile", speed = 6 }) end) end
end, hit = function(h)
  S.debuff(h.target, "elemental_broken", 3 * (1 + rank("hb_long") * 0.2), 50)
  local sl = rank("hb_slow"); if sl > 0 then S.debuff(h.target, "slow", 3, sl * 6) end
  local bl = rank("hb_blind"); if bl > 0 then S.debuff(h.target, "blind", 3, bl * 5) end
end }
skills[5] = { cast = function(e)          -- Shadow Blades
  local n, spread = 3 + rank("sb_more"), 15 * (1 + rank("sb_wide") * 0.1)
  S.projectile(e, { n = n, spread = spread, sprite = BLADE, speed = 5.5 })
  if chance("sb_again") then S.after(0.2, function() S.projectile(e, { n = n, spread = spread, sprite = BLADE, speed = 5.5 }) end) end
end, hit = function(h)
  if chance("sb_root") then S.debuff(h.target, "root", 1) end
end }
local beat = 0
skills[6] = { cast = function(e)          -- Dark Cadence
  S.melee(e, { anim = SWING, anim2 = BLOW, scale = 0.7, anim_speed = 0.6 })
  beat = beat + 1
  if beat >= (rank("dc_every") > 0 and 2 or 3) then
    beat = 0
    local scale = 0.6 * (1 + rank("dc_big") * 0.12)
    burst(e, e.precast_x, e.precast_y, scale)
    if chance("dc_double") then S.after(0.25, function() burst(e, e.precast_x, e.precast_y, scale) end) end
  end
end, hit = function(h)
  if chance("dc_stun") then S.debuff(h.target, "stun", 1) end
end }
skills[7] = { cast = function(e)          -- Grasping Chain
  S.projectile(e, { sprite = "spr_darkness_hook_flat", speed = 8 })
  if rank("gc_two") > 0 then S.projectile(e, { n = 2, spread = 20, sprite = "spr_darkness_hook_flat", speed = 8 }) end
end, hit = function(h)
  S.knock(h.target, -6, h.hero)            -- pulled toward the hero
  S.debuff(h.target, "root", 1 * (1 + rank("gc_root") * 0.2))
  local w = rank("gc_weak"); if w > 0 then S.debuff(h.target, "elemental_broken", 2 * (1 + w * 0.2), 50) end
  local d = rank("gc_daze"); if d > 0 then S.debuff(h.target, "daze", 3, d * 8) end
end }
skills[8] = { cast = function(e)          -- Void Rift
  local x, y = e.precast_x, e.precast_y
  S.anim(x, y, "spr_darkness_seal_outter", { scale = 1.2 })
  local delay = 0.6 * (1 - rank("vr_fast") * 0.1)
  S.after(delay, function()
    burst(e, x, y, 1.0, 1.6)
    if chance("vr_twice") then S.after(0.4, function() burst(e, x, y, 1.0, 1.6) end) end
  end)
end, hit = function(h)
  local sl = rank("vr_slow"); if sl > 0 then S.debuff(h.target, "slow", 3, sl * 8) end
  if chance("vr_stun") then S.debuff(h.target, "stun", 1.5) end
end }
skills[9] = { cast = function(e)          -- Shadow Storm
  local n, scale = 4 + rank("ss_more"), 1 + rank("ss_wide") * 0.1
  for i = 0, n - 1 do
    S.after(i * 0.1, function() S.melee(e, { anim = SWING, anim2 = BLOW, scale = scale, facing = (i % 2 == 0) and 1 or -1 }) end)
  end
  if rank("ss_burst") > 0 then S.after(n * 0.1, function() burst(e, e.x, e.y, 0.8) end) end
end, hit = function(h)
  local d = rank("ss_daze"); if d > 0 then S.debuff(h.target, "daze", 2, d * 6) end
end }
skills[10] = { cast = function(e)         -- The Fallen Lance
  S.projectile(e, { sprite = LANCE, speed = 4.5 })
  if chance("fl_two") then S.after(0.2, function() S.projectile(e, { sprite = LANCE, speed = 4.5 }) end) end
  if rank("fl_end") > 0 then
    local a = aim(e)
    local x, y = e.x + math.cos(a) * 320, e.y + math.sin(a) * 320
    S.after(1.1, function() burst(e, x, y, 0.8 * (1 + rank("fl_big") * 0.15)) end)
  end
end, hit = function(h)
  local sl = rank("fl_slow"); if sl > 0 then S.debuff(h.target, "slow", 3, sl * 8) end
end }
skills[0] = { cast = function(e)          -- Banner of Dread
  local x, y = e.precast_x, e.precast_y
  local banners = { { x, y } }
  if rank("bd_two") > 0 then banners[2] = { e.x - (e.facing or 1) * 64, e.y } end
  local seconds = 4 * (1 + rank("bd_long") * 0.15)
  local every = 1 / (1 + rank("bd_fast") * 0.08)
  local scale = 0.9 * (1 + rank("bd_wide") * 0.1)
  for _, b in ipairs(banners) do
    S.anim(b[1], b[2], "spr_cataclysm_shadow_symbol", { scale = 1 })
    local pulses, done = math.floor(seconds / every), 0
    local id
    id = S.every(every, function()
      done = done + 1
      if not S.exists(e.hero) then S.cancel(id); return end   -- (its hero gone: the banner with it)
      S.area(e, { x = b[1], y = b[2], mask = "spr_aoe_360", scale = scale, impact = IMPACT })
      S.anim(b[1], b[2], "spr_burst_darkness", { scale = scale })
      if done >= pulses then
        S.cancel(id)
        if rank("bd_end") > 0 then burst(e, b[1], b[2], scale) end
      end
    end)
  end
end, hit = function(h)
  S.debuff(h.target, "slow", 1.2, 10 + rank("bd_slow") * 5)
  local d = rank("bd_daze"); if d > 0 then S.debuff(h.target, "daze", 1.2, d * 5) end
  if rank("bd_weak") > 0 then S.debuff(h.target, "elemental_broken", 1.2, 50) end
end }
skills[1] = { cast = function(e)          -- Shadow Step
  local a = aim(e)
  local dist = math.min(240 * (1 + rank("st_far") * 0.1), math.sqrt((e.precast_x - e.x) ^ 2 + (e.precast_y - e.y) ^ 2))
  local scale = 0.8 * (1 + rank("st_wide") * 0.1)
  S.anim(e.x, e.y, "spr_burst_darkness", { scale = 0.8 })
  if rank("st_twice") > 0 then burst(e, e.x, e.y, scale) end
  S.knock(e.hero, -dist / 12)              -- a lunge toward the aim
  S.after(0.25, function()
    local hx, hy = S.get("x", e.hero, 0), S.get("y", e.hero, 0)
    burst(e, hx, hy, scale)
    local h = rank("st_haste"); if h > 0 then S.buff{ key = "evil_afterimage", target = e.hero, stats = { the_speed_percent = 10 + h * 5 }, duration = 3, name = "Afterimage" } end
    if rank("st_free") > 0 then S.buff{ key = "evil_phase", target = e.hero, stats = { reduced_damage_from_all_percent = 100 }, duration = 1, name = "Phase Walk" } end
  end)
end, hit = function(h)
  if chance("st_root") then S.debuff(h.target, "root", 1.5) end
  local b = rank("st_blind"); if b > 0 then S.debuff(h.target, "blind", 3, b * 6) end
end }
skills[2] = { cast = function(e)          -- Black Aegis
  local seconds = 4 * (1 + rank("ba_long") * 0.15)
  local res = 25 + rank("ba_more") * 5
  local stats = { res_phy_percent = res, res_mag_percent = res, thorns_percent = rank("ba_thorns") * 20,
    retaliate_percent = rank("ba_ret") * 10 }
  if rank("ba_aura") > 0 then stats.elemental_damage_percent = 20 end
  S.buff{ key = "evil_aegis", stats = stats, duration = seconds, name = "Black Aegis",
    description = "Increased Armor and Elemental Resistance." }
  S.anim(e.x, e.y, "spr_shadow_shield", { scale = 1, follow = e.hero })
  local heal = rank("ba_heal")
  if heal > 0 then
    local n, id = 0
    id = S.every(1, function()
      n = n + 1
      local max = S.get("final_the_max_health", e.hero, 0) or 0
      local hp = S.get("the_health", e.hero, 0) or 0
      S.set("the_health", math.min(max, hp + max * heal * 2 / 100), e.hero)
      if n >= seconds then S.cancel(id) end
    end)
  end
  if rank("ba_burst") > 0 then S.after(seconds, function() burst(e, S.get("x", e.hero, 0), S.get("y", e.hero, 0), 0.9) end) end
end }

-- ---- its legacies' keystones: ranks read twice a second
local LR = {}
local function lr(id) return LR[id] or 0 end
S.every(0.5, function()
  if S.get("current_hero") ~= C.hero then LR = {} return end   -- its legacies exist only while it is played
  for _, ks in pairs(KEYSTONES) do LR[ks[1]] = C.legacy_rank(ks[1]) end
end)
local KEYPER = {}
for _, ks in pairs(KEYSTONES) do KEYPER[ks[1]] = ks[3] end
local function roll(id) return math.random() * 100 < lr(id) * (KEYPER[id] or 0) end
local hero_ref                                -- the hero instance (from the last event that had it)
local function hero_now()                     -- it, or (gone: a room changed meanwhile) the hero now
  if hero_ref and S.exists(hero_ref) then return hero_ref end
  local h = (S.instances("obj_parent_hero") or {})[1]
  hero_ref = h and { ["$inst"] = h["$inst"] } or nil
  return hero_ref
end
local function heal(percent)
  if not hero_now() then return end
  local max = S.get("final_the_max_health", hero_ref, 0) or 0
  local hp = S.get("the_health", hero_ref, 0) or 0
  S.set("the_health", math.min(max, hp + max * percent / 100), hero_ref)
end
local function fake_cast(x, y)                 -- a class_cast-like event for pieces made outside a cast
  return { hero = hero_ref, skill = 3, forced = true, element = "none", dmg_type = "mag", x = x, y = y, precast_x = x, precast_y = y }
end

-- the class's mechanic: Malice, 1 per hit (up to 10), at 10 Unleashed Malice; shown on the HUD (C.meter)
local malice, unleashed, unleash_n = 0, false, 0   -- unleash_n: the latest unleash (an earlier one's end leaves it on)
C.meter(0, 10)

-- its legendaries: HERO = its hero number, so they drop for it only (the Knight's own do not drop for it)
local LEG = {}
LEG.crown = S.legendary{ id = "evil_crown", name = "Crown of Unending Malice", slot = "helm", icon = 49, hero = C.hero,
  description = "+@% Elemental Damage. Unleashed Malice lasts @% longer.",
  stats = { elemental_damage_percent = 20 }, values = { 50 } }
LEG.gauntlets = S.legendary{ id = "evil_gauntlets", name = "Gauntlets of the Black Aegis", slot = "glove", icon = 44, hero = C.hero,
  description = "Whenever you cast a Skill, you gain +@% Elemental Resistance for 3 seconds.", values = { 20 },
  effects = { { trigger = "cast", buff = { stats = { res_mag_percent = 20 }, duration = 3, name = "Black Aegis" } } } }
LEG.ring = S.legendary{ id = "evil_ring", name = "Ring of Festering Spite", slot = "ring", icon = 156, hero = C.hero,
  description = "+@% Critical Strike Chance. Your hits have a @% chance to grant 1 additional Malice.",
  stats = { crit_chance_percent = 6 }, values = { 20 } }
LEG.pauldrons = S.legendary{ id = "evil_pauldrons", name = "Pauldrons of the Fallen", slot = "shoulder", icon = 47, hero = C.hero,
  description = "Killing an enemy while your Malice is Unleashed heals you for @% of your Max Life.", values = { 3 },
  on_kill = function(e, r) if unleashed then heal(3 * r / 100) end end }
local function worn(leg) return S.legendary_equipped(leg.id) end   -- its roll (75-100) or nil

local function on_hit_malice(h)
  if not h.primary then return end
  local spite = worn(LEG.ring)
  malice = malice + 1 + (roll("storm") and 1 or 0) + (spite and math.random(100) <= 20 * spite / 100 and 1 or 0)
    + (C.reaper_active(61) and math.random(100) <= 25 and 1 or 0)          -- its Malice-Drinker
  if malice >= 10 then
    malice = 0
    S.buff_remove("evil_malice")
    local crown = worn(LEG.crown)
    local duration = 4 * (1 + lr("endless") * 0.1) * (1 + (crown and 0.5 * crown / 100 or 0))
      * (C.reaper_active(62) and 1.5 or 1)                                  -- its Gluttonous Malice-Drinker
    S.buff{ key = "evil_unleashed", stats = { elemental_damage_percent = 40 }, duration = duration,
      name = "Unleashed Malice", description = "+40% Elemental Damage." }
    if lr("corrupt") > 0 then heal(lr("corrupt") * 2) end
    unleashed = true
    C.meter(10, 10, true)
    C.achievement_add("unleash")
    unleash_n = unleash_n + 1
    local n = unleash_n
    S.after(duration, function() if unleash_n == n then unleashed = false; C.meter(malice, 10) end end)
  else
    if not unleashed then C.meter(malice, 10) end
    S.buff{ key = "evil_malice", stats = {}, duration = 8, stacks = 1, max_stacks = 10, name = "Malice",
      description = "At 10 Malice, your Malice is Unleashed." }
  end
end
local function on_hit_legacies(h)
  if roll("sanguine") then heal(2) end
  if roll("dread_chill") then S.debuff(h.target, "ice_chill", 2, 30) end
  if roll("terror") then S.debuff(h.target, "stun", 0.5) end
end

S.on("class_cast", function(e)
  if e.class ~= C.key then return end
  hero_ref = e.hero
  local s = skills[e.skill]
  if s and s.cast then
    local ok, err = pcall(s.cast, e)
    if not ok then S.log("cast " .. e.skill .. ": " .. tostring(err)) end
  end
  if lr("cloak") > 0 then
    S.buff{ key = "evil_cloak", stats = { dodge_percent = lr("cloak") * 4 }, duration = 2, name = "Shadow Cloak" }
  end
end)
S.on("class_hit", function(h)
  if h.class ~= C.key then return end
  hero_ref = h.hero or hero_ref
  local s = skills[h.skill]
  pcall(on_hit_malice, h)
  pcall(on_hit_legacies, h)
  if s and s.hit then
    local ok, err = pcall(s.hit, h)
    if not ok then S.log("hit " .. h.skill .. ": " .. tostring(err)) end
  end
end)
S.on("enemy_killed", function(k)
  if S.get("current_hero") ~= C.hero then return end
  pcall(function()
    if lr("frenzy") > 0 then
      S.buff{ key = "evil_frenzy", stats = { attack_speed_percent = lr("frenzy") * 3 }, duration = 3, name = "Killing Frenzy" }
    end
    if lr("crimson") > 0 then heal(lr("crimson") * 0.5) end
    if roll("herald") and k.x and hero_now() then burst(fake_cast(k.x, k.y), k.x, k.y, 0.7) end
  end)
end)
S.on("hero_hit", function(h)
  if S.get("current_hero") ~= C.hero then return end
  if roll("halo") and h.attacker and h.attacker["$inst"] then pcall(S.debuff, h.attacker, "blind", 3, 30) end
end)
S.every(0.5, function()                        -- Umbral Pact (below half Life) and Abyssal Gaze (slow around you)
  if S.get("current_hero") ~= C.hero then return end
  local hero = S.instances("obj_parent_hero")[1]
  if not hero then return end
  hero_ref = { ["$inst"] = hero["$inst"] }
  if lr("umbral") > 0 then
    local hp, max = S.get("the_health", hero_ref, 0) or 0, S.get("final_the_max_health", hero_ref, 0) or 1
    if hp < max / 2 then
      S.buff{ key = "evil_umbral", stats = { elemental_damage_percent = lr("umbral") * 5 }, duration = 0.6, name = "Umbral Pact" }
    end
  end
end)
-- the activable legacies' own effects
local dread_on = false
ACT_USE.dread = function(e) dread_on = e.on end
S.on("enemy_killed", function()                -- Lord of Dread
  if dread_on and S.get("current_hero") == C.hero then C.achievement_add("dread_kills") end
end)
ACT_USE.rift = function(e)
  hero_ref = e.hero or hero_ref
  if not hero_now() then return end
  C.achievement_add("rifts")
  local x, y = S.get("x", hero_ref, 0), S.get("y", hero_ref, 0)
  burst(fake_cast(x, y), x, y, 1.2 * (1 + C.legacy_rank("shadow_rift") * 0.1))
end
S.every(1, function()
  if not dread_on or S.get("current_hero") ~= C.hero or not hero_now() then return end
  pcall(function()
    local hx, hy = S.get("x", hero_ref, 0), S.get("y", hero_ref, 0)
    for _, en in ipairs(S.instances("obj_parent_enemy_NL") or {}) do
      if en["$x"] and (en["$x"] - hx) ^ 2 + (en["$y"] - hy) ^ 2 < 120 * 120 then
        S.debuff({ ["$inst"] = en["$inst"] }, "slow", 1.1, 20)
      end
    end
  end)
end)
S.every(1, function()
  if lr("gaze") <= 0 or S.get("current_hero") ~= C.hero or not hero_now() then return end
  pcall(function()
    local hx, hy = S.get("x", hero_ref, 0), S.get("y", hero_ref, 0)
    for _, en in ipairs(S.instances("obj_parent_enemy_NL") or {}) do
      if en["$x"] and (en["$x"] - hx) ^ 2 + (en["$y"] - hy) ^ 2 < 90 * 90 then
        S.debuff({ ["$inst"] = en["$inst"] }, "slow", 1.1, lr("gaze") * 4)
      end
    end
  end)
end)
-- its damage is elemental: right after each refresh of its stats (inside the game's, before any hit uses them), its
-- basic and elemental damage change places
S.on_stats(function(e)
  if e.hero_number ~= C.hero then return end
  for _, pair in ipairs{ { "final_min_basic_damage", "final_min_elemental_damage" },
      { "final_max_basic_damage", "final_max_elemental_damage" } } do
    local a, b = S.get(pair[1], e.hero, 0), S.get(pair[2], e.hero, 0)
    if a and b then S.set(pair[1], b, e.hero); S.set(pair[2], a, e.hero) end
  end
end)
