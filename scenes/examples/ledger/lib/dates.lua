-- Date arithmetic: scenes have no os.date, so the Gregorian calendar is done here.
local M = {}

-- The local day number (days since 1970-01-01) of wall time t (UTC seconds)
-- at offset_min minutes east of UTC.
function M.day_of(t, offset_min)
  return (t + offset_min * 60) // 86400
end

-- Day number -> year, month, day (Howard Hinnant's civil_from_days).
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
