-- Fails while building its tree: sim_smoke's scene_error expects main.lua:5.
local missing = nil

return ui.screen {
  ui.label { text = missing.text },
}
