-- Weather: the public Open-Meteo API (https://open-meteo.com), which needs no key.
-- A scene may only reach the hosts scene.json declares in permissions.http.
-- Fetched on open and then every 30 minutes.
local CITY = { name = "北京", lat = 39.90, lon = 116.40 }
local URL = string.format(
  "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
    .. "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
    .. "&daily=weather_code,temperature_2m_max,temperature_2m_min"
    .. "&timezone=auto&forecast_days=3",
  CITY.lat, CITY.lon)

-- WMO weather codes (the table in the Open-Meteo docs).
local function describe(code)
  if code == 0 then return "晴" end
  if code <= 2 then return "多云" end
  if code == 3 then return "阴" end
  if code <= 48 then return "雾" end
  if code <= 57 then return "毛毛雨" end
  if code <= 67 then return "雨" end
  if code <= 77 then return "雪" end
  if code <= 82 then return "阵雨" end
  if code <= 86 then return "阵雪" end
  return "雷雨"
end

-- wisp.http's error codes, in words for the user.
local ERRORS = {
  timeout = "网络超时，稍后再试",
  rate_limited = "刷新太频繁，稍后再试",
  permission_denied = "没有访问天气服务的权限",
  too_large = "天气数据太大，无法显示",
}

-- Missing data comes as null (nil in Lua) or another type: every field's type
-- is checked before it is used.
local function num(v)
  return type(v) == "number" and v or nil
end

local function str(v)
  return type(v) == "string" and v or nil
end

local function at(list, i)
  return type(list) == "table" and list[i] or nil
end

local function temp(v)
  v = num(v)
  return v and string.format("%.0f°", v) or "--"
end

local status = state("正在获取…")
local now = state(nil)
local days = state({})

local function fetch()
  status:set("正在获取…")
  wisp.http.get(URL, nil, function(err, res)
    if err then
      status:set(ERRORS[err] or "网络不可用，稍后再试")
      return
    end
    local j = res.json
    if res.status ~= 200 or type(j) ~= "table" then
      status:set("天气服务暂时不可用，稍后再试")
      return
    end
    local c = type(j.current) == "table" and j.current or {}
    now:set({
      temp = num(c.temperature_2m),
      humidity = num(c.relative_humidity_2m),
      code = num(c.weather_code),
      wind = num(c.wind_speed_10m),
    })
    local d = type(j.daily) == "table" and j.daily or {}
    local out = {}
    for i = 1, 3 do
      local date = str(at(d.time, i))
      if date then
        out[#out + 1] = {
          date = date:sub(6),
          code = num(at(d.weather_code, i)),
          max = at(d.temperature_2m_max, i),
          min = at(d.temperature_2m_min, i),
        }
      end
    end
    days:set(out)
    local time = str(c.time)
    status:set(time and ("更新于 " .. time:sub(12)) or "已更新")
  end)
end

fetch()
wisp.timer.every(30 * 60 * 1000, fetch)

local function describe_or_none(code)
  return code and describe(code) or "暂无数据"
end

local function day_row(i)
  return ui.row { w = "fill", gap = 8,
    hidden = bind(days, function(d) return d[i] == nil end),
    ui.label { w = 64, fg = "muted", text = bind(days, function(d) return d[i] and d[i].date or "" end) },
    ui.label { grow = 1, text = bind(days, function(d)
      return d[i] and describe_or_none(d[i].code) or ""
    end) },
    ui.label { text = bind(days, function(d)
      return d[i] and (temp(d[i].min) .. " / " .. temp(d[i].max)) or ""
    end) },
  }
end

return ui.screen { gap = 8, align = "center", justify = "center",
  ui.row { gap = 8, align = "center",
    ui.label { text = CITY.name, font = "lg" },
    ui.button { ref = "refresh", bg = "surface", on_click = fetch, ui.icon { name = "refresh" } },
  },
  ui.label { font = "xxl", text = bind(now, function(c) return c and temp(c.temp) or "--" end) },
  ui.label { text = bind(now, function(c)
    if not c then return "" end
    local parts = { describe_or_none(c.code) }
    if c.humidity then parts[#parts + 1] = string.format("湿度 %.0f%%", c.humidity) end
    if c.wind then parts[#parts + 1] = string.format("风速 %.0f km/h", c.wind) end
    return table.concat(parts, "  ")
  end) },
  ui.column { w = "fill", gap = 4, day_row(1), day_row(2), day_row(3) },
  ui.label { text = status, font = "sm", fg = "muted" },
}
