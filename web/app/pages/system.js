// System (spec 7.3): display and sound settings, the device's own screen
// language, its time zone and network time, paired browsers, firmware,
// restart and factory reset. The phone layout also reaches the log page
// from here.
//
// Settings: values from GET /settings, their ranges, choices and labels
// from GET /settings/schema (settings-schema spec 3.7), loaded together.
// The cards place the known keys by hand; a key no card places shows in
// "More settings" with a control chosen by its type.
//
// Power (scene-device spec 4.5): the level, light sleep, how many scene
// requests wait for the network and every wake lock with how long it has
// been held, refreshed while the tab is visible; with it the deep-standby
// delay and the light-sleep switch. Peripheral use (spec 6.3) lists who
// holds which device.
//
// Network time (spec 5.6): GET /system/time shows the last sync; "Sync
// now" posts /system/time/sync (409 no_network without Wi-Fi) and then
// waits for the attempt that answers it to finish (see TimeSyncCard's
// syncNow()). Times are shown in the device's own time zone
// (utc_offset_min), not the browser's.
//
// Restart: the device answers 202 and restarts a moment later; the page
// then polls GET /info until it answers with a different boot_id, which
// is the new boot (an answer from the old one before it went down does
// not count).
//
// Factory reset: the device asks its user to confirm on its own screen
// and reports the outcome as a system.factory_reset event ({state:
// "done"|"cancelled"|"failed"}; "failed": the erase could not start, so
// nothing was erased and the device keeps running). A reset revokes every paired browser, so this
// browser's socket may be closed (and its next request refused with 401)
// before the "done" event gets out; while waiting, either one counts as
// done. The page subscribes to events only for that wait.
//
// Firmware (spec 6.5): the file is streamed to POST /system/firmware with
// upload progress. The device checks it is a Wisp image for its own board
// and answers 202 once it is written and verified; its user then confirms
// on the screen. The outcome arrives as system.firmware events
// (received / confirmed / rejected / failed). After "confirmed" the device
// restarts: the page waits for a new boot_id, then reads the running
// version again -- a different one than uploaded means the new image did
// not start and the bootloader went back to the old one.

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError, errorDetail, logout, setUnauthorizedHandler, upload } from '../api.js';
import { navigate } from '../router.js';
import { LANGS, has, locale, t } from '../i18n/index.js';
import { subscribe } from '../ws.js';
import { formatUptime, useVisible } from '../util.js';
import { Card } from '../components/card.js';
import { Badge } from '../components/badge.js';
import { confirmDialog } from '../components/dialog.js';
import { toast } from '../components/toast.js';
import {
  IconAlert,
  IconBolt,
  IconBrowser,
  IconCheck,
  IconChevronRight,
  IconClock,
  IconDisplay,
  IconGlobe,
  IconImu,
  IconInfo,
  IconLogout,
  IconLogs,
  IconModel,
  IconMoon,
  IconNote,
  IconPackage,
  IconPower,
  IconReset,
  IconSpeaker,
  IconSun,
  IconSystem,
  IconUpload,
} from '../icons.js';

// How often the power card refreshes while the tab is visible.
const POWER_REFRESH_MS = 5000;
const REBOOT_POLL_MS = 2000;
// Past this the page says the restart is taking longer than usual (and
// keeps waiting).
const REBOOT_SLOW_MS = 90000;
// Clocks before 2020 mean the device had no real time yet.
const MIN_REAL_UNIX = 1577836800;

/** Asks first, then revokes this browser's pairing. */
export async function confirmLogout() {
  const ok = await confirmDialog({
    title: t('logout.title'),
    body: t('logout.body'),
    okText: t('logout.confirm'),
    danger: true,
  });
  if (!ok) return;
  try {
    await logout();
  } catch (e) {
    if (e?.status !== 401) toast(t('logout.failed', { reason: describeError(e) }), 'err');
  }
}

/**
 * @typedef {Object} Settings  GET/PUT /settings; ranges, choices and
 *   defaults: see GET /settings/schema
 * @property {number} brightness
 * @property {number} volume
 * @property {number} screen_off_s  0: never
 * @property {string} language  the device's own UI language
 * @property {number} utc_offset_min  the time zone, minutes east of UTC
 * @property {string} ntp_servers  time servers, comma-separated
 * @property {number} deep_after_s  0: never; deep standby after the screen is off this long
 * @property {number} notify_vol   the notification sounds
 * @property {boolean} tts_mute    silence spoken replies (text still shows)
 * @property {boolean} time_24h    24-hour clock on the device (false: 12-hour)
 * @property {number} agent_max_steps  tool calls per agent turn
 * @property {number} agent_turn_min   minutes per agent turn
 *
 * @typedef {Object} SettingSpec  one entry of GET /settings/schema's keys
 * @property {string} name
 * @property {'int'|'bool'|'enum'|'hosts'} type
 * @property {string} label  the web dictionary key is 'sys.' + label
 * @property {number} [min]
 * @property {number} [max]
 * @property {number|boolean|string} default
 * @property {string} [unit]
 * @property {boolean} [zero_never]  0 is also valid and means never
 * @property {number[]} [choices]  the values offered in a list
 * @property {string[]} [values]   an enum's codes
 * @property {number} [max_entries]
 * @property {number} [max_length]
 *
 * @typedef {Object} PowerStatus  GET /system/power
 * @property {'active'|'dim'|'standby'|'deep_standby'} level
 * @property {'on'|'off'|'unsupported'} light_sleep
 * @property {number} deferred  scene requests waiting for the network
 * @property {{id: number, holder: string, kind: 'network'|'cpu'|'screen', reason: string, held_ms: number}[]} locks
 *
 * @typedef {Object} TimeStatus  GET /system/time
 * @property {boolean} synced         a sync succeeded since the device started
 * @property {number|null} last_sync  unix seconds of that sync
 * @property {string} server          the server that answered
 * @property {number|null} time       the device's time now, unix seconds
 *
 * @typedef {Object} Client  GET /clients
 * @property {string} id
 * @property {string} label
 * @property {number} created    unix seconds (0 when the device had no clock)
 * @property {number} last_seen  unix seconds
 * @property {boolean} current   this browser
 */

export function SystemPage() {
  const [settings, setSettings] = useState(/** @type {Settings|null} */ (null));
  const [schema, setSchema] = useState(/** @type {{keys: SettingSpec[]}|null} */ (null));
  const [clients, setClients] = useState(/** @type {Client[]|null} */ (null));
  const [error, setError] = useState(null);
  const [clientsError, setClientsError] = useState(null);
  const alive = useRef(true);

  // The schema is read again with every load: after a restart (a firmware
  // update among them) the device may describe its settings differently.
  async function loadSettings() {
    try {
      const [s, sc] = await Promise.all([api('GET', '/settings'), api('GET', '/settings/schema')]);
      if (!Array.isArray(sc?.keys)) throw Object.assign(new Error('no settings schema'), { code: 'unknown' });
      if (!alive.current) return;
      setSchema(sc);
      setSettings(s);
      setError(null);
    } catch (e) {
      if (alive.current && e.status !== 401) setError(e);
    }
  }

  async function loadClients() {
    try {
      const r = await api('GET', '/clients');
      if (!alive.current) return;
      setClients(Array.isArray(r?.clients) ? r.clients : []);
      setClientsError(null);
    } catch (e) {
      if (alive.current && e.status !== 401) setClientsError(e);
    }
  }

  function loadAll() {
    loadSettings();
    loadClients();
  }

  useEffect(() => {
    alive.current = true;
    loadAll();
    return () => {
      alive.current = false;
    };
  }, []);

  // Sends one change; the reply is the full current set. On failure the
  // controls go back to what the device has.
  async function put(patch) {
    try {
      const s = await api('PUT', '/settings', patch);
      if (alive.current) setSettings(s);
      return true;
    } catch (e) {
      if (e.status !== 401) toast(t('sys.save_failed', { reason: describeError(e) }), 'err');
      if (alive.current) setSettings((s) => (s ? { ...s } : s));
      loadSettings();
      return false;
    }
  }

  // Every settings card needs both the values and their schema; until then
  // they show skeletons.
  const ready = settings && schema ? settings : null;
  const spec = schema ? specOf(schema) : null;

  return html`<div class="page">
    <header class="page-head">
      <div>
        <h1 class="page-title" tabindex="-1">${t('nav.system')}</h1>
        <p class="page-sub">${t('sys.sub')}</p>
      </div>
    </header>

    ${error && !ready
      ? html`<${Card} title=${t('sys.load_failed')} icon=${IconAlert} tone="err">
          <p class="muted">${describeError(error)}</p>
          ${errorDetail(error) ? html`<p class="err-detail mono">${errorDetail(error)}</p>` : null}
          <button type="button" class="btn btn-secondary" onClick=${loadAll}>${t('common.retry')}</button>
        </${Card}>`
      : null}

    <div class="grid grid-2">
      <${DisplayCard} settings=${ready} spec=${spec} put=${put} />
      <${LanguageCard} settings=${ready} spec=${spec} put=${put} />
      <${TimeZoneCard} settings=${ready} spec=${spec} put=${put} />
      <${TimeSyncCard} settings=${ready} spec=${spec} put=${put} />
    </div>
    <${PowerCard} settings=${ready} spec=${spec} put=${put} />
    <${AgentLimitsCard} settings=${ready} spec=${spec} put=${put} />
    <${MoreCard} settings=${ready} schema=${schema} put=${put} />
    <${OccupancyCard} />
    <${ClientsCard} clients=${clients} error=${clientsError} reload=${loadClients} />
    <${FirmwareCard} />
    <${MaintenanceCard} onBack=${loadAll} />
  </div>`;
}

/** `spec(name)`: the schema entry of the key `name`. */
function specOf(schema) {
  const byName = new Map(schema.keys.map((k) => [k.name, k]));
  return (name) => byName.get(name);
}

/** The keys the cards above place by hand; MoreCard shows the rest. */
const PLACED = new Set([
  'brightness',
  'volume',
  'notify_vol',
  'tts_mute',
  'screen_off_s',
  'deep_after_s',
  'language',
  'utc_offset_min',
  'time_24h',
  'ntp_servers',
  'agent_max_steps',
  'agent_turn_min',
]);

/** `choices`, with `value` added in order when it is not one of them. */
function withCurrent(choices, value) {
  return choices.includes(value) ? choices : [...choices, value].sort((a, b) => a - b);
}

/** Whether `v` is a valid value of the int key `s`. */
function intValid(s, v) {
  return Number.isInteger(v) && ((v >= s.min && v <= s.max) || (!!s.zero_never && v === 0));
}

/** A slider that shows its value while dragging and saves on release. */
function RangeField({ id, label, icon: Icon, min, max, value, format, onCommit, disabled }) {
  const [draft, setDraft] = useState(null);
  const shown = draft ?? value;
  const pct = ((shown - min) / (max - min)) * 100;
  return html`<div class="field range-field">
    <div class="field-row">
      <label class="field-label with-icon" for=${id}><${Icon} size=${16} />${label}</label>
      <output class="range-value mono" for=${id}>${format(shown)}</output>
    </div>
    <input
      id=${id}
      class="range"
      type="range"
      min=${min}
      max=${max}
      step="1"
      value=${shown}
      style=${{ '--fill': pct + '%' }}
      disabled=${disabled}
      aria-valuetext=${format(shown)}
      onInput=${(e) => setDraft(Number(e.currentTarget.value))}
      onChange=${async (e) => {
        const v = Number(e.currentTarget.value);
        setDraft(v);
        if (v !== value) await onCommit(v);
        setDraft(null);
      }}
    />
  </div>`;
}

function percent(v, min, max) {
  return new Intl.NumberFormat(locale(), { style: 'percent' }).format((v - min) / (max - min));
}

function screenOffLabel(s) {
  if (s === 0) return t('sys.screen_never');
  const fmt = (v, unit) => new Intl.NumberFormat(locale(), { style: 'unit', unit, unitDisplay: 'long' }).format(v);
  return s % 60 === 0 ? fmt(s / 60, 'minute') : fmt(s, 'second');
}

function DisplayCard({ settings, spec, put }) {
  if (!settings) return html`<${SkeletonCard} />`;
  const brightness = spec('brightness');
  const volume = spec('volume');
  const notify = spec('notify_vol');
  const choices = withCurrent(spec('screen_off_s').choices, settings.screen_off_s);
  return html`<${Card} title=${t('sys.display_title')} icon=${IconDisplay}>
    <div class="form">
      <${RangeField}
        id="sys-brightness"
        label=${t('sys.brightness')}
        icon=${IconSun}
        min=${brightness.min}
        max=${brightness.max}
        value=${settings.brightness}
        format=${(v) => percent(v, brightness.min, brightness.max)}
        onCommit=${(v) => put({ brightness: v })}
      />
      <${RangeField}
        id="sys-volume"
        label=${t('sys.volume')}
        icon=${IconSpeaker}
        min=${volume.min}
        max=${volume.max}
        value=${settings.volume}
        format=${(v) => percent(v, volume.min, volume.max)}
        onCommit=${(v) => put({ volume: v })}
      />
      <${RangeField}
        id="sys-notify-vol"
        label=${t('sys.notify_vol')}
        icon=${IconSpeaker}
        min=${notify.min}
        max=${notify.max}
        value=${settings.notify_vol}
        format=${(v) => percent(v, notify.min, notify.max)}
        onCommit=${(v) => put({ notify_vol: v })}
      />
      <label class="scene-check">
        <input
          type="checkbox"
          checked=${settings.tts_mute}
          onChange=${(e) => put({ tts_mute: e.currentTarget.checked })}
        />
        ${t('sys.tts_mute')}
      </label>
      <div class="field">
        <label class="field-label with-icon" for="sys-screen-off"><${IconMoon} size=${16} />${t('sys.screen_off')}</label>
        <select
          id="sys-screen-off"
          class="input select"
          value=${String(settings.screen_off_s)}
          onChange=${(e) => put({ screen_off_s: Number(e.currentTarget.value) })}
          aria-describedby="sys-screen-off-hint"
        >
          ${choices.map((s) => html`<option key=${s} value=${String(s)}>${screenOffLabel(s)}</option>`)}
        </select>
        <p id="sys-screen-off-hint" class="field-hint">${t('sys.screen_off_hint')}</p>
      </div>
    </div>
  </${Card}>`;
}

/** GET `path` every POWER_REFRESH_MS while the tab is visible. */
function usePolled(path) {
  const visible = useVisible();
  const [data, setData] = useState(null);
  const [error, setError] = useState(null);
  useEffect(() => {
    if (!visible) return undefined;
    let alive = true;
    const load = async () => {
      try {
        const r = await api('GET', path);
        if (!alive) return;
        setData(r);
        setError(null);
      } catch (e) {
        if (alive && e.status !== 401) setError(e);
      }
    };
    load();
    const timer = setInterval(load, POWER_REFRESH_MS);
    return () => {
      alive = false;
      clearInterval(timer);
    };
  }, [visible, path]);
  return [data, error];
}

function deepAfterLabel(s) {
  return s === 0 ? t('sys.deep_never') : screenOffLabel(s);
}

const LEVEL_TONE = { active: 'ok', dim: 'accent', standby: 'muted', deep_standby: 'muted' };

function PowerCard({ settings, spec, put }) {
  const [power, error] = usePolled('/system/power');
  const locks = Array.isArray(power?.locks) ? power.locks : [];
  const choices = settings ? withCurrent(spec('deep_after_s').choices, settings.deep_after_s) : [];
  return html`<${Card}
    title=${t('power.title')}
    icon=${IconBolt}
    actions=${power
      ? html`<${Badge} tone=${LEVEL_TONE[power.level] || 'muted'}>${t('power.level.' + power.level)}</${Badge}>`
      : null}
  >
    ${error && !power
      ? html`<div class="alert tone-err" role="alert">
          <${IconAlert} size=${18} /><span>${t('power.load_failed', { reason: describeError(error) })}</span>
        </div>`
      : !power
        ? html`<div class="skel skel-line"></div>`
        : html`<dl class="kv">
              <div><dt>${t('power.light_sleep')}</dt><dd>${t('power.light_sleep.' + power.light_sleep)}</dd></div>
              <div><dt>${t('power.deferred')}</dt><dd class="mono">${power.deferred}</dd></div>
            </dl>
            <h3 class="field-label">${t('power.locks')}</h3>
            ${locks.length === 0
              ? html`<p class="stat-empty">${t('power.no_locks')}</p>`
              : html`<ul class="rows">
                  ${locks.map(
                    (l) => html`<li key=${l.id} class="row">
                      <div class="row-main">
                        <span class="row-title mono">${l.holder}</span>
                        <span class="row-sub">${l.reason}</span>
                      </div>
                      <${Badge} tone="muted">${t('power.kind.' + l.kind)}</${Badge}>
                      <span class="mono" title=${t('power.held')}>${formatUptime(l.held_ms)}</span>
                    </li>`,
                  )}
                </ul>`}`}
    ${settings
      ? html`<div class="form fw-form">
          <div class="field">
            <label class="field-label with-icon" for="sys-deep-after"><${IconMoon} size=${16} />${t('sys.deep_after')}</label>
            <select
              id="sys-deep-after"
              class="input select"
              value=${String(settings.deep_after_s)}
              onChange=${(e) => put({ deep_after_s: Number(e.currentTarget.value) })}
              aria-describedby="sys-deep-after-hint"
            >
              ${choices.map((s) => html`<option key=${s} value=${String(s)}>${deepAfterLabel(s)}</option>`)}
            </select>
            <p id="sys-deep-after-hint" class="field-hint">${t('sys.deep_after_hint')}</p>
          </div>
        </div>`
      : null}
  </${Card}>`;
}

function OccupancyCard() {
  const [occ, error] = usePolled('/system/occupancy');
  const items = Array.isArray(occ?.items) ? occ.items : null;
  return html`<${Card} title=${t('occ.title')} icon=${IconImu}>
    ${error && !items
      ? html`<div class="alert tone-err" role="alert">
          <${IconAlert} size=${18} /><span>${t('occ.load_failed', { reason: describeError(error) })}</span>
        </div>`
      : !items
        ? html`<div class="skel skel-line"></div>`
        : items.length === 0
          ? html`<p class="stat-empty">${t('occ.none')}</p>`
          : html`<ul class="rows">
              ${items.map(
                (o) => html`<li key=${o.device + ' ' + o.holder} class="row">
                  <div class="row-main">
                    <span class="row-title mono">${o.device}</span>
                    <span class="row-sub mono">${o.holder}</span>
                  </div>
                  <${Badge} tone=${o.mode === 'exclusive' ? 'warn' : 'muted'}>${t('occ.mode.' + o.mode)}</${Badge}>
                  <span class="mono" title=${t('power.held')}>${formatUptime(o.held_ms)}</span>
                </li>`,
              )}
            </ul>`}
  </${Card}>`;
}

/** The device's languages: the schema's codes, named as the web names them. */
function deviceLangs(codes) {
  return codes.map((id) => LANGS.find((l) => l.id === id) || { id, label: id, html: id });
}

function LanguageCard({ settings, spec, put }) {
  if (!settings) return html`<${SkeletonCard} />`;
  const langs = deviceLangs(spec('language').values);
  return html`<${Card} title=${t('sys.lang_title')} icon=${IconGlobe}>
    <div class="form">
      <div class="field">
        <label class="field-label" for="sys-lang">${t('sys.lang_label')}</label>
        <select
          id="sys-lang"
          class="input select"
          value=${settings.language}
          onChange=${async (e) => {
            const v = e.currentTarget.value;
            if (await put({ language: v })) {
              toast(t('sys.lang_saved', { lang: langs.find((l) => l.id === v)?.label || v }), 'ok');
            }
          }}
          aria-describedby="sys-lang-hint"
        >
          ${langs.map((l) => html`<option key=${l.id} value=${l.id} lang=${l.html}>${l.label}</option>`)}
        </select>
      </div>
      <div class="inline-note" id="sys-lang-hint">
        <${IconInfo} size=${15} />
        <span>${t('sys.lang_hint')}</span>
      </div>
    </div>
  </${Card}>`;
}

/** "UTC+08:00", "UTC−03:30". */
function utcOffsetLabel(m) {
  const a = Math.abs(m);
  const hh = String(Math.floor(a / 60)).padStart(2, '0');
  const mm = String(a % 60).padStart(2, '0');
  return `UTC${m < 0 ? '\u2212' : '+'}${hh}:${mm}`;
}

function TimeZoneCard({ settings, spec, put }) {
  if (!settings) return html`<${SkeletonCard} />`;
  const choices = withCurrent(spec('utc_offset_min').choices, settings.utc_offset_min);
  return html`<${Card} title=${t('sys.tz_title')} icon=${IconClock}>
    <div class="form">
      <div class="field">
        <label class="field-label" for="sys-tz">${t('sys.tz_label')}</label>
        <select
          id="sys-tz"
          class="input select"
          value=${String(settings.utc_offset_min)}
          onChange=${(e) => put({ utc_offset_min: Number(e.currentTarget.value) })}
          aria-describedby="sys-tz-hint"
        >
          ${choices.map((m) => html`<option key=${m} value=${String(m)}>${utcOffsetLabel(m)}</option>`)}
        </select>
        <p id="sys-tz-hint" class="field-hint">${t('sys.tz_hint')}</p>
      </div>
      <label class="scene-check">
        <input
          type="checkbox"
          checked=${settings.time_24h}
          onChange=${(e) => put({ time_24h: e.currentTarget.checked })}
        />
        ${t('sys.time_24h')}
      </label>
    </div>
  </${Card}>`;
}

// How long "Sync now" waits before saying there is no answer yet. Name
// lookups are not bounded by the device's query timeout, so an attempt
// can outlast this; past it the page stops waiting but claims nothing.
const TIME_SYNC_WAIT_MS = 60000;
const TIME_SYNC_POLL_MS = 1500;

/** "2026-01-01 08:00 UTC+08:00": `unix` in the device's time zone. */
function deviceTime(unix, offsetMin) {
  if (!Number.isFinite(unix) || unix < MIN_REAL_UNIX) return '';
  const shifted = new Date((unix + offsetMin * 60) * 1000);
  const text = new Intl.DateTimeFormat(locale(), {
    dateStyle: 'medium',
    timeStyle: 'short',
    timeZone: 'UTC',
  }).format(shifted);
  return `${text} ${utcOffsetLabel(offsetMin)}`;
}

/** The ntp_servers value as typed, tidied: spaces dropped, empty entries removed. */
function tidyServers(text) {
  return text
    .split(/[\s,]+/)
    .filter(Boolean)
    .join(',');
}

/** The longest text a hosts key takes: its entries at full length, comma-separated. */
function hostsLength(s) {
  return s.max_entries * s.max_length + s.max_entries - 1;
}

function TimeSyncCard({ settings, spec, put }) {
  const [status, setStatus] = useState(/** @type {TimeStatus|null} */ (null));
  const [unavailable, setUnavailable] = useState(false);
  const [syncing, setSyncing] = useState(false);
  const [draft, setDraft] = useState(/** @type {string|null} */ (null));
  const alive = useRef(true);
  const stopWait = useRef(/** @type {(() => void)|null} */ (null));

  async function load() {
    try {
      const s = await api('GET', '/system/time');
      if (alive.current) setStatus(s);
    } catch (e) {
      if (alive.current && e.status === 503) setUnavailable(true);
    }
  }

  useEffect(() => {
    alive.current = true;
    load();
    return () => {
      alive.current = false;
      stopWait.current?.();
    };
  }, []);

  // The POST answers {after: n}: the outcome is that of the first attempt
  // to finish after n, which GET /system/time shows as attempts > n with
  // its last_result. The status is polled for it; a system.time event only
  // makes the next poll come at once (the event socket is a separate
  // connection and may miss or precede the attempt, so it decides
  // nothing). With no finished attempt by TIME_SYNC_WAIT_MS the page says
  // there is no answer yet, not that the sync failed.
  async function syncNow() {
    if (syncing) return;
    setSyncing(true);
    let over = false;
    let timer = 0;
    let poller = 0;
    let unsubscribe = () => {};
    const stop = () => {
      over = true;
      unsubscribe();
      clearTimeout(timer);
      clearInterval(poller);
      stopWait.current = null;
    };
    const finish = (tone, message) => {
      if (over) return;
      stop();
      if (!alive.current) return;
      setSyncing(false);
      load();
      if (message) toast(message, tone);
    };
    stopWait.current = stop;

    let after;
    try {
      const r = await api('POST', '/system/time/sync');
      after = Number(r?.after ?? 0);
    } catch (e) {
      finish(null, null);
      if (e.status !== 401 && alive.current) toast(describeError(e), 'err');
      return;
    }
    if (over) return;

    const poll = async () => {
      try {
        const s = await api('GET', '/system/time');
        if (over || !(Number(s?.attempts) > after)) return;
        if (s.last_result === 'synced') finish('ok', t('sys.time_synced', { server: s.server }));
        else finish('err', t('sys.time_failed'));
      } catch {
        // The next poll or the timeout decides.
      }
    };
    unsubscribe = subscribe(['events'], (msg) => {
      if (msg.t === 'event' && msg.topic === 'system.time') poll();
    });
    poller = setInterval(poll, TIME_SYNC_POLL_MS);
    timer = setTimeout(() => finish('warn', t('sys.time_pending')), TIME_SYNC_WAIT_MS);
    poll();
  }

  async function saveServers() {
    const value = tidyServers(draft ?? '');
    if (await put({ ntp_servers: value })) {
      setDraft(null);
      toast(t('sys.time_servers_saved'), 'ok');
    }
  }

  if (!settings) return html`<${SkeletonCard} />`;
  const offset = settings.utc_offset_min;
  const last = status?.synced ? deviceTime(status.last_sync, offset) : '';
  const now = status?.time != null ? deviceTime(status.time, offset) : '';
  const shown = draft ?? settings.ntp_servers;
  const changed = draft !== null && tidyServers(draft) !== settings.ntp_servers;
  return html`<${Card} title=${t('sys.time_title')} icon=${IconClock}>
    <div class="form">
      ${unavailable
        ? html`<p class="muted">${t('sys.time_unavailable')}</p>`
        : html`<dl class="kv">
              <div>
                <dt>${t('sys.time_last')}</dt>
                <dd>${last ? t('sys.time_from', { when: last, server: status.server }) : t('sys.time_never')}</dd>
              </div>
              <div>
                <dt>${t('sys.time_now')}</dt>
                <dd>${now || t('sys.time_unknown')}</dd>
              </div>
            </dl>
            <div>
              <button type="button" class="btn btn-secondary" onClick=${syncNow} disabled=${syncing}>
                ${syncing ? html`<span class="spinner" aria-hidden="true"></span>` : null}
                ${t('sys.time_sync')}
              </button>
            </div>`}
      <div class="field">
        <label class="field-label" for="sys-ntp">${t('sys.time_servers')}</label>
        <div class="input-group">
          <input
            id="sys-ntp"
            class="input mono"
            type="text"
            spellcheck="false"
            autocapitalize="off"
            autocomplete="off"
            maxlength=${hostsLength(spec('ntp_servers'))}
            value=${shown}
            onInput=${(e) => setDraft(e.currentTarget.value)}
            aria-describedby="sys-ntp-hint"
          />
          <button type="button" class="btn btn-secondary" onClick=${saveServers} disabled=${!changed}>
            ${t('sys.time_servers_save')}
          </button>
        </div>
        <p id="sys-ntp-hint" class="field-hint">${t('sys.time_servers_hint')}</p>
      </div>
    </div>
  </${Card}>`;
}

/** `text` as a valid value of the int key `s`, or null. */
function wholeIn(text, s) {
  const v = text.trim() === '' ? NaN : Number(text);
  return intValid(s, v) ? v : null;
}

// The agent's per-turn limits (agent-core spec 3.1): saved together, they
// apply from the next turn on.
function AgentLimitsCard({ settings, spec, put }) {
  const [steps, setSteps] = useState(/** @type {string|null} */ (null));
  const [minutes, setMinutes] = useState(/** @type {string|null} */ (null));
  const [saving, setSaving] = useState(false);
  if (!settings) return html`<${SkeletonCard} />`;
  const stepsSpec = spec('agent_max_steps');
  const minutesSpec = spec('agent_turn_min');
  const stepsShown = steps ?? String(settings.agent_max_steps);
  const minutesShown = minutes ?? String(settings.agent_turn_min);
  const stepsValue = wholeIn(stepsShown, stepsSpec);
  const minutesValue = wholeIn(minutesShown, minutesSpec);
  const valid = stepsValue !== null && minutesValue !== null;
  const changed = stepsValue !== settings.agent_max_steps || minutesValue !== settings.agent_turn_min;

  async function save() {
    if (!valid || saving) return;
    setSaving(true);
    if (await put({ agent_max_steps: stepsValue, agent_turn_min: minutesValue })) {
      setSteps(null);
      setMinutes(null);
      toast(t('sys.agent_saved'), 'ok');
    }
    setSaving(false);
  }

  const numberField = (id, label, shown, ok, min, max, set) => html`<div class="field">
    <label class="field-label" for=${id}>${label}</label>
    <input
      id=${id}
      class="input mono"
      type="number"
      inputmode="numeric"
      min=${min}
      max=${max}
      step="1"
      value=${shown}
      aria-invalid=${ok ? null : 'true'}
      aria-describedby="sys-agent-hint"
      onInput=${(e) => set(e.currentTarget.value)}
    />
  </div>`;

  return html`<${Card} title=${t('sys.agent_title')} icon=${IconModel}>
    <div class="form">
      <div class="grid grid-2">
        ${numberField('sys-agent-steps', t('sys.agent_steps'), stepsShown, stepsValue !== null, stepsSpec.min, stepsSpec.max, setSteps)}
        ${numberField('sys-agent-minutes', t('sys.agent_minutes'), minutesShown, minutesValue !== null, minutesSpec.min, minutesSpec.max, setMinutes)}
      </div>
      <p id="sys-agent-hint" class="field-hint">
        ${t('sys.agent_hint', { smin: stepsSpec.min, smax: stepsSpec.max, mmin: minutesSpec.min, mmax: minutesSpec.max })}
      </p>
      <div>
        <button type="button" class="btn btn-secondary" onClick=${save} disabled=${!valid || !changed || saving}>
          ${saving ? html`<span class="spinner" aria-hidden="true"></span>` : null}
          ${t('sys.agent_save')}
        </button>
      </div>
    </div>
  </${Card}>`;
}

// The keys GET /settings lists that no card above places, each with a
// control chosen by its type, so a new key can be changed here before
// anyone writes a card for it. Not shown when every key has its card.
function MoreCard({ settings, schema, put }) {
  if (!settings) return null;
  const keys = schema.keys.filter((k) => !PLACED.has(k.name) && k.name in settings);
  if (keys.length === 0) return null;
  return html`<${Card} title=${t('sys.more_title')} icon=${IconSystem}>
    <div class="form">
      ${keys.map((k) => html`<${MoreField} key=${k.name} spec=${k} value=${settings[k.name]} put=${put} />`)}
    </div>
  </${Card}>`;
}

function MoreField({ spec: s, value, put }) {
  const [draft, setDraft] = useState(/** @type {string|null} */ (null));
  const id = 'sys-more-' + s.name;
  const label = t('sys.' + s.label);

  if (s.type === 'bool') {
    return html`<label class="scene-check">
      <input type="checkbox" checked=${value} onChange=${(e) => put({ [s.name]: e.currentTarget.checked })} />
      ${label}
    </label>`;
  }

  if (s.type === 'enum') {
    return html`<div class="field">
      <label class="field-label" for=${id}>${label}</label>
      <select id=${id} class="input select" value=${value} onChange=${(e) => put({ [s.name]: e.currentTarget.value })}>
        ${s.values.map((v) => html`<option key=${v} value=${v}>${v}</option>`)}
      </select>
    </div>`;
  }

  // int and hosts: typed into a field, saved when it is left or Enter is
  // pressed, and only when the device would take it.
  const isInt = s.type === 'int';
  const shown = draft ?? String(value);
  const parsed = isInt ? wholeIn(shown, s) : tidyServers(shown);
  const ok = isInt ? parsed !== null : parsed.length > 0;
  async function commit() {
    if (draft === null) return;
    if (!ok || parsed === value) {
      if (ok) setDraft(null);
      return;
    }
    if (await put({ [s.name]: parsed })) setDraft(null);
  }
  const hint = isInt ? (s.zero_never ? `0, ${s.min}–${s.max}` : `${s.min}–${s.max}`) : null;
  return html`<div class="field">
    <label class="field-label" for=${id}>${label}</label>
    <input
      id=${id}
      class="input mono"
      type=${isInt ? 'number' : 'text'}
      inputmode=${isInt ? 'numeric' : null}
      min=${isInt ? s.min : null}
      max=${isInt ? s.max : null}
      step=${isInt ? '1' : null}
      maxlength=${isInt ? null : hostsLength(s)}
      spellcheck="false"
      autocapitalize="off"
      autocomplete="off"
      value=${shown}
      aria-invalid=${ok ? null : 'true'}
      aria-describedby=${hint ? id + '-hint' : null}
      onInput=${(e) => setDraft(e.currentTarget.value)}
      onChange=${commit}
    />
    ${hint ? html`<p id=${id + '-hint'} class="field-hint mono">${hint}</p>` : null}
  </div>`;
}

function SkeletonCard() {
  return html`<section class="card skeleton-card" aria-busy="true" aria-label=${t('common.loading')}>
    <div class="skel skel-title"></div>
    <div class="skel skel-line"></div>
    <br />
    <div class="skel skel-line"></div>
  </section>`;
}

function formatWhen(unix) {
  if (!Number.isFinite(unix) || unix < MIN_REAL_UNIX) return '';
  return new Intl.DateTimeFormat(locale(), { dateStyle: 'medium', timeStyle: 'short' }).format(new Date(unix * 1000));
}

function ClientsCard({ clients, error, reload }) {
  const [revoking, setRevoking] = useState('');

  async function revoke(c) {
    if (c.current) {
      confirmLogout();
      return;
    }
    const ok = await confirmDialog({
      title: t('clients.revoke_title'),
      body: t('clients.revoke_body', { label: c.label || t('clients.unknown') }),
      okText: t('clients.revoke'),
      danger: true,
    });
    if (!ok) return;
    setRevoking(c.id);
    try {
      await api('DELETE', '/clients/' + encodeURIComponent(c.id));
      toast(t('clients.revoked'), 'ok');
    } catch (e) {
      if (e.status !== 401) toast(t('clients.revoke_failed', { reason: describeError(e) }), 'err');
    } finally {
      setRevoking('');
      reload();
    }
  }

  const list = clients
    ? [...clients].sort((a, b) => (b.current ? 1 : 0) - (a.current ? 1 : 0) || (b.last_seen || 0) - (a.last_seen || 0))
    : null;

  return html`<${Card}
    title=${t('clients.title')}
    icon=${IconBrowser}
    actions=${list ? html`<span class="count-pill">${t('clients.count', { count: list.length })}</span>` : null}
  >
    ${error && !list
      ? html`<div class="alert tone-err" role="alert">
          <${IconAlert} size=${18} /><span>${t('clients.load_failed', { reason: describeError(error) })}</span>
        </div>`
      : !list
        ? html`<div class="skel skel-line"></div>`
        : list.length === 0
          ? html`<p class="stat-empty">${t('clients.none')}</p>`
          : html`<ul class="rows">
              ${list.map((c) => {
                const seen = formatWhen(c.last_seen);
                const created = formatWhen(c.created);
                const sub = [
                  created ? t('clients.paired_at', { when: created }) : '',
                  seen && !c.current ? t('clients.seen_at', { when: seen }) : '',
                ]
                  .filter(Boolean)
                  .join(' · ');
                return html`<li key=${c.id} class=${c.current ? 'row is-current' : 'row'}>
                  <div class="row-main">
                    <span class="row-title">
                      ${c.label || t('clients.unknown')}
                      ${c.current ? html` <${Badge} tone="accent">${t('clients.this')}</${Badge}>` : null}
                    </span>
                    <span class="row-sub">${sub || t('clients.no_time')}</span>
                  </div>
                  <button
                    type="button"
                    class="btn btn-ghost btn-sm"
                    onClick=${() => revoke(c)}
                    disabled=${revoking === c.id}
                  >
                    ${revoking === c.id ? html`<span class="spinner" aria-hidden="true"></span>` : null}
                    ${c.current ? t('clients.revoke_self') : t('clients.revoke')}
                  </button>
                </li>`;
              })}
            </ul>`}
  </${Card}>`;
}

// An ESP application image starts with this byte (esp_image_header_t).
const ESP_IMAGE_MAGIC = 0xe9;

/**
 * @typedef {Object} FirmwareInfo  GET /system/firmware
 * @property {boolean} supported  false: the board has no OTA slots
 * @property {string} version     the running image's
 * @property {string} board       the board it was built for
 * @property {string} running     the slot it runs from
 * @property {string|null} pending  uploaded, awaiting the device's answer
 */

/** A sentence for a system.firmware "failed" event's error code. */
function firmwareError(code) {
  if (code === 'aborted') return t('fw.aborted');
  return has('err.' + code) ? t('err.' + code) : t('err.unknown');
}

/**
 * Waits for the device to come back from a restart: polls GET /info until
 * a different boot answers. Returns a function that stops waiting.
 */
function waitForNewBoot(before, onBack, onSlow) {
  let stopped = false;
  const started = Date.now();
  const tick = async () => {
    if (stopped) return;
    try {
      const info = await api('GET', '/info');
      if (stopped) return;
      if (info?.boot_id && info.boot_id !== before) {
        onBack();
        return;
      }
    } catch {
      // Down while restarting: keep asking.
    }
    if (stopped) return;
    if (Date.now() - started > REBOOT_SLOW_MS) onSlow();
    setTimeout(tick, REBOOT_POLL_MS);
  };
  setTimeout(tick, REBOOT_POLL_MS);
  return () => {
    stopped = true;
  };
}

function FirmwareCard() {
  const [info, setInfo] = useState(/** @type {FirmwareInfo|null} */ (null));
  const [loadError, setLoadError] = useState(null);
  const [file, setFile] = useState(/** @type {File|null} */ (null));
  // null
  // | {phase: 'uploading', pct: number}
  // | {phase: 'confirm', version: string}       written, waiting for the device
  // | {phase: 'installing', version: string, slow?: boolean}  restarting into it
  // | {phase: 'done', version: string, running: string}
  // | {phase: 'rejected'}
  // | {phase: 'failed', reason: string, detail?: string}
  const [job, setJob] = useState(null);
  const alive = useRef(true);
  const cleanup = useRef([]);
  const input = useRef(null);

  async function load() {
    try {
      const r = await api('GET', '/system/firmware');
      if (!alive.current) return r;
      setInfo(r);
      setLoadError(null);
      return r;
    } catch (e) {
      if (alive.current && e.status !== 401) setLoadError(e);
      return null;
    }
  }

  function stopAll() {
    for (const fn of cleanup.current.splice(0)) fn();
  }

  useEffect(() => {
    alive.current = true;
    load();
    return () => {
      alive.current = false;
      stopAll();
    };
  }, []);

  async function pick(e) {
    const f = e.currentTarget.files?.[0] || null;
    setJob(null);
    if (!f) {
      setFile(null);
      return;
    }
    let first = -1;
    try {
      first = new Uint8Array(await f.slice(0, 1).arrayBuffer())[0] ?? -1;
    } catch {
      first = -1;
    }
    if (f.size === 0 || first !== ESP_IMAGE_MAGIC) {
      setFile(null);
      if (input.current) input.current.value = '';
      setJob({ phase: 'failed', reason: t('fw.not_image') });
      return;
    }
    setFile(f);
  }

  async function start() {
    if (!file) return;
    stopAll();
    let before = '';
    try {
      before = (await api('GET', '/info'))?.boot_id || '';
    } catch (e) {
      if (e.status !== 401) setJob({ phase: 'failed', reason: describeError(e) });
      return;
    }
    let expected = '';
    const set = (next) => alive.current && setJob(next);
    const finish = async () => {
      const now = await load();
      set({ phase: 'done', version: expected, running: now?.version || '' });
    };
    // Listening before uploading, so the device's answer is never missed.
    const unsubscribe = subscribe(['events'], (msg) => {
      if (msg.t !== 'event' || msg.topic !== 'system.firmware') return;
      const st = msg.payload?.state;
      const version = typeof msg.payload?.version === 'string' ? msg.payload.version : '';
      if (version) expected = version;
      if (st === 'received') {
        setJob((j) => (j?.phase === 'uploading' ? { phase: 'confirm', version } : j));
      } else if (st === 'confirmed') {
        set({ phase: 'installing', version: expected });
        cleanup.current.push(
          waitForNewBoot(
            before,
            () => {
              unsubscribe();
              finish();
            },
            () => setJob((j) => (j?.phase === 'installing' ? { ...j, slow: true } : j)),
          ),
        );
      } else if (st === 'rejected') {
        set({ phase: 'rejected' });
        load();
      } else if (st === 'failed') {
        setJob((j) =>
          j?.phase === 'failed' ? j : { phase: 'failed', reason: firmwareError(msg.payload?.error) },
        );
      }
    });
    cleanup.current.push(unsubscribe);

    set({ phase: 'uploading', pct: 0 });
    const req = upload('/system/firmware', file, {
      onProgress: (sent, total) => {
        const pct = total > 0 ? Math.min(100, Math.floor((sent / total) * 100)) : 0;
        setJob((j) => (j?.phase === 'uploading' ? { phase: 'uploading', pct } : j));
      },
    });
    cleanup.current.push(req.abort);
    try {
      const r = await req.done;
      if (typeof r?.version === 'string' && r.version) expected = r.version;
      setJob((j) => (j?.phase === 'uploading' ? { phase: 'confirm', version: expected } : j));
      load();
    } catch (e) {
      if (!alive.current || e.status === 401) return;
      set({ phase: 'failed', reason: describeError(e), detail: errorDetail(e) });
    }
  }

  function reset() {
    stopAll();
    setJob(null);
    setFile(null);
    if (input.current) input.current.value = '';
    load();
  }

  if (!info) {
    return loadError
      ? html`<${Card} title=${t('fw.title')} icon=${IconPackage}>
          <div class="alert tone-err" role="alert">
            <${IconAlert} size=${18} /><span>${t('fw.load_failed', { reason: describeError(loadError) })}</span>
          </div>
        </${Card}>`
      : html`<${SkeletonCard} />`;
  }

  const busy = job && (job.phase === 'uploading' || job.phase === 'confirm' || job.phase === 'installing');
  const waiting = !job && info.pending;
  return html`<${Card} title=${t('fw.title')} icon=${IconPackage}>
    <dl class="kv">
      <div><dt>${t('fw.version')}</dt><dd class="mono">${info.version || '—'}</dd></div>
      <div><dt>${t('fw.board')}</dt><dd class="mono">${info.board || '—'}</dd></div>
      ${info.running ? html`<div><dt>${t('fw.slot')}</dt><dd class="mono">${info.running}</dd></div>` : null}
    </dl>
    ${!info.supported
      ? html`<div class="inline-note"><${IconInfo} size=${15} /><span>${t('fw.unsupported')}</span></div>`
      : html`<div class="form fw-form">
          <div class="field">
            <label class="field-label" for="fw-file">${t('fw.file')}</label>
            <input
              id="fw-file"
              ref=${input}
              class="input"
              type="file"
              accept=".bin,application/octet-stream"
              disabled=${busy}
              onChange=${pick}
              aria-describedby="fw-file-hint"
            />
            <p id="fw-file-hint" class="field-hint">${t('fw.file_hint', { board: info.board })}</p>
          </div>
          <div class="form-actions">
            <button type="button" class="btn btn-primary" onClick=${start} disabled=${!file || busy}>
              <${IconUpload} size=${16} />${t('fw.upload')}
            </button>
          </div>
          ${waiting
            ? html`<div class="alert tone-warn" role="status">
                <${IconInfo} size=${18} />
                <span class="alert-body">${t('fw.pending', { version: info.pending })}</span>
              </div>`
            : null}
          ${job ? html`<${FirmwareStatus} job=${job} current=${info.version} onClose=${reset} />` : null}
        </div>`}
  </${Card}>`;
}

function FirmwareStatus({ job, current, onClose }) {
  if (job.phase === 'uploading') {
    return html`<div class="fw-progress" role="status">
      <div class="field-row">
        <span>${t('fw.uploading')}</span>
        <span class="mono">${new Intl.NumberFormat(locale(), { style: 'percent' }).format(job.pct / 100)}</span>
      </div>
      <div
        class="meter tone-accent"
        role="progressbar"
        aria-label=${t('fw.uploading')}
        aria-valuemin="0"
        aria-valuemax="100"
        aria-valuenow=${job.pct}
      >
        <div class="meter-fill" style=${{ width: job.pct + '%' }}></div>
      </div>
    </div>`;
  }
  if (job.phase === 'confirm') {
    return html`<div class="alert tone-warn" role="status">
      <span class="spinner" aria-hidden="true"></span>
      <span class="alert-body">
        <strong>${t('fw.on_device')}</strong><br />
        ${t('fw.on_device_body', { from: current, to: job.version || '?' })}
      </span>
    </div>`;
  }
  if (job.phase === 'installing') {
    return html`<div class="alert tone-accent" role="status">
      <span class="spinner" aria-hidden="true"></span>
      <span class="alert-body">
        <strong>${t('fw.installing')}</strong><br />
        ${job.slow ? t('sys.reboot_slow') : t('sys.reboot_wait')}
      </span>
    </div>`;
  }
  if (job.phase === 'done') {
    const ok = !job.version || job.running === job.version;
    return html`<div class=${ok ? 'alert tone-ok' : 'alert tone-err'} role="status">
      <${ok ? IconCheck : IconAlert} size=${18} />
      <span class="alert-body">
        ${ok ? t('fw.done', { version: job.running }) : t('fw.rolled_back', { version: job.running })}
        <button type="button" class="btn btn-link btn-inline" onClick=${onClose}>${t('common.ok')}</button>
      </span>
    </div>`;
  }
  if (job.phase === 'rejected') {
    return html`<div class="alert tone-muted" role="status">
      <${IconInfo} size=${18} />
      <span class="alert-body">
        ${t('fw.rejected')}
        <button type="button" class="btn btn-link btn-inline" onClick=${onClose}>${t('common.ok')}</button>
      </span>
    </div>`;
  }
  return html`<div class="alert tone-err" role="alert">
    <${IconAlert} size=${18} />
    <span class="alert-body">
      ${t('fw.failed', { reason: job.reason })}
      ${job.detail ? html`<br /><span class="err-detail mono">${job.detail}</span>` : null}
      <button type="button" class="btn btn-link btn-inline" onClick=${onClose}>${t('common.ok')}</button>
    </span>
  </div>`;
}

function MaintenanceCard({ onBack }) {
  // null | {phase: 'wait'|'slow'}
  const [reboot, setReboot] = useState(null);
  // null | 'sending' | 'waiting' | 'done' | 'cancelled' | 'failed' | 'unavailable'
  const [reset, setReset] = useState(null);
  const alive = useRef(true);
  const stopWait = useRef(null);

  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
      stopWait.current?.();
    };
  }, []);

  async function restart() {
    const ok = await confirmDialog({
      title: t('sys.reboot_title'),
      body: t('sys.reboot_body'),
      okText: t('sys.reboot_confirm'),
    });
    if (!ok) return;
    let before = '';
    try {
      before = (await api('GET', '/info'))?.boot_id || '';
      await api('POST', '/system/reboot');
    } catch (e) {
      if (e.status !== 401) toast(t('sys.reboot_failed', { reason: describeError(e) }), 'err');
      return;
    }
    const started = Date.now();
    setReboot({ phase: 'wait' });
    // Polls until a different boot answers; stops with the page.
    const tick = async () => {
      if (!alive.current) return;
      try {
        const info = await api('GET', '/info');
        if (info?.boot_id && info.boot_id !== before) {
          if (!alive.current) return;
          setReboot(null);
          toast(t('sys.reboot_done'), 'ok');
          onBack();
          return;
        }
      } catch {
        // Down while restarting: keep asking.
      }
      if (!alive.current) return;
      if (Date.now() - started > REBOOT_SLOW_MS) setReboot({ phase: 'slow' });
      setTimeout(tick, REBOOT_POLL_MS);
    };
    setTimeout(tick, REBOOT_POLL_MS);
  }

  function waitForReset() {
    let active = true;
    let accepted = false;
    let unsubscribe = () => {};
    let restore = () => {};
    const end = (state) => {
      if (!active) return;
      active = false;
      unsubscribe();
      // After "done" the (now inert) handler stays until the page goes, so
      // the 401s of a revoked token do not jump to the pairing page
      // underneath the result.
      if (state !== 'done') restore();
      if (alive.current) setReset(state);
    };
    unsubscribe = subscribe(
      ['events'],
      (msg) => {
        if (msg.t !== 'event' || msg.topic !== 'system.factory_reset') return;
        const st = msg.payload?.state;
        if (st === 'done' || st === 'cancelled' || st === 'failed') end(st);
      },
      { onState: (st) => accepted && st === 'unauthorized' && end('done') },
    );
    stopWait.current = () => {
      active = false;
      unsubscribe();
      restore();
    };
    // Once the device has accepted the request, the reset revokes this
    // browser: a 401 from then on means it went through.
    return () => {
      if (!active) return;
      accepted = true;
      restore = setUnauthorizedHandler(() => end('done'));
    };
  }

  async function factoryReset() {
    const ok = await confirmDialog({
      title: t('sys.reset_title'),
      body: t('sys.reset_body'),
      okText: t('sys.reset_confirm'),
      danger: true,
    });
    if (!ok) return;
    setReset('sending');
    // Listening before asking, so a quick answer on the device is not missed.
    const markAccepted = waitForReset();
    try {
      await api('POST', '/system/factory-reset');
      markAccepted();
      if (alive.current) setReset((r) => (r === 'sending' ? 'waiting' : r));
    } catch (e) {
      stopWait.current?.();
      stopWait.current = null;
      if (!alive.current || e.status === 401) return;
      if (e.status === 503 || e.code === 'unavailable' || e.code === 'unsupported') {
        setReset('unavailable');
      } else {
        setReset(null);
        toast(t('sys.reset_failed', { reason: describeError(e) }), 'err');
      }
    }
  }

  function stopWaiting() {
    stopWait.current?.();
    stopWait.current = null;
    setReset(null);
  }

  const resetBusy = reset === 'sending' || reset === 'waiting';
  return html`<${Card} title=${t('sys.maint_title')} icon=${IconSystem} class="list-card maint-card">
    <ul class="link-list">
      <li>
        <a class="link-row" href="#/scenes">
          <span class="dev-icon tone-accent"><${IconPackage} size=${18} /></span>
          <span class="dev-main">
            <span class="row-title">${t('nav.scenes')}</span>
            <span class="row-sub">${t('sys.scenes_desc')}</span>
          </span>
          <${IconChevronRight} size=${18} />
        </a>
      </li>
      <li>
        <a class="link-row" href="#/memory">
          <span class="dev-icon tone-accent"><${IconNote} size=${18} /></span>
          <span class="dev-main">
            <span class="row-title">${t('nav.memory')}</span>
            <span class="row-sub">${t('sys.memory_desc')}</span>
          </span>
          <${IconChevronRight} size=${18} />
        </a>
      </li>
      <li>
        <a class="link-row" href="#/logs">
          <span class="dev-icon tone-accent"><${IconLogs} size=${18} /></span>
          <span class="dev-main">
            <span class="row-title">${t('nav.logs')}</span>
            <span class="row-sub">${t('sys.logs_desc')}</span>
          </span>
          <${IconChevronRight} size=${18} />
        </a>
      </li>
      <li>
        <button type="button" class="link-row" onClick=${restart} disabled=${!!reboot || resetBusy}>
          <span class="dev-icon tone-warn"><${IconPower} size=${18} /></span>
          <span class="dev-main">
            <span class="row-title">${t('sys.reboot')}</span>
            <span class="row-sub">${t('sys.reboot_desc')}</span>
          </span>
        </button>
        ${reboot
          ? html`<div class="row-status">
              <div class="alert tone-accent" role="status">
                <span class="spinner" aria-hidden="true"></span>
                <span>
                  <strong>${t('sys.rebooting')}</strong><br />
                  ${reboot.phase === 'slow' ? t('sys.reboot_slow') : t('sys.reboot_wait')}
                </span>
              </div>
            </div>`
          : null}
      </li>
      <li>
        <button type="button" class="link-row" onClick=${factoryReset} disabled=${!!reboot || resetBusy || reset === 'done'}>
          <span class="dev-icon tone-err"><${IconReset} size=${18} /></span>
          <span class="dev-main">
            <span class="row-title">${t('sys.reset')}</span>
            <span class="row-sub">${t('sys.reset_desc')}</span>
          </span>
        </button>
        ${reset ? html`<div class="row-status"><${ResetStatus} state=${reset} onStop=${stopWaiting} onClose=${() => setReset(null)} /></div>` : null}
      </li>
      <li>
        <button type="button" class="link-row" onClick=${confirmLogout}>
          <span class="dev-icon tone-err"><${IconLogout} size=${18} /></span>
          <span class="dev-main">
            <span class="row-title">${t('app.logout')}</span>
            <span class="row-sub">${t('logout.desc')}</span>
          </span>
        </button>
      </li>
    </ul>
  </${Card}>`;
}

function ResetStatus({ state, onStop, onClose }) {
  if (state === 'sending' || state === 'waiting') {
    return html`<div class="alert tone-warn" role="status">
      <span class="spinner" aria-hidden="true"></span>
      <span class="alert-body">
        <strong>${t('sys.reset_on_device')}</strong><br />
        ${t('sys.reset_on_device_body')}
        <button type="button" class="btn btn-link btn-inline" onClick=${onStop}>${t('sys.reset_stop')}</button>
      </span>
    </div>`;
  }
  if (state === 'done') {
    return html`<div class="alert tone-ok" role="status">
      <${IconCheck} size=${18} />
      <span class="alert-body">
        <strong>${t('sys.reset_done')}</strong><br />
        ${t('sys.reset_done_body')}
        <button type="button" class="btn btn-link btn-inline" onClick=${() => navigate('/pair')}>${t('sys.reset_repair')}</button>
      </span>
    </div>`;
  }
  const text =
    state === 'cancelled'
      ? t('sys.reset_cancelled')
      : state === 'failed'
        ? t('sys.reset_not_started')
        : t('sys.reset_unavailable');
  return html`<div class=${state === 'cancelled' ? 'alert tone-muted' : 'alert tone-warn'} role="status">
    <${state === 'cancelled' ? IconInfo : IconAlert} size=${18} />
    <span class="alert-body">
      ${text}
      <button type="button" class="btn btn-link btn-inline" onClick=${onClose}>${t('common.ok')}</button>
    </span>
  </div>`;
}
