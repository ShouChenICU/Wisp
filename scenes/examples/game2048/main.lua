-- 2048: swipe the 4x4 board to move tiles using on_swipe.
-- wisp.store persists the best score; undo restores the previous move.
-- The canvas draws tile backgrounds; labels render the numbers above them.
-- Every board layer handles swipes so gestures work anywhere on the board.
local N = 4
local CELL = 64          -- Cell size.
local TILE = CELL - 6    -- Leave a 3 px gap around each tile.
local BOARD = CELL * N

local function new_board()
  local b = {}
  for i = 1, N * N do b[i] = 0 end
  return b
end

local function copy(b)
  local c = {}
  for i = 1, #b do c[i] = b[i] end
  return c
end

local function add_tile(b)
  local free = {}
  for i = 1, #b do
    if b[i] == 0 then free[#free + 1] = i end
  end
  if #free == 0 then return end
  b[free[math.random(#free)]] = math.random(10) <= 1 and 4 or 2
end

local function fresh()
  local b = new_board()
  add_tile(b)
  add_tile(b)
  return b
end

-- Compress and merge a row to the left, returning the row and score gained.
local function slide(line)
  local packed = {}
  for _, v in ipairs(line) do
    if v ~= 0 then packed[#packed + 1] = v end
  end
  local out, gain, i = {}, 0, 1
  while i <= #packed do
    if i < #packed and packed[i] == packed[i + 1] then
      out[#out + 1] = packed[i] * 2
      gain = gain + packed[i] * 2
      i = i + 2
    else
      out[#out + 1] = packed[i]
      i = i + 1
    end
  end
  while #out < N do out[#out + 1] = 0 end
  return out, gain
end

-- Map row/column coordinates to board indices for each move direction.
local DIRS = {
  up    = function(r, c) return c * N + r + 1 end,
  down  = function(r, c) return c * N + (N - 1 - r) + 1 end,
  left  = function(r, c) return r * N + c + 1 end,
  right = function(r, c) return r * N + (N - 1 - c) + 1 end,
}

local function stuck(b)
  for i = 1, #b do
    if b[i] == 0 then return false end
    local r, c = (i - 1) // N, (i - 1) % N
    if c + 1 < N and b[i] == b[i + 1] then return false end
    if r + 1 < N and b[i] == b[i + N] then return false end
  end
  return true
end

local function top_tile(b)
  local t = 0
  for i = 1, #b do
    if b[i] > t then t = b[i] end
  end
  return t
end

-- Persist the game on close so the next session resumes it.
-- Remember reaching 2048 without replaying the win prompt.
local function load_game()
  local s = wisp.store.get("game")
  if type(s) ~= "table" then return {} end
  local b = {}
  for i = 1, N * N do
    local v = s.board and s.board[i]
    if type(v) ~= "number" or v < 0 then return {} end
    b[i] = v
  end
  return {
    board = b,
    score = math.max(0, math.floor(tonumber(s.score) or 0)),
    status = s.status == "lose" and "lose" or "",
    won = s.won and true or false,
  }
end

local saved = load_game()
local board = state(saved.board or fresh())
local score = state(saved.score or 0)
local best = state(wisp.store.get("best") or 0)
local status = state(saved.status or "") -- "" | "win" | "lose"
local undo = nil
local won = saved.won

local function save_game()
  wisp.store.set("game", {
    board = board:get(),
    score = score:get(),
    status = status:get(),
    won = won,
  })
  wisp.log.info("saved score=" .. score:get())
end

local function new_game()
  undo = nil
  won = false
  board:set(fresh())
  score:set(0)
  status:set("")
  save_game()
  wisp.log.info("new game")
end

local function move(dir)
  if status:get() == "lose" then return end
  local old = board:get()
  local b = copy(old)
  local f = DIRS[dir]
  local gain, changed = 0, false
  for r = 0, N - 1 do
    local line = {}
    for c = 0, N - 1 do line[c + 1] = b[f(r, c)] end
    local out, g = slide(line)
    gain = gain + g
    for c = 0, N - 1 do
      local i = f(r, c)
      if b[i] ~= out[c + 1] then changed = true end
      b[i] = out[c + 1]
    end
  end
  if not changed then return end
  undo = { b = old, s = score:get() }
  add_tile(b)
  board:set(b)
  if gain > 0 then
    local s = score:get() + gain
    score:set(s)
    if s > best:get() then
      best:set(s)
      wisp.store.set("best", s)
    end
  end
  if top_tile(b) >= 2048 and not won then
    won = true
    status:set("win")
  elseif stuck(b) then
    status:set("lose")
  end
  wisp.log.info("move " .. dir .. " score=" .. score:get())
end

local function undo_move()
  if not undo then return end
  board:set(undo.b)
  score:set(undo.s)
  status:set("")
  undo = nil
  wisp.log.info("undo")
end

-- Save when returning, closing, or moving to the background.
scene.on_hide = save_game
scene.on_close = save_game

local TILE_BG = {
  [0] = "#CDC1B4", [2] = "#EEE4DA", [4] = "#EDE0C8", [8] = "#F2B179",
  [16] = "#F59563", [32] = "#F67C5F", [64] = "#F65E3B", [128] = "#EDCF72",
  [256] = "#EDCC61", [512] = "#EDC850", [1024] = "#EDC53F", [2048] = "#EDC22E",
}

local function tile_bg(v)
  return TILE_BG[v] or "#B6A8D8"
end

-- Draw tile backgrounds; overlay labels supply the numbers.
local function draw(g)
  local b = board:get()
  g:fill("#BBADA0")
  for i = 0, N * N - 1 do
    local r, cc = i // N, i % N
    g:rect(cc * CELL + 3, r * CELL + 3, TILE, TILE,
      { color = tile_bg(b[i + 1]), fill = true, radius = 6 })
  end
end

local cells = {}
for i = 1, N * N do
  cells[i] = ui.stack {
    w = CELL, h = CELL, align = "center", justify = "center",
    on_swipe = move, -- Handle swipes that start and end on the same cell.
    ui.label {
      text = bind(board, function(b) return b[i] > 0 and tostring(b[i]) or "" end),
      fg = "#776E65", font = 24,
    },
  }
end

-- Centre the score row, board and hint vertically with equal space above and below.
-- This keeps the hint clear of the lower edge on round screens.
return ui.screen {
  bg = "#FAF3E8", align = "center", justify = "center", gap = 8,
  -- Score, best score and two action buttons.
  ui.row { gap = 8, align = "center",
    ui.label { text = bind(score, function(v) return "分 " .. v end), font = 16, fg = "#776E65" },
    ui.label { text = bind(best, function(v) return "最佳 " .. v end), font = 16, fg = "#776E65" },
    ui.button { text = "悔棋", font = 16, h = 30, on_click = undo_move },
    ui.button { text = "新局", font = 16, h = 30, on_click = new_game },
  },
  -- Board layers: backgrounds, numbers, then the win/lose overlay.
  -- Each layer handles swipes anywhere on the board.
  ui.stack { align = "center", justify = "center", on_swipe = move,
    ui.canvas { w = BOARD, h = BOARD, radius = 8, draw = draw, on_swipe = move },
    ui.grid { cols = N, gap = 0, on_swipe = move, table.unpack(cells) },
    ui.stack {
      w = BOARD, h = BOARD, radius = 8, bg = "#EEE4DA",
      align = "center", justify = "center",
      hidden = bind(status, function(s) return s == "" end),
      ui.column { gap = 10, align = "center",
        ui.label {
          text = bind(status, function(s) return s == "win" and "到 2048 了" or "没有路了" end),
          font = "lg", fg = "#776E65",
        },
        ui.row { gap = 8,
          ui.button {
            text = "继续", font = 16,
            hidden = bind(status, function(s) return s ~= "win" end),
            on_click = function() status:set("") end,
          },
          ui.button { text = "再来一局", font = 16, on_click = new_game },
        },
      },
    },
  },
  ui.label { text = "在棋盘上滑动", font = 16, fg = "muted" },
}
