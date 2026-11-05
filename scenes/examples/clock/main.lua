-- Clock face: a canvas dial, redrawn on every whole minute (wisp.device.on("minute")),
-- with the time in large digits over its lower third.
-- The scene gets the whole screen: the canvas covers it, and the dial is the
-- largest circle centred on it (on a round screen, the screen itself), sized from
-- width and height. A centred circle that fits the width and height is visible on
-- a rounded screen too, whose corners it never reaches, and so is anything well
-- inside it.
-- Local time is info.time + info.utc_offset (minutes); a time zone change
-- fires "minute" too.
local WEEKDAYS = { "四", "五", "六", "日", "一", "二", "三" } -- 1970-01-01 was a Thursday

local info = wisp.device.info()
-- The whole info table: a new time or a new time zone both redraw.
local now = state(info)

wisp.device.on("minute", function(i) now:set(i) end)

local W, H = info.width, info.height
local CX, CY = W // 2, H // 2
local R = math.min(W, H) // 2 - 2

local function polar(angle_deg, r)
  local a = math.rad(angle_deg - 90) -- 0 degrees is up, clockwise
  return CX + math.floor(r * math.cos(a) + 0.5), CY + math.floor(r * math.sin(a) + 0.5)
end

-- Hours, minutes and days since 1970-01-01 in local time.
local function local_time(i)
  local local_s = i.time + i.utc_offset * 60
  return local_s // 3600 % 24, local_s // 60 % 60, local_s // 86400
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
  local i = now:get()
  if not i.time then return end
  local hours, mins = local_time(i)
  hand(g, (hours % 12) * 30 + mins / 2, R * 5 // 10, 8, "text")
  hand(g, mins * 6, R * 8 // 10, 5, "accent")
  g:circle(CX, CY, 8, { color = "accent", fill = true })
end

-- The digits and the weekday as labels stacked over the canvas, from a third of
-- the radius below the centre down, on a plate in the "surface" colour -- the
-- same token the dial's fill uses, though the canvas quantises colour and the
-- dial reads back as a slightly different shade: the hands pass under the
-- plate instead of through the digits.
return ui.screen {
  ui.stack { w = W, h = H,
    ui.canvas { w = W, h = H, draw = draw },
    ui.column { w = W, h = H, align = "center", pad = { CY + R // 3, 0, 0, 0 },
      ui.column { align = "center", gap = 0, bg = "surface", radius = 8, pad = { 0, 8, 4, 8 },
        ui.label { font = 56, text = bind(now, function(i)
          if not i.time then return "--:--" end
          local hours, mins = local_time(i)
          return string.format("%02d:%02d", hours, mins)
        end) },
        ui.label { font = "sm", fg = "muted", text = bind(now, function(i)
          if not i.time then return "" end
          local _, _, day = local_time(i)
          return "星期" .. WEEKDAYS[day % 7 + 1]
        end) },
      },
    },
  },
}
