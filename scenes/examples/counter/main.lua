-- Counter: the smallest complete scene (scene-runtime-core spec 4.1); wisp.store
-- remembers the count. The scene gets the whole screen; padding it by the safe
-- area (plus a margin) keeps this centred block visible on round and rounded
-- screens alike.
local count = state(wisp.store.get("count") or 0)

local info = wisp.device.info()
local sa = info.safe_area
local M = 16 -- a margin inside the safe area, which reaches the edges of a rect screen

local function save(n)
  count:set(n)
  wisp.store.set("count", n)
  wisp.log.info("count=" .. n)
end

return ui.screen { align = "center", justify = "center",
  pad = { sa.y1 + M, info.width - 1 - sa.x2 + M, info.height - 1 - sa.y2 + M, sa.x1 + M },
  ui.column { gap = 16, align = "center",
    -- A size in pixels: from 20 on, text is drawn from the vector font.
    ui.label { text = count, font = 96 },
    ui.label { text = bind(count, function(v) return "已点击 " .. v .. " 次" end), fg = "muted" },
    ui.row { gap = 12,
      ui.button { text = "清零", on_click = function() save(0) end },
      ui.button { ref = "plus", text = "+1", on_click = function() save(count:get() + 1) end },
    },
  },
}
