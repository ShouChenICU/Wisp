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

-- Sizes come from the safe area, not fixed pixels, so it fits small and round screens.
local sa = wisp.device.info().safe_area
local side = math.min(sa.x2 - sa.x1 + 1, sa.y2 - sa.y1 + 1)
local DIAL = side * 3 // 5

return ui.screen { align = "center", justify = "center", gap = 16,
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
  -- The button row is as wide as the safe area; the three buttons share it.
  ui.row { w = "fill", gap = 8,
    ui.button { ref = "toggle", grow = 1,
      text = bind(running, function(r) return r and "暂停" or "开始" end),
      on_click = function() if running:get() then pause() else start() end end },
    ui.button { ref = "skip", grow = 1, text = "跳过", on_click = function() switch_phase(false) end },
    ui.button { ref = "reset", grow = 1, text = "重置", on_click = function()
      pause()
      left:set(PHASES[phase:get()].secs)
      save()
    end },
  },
}
