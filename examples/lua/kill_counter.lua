-- Kill Counter: an example Lua mod for SlormAPI.
-- Copy it into <The Slormancer>\mods\lua\ - it loads right away, and reloads every time you save it.
-- It counts your kills (and bosses) this session and shows them in the pause menu, with a Reset button and a
-- setting in Options > Mods to hide it.

slorm.mod{
  name = "Kill Counter",
  version = "1.0",
  author = "thothas",
  description = "Counts your kills; shown in the pause menu",
}

local settings = slorm.settings{
  { key = "show", label = "Show the counter in the pause menu", type = "bool", default = true },
}

local kills, bosses = 0, 0

local function draw()
  if not slorm.setting("show") then
    slorm.ui_remove("main")
    return
  end
  slorm.ui("main", {
    slorm.text(1300, 120, "Kills this session: " .. kills, "ft_standard_16"),
    slorm.text(1300, 148, "Bosses: " .. bosses, "ft_standard_12", "grey"),
    slorm.button("reset", 1300, 176, "Reset", "Start counting from zero"),
  })
end

slorm.on("enemy_killed", function(e)
  kills = kills + 1
  if e.profile == "boss" then bosses = bosses + 1 end
  draw()
end)

slorm.on_click(function(id)
  if id == "reset" then
    kills, bosses = 0, 0
    draw()
  end
end)

slorm.on("setting_changed", function(e)
  if e.mod == "Kill Counter" then draw() end
end)

draw()
print("Kill Counter ready")
