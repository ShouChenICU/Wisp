// Live logs (spec 7.3). Streaming is opt-in: the page subscribes to "log"
// only after the user presses start, and stops again when the user leaves
// the page or the tab goes to the background, so an idle browser never
// keeps the device busy formatting log lines for it.
//
// The device replays its recent log ring first, then streams new lines --
// on every subscription, so each Start after a Stop and each reconnect
// replays lines this page already shows. Timestamps are ms since boot
// (spec 6.2), so a replayed line no newer than the newest line already
// seen is a repeat and is dropped; so is a live line that the replay just
// delivered (the device may send a line written during the replay twice).
// A reboot resets the device's clock, which would make every new line look
// old: after each (re)subscription the page holds incoming lines until
// /device's uptime_ms says whether the clock went back, and starts over if
// it did. At most MAX_LINES are kept. The line list is written straight to the DOM
// (text nodes only -- log text is untrusted) in one batch per animation
// frame instead of being diffed by Preact, so a chatty device stays smooth.

import { Component, html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api } from '../api.js';
import { subscribe } from '../ws.js';
import { t, useLang } from '../i18n/index.js';
import { Badge } from '../components/badge.js';
import { IconArrowDown, IconInfo, IconLogs, IconPause, IconPlay, IconSearch, IconStop, IconTrash } from '../icons.js';
import { formatClock, formatCount } from '../util.js';

const MAX_LINES = 2000;
const RANK = { D: 0, I: 1, W: 2, E: 3 };
const FILTERS = [
  { id: 'D', label: 'logs.level_all' },
  { id: 'I', label: 'logs.level_info' },
  { id: 'W', label: 'logs.level_warn' },
  { id: 'E', label: 'logs.level_error' },
];
// Replayed ring lines arrive as {ts:0, level:"I", tag:"", msg:<raw line>}
// with the raw line formatted "<ts> <L> <tag>: <msg>" by the device's log
// module; parsing it back recovers the real time, level and tag.
const RING_LINE = /^(\d+) ([DIWE]) ([^:]*): ([\s\S]*)$/;
// How close to the bottom still counts as "following" the stream.
const STICK_PX = 32;
// A live line repeating a replayed one carries a slightly later stamp (the
// ring and the stream read the clock separately).
const BOUNDARY_MS = 1000;
// Replayed lines remembered for that live/replay boundary check.
const RECENT_REPLAY = 64;
// Lines are held at most this long waiting for the reboot check.
const HOLD_MAX_MS = 5000;

/**
 * @typedef {Object} LogEntry
 * @property {number} id      increasing, assigned on arrival
 * @property {'log'|'dropped'|'restart'} kind
 * @property {number} [ts]    ms since the device booted
 * @property {string} [level] D/I/W/E
 * @property {string} [tag]
 * @property {string} [text]
 * @property {boolean} [replay] came from the ring replay
 * @property {number} [count] lines the device dropped (kind "dropped")
 */

function toEntry(msg, id) {
  if (msg.t === 'dropped') return { id, kind: 'dropped', count: Number(msg.count) || 0 };
  let ts = Number(msg.ts) || 0;
  let level = typeof msg.level === 'string' ? msg.level : 'I';
  let tag = typeof msg.tag === 'string' ? msg.tag : '';
  let text = typeof msg.msg === 'string' ? msg.msg : String(msg.msg ?? '');
  let replay = false;
  if (!ts && !tag) {
    const m = RING_LINE.exec(text);
    if (m) {
      ts = Number(m[1]);
      level = m[2];
      tag = m[3];
      text = m[4];
      replay = true;
    }
  }
  return { id, kind: 'log', ts, level, tag, text, replay };
}

function freshSeen() {
  return { maxTs: -1, atMax: new Set(), recent: [] };
}

function entryKey(e) {
  return e.level + '\u0000' + e.tag + '\u0000' + e.text;
}

/**
 * Whether `e` is new to this page, recording it in `seen` if so (see the
 * file header).
 */
function firstSight(seen, e) {
  if (e.kind !== 'log') return true;
  const key = entryKey(e);
  if (e.replay) {
    // Remembered even when it is a repeat, so a live copy right after the
    // replay is caught too.
    seen.recent.push({ key, ts: e.ts });
    if (seen.recent.length > RECENT_REPLAY) seen.recent.shift();
    if (e.ts < seen.maxTs || (e.ts === seen.maxTs && seen.atMax.has(key))) return false;
  } else {
    const i = seen.recent.findIndex((r) => r.key === key && e.ts >= r.ts && e.ts - r.ts <= BOUNDARY_MS);
    if (i >= 0) {
      seen.recent.splice(i, 1);
      return false;
    }
  }
  if (e.ts > seen.maxTs) {
    seen.maxTs = e.ts;
    seen.atMax = new Set([key]);
  } else if (e.ts === seen.maxTs) {
    seen.atMax.add(key);
  }
  return true;
}

function makeFilter(minLevel, query) {
  const q = query.trim().toLowerCase();
  const min = RANK[minLevel] ?? 0;
  return (e) => {
    if (e.kind !== 'log') return true;
    if ((RANK[e.level] ?? 1) < min) return false;
    if (!q) return true;
    return e.tag.toLowerCase().includes(q) || e.text.toLowerCase().includes(q);
  };
}

function span(cls, text) {
  const s = document.createElement('span');
  s.className = cls;
  s.textContent = text;
  return s;
}

function makeRow(e) {
  const row = document.createElement('div');
  row.dataset.id = String(e.id);
  if (e.kind === 'dropped') {
    row.className = 'log-row log-dropped';
    row.textContent = t('logs.dropped', { count: formatCount(e.count) });
    return row;
  }
  if (e.kind === 'restart') {
    row.className = 'log-row log-dropped log-restart';
    row.textContent = t('logs.restarted');
    return row;
  }
  row.className = 'log-row lv-' + e.level + (e.replay ? ' is-replay' : '');
  row.append(span('log-ts', formatClock(e.ts)), span('log-lv', e.level));
  if (e.tag) row.append(span('log-tag', e.tag));
  row.append(span('log-msg', e.text));
  return row;
}

// Owns the line list's DOM; Preact renders it once and never again.
class LogLines extends Component {
  shouldComponentUpdate() {
    return false;
  }

  /** Appends the entries that pass `filter`. */
  append(entries, filter) {
    const frag = document.createDocumentFragment();
    for (const e of entries) if (filter(e)) frag.append(makeRow(e));
    this.el.append(frag);
  }

  /** Drops rows for entries older than `minId` (evicted from the buffer). */
  trimBefore(minId) {
    let first = this.el.firstElementChild;
    while (first && Number(first.dataset.id) < minId) {
      const next = first.nextElementSibling;
      first.remove();
      first = next;
    }
  }

  rebuild(entries, filter) {
    this.el.replaceChildren();
    this.append(entries, filter);
  }

  get count() {
    return this.el ? this.el.childElementCount : 0;
  }

  render() {
    return html`<div
      class="log-lines"
      role="log"
      aria-live="off"
      aria-label=${t('logs.region')}
      ref=${(el) => {
        this.el = el;
      }}
    ></div>`;
  }
}

export function LogsPage() {
  const [status, setStatus] = useState('idle'); // idle|connecting|live|retrying|stopped
  const [stopNote, setStopNote] = useState('');
  const [paused, setPaused] = useState(false);
  const [missed, setMissed] = useState(0);
  const [minLevel, setMinLevel] = useState('D');
  const [query, setQuery] = useState('');
  const [stats, setStats] = useState({ total: 0, shown: 0 });
  const [following, setFollowing] = useState(true);

  const buffer = useRef(/** @type {LogEntry[]} */ ([]));
  const pending = useRef(/** @type {LogEntry[]} */ ([]));
  const nextId = useRef(1);
  const frame = useRef(0);
  const unsub = useRef(null);
  const lines = useRef(null);
  const scroller = useRef(null);
  const stick = useRef(true);
  // Where scrollToEnd() last left the list; scrolling above it is the user.
  const autoTop = useRef(0);
  // Dedupe state (see the file header), and lines held while the reboot
  // check after a (re)subscription is in flight.
  const seen = useRef(freshSeen());
  const held = useRef(/** @type {any[]|null} */ (null));
  const holdGen = useRef(0);
  // Mirrors of state read from callbacks that outlive a render.
  const pausedRef = useRef(false);
  const filterRef = useRef(makeFilter('D', ''));

  function scrollToEnd() {
    const el = scroller.current;
    if (!el) return;
    el.scrollTop = el.scrollHeight;
    autoTop.current = el.scrollTop;
  }

  function publishStats() {
    setStats({ total: buffer.current.length, shown: lines.current ? lines.current.count : 0 });
  }

  function flush() {
    frame.current = 0;
    const view = lines.current;
    if (!view) return;
    const batch = pending.current;
    pending.current = [];
    if (batch.length) view.append(batch, filterRef.current);
    if (buffer.current.length) view.trimBefore(buffer.current[0].id);
    if (stick.current) scrollToEnd();
    publishStats();
  }

  function schedule() {
    if (!frame.current) frame.current = requestAnimationFrame(flush);
  }

  function onMessage(msg) {
    if (held.current) held.current.push(msg);
    else ingest(msg);
  }

  // A replay is on its way. If this page has seen lines before, hold the
  // stream until /device says whether the device rebooted since.
  function onSubscribed() {
    if (seen.current.maxTs < 0) return;
    const gen = ++holdGen.current;
    held.current = held.current || [];
    let done = false;
    const release = (rebooted) => {
      if (done || gen !== holdGen.current) return;
      done = true;
      clearTimeout(timer);
      if (rebooted) {
        seen.current = freshSeen();
        addEntry({ id: 0, kind: 'restart' });
      }
      const msgs = held.current || [];
      held.current = null;
      for (const m of msgs) ingest(m);
    };
    const timer = setTimeout(() => release(false), HOLD_MAX_MS);
    api('GET', '/device').then(
      (d) => {
        const up = Number(d?.status?.uptime_ms);
        release(Number.isFinite(up) && up < seen.current.maxTs);
      },
      () => release(false),
    );
  }

  function ingest(msg) {
    const entry = toEntry(msg, 0);
    if (!firstSight(seen.current, entry)) return;
    addEntry(entry);
  }

  function addEntry(entry) {
    entry.id = nextId.current++;
    const buf = buffer.current;
    buf.push(entry);
    if (buf.length > MAX_LINES) buf.splice(0, buf.length - MAX_LINES);
    if (pausedRef.current) {
      setMissed((n) => n + 1);
      return;
    }
    pending.current.push(entry);
    schedule();
  }

  function stop(note = '') {
    if (unsub.current) {
      unsub.current();
      unsub.current = null;
    }
    // Lines held for a reboot check that will no longer matter.
    holdGen.current++;
    const msgs = held.current || [];
    held.current = null;
    for (const m of msgs) ingest(m);
    setStatus('stopped');
    setStopNote(note);
  }

  function start() {
    if (unsub.current) return;
    setStopNote('');
    setStatus('connecting');
    stick.current = true;
    setFollowing(true);
    unsub.current = subscribe(['log'], onMessage, {
      onState: (st) => {
        if (st === 'open') {
          onSubscribed();
          setStatus('live');
        }
        else if (st === 'connecting') setStatus('connecting');
        else if (st === 'retrying') setStatus('retrying');
        else if (st === 'unauthorized') stop('logs.stopped_unpaired');
      },
    });
  }

  function rerender() {
    const view = lines.current;
    if (!view) return;
    pending.current = [];
    view.rebuild(buffer.current, filterRef.current);
    if (stick.current) scrollToEnd();
    publishStats();
  }

  function togglePause() {
    const next = !pausedRef.current;
    pausedRef.current = next;
    setPaused(next);
    if (!next) {
      setMissed(0);
      rerender();
    }
  }

  function clear() {
    buffer.current = [];
    pending.current = [];
    setMissed(0);
    lines.current?.rebuild([], filterRef.current);
    stick.current = true;
    setFollowing(true);
    publishStats();
  }

  // Only the user unsticks the list. The list also leaves the bottom on
  // its own -- rows laid out after a scrollToEnd() (wrapped lines, rows
  // whose real height replaces the content-visibility estimate) grow it
  // below the viewport -- and that must not stop following.
  function onScroll() {
    const el = scroller.current;
    if (!el) return;
    const atEnd = el.scrollHeight - el.scrollTop - el.clientHeight < STICK_PX;
    let next = stick.current;
    if (atEnd) next = true;
    else if (el.scrollTop < autoTop.current - 2) next = false;
    else if (stick.current) schedule();
    if (next !== stick.current) {
      stick.current = next;
      setFollowing(next);
    }
  }

  // The imperative rows hold translated text (the dropped markers).
  const lang = useLang();
  useEffect(() => {
    if (!pausedRef.current) rerender();
  }, [lang]);

  // Filters apply to what is already on screen too.
  useEffect(() => {
    filterRef.current = makeFilter(minLevel, query);
    if (!pausedRef.current) rerender();
  }, [minLevel, query]);

  // Leaving the page or hiding the tab ends the stream (opt-in only).
  useEffect(() => {
    const onVisibility = () => {
      if (document.hidden && unsub.current) stop('logs.stopped_hidden');
    };
    document.addEventListener('visibilitychange', onVisibility);
    return () => {
      document.removeEventListener('visibilitychange', onVisibility);
      if (frame.current) cancelAnimationFrame(frame.current);
      if (unsub.current) unsub.current();
      unsub.current = null;
    };
  }, []);

  const streaming = status === 'live' || status === 'connecting' || status === 'retrying';
  const empty = stats.shown === 0;

  return html`<div class="page page-logs">
    <header class="page-head">
      <div>
        <h1 class="page-title" tabindex="-1">${t('nav.logs')}</h1>
        <p class="page-sub">${t('logs.subtitle')}</p>
      </div>
      <div class="page-head-actions">
        <${StatusBadge} status=${status} />
      </div>
    </header>

    <div class="log-toolbar">
      <div class="toolbar-group">
        ${streaming
          ? html`<button type="button" class="btn btn-secondary" onClick=${() => stop()}>
              <${IconStop} size=${16} />${t('logs.stop')}
            </button>`
          : html`<button type="button" class="btn btn-primary" onClick=${start}>
              <${IconPlay} size=${16} />${t('logs.start')}
            </button>`}
        <button
          type="button"
          class=${paused ? 'btn btn-warn-soft' : 'btn btn-ghost'}
          onClick=${togglePause}
          aria-pressed=${paused ? 'true' : 'false'}
          disabled=${!streaming && !paused}
        >
          ${paused
            ? html`<${IconPlay} size=${16} />${t('logs.resume')}`
            : html`<${IconPause} size=${16} />${t('logs.pause')}`}
        </button>
        <button type="button" class="btn btn-ghost" onClick=${clear} disabled=${stats.total === 0 && missed === 0}>
          <${IconTrash} size=${16} />${t('logs.clear')}
        </button>
      </div>
      <div class="toolbar-group toolbar-filters">
        <div class="segmented" role="radiogroup" aria-label=${t('logs.level_label')}>
          ${FILTERS.map(
            (f) => html`<button
              key=${f.id}
              type="button"
              role="radio"
              aria-checked=${minLevel === f.id ? 'true' : 'false'}
              class=${minLevel === f.id ? 'seg is-active' : 'seg'}
              onClick=${() => setMinLevel(f.id)}
            >
              ${f.id !== 'D' ? html`<span class=${'lv-dot lv-' + f.id} aria-hidden="true"></span>` : null}${t(f.label)}
            </button>`,
          )}
        </div>
        <label class="search">
          <${IconSearch} size=${16} />
          <span class="sr-only">${t('logs.search_label')}</span>
          <input
            type="search"
            placeholder=${t('logs.search_ph')}
            value=${query}
            onInput=${(e) => setQuery(e.currentTarget.value)}
            autocomplete="off"
            spellcheck=${false}
          />
        </label>
      </div>
    </div>

    ${paused
      ? html`<div class="alert tone-warn log-banner" role="status">
          <${IconPause} size=${16} />
          <span>${missed ? t('logs.paused_missed', { count: formatCount(missed) }) : t('logs.paused')}</span>
          <button type="button" class="btn btn-link" onClick=${togglePause}>${t('logs.resume')}</button>
        </div>`
      : null}
    ${stopNote
      ? html`<div class="alert tone-muted log-banner" role="status">
          <${IconInfo} size=${16} />
          <span>${t('logs.stopped', { reason: t(stopNote) })}</span>
          <button type="button" class="btn btn-link" onClick=${start}>${t('logs.restart')}</button>
        </div>`
      : null}

    <div class="log-panel">
      <div class="log-scroll" ref=${scroller} onScroll=${onScroll} tabindex="0" aria-label=${t('logs.list')}>
        <${LogLines} ref=${lines} />
      </div>
      ${empty
        ? html`<div class="log-empty">
            ${status === 'idle'
              ? html`<span class="log-empty-icon"><${IconLogs} size=${28} /></span>
                  <p class="empty-title">${t('logs.idle_title')}</p>
                  <p class="muted">${t('logs.idle_body')}</p>
                  <button type="button" class="btn btn-primary" onClick=${start}>
                    <${IconPlay} size=${16} />${t('logs.start')}
                  </button>`
              : stats.total > 0
                ? html`<p class="empty-title">${t('logs.no_match_title')}</p>
                    <p class="muted">${t('logs.no_match_body', { count: formatCount(stats.total) })}</p>`
                : streaming
                  ? html`<span class="spinner spinner-lg" aria-hidden="true"></span>
                      <p class="muted">${status === 'live' ? t('logs.waiting') : t('logs.connecting')}</p>`
                  : html`<p class="muted">${t('logs.none')}</p>`}
          </div>`
        : null}
      ${!following && !empty
        ? html`<button
            type="button"
            class="btn btn-primary btn-sm log-jump"
            onClick=${() => {
              stick.current = true;
              setFollowing(true);
              scrollToEnd();
            }}
          >
            <${IconArrowDown} size=${16} />${t('logs.jump')}
          </button>`
        : null}
    </div>
    <p class="log-foot">
      <span class="num">
        ${t('logs.total', { count: formatCount(stats.total) })}${stats.shown !== stats.total
          ? t('logs.shown', { count: formatCount(stats.shown) })
          : ''}
      </span>
      <span class="dim">${t('logs.cap', { count: formatCount(MAX_LINES) })}</span>
    </p>
  </div>`;
}

function StatusBadge({ status }) {
  switch (status) {
    case 'live':
      return html`<${Badge} tone="ok" dot pulse>${t('live.open')}</${Badge}>`;
    case 'connecting':
      return html`<${Badge} tone="muted" dot>${t('live.connecting')}</${Badge}>`;
    case 'retrying':
      return html`<${Badge} tone="warn" dot>${t('live.retrying')}</${Badge}>`;
    case 'stopped':
      return html`<${Badge} tone="muted" dot>${t('logs.status_stopped')}</${Badge}>`;
    default:
      return html`<${Badge} tone="muted" dot>${t('logs.status_off')}</${Badge}>`;
  }
}
