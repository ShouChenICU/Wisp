-- 记账：每一笔存在集合 entries 里，按日期降序列出，柱状图显示最近 7 天的支出。
local dates = wisp.import("lib/dates")
local entries = wisp.store.collection("entries")

-- 今天的日序号；设备还不知道时间时为 nil。
local function today()
  local i = wisp.device.info()
  return i.time and dates.day_of(i.time, i.utc_offset) or nil
end

local function load()
  return entries:find(nil, { sort = "day", desc = true, limit = 100 })
end

local list = state(load())
local amount = state("")

-- 最近 7 天每天的合计（元），最早的一天在左边。
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
    -- 不知道日期就不记，免得这一笔记到 1970 年。
    hint:set("设备还不知道日期，联网对时后再记")
    return
  end
  entries:insert({ day = day, amount = n, note = note })
  amount:set("")
  hint:set("")
  list:set(load())
end

-- 柱状图下面的日期：第 i 根柱子是 7 - i 天前，最右边是今天。
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
  ui.row { w = "fill", day_label(1), day_label(2), day_label(3), day_label(4), day_label(5),
    day_label(6), day_label(7) },
  ui.row { w = "fill", gap = 8, align = "center",
    ui.input { ref = "amount", grow = 1, h = 48, placeholder = "金额", max_len = 10,
      text = amount, on_change = function(v) amount:set(v) end },
    category("餐饮"),
    category("其他"),
  },
  ui.label { text = hint, font = "sm", fg = "warn", hidden = bind(hint, function(h) return h == "" end) },
  ui.list { w = "fill", grow = 1, row_h = 44, items = list,
    row = function(e)
      return ui.row { gap = 8, pad = 4, align = "center",
        ui.label { text = dates.label(e.day), fg = "muted", w = 80 },
        ui.label { text = e.note or "", grow = 1 },
        ui.label { text = yuan(e.amount) },
      }
    end },
}
