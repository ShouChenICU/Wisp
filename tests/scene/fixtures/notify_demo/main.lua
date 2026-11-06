-- Posts one notification a second after it first goes to the background,
-- where a notification reaches the unread list as well as the banner (one
-- posted from the foreground only shows a banner). The simulator smoke test
-- (tests/smoke/sim_smoke.py) screenshots that banner and the pull-down panel.
local posted = false

scene.on_state = function(state)
  wisp.log.info("state=" .. state)
  if state ~= "background" or posted then return end
  posted = true
  wisp.timer.after(1000, function()
    local id, err = wisp.notify { title = "hi", body = "来自后台场景的通知" }
    wisp.log.info("notified=" .. tostring(id or err))
  end)
end

local info = wisp.device.info()
local sa = info.safe_area
return ui.screen { align = "center", justify = "center",
  pad = { sa.y1, info.width - 1 - sa.x2, info.height - 1 - sa.y2, sa.x1 },
  ui.label { text = "通知示例" },
}
