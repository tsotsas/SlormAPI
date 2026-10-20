-- My Mod: a starting point for a SlormAPI Lua mod (see ../README.md). Its name, version, author and description
-- are in lua-pack.json. Every function: https://github.com/tsotsas/SlormAPI/blob/main/docs/API.md#lua-reference

slorm.settings{
  { key = "greet", label = "Show a greeting in the main menu", type = "bool", default = true },
}

local function draw()
  if slorm.setting("greet") then
    slorm.ui("main", { slorm.text(1300, 260, "Hello from " .. slorm.display_name(), "ft_standard_16") })
  else
    slorm.ui_remove("main")
  end
end

slorm.on("setting_changed", function(e)
  if e.mod == slorm.display_name() then draw() end
end)

draw()
print(slorm.display_name() .. " ready")
