// Small helpers shared by the pages.

import { useEffect, useState } from '../vendor/htm-preact-3.1.1.mjs';
import { locale } from './i18n/index.js';

/** True while the tab is visible; re-renders on visibility changes. */
export function useVisible() {
  const [visible, setVisible] = useState(!document.hidden);
  useEffect(() => {
    const onChange = () => setVisible(!document.hidden);
    document.addEventListener('visibilitychange', onChange);
    return () => document.removeEventListener('visibilitychange', onChange);
  }, []);
  return visible;
}

/** Date.now(), refreshed every `ms` while mounted. */
export function useNow(ms = 1000) {
  const [now, setNow] = useState(Date.now());
  useEffect(() => {
    const id = setInterval(() => setNow(Date.now()), ms);
    return () => clearInterval(id);
  }, [ms]);
  return now;
}

function unit(value, name) {
  return new Intl.NumberFormat(locale(), { style: 'unit', unit: name, unitDisplay: 'long' }).format(value);
}

/** A duration as its two largest units, e.g. "3 days 4 hours" / "3天 4小时". */
export function formatUptime(ms) {
  if (!Number.isFinite(ms) || ms < 0) return '—';
  const s = Math.floor(ms / 1000);
  const d = Math.floor(s / 86400);
  const h = Math.floor((s % 86400) / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = s % 60;
  if (d > 0) return `${unit(d, 'day')} ${unit(h, 'hour')}`;
  if (h > 0) return `${unit(h, 'hour')} ${unit(m, 'minute')}`;
  if (m > 0) return `${unit(m, 'minute')} ${unit(sec, 'second')}`;
  return unit(sec, 'second');
}

/** Bytes in kB/MB, with one decimal where it helps. */
export function formatBytes(n) {
  if (!Number.isFinite(n) || n < 0) return '—';
  const fmt = (v, name, digits) =>
    new Intl.NumberFormat(locale(), {
      style: 'unit',
      unit: name,
      unitDisplay: 'short',
      maximumFractionDigits: digits,
    }).format(v);
  if (n < 1024) return fmt(n, 'byte', 0);
  if (n < 1024 * 1024) return fmt(n / 1024, 'kilobyte', n < 10 * 1024 ? 1 : 0);
  return fmt(n / (1024 * 1024), 'megabyte', 1);
}

/** An integer with the locale's digit grouping. */
export function formatCount(n) {
  return new Intl.NumberFormat(locale()).format(n);
}

/** Milliseconds since boot as "H:MM:SS.mmm" (hours are not wrapped). */
export function formatClock(ms) {
  if (!Number.isFinite(ms) || ms <= 0) return '';
  const total = Math.floor(ms);
  const h = Math.floor(total / 3600000);
  const m = Math.floor((total % 3600000) / 60000);
  const s = Math.floor((total % 60000) / 1000);
  const frac = total % 1000;
  const p2 = (v) => String(v).padStart(2, '0');
  return `${h}:${p2(m)}:${p2(s)}.${String(frac).padStart(3, '0')}`;
}
