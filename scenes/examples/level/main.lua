-- Level: a bubble on a dial and the tilt from flat in large digits, from the
-- accelerometer (wisp.sensor, 25 Hz). Lying flat, the IMU feels gravity along
-- its z axis only; the tilt is the angle between that axis and gravity, and
-- the bubble floats towards the higher edge, the way it does in a spirit
-- level. Which way is "up" on the screen follows the IMU's own x and y axes.
-- Sensors are for the scene in front only: the subscription pauses by itself
-- while the screen is off.
--
-- The scene gets the whole screen and sizes itself from wisp.device: the dial
-- and the angle below it are one centred block, with the dial as large as it
-- can be while its ring and the angle's row stay visible (row_span() at their
-- own rows), so a round screen gets the dial it has room for.
local info = wisp.device.info()
local W, H = info.width, info.height
local MARGIN = 16 -- from the visible edge
local GAP = 8
local LABEL_H = 64 -- the angle in "xxl" (48 px) with its line spacing
local LABEL_W = 160 -- "180°" and a little more

local function visible(x, y)
  local x1, x2 = wisp.device.row_span(y)
  return x1 ~= nil and x >= x1 + MARGIN // 2 and x <= x2 - MARGIN // 2
end

local DIAL, TOP
for d = math.min(W, H - GAP - LABEL_H) - 2 * MARGIN, 64, -4 do
  TOP = (H - d - GAP - LABEL_H) // 2
  local cx, cy, r = W // 2, TOP + d // 2, d // 2
  local y1, y2 = TOP + d + GAP, TOP + d + GAP + LABEL_H - 1
  local ok = visible(cx - LABEL_W // 2, y1) and visible(cx + LABEL_W // 2, y1)
    and visible(cx - LABEL_W // 2, y2) and visible(cx + LABEL_W // 2, y2)
  for k = 0, 23 do
    if not ok then break end
    local a = k * math.pi / 12
    ok = visible(cx + math.floor(r * math.cos(a) + 0.5), cy + math.floor(r * math.sin(a) + 0.5))
  end
  DIAL = d
  if ok then break end
end

-- x, y: where the bubble sits, -1..1 of the dial's reach; angle in whole degrees.
local tilt = state({ x = 0, y = 0, angle = 0 })
local note = state(nil) -- why there is no reading, if there is none

local function draw(g)
  local t = tilt:get()
  local c, r = DIAL // 2, DIAL // 2 - 2
  g:fill("bg")
  g:circle(c, c, r, { color = "surface", fill = true })
  g:circle(c, c, r * 2 // 3, { color = "muted" })
  g:circle(c, c, r // 3, { color = "muted" })
  g:line(c - r, c, c + r, c, { color = "muted" })
  g:line(c, c - r, c, c + r, { color = "muted" })
  local br = math.max(r // 6, 8)
  local reach = r - br - 2
  local bx = c + math.floor(t.x * reach + 0.5)
  local by = c - math.floor(t.y * reach + 0.5)
  g:circle(bx, by, br, { color = t.angle <= 1 and "ok" or "accent", fill = true })
end

local logged = nil

-- One batch of samples (every 100 ms): their mean, in m/s².
local function on_samples(samples)
  local n = #samples
  if n == 0 then return end
  local sx, sy, sz = 0, 0, 0
  for _, s in ipairs(samples) do
    sx, sy, sz = sx + s.x, sy + s.y, sz + s.z
  end
  sx, sy, sz = sx / n, sy / n, sz / n
  local g = math.sqrt(sx * sx + sy * sy + sz * sz)
  if g < 1 then return end -- falling, or no reading: nothing to level against
  local angle = math.floor(math.deg(math.atan(math.sqrt(sx * sx + sy * sy), sz)) + 0.5)
  if angle ~= logged then
    logged = angle
    wisp.log.info("angle=" .. angle)
  end
  -- The bubble rises against gravity's pull in the screen's plane.
  local x, y = sx / g, sy / g
  local cur = tilt:get()
  if angle ~= cur.angle or math.abs(x - cur.x) > 0.01 or math.abs(y - cur.y) > 0.01 then
    tilt:set({ x = x, y = y, angle = angle })
  end
end

if wisp.sensor.available().accel then
  local h, err = wisp.sensor.subscribe("accel", { rate = 25 }, on_samples)
  if not h then note:set("加速度传感器不可用（" .. tostring(err) .. "）") end
else
  note:set("没有加速度传感器")
end

return ui.screen { gap = GAP, align = "center", pad = { TOP, 0, 0, 0 },
  ui.canvas { w = DIAL, h = DIAL, draw = draw },
  ui.label { font = "xxl", hidden = bind(note, function(n) return n ~= nil end),
             text = bind(tilt, function(t) return t.angle .. "°" end) },
  ui.label { fg = "muted", hidden = bind(note, function(n) return n == nil end),
             text = bind(note, function(n) return n or "" end) },
}
