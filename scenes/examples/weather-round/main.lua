-- Weather uses the public Open-Meteo API (https://open-meteo.com), without a key.
-- Fetch on open and every 30 minutes; the top-right button refreshes manually.
-- Tapping the city cycles presets; wisp.store persists the selected city.
-- Current conditions and hourly temperatures precede a scrollable four-day forecast.
-- The device safe area keeps the content visible on round screens.
local info = wisp.device.info()
local W, H = info.width, info.height
local SA = info.safe_area

local CITIES = {
  { name = "深圳", lat = 22.5431, lon = 114.0579 },
  { name = "广州", lat = 23.1291, lon = 113.2644 },
  { name = "北京", lat = 39.9042, lon = 116.4074 },
  { name = "上海", lat = 31.2304, lon = 121.4737 },
  { name = "杭州", lat = 30.2741, lon = 120.1551 },
  { name = "成都", lat = 30.5728, lon = 104.0668 },
  { name = "西安", lat = 34.3416, lon = 108.9398 },
}

local function load_city()
  local i = wisp.store.get("city")
  if type(i) ~= "number" or i ~= math.floor(i) or i < 1 or i > #CITIES then
    return 1
  end
  return i
end

local city_i = state(load_city())

-- Data parsing.

local function num(v) return type(v) == "number" and v or nil end
local function str(v) return type(v) == "string" and v or nil end
local function at(t, i) return type(t) == "table" and t[i] or nil end

-- WMO weather codes map to descriptions and day/night icons.
local function describe(code, day)
  if code == nil then return "未知", "cloud", "muted" end
  if code == 0 then return "晴", day and "sun" or "moon", "warn" end
  if code <= 2 then return "多云", day and "cloud_sun" or "cloud_moon", "accent" end
  if code == 3 then return "阴", "cloud", "muted" end
  if code <= 48 then return "雾", "cloud", "muted" end
  if code <= 57 then return "毛毛雨", "cloud_rain", "accent" end
  if code <= 67 then return "雨", "cloud_rain", "accent" end
  if code <= 77 then return "雪", "snowflake", "accent" end
  if code <= 86 then return "阵雨", "cloud_rain", "accent" end
  return "雷雨", "bolt", "danger"
end

local WD = { "周日", "周一", "周二", "周三", "周四", "周五", "周六" }
local function weekday(y, m, d)
  if m < 3 then y = y - 1 end
  local off = { [1] = 0, [2] = 3, [3] = 2, [4] = 5, [5] = 0, [6] = 3, [7] = 5,
                [8] = 1, [9] = 4, [10] = 6, [11] = 2, [12] = 4 }
  return (y + y // 4 - y // 100 + y // 400 + off[m] + d) % 7
end

-- Forecast dates use local YYYY-MM-DD; labels depend on their list position.
local function day_label(date, k)
  if k == 1 then return "今天" end
  if k == 2 then return "明天" end
  if k == 3 then return "后天" end
  local y, m, d = date:match("^(%d+)-(%d+)-(%d+)$")
  if y then return WD[weekday(tonumber(y), tonumber(m), tonumber(d)) + 1] end
  return date:sub(6)
end

local function fmt_temp(v)
  v = num(v)
  return v and string.format("%d°", math.floor(v + 0.5)) or "--"
end

local function url_for(c)
  return string.format(
    "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
      .. "&current=temperature_2m,apparent_temperature,relative_humidity_2m"
      .. ",weather_code,wind_speed_10m,is_day"
      .. "&hourly=temperature_2m,precipitation_probability"
      .. "&daily=weather_code,temperature_2m_max,temperature_2m_min"
      .. ",precipitation_probability_max,uv_index_max,sunrise,sunset"
      .. "&timezone=auto&forecast_days=4&wind_speed_unit=kmh",
    c.lat, c.lon)
end

local ERR = {
  timeout = "网络超时，稍后再试",
  rate_limited = "刷新太频繁，稍后再试",
  permission_denied = "没有访问天气服务的权限",
  too_large = "数据太大，显示不了",
  dns = "解析天气服务失败",
  connect = "连不上天气服务",
  tls = "安全连接失败",
  deferred_timeout = "待机中网络没醒，稍后再试",
}

-- Reactive state.

local status = state("正在获取…")
local advice = state("")
local big_temp = state("--")
local big_desc = state("")
local big_icon = state("cloud")
local big_fg = state("muted")
local big_range = state("")
local feels = state("--")
local humid = state("--")
local wind = state("--")
local astro = state("")
local hours = state({})          -- Hourly temperatures for the chart, from 00:00 through 23:00.
local lo, hi = state(-10), state(45)
local days = state({})          -- Forecast rows.

local function set_day_rows(d, today)
  local out = {}
  for i = 1, 4 do
    local date = str(at(d.time, i))
    if date then
      local code = num(at(d.weather_code, i))
      local text, icon, fg = describe(code, true)
      local prob = num(at(d.precipitation_probability_max, i))
      out[#out + 1] = {
        label = day_label(date, i),
        icon = icon,
        fg = fg,
        desc = text,
        rain = prob and string.format("%d%%", math.floor(prob + 0.5)) or "",
        range = fmt_temp(at(d.temperature_2m_min, i)) .. " " .. fmt_temp(at(d.temperature_2m_max, i)),
      }
    end
  end
  days:set(out)
end

-- Build the chart from the full day of hourly temperatures.
local function set_hours(h, today)
  local vals, n = {}, 0
  local times = h.time
  if type(times) ~= "table" then return end
  for i = 1, #times do
    if str(times[i]) == today .. "T00:00" then
      for k = i, math.min(i + 23, #times) do
        local v = num(at(h.temperature_2m, k))
        if v then
          n = n + 1
          vals[n] = v
        end
      end
      break
    end
  end
  if n < 2 then return end
  local mn, mx = vals[1], vals[1]
  for _, v in ipairs(vals) do
    if v < mn then mn = v end
    if v > mx then mx = v end
  end
  lo:set(mn - 2)
  hi:set(mx + 2)
  hours:set(vals)
end

local function set_advice(prob, uv, t)
  if prob and prob >= 70 then
    advice:set("今天大概率有雨，出门带伞")
  elseif prob and prob >= 40 then
    advice:set("可能有雨，最好带把伞")
  elseif uv and uv >= 8 then
    advice:set("紫外线很强，注意防晒")
  elseif t and t >= 33 then
    advice:set("天气炎热，记得多补水")
  elseif t and t <= 5 then
    advice:set("天冷，注意保暖")
  else
    advice:set("")
  end
end

local function fetch()
  local idx = city_i:get()
  status:set("正在获取…")
  wisp.http.get(url_for(CITIES[idx]), nil, function(err, res)
    if idx ~= city_i:get() then return end -- Ignore responses for a city that is no longer selected.
    if err then
      status:set(ERR[err] or "网络不可用，稍后再试")
      return
    end
    local j = res.json
    if res.status ~= 200 or type(j) ~= "table" then
      status:set("天气服务暂时不可用，稍后再试")
      return
    end
    local c = type(j.current) == "table" and j.current or {}
    local code = num(c.weather_code)
    local day = c.is_day ~= 0
    local text, icon, fg = describe(code, day)
    big_temp:set(fmt_temp(c.temperature_2m))
    big_desc:set(text)
    big_icon:set(icon)
    big_fg:set(fg)
    feels:set(fmt_temp(c.apparent_temperature))
    humid:set(num(c.relative_humidity_2m) and (string.format("%d%%", math.floor(c.relative_humidity_2m + 0.5))) or "--")
    wind:set(num(c.wind_speed_10m) and (string.format("%.0f km/h", c.wind_speed_10m)) or "--")

    local d = type(j.daily) == "table" and j.daily or {}
    local today = (str(c.time) or ""):sub(1, 10)
    local mx = num(at(d.temperature_2m_max, 1))
    local mn = num(at(d.temperature_2m_min, 1))
    if mx or mn then
      big_range:set((mn and string.format("%d°", math.floor(mn + 0.5)) or "--")
        .. " ~ " .. (mx and string.format("%d°", math.floor(mx + 0.5)) or "--"))
    else
      big_range:set("")
    end
    local sr, ss = str(at(d.sunrise, 1)), str(at(d.sunset, 1))
    if sr and ss then astro:set("日出 " .. sr:sub(12, 16) .. " · 日落 " .. ss:sub(12, 16)) end
    set_day_rows(d, today)
    set_hours(type(j.hourly) == "table" and j.hourly or {}, today)
    set_advice(num(at(d.precipitation_probability_max, 1)), num(at(d.uv_index_max, 1)), num(c.temperature_2m))

    local t = str(c.time)
    if not t then
      status:set("已更新")
    elseif res.deferred then
      status:set("上次更新 " .. t:sub(12, 16))
    else
      status:set("更新于 " .. t:sub(12, 16))
    end
    wisp.log.info("ok " .. CITIES[idx].name .. " " .. big_temp:get())
  end)
end

local function next_city()
  local i = city_i:get() % #CITIES + 1
  city_i:set(i)
  wisp.store.set("city", i)
  days:set({})
  hours:set({})
  advice:set("")
  big_range:set("")
  astro:set("")
  wisp.log.info("city -> " .. CITIES[i].name)
  fetch()
end

fetch()
wisp.timer.every(30 * 60 * 1000, fetch)

-- Interface.

local function stat_cell(label, value)
  return ui.column { w = "fill", gap = 2, align = "center",
    ui.label { text = label, font = "xs", fg = "muted" },
    ui.label { text = value, font = "md", fg = "text" },
  }
end

local function day_row(d)
  return ui.row { w = "fill", h = 46, gap = 10, align = "center",
    ui.label { text = d.label, font = "sm", fg = "text" },
    ui.icon { name = d.icon, font = 20, fg = d.fg },
    ui.label { text = d.desc, font = "xs", fg = "muted", grow = 1 },
    ui.label { text = d.rain, font = "xs", fg = "accent" },
    ui.label { text = d.range, font = "sm", fg = "text" },
  }
end

return ui.screen { bg = "bg",
  pad = { SA.y1, W - 1 - SA.x2, H - 1 - SA.y2, SA.x1 },
  ui.column { w = "fill", grow = 1, gap = 10,
    -- Header: tap the city to cycle presets, or refresh manually.
    ui.row { w = "fill", gap = 8, align = "center",
      ui.icon { name = "map_marker", font = 18, fg = "accent" },
      ui.label { text = bind(city_i, function(i) return CITIES[i].name end),
        font = 20, fg = "text", on_click = next_city },
      ui.label { text = "换", font = "xs", fg = "muted", on_click = next_city },
      ui.spacer { grow = 1 },
      ui.icon { name = "refresh", font = 20, fg = "accent", on_click = fetch },
    },
    -- Current conditions.
    ui.row { w = "fill", gap = 10, align = "center",
      ui.icon { name = big_icon, font = 44, fg = big_fg },
      ui.label { text = big_temp, font = 56, fg = "text" },
      ui.column { gap = 2,
        ui.label { text = big_desc, font = "md", fg = "text" },
        ui.label { text = big_range, font = "sm", fg = "muted" },
      },
    },
    ui.row { w = "fill",
      stat_cell("体感", feels), stat_cell("湿度", humid), stat_cell("风", wind),
    },
    -- Bottom padding keeps the last forecast row visible at the end of scrolling.
    ui.scroll { w = "fill", grow = 1, gap = 8, pad = { 0, 0, 26, 0 },
      ui.row { w = "fill", justify = "between",
        ui.label { text = "今天逐时气温", font = "xs", fg = "muted" },
        ui.label { text = status, font = "xs", fg = "muted" },
      },
      ui.chart { w = "fill", h = 72, type = "line", series = bind(hours, function(v) return { v } end),
        min = lo, max = hi, bg = "surface", radius = 8 },
      ui.row { w = "fill", justify = "between",
        ui.label { text = "0时", font = "xs", fg = "muted" },
        ui.label { text = "6时", font = "xs", fg = "muted" },
        ui.label { text = "12时", font = "xs", fg = "muted" },
        ui.label { text = "18时", font = "xs", fg = "muted" },
        ui.label { text = "23时", font = "xs", fg = "muted" },
      },
      ui.label { text = advice, w = "fill", font = "xs", fg = "warn",
        hidden = bind(advice, function(t) return t == "" end) },
      ui.label { text = astro, w = "fill", font = "xs", fg = "muted",
        hidden = bind(astro, function(t) return t == "" end) },
      ui.label { text = "未来几天", font = "xs", fg = "muted" },
      ui.list { w = "fill", h = 4 * 46, items = days, row = day_row, row_h = 46 },
      ui.spacer { h = 6 },
    },
  },
}
