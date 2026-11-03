// System (spec 7.3): display and sound settings, the device's own screen
// language, paired browsers, restart and factory reset. The phone layout
// also reaches the log page from here.
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

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError, errorDetail, logout, setUnauthorizedHandler } from '../api.js';
import { navigate } from '../router.js';
import { LANGS, locale, t } from '../i18n/index.js';
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
  IconDisplay,
  IconGlobe,
  IconInfo,
  IconLogout,
  IconLogs,
  IconMoon,
  IconPower,
  IconReset,
  IconSpeaker,
  IconSun,
  IconSystem,
} from '../icons.js';

// SystemSettings' accepted ranges (spec 5.4).
const BRIGHTNESS_MIN = 10;
const BRIGHTNESS_MAX = 255;
const VOLUME_MAX = 100;
// Screen-off choices in seconds; 0 = never.
const SCREEN_OFF = [0, 15, 30, 60, 120, 300, 600];
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
    </div>
    <${ClientsCard} clients=${clients} error=${clientsError} reload=${loadClients} />
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
