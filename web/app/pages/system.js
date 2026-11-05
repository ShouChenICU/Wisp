// System (spec 7.3): display and sound settings, the device's own screen
// language, its time zone and network time, paired browsers, firmware,
// restart and factory reset. The phone layout also reaches the log page
// from here.
//
// Network time (spec 5.6): GET /system/time shows the last sync; "Sync
// now" posts /system/time/sync (409 no_network without Wi-Fi) and then
// waits for the outcome, subscribing to events only for that wait (see
// TimeSyncCard's syncNow()). Times are shown in the device's own time zone
// (utc_offset_min), not the browser's.
//
// Restart: the device answers 202 and restarts a moment later; the page
// then polls GET /info until it answers with a different boot_id, which
// is the new boot (an answer from the old one before it went down does
// not count).
//
// Factory reset: the device asks its user to confirm on its own screen
// and reports the outcome as a system.factory_reset event ({state:
// "done"|"cancelled"}). A reset revokes every paired browser, so this
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
import { Card } from '../components/card.js';
import { Badge } from '../components/badge.js';
import { confirmDialog } from '../components/dialog.js';
import { toast } from '../components/toast.js';
import {
  IconAlert,
  IconBrowser,
  IconCheck,
  IconChevronRight,
  IconClock,
  IconDisplay,
  IconGlobe,
  IconInfo,
  IconLogout,
  IconLogs,
  IconMoon,
  IconPackage,
  IconPower,
  IconReset,
  IconSpeaker,
  IconSun,
  IconSystem,
  IconUpload,
} from '../icons.js';

// SystemSettings' accepted ranges (spec 5.4).
const BRIGHTNESS_MIN = 10;
const BRIGHTNESS_MAX = 255;
const VOLUME_MAX = 100;
// Screen-off choices in seconds; 0 = never.
const SCREEN_OFF = [0, 15, 30, 60, 120, 300, 600];
// Time zone choices in minutes east of UTC: the offsets in use today.
// SystemSettings takes any whole minute from -720 to 840.
const UTC_OFFSETS = [
  -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180, -120, -60, 0, 60, 120,
  180, 210, 240, 270, 300, 330, 345, 360, 390, 420, 480, 525, 540, 570, 600, 630, 660, 720, 765,
  780, 840,
];
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
 * @typedef {Object} Settings  GET/PUT /settings
 * @property {number} brightness   10-255
 * @property {number} volume       0-100
 * @property {number} screen_off_s 0 (never) or 15-3600
 * @property {'zh'|'ja'|'en'} language  the device's own UI language
 * @property {number} utc_offset_min  the time zone, minutes east of UTC (-720 to 840)
 * @property {string} ntp_servers  1-2 time servers, comma-separated
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
  const [clients, setClients] = useState(/** @type {Client[]|null} */ (null));
  const [error, setError] = useState(null);
  const [clientsError, setClientsError] = useState(null);
  const alive = useRef(true);

  async function loadSettings() {
    try {
      const s = await api('GET', '/settings');
      if (!alive.current) return;
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

  return html`<div class="page">
    <header class="page-head">
      <div>
        <h1 class="page-title" tabindex="-1">${t('nav.system')}</h1>
        <p class="page-sub">${t('sys.sub')}</p>
      </div>
    </header>

    ${error && !settings
      ? html`<${Card} title=${t('sys.load_failed')} icon=${IconAlert} tone="err">
          <p class="muted">${describeError(error)}</p>
          ${errorDetail(error) ? html`<p class="err-detail mono">${errorDetail(error)}</p>` : null}
          <button type="button" class="btn btn-secondary" onClick=${loadAll}>${t('common.retry')}</button>
        </${Card}>`
      : null}

    <div class="grid grid-2">
      <${DisplayCard} settings=${settings} put=${put} />
      <${LanguageCard} settings=${settings} put=${put} />
      <${TimeZoneCard} settings=${settings} put=${put} />
      <${TimeSyncCard} settings=${settings} put=${put} />
    </div>
    <${ClientsCard} clients=${clients} error=${clientsError} reload=${loadClients} />
    <${FirmwareCard} />
    <${MaintenanceCard} onBack=${loadAll} />
  </div>`;
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

function DisplayCard({ settings, put }) {
  if (!settings) return html`<${SkeletonCard} />`;
  const choices = SCREEN_OFF.includes(settings.screen_off_s)
    ? SCREEN_OFF
    : [...SCREEN_OFF, settings.screen_off_s].sort((a, b) => a - b);
  return html`<${Card} title=${t('sys.display_title')} icon=${IconDisplay}>
    <div class="form">
      <${RangeField}
        id="sys-brightness"
        label=${t('sys.brightness')}
        icon=${IconSun}
        min=${BRIGHTNESS_MIN}
        max=${BRIGHTNESS_MAX}
        value=${settings.brightness}
        format=${(v) => percent(v, BRIGHTNESS_MIN, BRIGHTNESS_MAX)}
        onCommit=${(v) => put({ brightness: v })}
      />
      <${RangeField}
        id="sys-volume"
        label=${t('sys.volume')}
        icon=${IconSpeaker}
        min=${0}
        max=${VOLUME_MAX}
        value=${settings.volume}
        format=${(v) => percent(v, 0, VOLUME_MAX)}
        onCommit=${(v) => put({ volume: v })}
      />
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

function LanguageCard({ settings, put }) {
  if (!settings) return html`<${SkeletonCard} />`;
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
              toast(t('sys.lang_saved', { lang: LANGS.find((l) => l.id === v)?.label || v }), 'ok');
            }
          }}
          aria-describedby="sys-lang-hint"
        >
          ${LANGS.map((l) => html`<option key=${l.id} value=${l.id} lang=${l.html}>${l.label}</option>`)}
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

function TimeZoneCard({ settings, put }) {
  if (!settings) return html`<${SkeletonCard} />`;
  const choices = UTC_OFFSETS.includes(settings.utc_offset_min)
    ? UTC_OFFSETS
    : [...UTC_OFFSETS, settings.utc_offset_min].sort((a, b) => a - b);
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
    </div>
  </${Card}>`;
}

// How long "Sync now" waits for the outcome event: two servers at their
// query timeout plus name lookups, with room to spare.
const TIME_SYNC_WAIT_MS = 20000;
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

function TimeSyncCard({ settings, put }) {
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

  // The outcome normally arrives as a system.time event. The event socket
  // and the POST are separate connections, so the device may answer
  // before it has the socket's subscription: the status is also polled,
  // and a sync it shows counts as the answer. With neither by
  // TIME_SYNC_WAIT_MS, the attempt failed.
  function syncNow() {
    if (syncing) return;
    setSyncing(true);
    const before = status?.last_sync ?? null;
    let posted = false;
    let over = false;
    let timer = 0;
    let poller = 0;
    const finish = (ok, server) => {
      if (over) return;
      over = true;
      unsubscribe();
      clearTimeout(timer);
      clearInterval(poller);
      stopWait.current = null;
      if (!alive.current) return;
      setSyncing(false);
      load();
      if (ok === true) toast(t('sys.time_synced', { server }), 'ok');
      else if (ok === false) toast(t('sys.time_failed'), 'err');
    };
    const poll = async () => {
      try {
        const s = await api('GET', '/system/time');
        if (s?.synced && s.last_sync !== before) finish(true, s.server);
      } catch {
        // The next poll or the timeout decides.
      }
    };
    const unsubscribe = subscribe(
      ['events'],
      (msg) => {
        if (msg.t !== 'event' || msg.topic !== 'system.time') return;
        const ok = msg.payload?.state === 'synced';
        finish(ok, String(msg.payload?.server || ''));
      },
      {
        onState: (st) => {
          if (st !== 'open' || posted) return;
          posted = true;
          api('POST', '/system/time/sync').then(
            () => {
              if (!over) poller = setInterval(poll, TIME_SYNC_POLL_MS);
            },
            (e) => {
              finish(null);
              if (e.status !== 401) toast(describeError(e), 'err');
            },
          );
        },
      },
    );
    stopWait.current = () => {
      over = true;
      unsubscribe();
      clearTimeout(timer);
      clearInterval(poller);
    };
    timer = setTimeout(() => finish(false), TIME_SYNC_WAIT_MS);
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
            maxlength="127"
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
  // null | 'sending' | 'waiting' | 'done' | 'cancelled' | 'unavailable'
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
        if (st === 'done' || st === 'cancelled') end(st);
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
  const text = state === 'cancelled' ? t('sys.reset_cancelled') : t('sys.reset_unavailable');
  return html`<div class=${state === 'cancelled' ? 'alert tone-muted' : 'alert tone-warn'} role="status">
    <${state === 'cancelled' ? IconInfo : IconAlert} size=${18} />
    <span class="alert-body">
      ${text}
      <button type="button" class="btn btn-link btn-inline" onClick=${onClose}>${t('common.ok')}</button>
    </span>
  </div>`;
}
