-- 天气：Open-Meteo（https://open-meteo.com）的公开接口，不需要 Key。
-- 只能访问 scene.json 里 permissions.http 声明的主机。打开时取一次，之后每 30 分钟一次。
local CITY = { name = "北京", lat = 39.90, lon = 116.40 }
local URL = string.format(
  "https://api.open-meteo.com/v1/forecast?latitude=%.2f&longitude=%.2f"
    .. "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
    .. "&daily=weather_code,temperature_2m_max,temperature_2m_min"
    .. "&timezone=auto&forecast_days=3",
  CITY.lat, CITY.lon)

-- WMO 天气代码（Open-Meteo 文档里的表）。
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

local status = state("正在获取…")
local now = state(nil)
local days = state({})

local function fetch()
  status:set("正在获取…")
  wisp.http.get(URL, nil, function(err, res)
    if err then
      status:set("获取失败：" .. err)
      return
    end
    local j = res.json
    if res.status ~= 200 or type(j) ~= "table" or type(j.current) ~= "table" then
      status:set("获取失败：HTTP " .. tostring(res.status))
      return
    end
    now:set(j.current)
    local out = {}
    local d = j.daily or {}
    for i, date in ipairs(d.time or {}) do
      out[#out + 1] = {
        date = date:sub(6),
        code = (d.weather_code or {})[i] or 0,
        max = (d.temperature_2m_max or {})[i] or 0,
        min = (d.temperature_2m_min or {})[i] or 0,
      }
    end
    days:set(out)
    status:set("更新于 " .. tostring(j.current.time):sub(12))
  end)
end

fetch()
wisp.timer.every(30 * 60 * 1000, fetch)

local function day_row(i)
  return ui.row { w = "fill", gap = 8,
    hidden = bind(days, function(d) return d[i] == nil end),
    ui.label { w = 64, fg = "muted", text = bind(days, function(d) return d[i] and d[i].date or "" end) },
    ui.label { grow = 1, text = bind(days, function(d) return d[i] and describe(d[i].code) or "" end) },
    ui.label { text = bind(days, function(d)
      return d[i] and string.format("%.0f° / %.0f°", d[i].min, d[i].max) or ""
    end) },
  }
end

return ui.screen { gap = 8, align = "center", justify = "center",
  ui.row { gap = 8, align = "center",
    ui.label { text = CITY.name, font = "lg" },
    ui.button { ref = "refresh", bg = "surface", on_click = fetch, ui.icon { name = "refresh" } },
  },
  ui.label { font = "xxl", text = bind(now, function(c)
    return c and string.format("%.0f°", c.temperature_2m) or "--"
  end) },
  ui.label { text = bind(now, function(c)
    if not c then return "" end
    return string.format("%s  湿度 %d%%  风速 %.0f km/h", describe(c.weather_code),
                         c.relative_humidity_2m, c.wind_speed_10m)
  end) },
  ui.column { w = "fill", gap = 4, day_row(1), day_row(2), day_row(3) },
  ui.label { text = status, font = "sm", fg = "muted" },
}
