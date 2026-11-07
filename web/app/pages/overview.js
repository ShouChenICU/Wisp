// Overview (spec 7.3): battery, Wi-Fi, storage, uptime and firmware at a
// glance. A snapshot comes from REST; while the tab is visible the page
// also follows the device's events and patches the snapshot in place.

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError, errorDetail, getInfo } from '../api.js';
import { t } from '../i18n/index.js';
import { subscribe } from '../ws.js';
import { Card } from '../components/card.js';
import { Badge } from '../components/badge.js';
import { SignalBars, Ssid, allRejected, reasonText } from '../components/wifi.js';
import {
  IconAlert,
  IconBattery,
  IconBolt,
  IconChat,
  IconChevronRight,
  IconClock,
  IconPackage,
  IconRefresh,
  IconStorage,
  IconWifi,
  WispGlyph,
} from '../icons.js';
import { formatBytes, formatCount, formatUptime, useNow, useVisible } from '../util.js';

// Battery percentage and heap have no events, so the snapshot is
// refreshed this often while the page is visible.
const REFRESH_MS = 30000;
// A socket (re)opening right after a load does not trigger another one.
const RELOAD_GAP_MS = 2000;

/**
 * @typedef {Object} Battery
 * @property {number} percent   0-100, or -1 when the gauge does not know
 * @property {boolean} charging
 * @property {boolean} usb_present
 *
 * @typedef {Object} Wifi  GET /wifi, kept current by net.wifi.state / net.ap.state
 * @property {'off'|'connecting'|'backoff'|'connected'} state
 * @property {string} ssid
 * @property {string} ip
 * @property {number} rssi
 * @property {''|'not_found'|'auth_failed'|'other'} reason
 * @property {{ssid: string, ssid_hex: string, auth_failed: boolean}[]} saved
 * @property {{active: boolean, ssid: string, clients: number}} ap
 */

export function OverviewPage() {
  const visible = useVisible();
  const [snap, setSnap] = useState(null); // {info, device, wifi, battery, sd, at}
  // undefined: not loaded yet; 'unavailable': 404/503 (older firmware, or the
  // agent is off); otherwise GET /agent/conversation's body. Kept apart from
  // snap since its own fetch can fail or lag behind without blunting the
  // rest of the page.
  const [agent, setAgent] = useState(undefined);
  const [error, setError] = useState(null);
  const [live, setLive] = useState('closed');
  const [loading, setLoading] = useState(false);
  const alive = useRef(true);
  const inFlight = useRef(false);
  const again = useRef(false);
  const lastDone = useRef(0);

  // Catching up after the socket (re)opens: skipped when a load is running
  // or has just finished, since that one already saw the current state.
  function catchUp() {
    if (inFlight.current || Date.now() - lastDone.current < RELOAD_GAP_MS) return;
    load();
  }

  // Load again, after the running load if there is one: it may predate
  // what prompted this (dropped messages, or an event about state only a
  // load returns).
  function reload() {
    if (inFlight.current) again.current = true;
    else load();
  }

  async function load() {
    if (inFlight.current) return;
    inFlight.current = true;
    again.current = false;
    setLoading(true);
    try {
      const [info, device, wifi] = await Promise.all([
        getInfo(),
        api('GET', '/device'),
        api('GET', '/wifi'),
      ]);
      if (!alive.current) return;
      setSnap({
        info,
        device,
        wifi,
        battery: device?.status?.battery ?? null,
        sd: null,
        at: Date.now(),
      });
      setError(null);
    } catch (e) {
      if (alive.current && e.status !== 401) setError(e);
    } finally {
      inFlight.current = false;
      lastDone.current = Date.now();
      if (alive.current) setLoading(false);
    }
    loadAgent();
    if (again.current && alive.current) load();
  }

  // Its own request, apart from the Promise.all above: older firmware
  // without this route (or the agent turned off) must not blunt the rest
  // of the page, only this one card.
  async function loadAgent() {
    try {
      const c = await api('GET', '/agent/conversation');
      if (alive.current) setAgent(c);
    } catch (e) {
      if (alive.current && e.status !== 401) setAgent('unavailable');
    }
  }

  function onEvent(msg) {
    if (msg.t === 'dropped') {
      reload();
      return;
    }
    if (msg.t === 'agent') {
      if (msg.k === 'state') {
        setAgent((a) =>
          a && a !== 'unavailable' ? { ...a, state: msg.state, tool: msg.state === 'tool' ? msg.tool : null } : a,
        );
      }
      return;
    }
    const p = msg.payload || {};
    // The saved list is only in GET /wifi: a rejected password may have
    // parked a network (the card then asks for a new password), and a
    // connection may follow a network added or re-entered elsewhere.
    if (
      msg.topic === 'net.wifi.state' &&
      ((p.state === 'backoff' && p.reason === 'auth_failed') || p.state === 'connected')
    ) {
      reload();
    }
    setSnap((s) => {
      if (!s) return s;
      switch (msg.topic) {
        case 'net.wifi.state':
          return { ...s, wifi: { ...s.wifi, ...p } };
        case 'net.ap.state':
          return { ...s, wifi: { ...s.wifi, ap: { ...s.wifi?.ap, ...p } } };
        case 'power.charging.changed':
          return {
            ...s,
            battery: {
              ...s.battery,
              charging: !!p.charging,
              usb_present: !!p.usb_present,
              percent: p.battery_percent ?? s.battery?.percent ?? -1,
            },
          };
        case 'power.battery.low':
          return { ...s, battery: { ...s.battery, percent: p.battery_percent } };
        case 'storage.sd.inserted':
          return { ...s, sd: 'ok' };
        case 'storage.sd.removed':
          return { ...s, sd: 'missing' };
        default:
          return s;
      }
    });
  }

  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
    };
  }, []);

  // Only while visible: hidden tabs give their WebSocket slot back.
  useEffect(() => {
    if (!visible) return undefined;
    load();
    const unsubscribe = subscribe(['events', 'agent'], onEvent, {
      onState: (st) => {
        setLive(st);
        // Events sent while the socket was down are lost; catch up.
        if (st === 'open') catchUp();
      },
    });
    const timer = setInterval(load, REFRESH_MS);
    return () => {
      unsubscribe();
      clearInterval(timer);
      setLive('closed');
    };
  }, [visible]);

  const info = snap?.info;
  const boot = snap?.device?.boot;

  return html`<div class="page">
    <header class="hero">
      <div class="hero-glyph"><${WispGlyph} size=${56} /></div>
      <div class="hero-text">
        <h1 class="page-title" tabindex="-1">${info?.name || 'Wisp'}</h1>
        <p class="page-sub">
          ${info ? t('common.fw', { version: info.fw || '—' }) : t('ov.loading_info')}${boot?.board ? ` · ${boot.board}` : ''}
        </p>
      </div>
      <div class="hero-side">
        <${LiveBadge} state=${live} />
        <button
          type="button"
          class="btn btn-icon btn-ghost"
          onClick=${load}
          disabled=${loading}
          aria-label=${t('common.refresh')}
          title=${t('common.refresh')}
        >
          <${IconRefresh} size=${18} class=${loading ? 'spin' : ''} />
        </button>
      </div>
    </header>

    ${error && !snap
      ? html`<${Card} title=${t('ov.load_failed')} icon=${IconAlert} tone="err">
          <p class="muted">${describeError(error)}</p>
          ${errorDetail(error) ? html`<p class="err-detail mono">${errorDetail(error)}</p>` : null}
          <button type="button" class="btn btn-secondary" onClick=${load}>${t('common.retry')}</button>
        </${Card}>`
      : !snap
        ? html`<${Skeleton} />`
        : html`<div class="grid grid-overview">
            <${BatteryCard} battery=${snap.battery} />
            <${WifiCard} wifi=${snap.wifi} />
            <${StorageCard} device=${snap.device} sd=${snap.sd} />
            <${UptimeCard} device=${snap.device} at=${snap.at} />
            <${FirmwareCard} info=${info} device=${snap.device} />
            <${AssistantCard} agent=${agent} />
          </div>`}
    ${error && snap
      ? html`<p class="stale-note" role="status">
          <${IconAlert} size=${16} /> ${t('ov.stale', { error: describeError(error) })}
        </p>`
      : null}
  </div>`;
}

function LiveBadge({ state }) {
  if (state === 'open') return html`<${Badge} tone="ok" dot pulse>${t('live.open')}</${Badge}>`;
  if (state === 'connecting') return html`<${Badge} tone="muted" dot>${t('live.connecting')}</${Badge}>`;
  if (state === 'retrying') return html`<${Badge} tone="warn" dot>${t('live.retrying')}</${Badge}>`;
  return html`<${Badge} tone="muted" dot>${t('live.closed')}</${Badge}>`;
}

function Skeleton() {
  return html`<div class="grid grid-overview" aria-busy="true" aria-label=${t('common.loading')}>
    ${[0, 1, 2, 3, 4, 5].map(
      (i) => html`<section key=${i} class="card skeleton-card">
        <div class="skel skel-title"></div>
        <div class="skel skel-big"></div>
        <div class="skel skel-line"></div>
      </section>`,
    )}
  </div>`;
}

function BatteryCard({ battery }) {
  if (!battery) {
    return html`<${Card} title=${t('bat.title')} icon=${IconBattery}>
      <p class="stat-empty">${t('bat.none')}</p>
    </${Card}>`;
  }
  const known = Number.isFinite(battery.percent) && battery.percent >= 0;
  const pct = known ? Math.max(0, Math.min(100, Math.round(battery.percent))) : 0;
  const low = known && pct <= 15;
  const tone = battery.charging ? 'ok' : low ? 'err' : 'accent';
  const status = battery.charging
    ? html`<${Badge} tone="ok"><${IconBolt} size=${13} />${t('bat.charging')}</${Badge}>`
    : battery.usb_present
      ? html`<${Badge} tone="accent">${t('bat.usb')}</${Badge}>`
      : html`<${Badge} tone=${low ? 'err' : 'muted'}>${low ? t('bat.low') : t('bat.on_battery')}</${Badge}>`;
  return html`<${Card} title=${t('bat.title')} icon=${IconBattery} tone=${tone} actions=${status}>
    <div class="stat">
      <span class="stat-value">${known ? pct : '—'}</span><span class="stat-unit">${known ? '%' : ''}</span>
    </div>
    <div
      class=${'meter tone-' + tone}
      role="meter"
      aria-label=${t('bat.title')}
      aria-valuemin="0"
      aria-valuemax="100"
      aria-valuenow=${known ? pct : undefined}
    >
      <div class="meter-fill" style=${{ width: pct + '%' }}></div>
    </div>
  </${Card}>`;
}

function WifiCard({ wifi }) {
  const w = wifi || {};
  let badge;
  let body;
  if (w.state === 'connected') {
    badge = html`<${Badge} tone="ok" dot>${t('wifi.connected')}</${Badge}>`;
    body = html`<div class="stat-text"><${Ssid} ssid=${w.ssid} /></div>
      <dl class="kv">
        <div><dt>${t('wifi.ip')}</dt><dd class="mono">${w.ip || '—'}</dd></div>
        <div>
          <dt>${t('wifi.signal')}</dt>
          <dd><${SignalBars} rssi=${w.rssi} /> <span class="mono dim">${w.rssi} dBm</span></dd>
        </div>
      </dl>`;
  } else if (w.state === 'connecting') {
    badge = html`<${Badge} tone="warn" dot pulse>${t('wifi.connecting')}</${Badge}>`;
    body = html`<div class="stat-text">${w.ssid ? html`<${Ssid} ssid=${w.ssid} />` : t('wifi.connecting')}</div>
      <p class="muted">${t('wifi.connecting_hint')}</p>`;
  } else if (w.state === 'backoff' && allRejected(w)) {
    badge = html`<${Badge} tone="err" dot>${t('wifi.rejected')}</${Badge}>`;
    body = html`<div class="stat-text">${w.ssid ? html`<${Ssid} ssid=${w.ssid} />` : t('wifi.off')}</div>
      <p class="muted">${t('wifi.rejected_hint')}</p>
      <a href="#/network" class="link card-link">${t('wifi.settings_link')}<${IconChevronRight} size=${14} /></a>`;
  } else if (w.state === 'backoff') {
    badge = html`<${Badge} tone="warn" dot>${t('wifi.backoff')}</${Badge}>`;
    body = html`<div class="stat-text">${w.ssid ? html`<${Ssid} ssid=${w.ssid} />` : t('wifi.off')}</div>
      <p class="muted">${t('wifi.retry_hint', { reason: reasonText(w.reason) })}</p>`;
  } else {
    badge = html`<${Badge} tone="muted" dot>${t('wifi.off')}</${Badge}>`;
    body = html`<div class="stat-text dim">${t('wifi.off')}</div>
      <p class="muted">${w.saved && w.saved.length ? t('wifi.saved_count', { count: w.saved.length }) : t('wifi.none_saved')}</p>
      <a href="#/network" class="link card-link">${t('wifi.settings_link')}<${IconChevronRight} size=${14} /></a>`;
  }
  const ap = w.ap && w.ap.active
    ? html`<div class="inline-note">
        <${IconWifi} size=${15} />
        <span>${t('wifi.ap_on', { ssid: w.ap.ssid, clients: w.ap.clients })}</span>
      </div>`
    : null;
  return html`<${Card} title=${t('wifi.title')} icon=${IconWifi} tone=${w.state === 'connected' ? 'ok' : 'accent'} actions=${badge}>
    ${body}${ap}
  </${Card}>`;
}

const STORAGE_NAME = { 'storage.int': 'storage.int', 'storage.sd': 'storage.sd' };
const STATE_TONE = { ok: 'ok', degraded: 'warn', missing: 'muted', failed: 'err' };

function StorageCard({ device, sd }) {
  const caps = new Map();
  for (const d of device?.manifest?.devices || []) caps.set(d.id, d.caps || {});
  const list = (device?.boot?.devices || []).filter((d) => d.class === 'storage');
  return html`<${Card} title=${t('storage.title')} icon=${IconStorage}>
    ${list.length === 0
      ? html`<p class="stat-empty">${t('storage.none')}</p>`
      : html`<ul class="rows">
          ${list.map((d) => {
            const state = d.id === 'storage.sd' && sd ? sd : d.state;
            const c = caps.get(d.id) || {};
            const detail = [c.fs ? String(c.fs).toUpperCase() : null, c.removable ? t('storage.removable') : null]
              .filter(Boolean)
              .join(' · ');
            return html`<li key=${d.id} class="row">
              <div class="row-main">
                <span class="row-title">${STORAGE_NAME[d.id] ? t(STORAGE_NAME[d.id]) : d.id}</span>
                ${detail ? html`<span class="row-sub">${detail}</span>` : null}
              </div>
              <${Badge} tone=${STATE_TONE[state] || 'muted'}>${STATE_TONE[state] ? t('state.' + state) : state}</${Badge}>
            </li>`;
          })}
        </ul>`}
  </${Card}>`;
}

function UptimeCard({ device, at }) {
  const now = useNow(1000);
  const status = device?.status || {};
  const base = Number(status.uptime_ms);
  const uptime = Number.isFinite(base) ? base + (now - at) : NaN;
  const heap = status.heap || {};
  return html`<${Card} title=${t('uptime.title')} icon=${IconClock}>
    <div class="stat"><span class="stat-value stat-value-sm">${formatUptime(uptime)}</span></div>
    <dl class="kv">
      ${device?.boot?.boot_ms != null
        ? html`<div><dt>${t('uptime.boot')}</dt><dd class="mono">${device.boot.boot_ms} ms</dd></div>`
        : null}
      ${Number.isFinite(heap.internal_free)
        ? html`<div><dt>${t('uptime.heap')}</dt><dd class="mono">${formatBytes(heap.internal_free)}</dd></div>`
        : null}
      ${Number.isFinite(heap.large_free) && heap.large_free > 0
        ? html`<div><dt>${t('uptime.psram')}</dt><dd class="mono">${formatBytes(heap.large_free)}</dd></div>`
        : null}
    </dl>
  </${Card}>`;
}

function FirmwareCard({ info, device }) {
  const devices = device?.boot?.devices || [];
  const bad = devices.filter((d) => d.state !== 'ok').length;
  return html`<${Card} title=${t('fw.title')} icon=${IconPackage}>
    <div class="stat"><span class="stat-value stat-value-sm mono">${info?.fw || '—'}</span></div>
    <dl class="kv">
      <div><dt>${t('fw.board')}</dt><dd class="mono">${device?.boot?.board || device?.manifest?.board || '—'}</dd></div>
      <div>
        <dt>${t('fw.devices')}</dt>
        <dd>
          <a href="#/devices" class="link">
            ${bad
              ? t('fw.devices_bad', { count: formatCount(devices.length), bad: formatCount(bad) })
              : t('fw.devices_ok', { count: formatCount(devices.length) })}<${IconChevronRight} size=${14} />
          </a>
        </dd>
      </div>
    </dl>
  </${Card}>`;
}

// agent-core spec 3.8's `state` values, as GET /agent/conversation reports
// them (agent-core spec 7.1): idle, thinking, tool (+tool name), output,
// waiting_confirm, error. chat.js's running status line groups
// thinking/tool/output as "running" the same way.
function AssistantCard({ agent }) {
  if (agent === 'unavailable') {
    return html`<${Card} title=${t('ai.title')} icon=${IconChat}>
      <p class="stat-empty">${t('ai.unavailable')}</p>
    </${Card}>`;
  }
  if (!agent) {
    return html`<${Card} title=${t('ai.title')} icon=${IconChat}>
      <p class="stat-empty">${t('common.loading')}</p>
    </${Card}>`;
  }
  let badge;
  if (agent.state === 'waiting_confirm') {
    badge = html`<${Badge} tone="warn" dot>${t('chat.confirm_title')}</${Badge}>`;
  } else if (agent.state === 'error') {
    badge = html`<${Badge} tone="err">${t('state.failed')}</${Badge}>`;
  } else if (agent.state === 'tool') {
    badge = html`<${Badge} tone="accent" dot pulse>${t('chat.tool_running', { name: agent.tool || '' })}</${Badge}>`;
  } else if (agent.state === 'thinking' || agent.state === 'output') {
    badge = html`<${Badge} tone="accent" dot pulse>${t('chat.thinking')}</${Badge}>`;
  } else if (agent.state === 'idle') {
    badge = html`<${Badge} tone="muted">${t('ai.idle')}</${Badge}>`;
  } else {
    badge = html`<${Badge} tone="muted">${agent.state}</${Badge}>`;
  }
  const tone = agent.state === 'error' ? 'err' : agent.state === 'waiting_confirm' ? 'warn' : 'accent';
  return html`<${Card} title=${t('ai.title')} icon=${IconChat} tone=${tone} actions=${badge}>
    ${agent.model
      ? html`<div class="stat-text">${agent.model.name}<span class="mono dim"> · ${agent.model.model}</span></div>`
      : html`<p class="muted">${t('ai.no_model')}</p>`}
    <dl class="kv">
      <div><dt>${t('ai.messages')}</dt><dd>${formatCount(agent.total)}</dd></div>
    </dl>
    ${!agent.model
      ? html`<div><a href="#/models" class="link card-link">${t('chat.model_link')}<${IconChevronRight} size=${14} /></a></div>`
      : null}
    <div><a href="#/chat" class="link card-link">${t('ai.go_chat')}<${IconChevronRight} size=${14} /></a></div>
  </${Card}>`;
}
