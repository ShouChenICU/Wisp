-- To-do: an input (focusing it opens the system on-screen keyboard) and a list;
-- the items are records in the collection "todos".
--
-- The scene gets the whole screen, rounded corners and all, and lays itself out
-- from wisp.device: the title and the input are inset by row_span() at their own
-- rows, and the list runs to the bottom edge, its rows passing through the
-- corners as they scroll. The list's side and bottom padding put its first row
-- (scrolled to the top) and its last row (scrolled to the end) where all four of
-- their corners are visible.
local todos = wisp.store.collection("todos")

local info = wisp.device.info()
local W, H = info.width, info.height
local MARGIN = 16 -- from the visible edge
local GAP = 8
local TITLE_H, INPUT_H, ROW_H = 32, 48, 56

-- How far in from the screen's sides rows y1..y2 must start for both of their ends
-- to be visible (the shape is convex: the narrowest row is one of the two ends).
local function inset(y1, y2)
  local need = 0
  for _, y in ipairs({ y1, y2 }) do
    local x1, x2 = wisp.device.row_span(y)
    if not x1 then return W // 2 end
    need = math.max(need, x1, W - 1 - x2)
  end
  return need
end

-- Content on rows y1..y2 inset to the visible edge plus the margin.
local function side_pad(y1, y2)
  local i = inset(y1, y2) + MARGIN
  return { 0, i, 0, i }
end

local TOP = math.max(GAP, info.safe_area.y1 // 2)
local input_y = TOP + TITLE_H + GAP
local list_y = input_y + INPUT_H + GAP

-- The list from list_y to the bottom edge: rows inset by `side`, and the bottom
-- padding (at least GAP) that lifts the last row until its corners are visible
-- (half a margin from the edge), keeping at least three rows in view at the end.
local side = inset(list_y, list_y + ROW_H - 1) + MARGIN
local max_pad = math.max(0, H - list_y - 3 * ROW_H)
local bottom = GAP
while bottom < max_pad and inset(H - bottom - ROW_H, H - 1 - bottom) + MARGIN // 2 > side do
  bottom = bottom + 2
end
side = math.max(side, inset(H - bottom - ROW_H, H - 1 - bottom) + MARGIN // 2)

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

return ui.screen { gap = GAP, pad = { TOP, 0, 0, 0 },
  ui.row { w = "fill", h = TITLE_H, justify = "center", align = "center",
    pad = side_pad(TOP, TOP + TITLE_H - 1),
    ui.label { font = "lg",
      text = bind(remaining, function(n) return "待办（还剩 " .. n .. " 项）" end) },
  },
  ui.row { w = "fill", h = INPUT_H, gap = 8, align = "center",
    pad = side_pad(input_y, input_y + INPUT_H - 1),
    ui.input { ref = "new", grow = 1, h = INPUT_H, placeholder = "新的待办", max_len = 200,
      text = draft, on_change = function(v) draft:set(v) end },
    ui.button { ref = "add", text = "添加", on_click = add },
  },
  ui.list { ref = "list", w = "fill", grow = 1, row_h = ROW_H, items = items,
    pad = { 0, side, bottom, side },
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
