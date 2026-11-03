// Devices (spec 7.3): the registry with each device's boot state; a row
// expands to show the capabilities (caps) the driver declared.

import { html, useEffect, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError, errorDetail } from '../api.js';
import { t } from '../i18n/index.js';
import { formatCount } from '../util.js';
import { Card } from '../components/card.js';
import { Badge } from '../components/badge.js';
import { IconAlert, IconChevronDown, IconRefresh, classIcon } from '../icons.js';

/**
 * @typedef {Object} DeviceRow  one entry of GET /devices
 * @property {string} id      "<class>.<name>"
 * @property {string} class
 * @property {string} model   empty for devices that never registered
 * @property {'ok'|'degraded'|'missing'|'failed'} state
 * @property {Object<string, any>} caps
 */

// Classes with a translated name (`class.<name>`); others show as-is.
const KNOWN_CLASSES = new Set([
  'display',
  'touch',
  'imu',
  'power',
  'clock',
  'button',
  'audio_out',
  'audio_in',
  'storage',
  'network',
]);
const STATE_TONE = { ok: 'ok', degraded: 'warn', missing: 'muted', failed: 'err' };

export function DevicesPage() {
  const [devices, setDevices] = useState(null);
  const [error, setError] = useState(null);
  const [loading, setLoading] = useState(false);
  const [open, setOpen] = useState(() => new Set());

  async function load() {
    setLoading(true);
    try {
      const r = await api('GET', '/devices');
      setDevices(Array.isArray(r?.devices) ? r.devices : []);
      setError(null);
    } catch (e) {
      if (e.status !== 401) setError(e);
    } finally {
      setLoading(false);
    }
  }

  useEffect(() => {
    load();
  }, []);

  function toggle(id) {
    setOpen((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  }

  const list = devices || [];
  const counts = { ok: 0, other: 0 };
  for (const d of list) counts[d.state === 'ok' ? 'ok' : 'other']++;
  const allOpen = list.length > 0 && list.every((d) => open.has(d.id));

  return html`<div class="page">
    <header class="page-head">
      <div>
        <h1 class="page-title" tabindex="-1">${t('nav.devices')}</h1>
        <p class="page-sub">
          ${devices
            ? html`${t('dev.count', { count: formatCount(list.length) })}${' · '}${counts.other
                ? html`<span class="text-warn">${t('dev.some_bad', { count: formatCount(counts.other) })}</span>`
                : html`<span class="text-ok">${t('dev.all_ok')}</span>`}`
            : t('dev.subtitle')}
        </p>
      </div>
      <div class="page-head-actions">
        ${list.length
          ? html`<button
              type="button"
              class="btn btn-ghost btn-sm"
              onClick=${() => setOpen(allOpen ? new Set() : new Set(list.map((d) => d.id)))}
            >
              ${allOpen ? t('dev.collapse_all') : t('dev.expand_all')}
            </button>`
          : null}
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

    ${error && !devices
      ? html`<${Card} title=${t('dev.load_failed')} icon=${IconAlert} tone="err">
          <p class="muted">${describeError(error)}</p>
          ${errorDetail(error) ? html`<p class="err-detail mono">${errorDetail(error)}</p>` : null}
          <button type="button" class="btn btn-secondary" onClick=${load}>${t('common.retry')}</button>
        </${Card}>`
      : !devices
        ? html`<div class="card list-card" aria-busy="true" aria-label=${t('common.loading')}>
            ${[0, 1, 2, 3, 4, 5].map(
              (i) => html`<div key=${i} class="dev-row skeleton-row">
                <div class="skel skel-icon"></div>
                <div class="skel skel-line"></div>
              </div>`,
            )}
          </div>`
        : list.length === 0
          ? html`<div class="empty">
              <p class="empty-title">${t('dev.empty_title')}</p>
              <p class="muted">${t('dev.empty_body')}</p>
            </div>`
          : html`<ul class="card list-card dev-list">
              ${list.map((d) => html`<${DeviceItem} key=${d.id} dev=${d} open=${open.has(d.id)} onToggle=${toggle} />`)}
            </ul>`}
  </div>`;
}

function DeviceItem({ dev, open, onToggle }) {
  const Icon = classIcon(dev.class);
  const panelId = 'dev-' + dev.id.replace(/[^A-Za-z0-9_-]/g, '_');
  const caps = dev.caps && typeof dev.caps === 'object' ? Object.entries(dev.caps) : [];
  return html`<li class=${open ? 'dev-item is-open' : 'dev-item'}>
    <button
      type="button"
      class="dev-row"
      aria-expanded=${open ? 'true' : 'false'}
      aria-controls=${panelId}
      onClick=${() => onToggle(dev.id)}
    >
      <span class=${'dev-icon tone-' + (STATE_TONE[dev.state] || 'muted')}><${Icon} size=${18} /></span>
      <span class="dev-main">
        <span class="dev-id mono">${dev.id}</span>
        <span class="dev-sub">
          ${KNOWN_CLASSES.has(dev.class) ? t('class.' + dev.class) : dev.class}${dev.model ? ' · ' : ''}${dev.model
            ? html`<span class="mono">${dev.model}</span>`
            : null}
        </span>
      </span>
      <${Badge} tone=${STATE_TONE[dev.state] || 'muted'}>${STATE_TONE[dev.state] ? t('state.' + dev.state) : dev.state}</${Badge}>
      <span class="dev-chevron"><${IconChevronDown} size=${18} /></span>
    </button>
    ${open
      ? html`<div class="dev-panel" id=${panelId}>
          ${caps.length === 0
            ? html`<p class="muted">${dev.model ? t('dev.no_caps') : t('dev.unregistered')}</p>`
            : html`<dl class="caps">
                ${caps.map(
                  ([k, v]) => html`<div key=${k} class="cap">
                    <dt class="mono">${k}</dt>
                    <dd><${CapValue} value=${v} /></dd>
                  </div>`,
                )}
              </dl>`}
        </div>`
      : null}
  </li>`;
}

function CapValue({ value }) {
  if (value === true) return html`<span class="cap-bool on">${t('dev.yes')}</span>`;
  if (value === false) return html`<span class="cap-bool">${t('dev.no')}</span>`;
  if (Array.isArray(value)) {
    return html`<span class="chips">${value.map((v, i) => html`<span key=${i} class="chip mono">${fmt(v)}</span>`)}</span>`;
  }
  return html`<span class="mono">${fmt(value)}</span>`;
}

function fmt(v) {
  if (v === null || v === undefined) return '—';
  if (typeof v === 'object') return JSON.stringify(v);
  return String(v);
}
