-- Pomodoro: 25 minutes of work, 5 of rest. The time left comes from the wall
-- clock (wisp.device.info().time), not from counting ticks: timers pause while
-- the screen is off and missed "every" ticks fire only once when it comes back,
-- so on_show recomputes it from the end time. Without a clock it falls back to
-- one second per tick.
local PHASES = {
  work = { secs = 25 * 60, title = "专注", next = "rest" },
  rest = { secs = 5 * 60, title = "休息", next = "work" },
}

-- Kept in wisp.store, so the timer carries on after the scene is closed and reopened.
local saved = wisp.store.get("pomodoro") or {}
local phase = state(PHASES[saved.phase] and saved.phase or "work")
local left = state(saved.left or PHASES[phase:get()].secs)
local running = state(false)
local ends_at = nil -- while running: the wall time (s) this phase ends
local done_count = state(wisp.store.get("done") or 0)

local function now()
  return wisp.device.info().time
end

local function save()
  wisp.store.set("pomodoro", {
    phase = phase:get(),
    left = left:get(),
    ends_at = running:get() and ends_at or nil,
  })
end

local function start()
  local t = now()
  ends_at = t and (t + left:get()) or nil
  running:set(true)
  save()
end

local function pause()
  running:set(false)
  ends_at = nil
  save()
end

-- finished: the phase ran out (a skipped work phase does not count as done).
local function switch_phase(finished)
  if finished and phase:get() == "work" then
    done_count:set(done_count:get() + 1)
    wisp.store.set("done", done_count:get())
  end
  phase:set(PHASES[phase:get()].next)
  left:set(PHASES[phase:get()].secs)
  wisp.log.info("phase=" .. phase:get())
  if running:get() then start() else save() end
end

local function sync()
  if not running:get() then return end
  local t = now()
  local rest
  if t and ends_at then
    rest = ends_at - t
  else
    rest = left:get() - 1
  end
  if rest <= 0 then
    switch_phase(true)
  else
    left:set(rest)
  end
end

-- It was running when the scene closed: carry on towards the same end time.
if saved.ends_at and now() then
  ends_at = saved.ends_at
  running:set(true)
  sync()
end

wisp.timer.every(1000, sync)
scene.on_show = sync
scene.on_close = save

local function mmss(s)
  return string.format("%02d:%02d", s // 60, s % 60)
end

-- The scene gets the whole screen and sizes itself from wisp.device: the dial is
-- as large as fits above the buttons, and the button row is inset by row_span()
-- at its own rows, so on a round screen it uses the width there is at that height.
local info = wisp.device.info()
local W, H = info.width, info.height
local MARGIN = 16 -- from the visible edge
local GAP, BTN_H = 16, 48
local BTN_ROW_MIN = 3 * 72 + 2 * 8 -- three buttons that still hold their text

local function visible(x, y)
  local x1, x2 = wisp.device.row_span(y)
  return x1 ~= nil and x >= x1 + MARGIN // 2 and x <= x2 - MARGIN // 2
end

-- The inset from both sides that keeps rows y1..y2 visible (the shape is convex:
-- the narrowest row is one of the two ends).
local function inset(y1, y2)
  local need = 0
  for _, y in ipairs({ y1, y2 }) do
    local x1, x2 = wisp.device.row_span(y)
    if not x1 then return W // 2 end
    need = math.max(need, x1, W - 1 - x2)
  end
  return need + MARGIN
end

-- The dial and the button row as one centred block, a margin clear of the top and
-- bottom edges: the largest dial whose ring is visible all round while the button
-- row below it is still wide enough.
local DIAL, TOP, btn_y
for d = math.min(W, H - GAP - BTN_H) - 2 * MARGIN, 64, -4 do
  TOP = (H - d - GAP - BTN_H) // 2
  btn_y = TOP + d + GAP
  local cx, cy, r = W // 2, TOP + d // 2, d // 2
  local ok = W - 2 * inset(btn_y, btn_y + BTN_H - 1) >= BTN_ROW_MIN
  for k = 0, 23 do
    if not ok then break end
    local a = k * math.pi / 12
    ok = visible(cx + math.floor(r * math.cos(a) + 0.5), cy + math.floor(r * math.sin(a) + 0.5))
  end
  DIAL = d
  if ok then break end
end
local side = inset(btn_y, btn_y + BTN_H - 1)

return ui.screen { gap = GAP, align = "center", pad = { TOP, 0, 0, 0 },
  ui.stack { w = DIAL, h = DIAL, align = "center",
    ui.arc { w = "fill", h = "fill", width = math.max(DIAL // 16, 4), min = 0,
      max = bind(phase, function(p) return PHASES[p].secs end),
      value = left,
      fg = bind(phase, function(p) return p == "work" and "accent" or "ok" end) },
    ui.column { gap = 4, align = "center",
      ui.label { text = bind(phase, function(p) return PHASES[p].title end), fg = "muted" },
      ui.label { text = bind(left, mmss), font = "xxl" },
      ui.label { text = bind(done_count, function(n) return "已完成 " .. n .. " 个" end),
                 font = "sm", fg = "muted" },
    },
  },
  -- As wide as the screen is at the row's height; the three buttons share it.
  ui.row { w = "fill", h = BTN_H, gap = 8, pad = { 0, side, 0, side },
    ui.button { ref = "toggle", grow = 1, h = BTN_H,
      text = bind(running, function(r) return r and "暂停" or "开始" end),
      on_click = function() if running:get() then pause() else start() end end },
    ui.button { ref = "skip", grow = 1, h = BTN_H, text = "跳过",
      on_click = function() switch_phase(false) end },
    ui.button { ref = "reset", grow = 1, h = BTN_H, text = "重置", on_click = function()
      pause()
      left:set(PHASES[phase:get()].secs)
      save()
    end },
  },
}
