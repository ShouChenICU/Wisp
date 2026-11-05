-- 记账：每一笔存在集合 entries 里，按日期降序列出，柱状图显示最近 7 天的支出。
local dates = wisp.import("lib/dates")
local entries = wisp.store.collection("entries")

local function today()
  local t = wisp.device.info().time
  return t and dates.day_of(t) or 0
end

local function load()
  return entries:find(nil, { sort = "day", desc = true, limit = 100 })
end

local list = state(load())
local amount = state("")

-- 最近 7 天每天的合计（元），最早的一天在左边。
local week = computed(function()
  local last = today()
  local sums = { 0, 0, 0, 0, 0, 0, 0 }
  for _, e in ipairs(list:get()) do
    local i = 7 - (last - e.day)
    if i >= 1 and i <= 7 then sums[i] = sums[i] + e.amount end
  end
  return sums
end)

local function add(note)
  local n = tonumber(amount:get())
  if not n or n <= 0 then return end
  entries:insert({ day = today(), amount = n, note = note })
  amount:set("")
  list:set(load())
end

local function yuan(n)
  return string.format("%.2f", n)
end

local function category(name)
  return ui.button { text = name, on_click = function() add(name) end }
end

return ui.screen { gap = 8,
  ui.row { w = "fill", gap = 8, align = "center",
    ui.label { text = "最近 7 天", font = "lg", grow = 1 },
    ui.label { fg = "muted", text = bind(week, function(s)
      local total = 0
      for _, v in ipairs(s) do total = total + v end
      return "合计 " .. yuan(total)
    end) },
  },
  ui.chart { type = "bar", w = "fill", h = 96,
    series = bind(week, function(s) return { s } end),
    max = bind(week, function(s)
      local m = 10
      for _, v in ipairs(s) do if v > m then m = v end end
      return m
    end) },
  ui.row { w = "fill", gap = 8, align = "center",
    ui.input { ref = "amount", grow = 1, h = 48, placeholder = "金额", max_len = 10,
      text = amount, on_change = function(v) amount:set(v) end },
    category("餐饮"),
    category("其他"),
  },
  ui.list { w = "fill", grow = 1, row_h = 44, items = list,
    row = function(e)
      return ui.row { gap = 8, pad = 4, align = "center",
        ui.label { text = dates.label(e.day), fg = "muted", w = 80 },
        ui.label { text = e.note or "", grow = 1 },
        ui.label { text = yuan(e.amount) },
      }
    end },
}
