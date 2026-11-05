-- 时钟表盘：canvas 画指针表盘，每到整分钟（wisp.device.on("minute")）重画一次。
-- 画布铺满根节点留出的安全区，表盘大小按 safe_area 算，圆屏和圆角屏都整个可见。
-- 设备还没有时区设置，先按东八区显示。
local TZ_OFFSET = 8 * 3600
local WEEKDAYS = { "四", "五", "六", "日", "一", "二", "三" } -- 1970-01-01 是星期四

local info = wisp.device.info()
local now = state(info.time)

wisp.device.on("minute", function(i) now:set(i.time) end)

local sa = info.safe_area
local W, H = sa.x2 - sa.x1 + 1, sa.y2 - sa.y1 + 1
local CX, CY = W // 2, H // 2
local R = math.min(W, H) // 2 - 4

local function polar(angle_deg, r)
  local a = math.rad(angle_deg - 90) -- 0 度朝上，顺时针
  return CX + math.floor(r * math.cos(a) + 0.5), CY + math.floor(r * math.sin(a) + 0.5)
end

local function hand(g, angle, len, width, color)
  local x, y = polar(angle, len)
  g:line(CX, CY, x, y, { color = color, width = width })
end

local function draw(g)
  g:fill("bg")
  g:circle(CX, CY, R, { color = "surface", fill = true })
  for i = 0, 59 do
    local major = i % 5 == 0
    local x1, y1 = polar(i * 6, R - (major and R // 8 or R // 20))
    local x2, y2 = polar(i * 6, R - 4)
    g:line(x1, y1, x2, y2, { color = major and "text" or "muted", width = major and 4 or 1 })
  end
  local t = now:get()
  if not t then
    g:text(CX - 30, CY - 12, "--:--", { font = "lg", color = "muted" })
    return
  end
  local local_s = t + TZ_OFFSET
  local mins = local_s // 60 % 60
  local hours = local_s // 3600 % 24
  local day = local_s // 86400
  g:text(CX - 40, CY + R // 3, string.format("星期%s %02d:%02d", WEEKDAYS[day % 7 + 1], hours, mins),
         { font = "sm", color = "muted" })
  hand(g, (hours % 12) * 30 + mins / 2, R * 5 // 10, 8, "text")
  hand(g, mins * 6, R * 8 // 10, 5, "accent")
  g:circle(CX, CY, 8, { color = "accent", fill = true })
end

return ui.screen {
  ui.canvas { w = W, h = H, draw = draw },
}
