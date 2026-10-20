-- My Mod: a starting point for a SlormAPI Lua mod (see ../README.md).
-- Every function: https://github.com/tsotsas/SlormAPI/blob/main/docs/API.md#lua-reference

-- the name its settings are kept under (Options > Mods); the version comes from lua-pack.json
slorm.mod{ name = "My Mod", author = "you", description = "What your mod does, in one line." }

slorm.settings{
  { key = "greet", label = "Show a greeting in the main menu", type = "bool", default = true },
}

local function draw()
  if slorm.setting("greet") then
    slorm.ui("main", { slorm.text(1300, 260, "Hello from My Mod", "ft_standard_16") })
  else
    slorm.ui_remove("main")
  end
end

slorm.on("setting_changed", function(e)
  if e.mod == "My Mod" then draw() end
end)

draw()
print("My Mod ready")
