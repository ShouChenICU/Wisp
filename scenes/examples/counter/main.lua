-- 计数器：最小完整示例（scene-runtime-core spec 4.1），用 wisp.store 记住次数。
local count = state(wisp.store.get("count") or 0)

local function save(n)
  count:set(n)
  wisp.store.set("count", n)
  wisp.log.info("count=" .. n)
end

return ui.screen { align = "center", justify = "center",
  ui.column { gap = 16, align = "center",
    ui.label { text = bind(count, function(v) return "已点击 " .. v .. " 次" end), font = "xl" },
    ui.row { gap = 12,
      ui.button { text = "清零", on_click = function() save(0) end },
      ui.button { ref = "plus", text = "+1", on_click = function() save(count:get() + 1) end },
    },
  },
}
