-- A board moved by swipes: on_swipe sits on the stack that holds the
-- canvas and the widgets drawn over it, so a swipe anywhere on the board
-- (also over the label or the button) moves it. A tap on the button only
-- counts taps.
local last = state("none")
local swipes = state(0)
local taps = state(0)

local d = wisp.device.info()
local s = d.safe_area

return ui.screen {
  align = "center", justify = "center",
  pad = { s.y1, d.width - 1 - s.x2, d.height - 1 - s.y2, s.x1 },
  ui.stack { ref = "board", w = "fill", h = "fill", align = "center",
    on_swipe = function(dir)
      last:set(dir)
      swipes:set(swipes:get() + 1)
    end,
    ui.canvas { ref = "grid", draw = function(g) g:fill("surface") end },
    ui.column { align = "center", gap = 12,
      ui.label { ref = "status",
        text = computed(function() return "swipe " .. last:get() .. " " .. swipes:get() end) },
      ui.button { ref = "tap", text = "tap", w = 160, h = 64,
        on_click = function() taps:set(taps:get() + 1) end },
      ui.label { text = bind(taps, function(v) return "taps " .. v end) },
    },
  },
}
