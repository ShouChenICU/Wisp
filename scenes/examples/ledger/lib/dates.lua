-- 日期换算：场景里没有 os.date，这里按公历自己算。
local M = {}

-- 墙上时间（UTC 秒）在 UTC 偏移 offset_min 分钟处的本地日序号（自 1970-01-01 起的天数）。
function M.day_of(t, offset_min)
  return (t + offset_min * 60) // 86400
end

-- 日序号 -> 年、月、日（Howard Hinnant 的 civil_from_days）。
function M.civil(day)
  local z = day + 719468
  local era = z // 146097
  local doe = z - era * 146097
  local yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
  local y = yoe + era * 400
  local doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
  local mp = (5 * doy + 2) // 153
  local d = doy - (153 * mp + 2) // 5 + 1
  local m = mp < 10 and mp + 3 or mp - 9
  if m <= 2 then y = y + 1 end
  return y, m, d
end

function M.label(day)
  local _, m, d = M.civil(day)
  return string.format("%d月%d日", m, d)
end

return M
