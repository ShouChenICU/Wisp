-- System info: every field of wisp.device.info(), refreshed when the battery or
-- the language changes (the battery and lang events) and on every minute, which
-- also comes when the time zone changes. The scene gets the whole screen;
-- padding it by the safe area (plus a margin) keeps this centred block visible
-- on round and rounded screens alike.
local first = wisp.device.info()
local info = state(first)
local sa = first.safe_area
local M = 16 -- a margin inside the safe area, which reaches the edges of a rect screen

local function refresh(i) info:set(i) end
wisp.device.on("battery", refresh)
wisp.device.on("lang", refresh)
wisp.device.on("minute", refresh)

local SHAPES = { rect = "矩形", rounded_rect = "圆角矩形", circle = "圆形" }
local WIFI = { connected = "已连接", connecting = "连接中", backoff = "等待重试" }

-- "UTC+8", "UTC-3:30", "UTC+5:45".
local function zone(offset_min)
  local sign = offset_min < 0 and "-" or "+"
  local m = math.abs(offset_min)
  local text = "UTC" .. sign .. (m // 60)
  if m % 60 ~= 0 then text = text .. string.format(":%02d", m % 60) end
  return text
end

local function field(name, fn)
  return ui.row { w = "fill", gap = 12,
    ui.label { text = name, fg = "muted", w = 72 },
    ui.label { text = bind(info, fn), grow = 1 },
  }
end

return ui.screen { gap = 6, justify = "center",
  pad = { sa.y1 + M, first.width - 1 - sa.x2 + M, first.height - 1 - sa.y2 + M, sa.x1 + M },
  ui.label { text = "系统状态", font = "lg" },
  field("屏幕", function(i)
    return string.format("%d×%d %s", i.width, i.height, SHAPES[i.shape] or i.shape)
  end),
  field("圆角", function(i) return i.corner_radius .. " px" end),
  field("安全区", function(i)
    local s = i.safe_area
    return string.format("(%d,%d)-(%d,%d)", s.x1, s.y1, s.x2, s.y2)
  end),
  field("语言", function(i) return i.lang end),
  field("电量", function(i)
    if not i.battery then return "未知" end
    return i.battery .. "%" .. (i.charging and "（充电中）" or "")
  end),
  field("Wi-Fi", function(i) return WIFI[i.wifi] or "未连接" end),
  field("时间", function(i)
    if not i.time then return "未知" end
    local t = i.time + i.utc_offset * 60
    return string.format("%02d:%02d（%s）", t // 3600 % 24, t // 60 % 60, zone(i.utc_offset))
  end),
}
