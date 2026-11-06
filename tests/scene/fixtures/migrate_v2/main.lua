-- Version 2 of the data adds done = false to every item. The launch
-- arguments pick how the migration goes: mode = "boom" fails, "loop"
-- never returns, "none" defines no migrate at all.
print("v2 main")
-- A top-level write: during a migration it lands or rolls back with it.
wisp.store.set("v2_seen", true)
local args = wisp.scene.args() or {}
local mode = args.mode

if mode ~= "none" then
  scene.migrate = function(old)
    print("migrate from " .. old)
    if mode == "boom" then error("boom") end
    if mode == "loop" then while true do end end
    local items = wisp.store.collection("items")
    for _, r in ipairs(items:find()) do
      r.done = false
      items:update(r.id, r)
    end
  end
end

return ui.screen { ui.label { text = "v2" } }
