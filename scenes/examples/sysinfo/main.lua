-- 系统状态：wisp.device.info() 的各项，电量和语言变化时（battery / lang 事件）
-- 刷新，整分钟时顺带刷新时间。
local info = state(wisp.device.info())

local function refresh(i) info:set(i) end
wisp.device.on("battery", refresh)
wisp.device.on("lang", refresh)
wisp.device.on("minute", refresh)

local SHAPES = { rect = "矩形", rounded_rect = "圆角矩形", circle = "圆形" }
local WIFI = { connected = "已连接", connecting = "连接中", backoff = "等待重试" }

local function field(name, fn)
  return ui.row { w = "fill", gap = 12,
    ui.label { text = name, fg = "muted", w = 72 },
    ui.label { text = bind(info, fn), grow = 1 },
  }
end

return ui.screen { gap = 6, justify = "center",
  ui.label { text = "系统状态", font = "lg" },
  field("屏幕", function(i)
    return string.format("%d×%d %s", i.width, i.height, SHAPES[i.shape] or i.shape)
  end),
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
    return "UTC " .. string.format("%02d:%02d", i.time // 3600 % 24, i.time // 60 % 60)
  end),
}
