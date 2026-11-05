-- Ledger: each entry is a record in the collection "entries", listed newest day
-- first; a bar chart shows the spending of the last 7 days.
--
-- The scene gets the whole screen and lays itself out from wisp.device: every
-- band above the list (title, chart, dates, input, hint) has a fixed height, so
-- its rows are known and it is inset by row_span() there. The list runs to the
-- bottom edge; its side and bottom padding put its first and last rows, at the
-- two scroll ends, where all four of their corners are visible.
local dates = wisp.import("lib/dates")
local entries = wisp.store.collection("entries")

local info = wisp.device.info()
local W, H = info.width, info.height
local MARGIN = 16 -- from the visible edge
local GAP = 8
local TITLE_H, CHART_H, DAYS_H, INPUT_H, HINT_H, ROW_H = 32, 96, 20, 48, 20, 44

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

-- The bands from the top, each on its own rows: y and padding.
local TOP = math.max(GAP, info.safe_area.y1 // 2)
local band = {}
local y = TOP
for _, b in ipairs({ { "title", TITLE_H }, { "chart", CHART_H }, { "days", DAYS_H },
                     { "input", INPUT_H }, { "hint", HINT_H } }) do
  local i = inset(y, y + b[2] - 1) + MARGIN
  band[b[1]] = { 0, i, 0, i }
  y = y + b[2] + GAP
end
-- The date labels line up under the chart's bars.
band.days = band.chart
local list_y = y

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

-- Today's local day number; nil while the device does not know the time.
local function today()
  local i = wisp.device.info()
  return i.time and dates.day_of(i.time, i.utc_offset) or nil
end

local function load()
  return entries:find(nil, { sort = "day", desc = true, limit = 100 })
end

local list = state(load())
local amount = state("")

-- The total of each of the last 7 days, the oldest on the left.
local week = computed(function()
  local sums = { 0, 0, 0, 0, 0, 0, 0 }
  local last = today()
  if not last then return sums end
  for _, e in ipairs(list:get()) do
    local i = 7 - (last - e.day)
    if i >= 1 and i <= 7 then sums[i] = sums[i] + e.amount end
  end
  return sums
end)

local hint = state("")

local function add(note)
  local n = tonumber(amount:get())
  if not n or n <= 0 then
    hint:set("请先输入金额")
    return
  end
  local day = today()
  if not day then
    -- Without a date the entry would land on 1970-01-01: refuse it.
    hint:set("设备还不知道日期，联网对时后再记")
    return
  end
  entries:insert({ day = day, amount = n, note = note })
  amount:set("")
  hint:set("")
  list:set(load())
end

-- The dates under the chart: bar i is 7 - i days ago, today on the right.
local function day_label(i)
  return ui.column { grow = 1, align = "center",
    ui.label { font = "xs", fg = "muted", text = bind(week, function()
      local last = today()
      if not last then return "" end
      if i == 7 then return "今天" end
      local _, _, d = dates.civil(last - 7 + i)
      return d .. "日"
    end) },
  }
end

local function yuan(n)
  return string.format("%.2f", n)
end

local function category(name)
  return ui.button { text = name, on_click = function() add(name) end }
end

return ui.screen { gap = GAP, pad = { TOP, 0, 0, 0 },
  ui.row { w = "fill", h = TITLE_H, gap = 8, align = "center", pad = band.title,
    ui.label { text = "最近 7 天", font = "lg", grow = 1 },
    ui.label { fg = "muted", text = bind(week, function(s)
      local total = 0
      for _, v in ipairs(s) do total = total + v end
      return "合计 " .. yuan(total)
    end) },
  },
  ui.row { w = "fill", h = CHART_H, pad = band.chart,
    ui.chart { type = "bar", w = "fill", h = CHART_H,
      series = bind(week, function(s) return { s } end),
      max = bind(week, function(s)
        local m = 10
        for _, v in ipairs(s) do if v > m then m = v end end
        return m
      end) },
  },
  ui.row { w = "fill", h = DAYS_H, pad = band.days, day_label(1), day_label(2), day_label(3),
    day_label(4), day_label(5), day_label(6), day_label(7) },
  ui.row { w = "fill", h = INPUT_H, gap = 8, align = "center", pad = band.input,
    ui.input { ref = "amount", grow = 1, h = INPUT_H, placeholder = "金额", max_len = 10,
      text = amount, on_change = function(v) amount:set(v) end },
    category("餐饮"),
    category("其他"),
  },
  -- Always takes its rows, so the list below does not move when a hint shows.
  ui.row { w = "fill", h = HINT_H, align = "center", pad = band.hint,
    ui.label { text = hint, font = "sm", fg = "warn" },
  },
  ui.list { w = "fill", grow = 1, row_h = ROW_H, items = list,
    pad = { 0, side, bottom, side },
    row = function(e)
      return ui.row { gap = 8, pad = 4, align = "center",
        ui.label { text = dates.label(e.day), fg = "muted", w = 80 },
        ui.label { text = e.note or "", grow = 1 },
        ui.label { text = yuan(e.amount) },
      }
    end },
}
