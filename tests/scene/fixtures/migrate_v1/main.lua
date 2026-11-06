-- Version 1 of the data: two items with only a name.
local items = wisp.store.collection("items")
if #items:find() == 0 then
  items:insert({ name = "a" })
  items:insert({ name = "b" })
end

return ui.screen { ui.label { text = "v1" } }
