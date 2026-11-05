-- To-do: an input (focusing it opens the system on-screen keyboard) and a list;
-- the items are records in the collection "todos".
local todos = wisp.store.collection("todos")

local function load()
  return todos:find(nil, { sort = "id" })
end

local items = state(load())
local draft = state("")

local function refresh()
  items:set(load())
end

local function add()
  local text = draft:get():match("^%s*(.-)%s*$")
  if text == "" then return end
  todos:insert({ text = text, done = false })
  draft:set("")
  refresh()
end

local remaining = computed(function()
  local n = 0
  for _, t in ipairs(items:get()) do
    if not t.done then n = n + 1 end
  end
  return n
end)

return ui.screen { gap = 8,
  ui.label { font = "lg",
    text = bind(remaining, function(n) return "待办（还剩 " .. n .. " 项）" end) },
  ui.row { w = "fill", gap = 8, align = "center",
    ui.input { ref = "new", grow = 1, h = 48, placeholder = "新的待办", max_len = 200,
      text = draft, on_change = function(v) draft:set(v) end },
    ui.button { ref = "add", text = "添加", on_click = add },
  },
  ui.list { ref = "list", w = "fill", grow = 1, row_h = 56, items = items,
    row = function(item)
      return ui.row { gap = 8, pad = 4, align = "center",
        ui.checkbox { grow = 1, text = item.text, checked = item.done,
          on_change = function(v)
            todos:update(item.id, { text = item.text, done = v })
            refresh()
          end },
        ui.button { w = 48, bg = "surface",
          on_click = function()
            todos:delete(item.id)
            refresh()
          end,
          ui.icon { name = "trash", fg = "danger" } },
      }
    end },
}
