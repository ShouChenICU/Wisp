// Network (spec 7.3): the current connection, the saved networks (up to
// five, each removable) and a scan to add one. While the tab is visible
// the page follows net.wifi.state / net.ap.state, so a join started here
// (or anywhere else) shows up as it happens.

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError, errorDetail } from '../api.js';
import { t } from '../i18n/index.js';
import { subscribe } from '../ws.js';
import { Card } from '../components/card.js';
import { Badge } from '../components/badge.js';
import { confirmDialog } from '../components/dialog.js';
import { toast } from '../components/toast.js';
import {
  JoinForm,
  ScanList,
  SignalBars,
  Ssid,
  displaySsid,
  joinFailureText,
  joinNetwork,
  prepareScan,
  reasonText,
} from '../components/wifi.js';
import { IconAlert, IconCheck, IconPlus, IconRefresh, IconSearch, IconTrash, IconWifi } from '../icons.js';
import { useVisible } from '../util.js';

// WifiService keeps at most this many networks (spec 4.2).
const MAX_SAVED = 5;
const MANUAL = '\u0000manual';

/**
 * @typedef {Object} WifiStatus  GET /wifi
 * @property {'off'|'connecting'|'backoff'|'connected'} state
 * @property {string} ssid
 * @property {string} ip
 * @property {number} rssi
 * @property {''|'not_found'|'auth_failed'|'other'} reason
 * @property {string[]} saved
 * @property {{active: boolean, ssid: string, clients: number}} ap
 */

export function NetworkPage() {
  const visible = useVisible();
  const [wifi, setWifi] = useState(/** @type {WifiStatus|null} */ (null));
  const [error, setError] = useState(null);
  const [loading, setLoading] = useState(false);
  const [scan, setScan] = useState(null); // {list, hidden}
  const [scanning, setScanning] = useState(false);
  const [scanError, setScanError] = useState(null);
  const [selected, setSelected] = useState('');
  // {ssid, phase: 'busy'|'ok'|'fail', text?, ip?}
  const [join, setJoin] = useState(null);
  const [removing, setRemoving] = useState('');
  const alive = useRef(true);
  const stopJoin = useRef(null);

  async function load() {
    setLoading(true);
    try {
      const w = await api('GET', '/wifi');
      if (!alive.current) return;
      setWifi(w);
      setError(null);
    } catch (e) {
      if (alive.current && e.status !== 401) setError(e);
    } finally {
      if (alive.current) setLoading(false);
    }
  }

  function onEvent(msg) {
    if (msg.t === 'dropped') {
      load();
      return;
    }
    const p = msg.payload || {};
    if (msg.topic === 'net.wifi.state') setWifi((w) => (w ? { ...w, ...p } : w));
    else if (msg.topic === 'net.ap.state') setWifi((w) => (w ? { ...w, ap: { ...w.ap, ...p } } : w));
  }

  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
      stopJoin.current?.();
    };
  }, []);

  // Only while visible: a hidden tab gives its WebSocket slot back.
  useEffect(() => {
    if (!visible) return undefined;
    load();
    return subscribe(['events'], onEvent, {
      // Events sent while the socket was down are lost; catch up.
      onState: (st) => st === 'open' && load(),
    });
  }, [visible]);

  async function runScan() {
    if (scanning) return;
    setScanning(true);
    setScanError(null);
    try {
      const r = await api('GET', '/wifi/scan');
      if (!alive.current) return;
      setScan(prepareScan(r?.networks));
    } catch (e) {
      if (alive.current && e.status !== 401) setScanError(e);
    } finally {
      if (alive.current) setScanning(false);
    }
  }

  function pick(ssid) {
    if (join?.phase === 'busy') return;
    setSelected(selected === ssid ? '' : ssid);
    setJoin(null);
  }

  function startJoin(ssid, password) {
    stopJoin.current?.();
    setJoin({ ssid, phase: 'busy' });
    stopJoin.current = joinNetwork(ssid, password, (r) => {
      stopJoin.current = null;
      if (!alive.current) return;
      if (r.ok) {
        setJoin({ ssid, phase: 'ok', ip: r.ip });
        setSelected('');
        toast(t('net.join_ok_toast', { ssid: displaySsid(ssid) }), 'ok');
        setScan((s) => (s ? { ...s, list: s.list.map((n) => (n.ssid === ssid ? { ...n, saved: true } : n)) } : s));
      } else {
        setJoin({ ssid, phase: 'fail', text: joinFailureText(r) });
      }
      load();
    });
  }

  async function forget(ssid) {
    const inUse = wifi && wifi.ssid === ssid && (wifi.state === 'connected' || wifi.state === 'connecting');
    const ok = await confirmDialog({
      title: t('net.forget_title', { ssid: displaySsid(ssid) }),
      body: inUse ? t('net.forget_body_current') : t('net.forget_body'),
      okText: t('net.forget_confirm'),
      danger: true,
    });
    if (!ok) return;
    setRemoving(ssid);
    try {
      await api('DELETE', '/wifi/networks/' + encodeURIComponent(ssid));
      if (!alive.current) return;
      toast(t('net.forgot_toast', { ssid: displaySsid(ssid) }), 'ok');
      setScan((s) => (s ? { ...s, list: s.list.map((n) => (n.ssid === ssid ? { ...n, saved: false } : n)) } : s));
    } catch (e) {
      if (e.status !== 401) toast(t('net.forget_failed', { reason: describeError(e) }), 'err');
    } finally {
      if (alive.current) setRemoving('');
      load();
    }
  }

  const busy = join?.phase === 'busy';
  const joinError = join?.phase === 'fail' ? join.text : '';
  const form = (network) =>
    html`<div class="scan-join">
      ${busy
        ? html`<p class="join-progress" role="status">
            <span class="spinner" aria-hidden="true"></span>
            ${t('net.joining', { ssid: displaySsid(join.ssid) })}
          </p>`
        : null}
      <${JoinForm}
        key=${network ? network.ssid : MANUAL}
        network=${network}
        busy=${busy}
        error=${joinError}
        idPrefix="net-join"
        onSubmit=${startJoin}
        onCancel=${() => {
          stopJoin.current?.();
          stopJoin.current = null;
          setSelected('');
          setJoin(null);
        }}
      />
    </div>`;

  return html`<div class="page">
    <header class="page-head">
      <div>
        <h1 class="page-title" tabindex="-1">${t('nav.network')}</h1>
        <p class="page-sub">${t('net.sub')}</p>
      </div>
      <div class="page-head-actions">
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

    ${error && !wifi
      ? html`<${Card} title=${t('net.load_failed')} icon=${IconAlert} tone="err">
          <p class="muted">${describeError(error)}</p>
          ${errorDetail(error) ? html`<p class="err-detail mono">${errorDetail(error)}</p>` : null}
          <button type="button" class="btn btn-secondary" onClick=${load}>${t('common.retry')}</button>
        </${Card}>`
      : null}

    <div class="grid grid-2">
      <${CurrentCard} wifi=${wifi} join=${join} />
      <${SavedCard} wifi=${wifi} removing=${removing} onForget=${forget} />
    </div>

    <${Card}
      title=${t('net.add_title')}
      icon=${IconSearch}
      actions=${html`<button type="button" class="btn btn-secondary btn-sm" onClick=${runScan} disabled=${scanning || busy}>
        ${scanning ? html`<span class="spinner" aria-hidden="true"></span>` : html`<${IconRefresh} size=${16} />`}
        ${scan ? t('net.rescan') : t('net.scan')}
      </button>`}
    >
      ${scanError
        ? html`<div class="alert tone-err" role="alert">
            <${IconAlert} size=${18} /><span>${t('net.scan_failed', { reason: describeError(scanError) })}</span>
          </div>`
        : null}
      ${scanning && !scan
        ? html`<p class="scan-wait muted" role="status"><span class="spinner" aria-hidden="true"></span>${t('net.scanning')}</p>`
        : null}
      ${!scan && !scanning && !scanError ? html`<p class="muted">${t('net.scan_hint')}</p>` : null}
      ${scan && scan.list.length === 0 && !scanning
        ? html`<p class="muted">${t('net.scan_empty')}</p>`
        : null}
      ${scan && scan.list.length
        ? html`<${ScanList}
            networks=${scan.list}
            hidden=${scan.hidden}
            selected=${selected}
            current=${wifi?.state === 'connected' ? wifi.ssid : ''}
            onPick=${(n) => pick(n.ssid)}
            disabled=${busy}
          >
            ${form(scan.list.find((n) => n.ssid === selected))}
          </${ScanList}>`
        : null}
      <div class="scan-manual">
        ${selected === MANUAL
          ? html`<div class="scan-item is-selected">
              <p class="scan-manual-title">${t('net.manual_title')}</p>
              ${form(undefined)}
            </div>`
          : html`<button type="button" class="btn btn-link" onClick=${() => pick(MANUAL)} disabled=${busy}>
              <${IconPlus} size=${16} />${t('net.manual')}
            </button>`}
      </div>
    </${Card}>
  </div>`;
}

function CurrentCard({ wifi, join }) {
  if (!wifi) {
    return html`<section class="card skeleton-card" aria-busy="true" aria-label=${t('common.loading')}>
      <div class="skel skel-title"></div>
      <div class="skel skel-big"></div>
      <div class="skel skel-line"></div>
    </section>`;
  }
  let badge;
  let body;
  if (wifi.state === 'connected') {
    badge = html`<${Badge} tone="ok" dot>${t('wifi.connected')}</${Badge}>`;
    body = html`<div class="stat-text"><${Ssid} ssid=${wifi.ssid} /></div>
      <dl class="kv">
        <div><dt>${t('wifi.ip')}</dt><dd class="mono">${wifi.ip || '—'}</dd></div>
        <div>
          <dt>${t('wifi.signal')}</dt>
          <dd><${SignalBars} rssi=${wifi.rssi} /> <span class="mono dim">${wifi.rssi} dBm</span></dd>
        </div>
      </dl>`;
  } else if (wifi.state === 'connecting') {
    badge = html`<${Badge} tone="warn" dot pulse>${t('wifi.connecting')}</${Badge}>`;
    body = html`<div class="stat-text">${wifi.ssid ? html`<${Ssid} ssid=${wifi.ssid} />` : t('wifi.connecting')}</div>
      <p class="muted">${t('net.connecting_hint')}</p>`;
  } else if (wifi.state === 'backoff') {
    badge = html`<${Badge} tone="warn" dot>${t('wifi.backoff')}</${Badge}>`;
    body = html`<div class="stat-text">${wifi.ssid ? html`<${Ssid} ssid=${wifi.ssid} />` : t('wifi.off')}</div>
      <p class="muted">${t('wifi.retry_hint', { reason: reasonText(wifi.reason) })}</p>`;
  } else {
    badge = html`<${Badge} tone="muted" dot>${t('wifi.off')}</${Badge}>`;
    body = html`<div class="stat-text dim">${t('wifi.off')}</div>
      <p class="muted">${wifi.saved?.length ? t('net.off_hint_saved') : t('net.off_hint_none')}</p>`;
  }
  const ok = join?.phase === 'ok' && wifi.state === 'connected' && wifi.ssid === join.ssid;
  return html`<${Card}
    title=${t('net.current_title')}
    icon=${IconWifi}
    tone=${wifi.state === 'connected' ? 'ok' : 'accent'}
    actions=${badge}
  >
    ${body}
    ${ok
      ? html`<div class="inline-note tone-ok" role="status">
          <${IconCheck} size=${15} /><span>${t('net.join_ok', { ssid: displaySsid(join.ssid) })}</span>
        </div>`
      : null}
    ${wifi.ap?.active
      ? html`<div class="inline-note">
          <${IconWifi} size=${15} />
          <span>${t('wifi.ap_on', { ssid: wifi.ap.ssid, clients: wifi.ap.clients })}</span>
        </div>`
      : null}
  </${Card}>`;
}

function SavedCard({ wifi, removing, onForget }) {
  const saved = wifi?.saved || [];
  return html`<${Card}
    title=${t('net.saved_title')}
    icon=${IconWifi}
    actions=${wifi ? html`<span class="count-pill">${t('net.saved_count', { count: saved.length, max: MAX_SAVED })}</span>` : null}
  >
    ${!wifi
      ? html`<div class="skel skel-line"></div>`
      : saved.length === 0
        ? html`<p class="stat-empty">${t('wifi.none_saved')}</p>`
        : html`<ul class="rows">
            ${saved.map((ssid) => {
              const here = wifi.ssid === ssid && wifi.state !== 'off';
              const status = here
                ? wifi.state === 'connected'
                  ? html`<${Badge} tone="ok" dot>${t('wifi.connected')}</${Badge}>`
                  : wifi.state === 'connecting'
                    ? html`<${Badge} tone="warn" dot pulse>${t('wifi.connecting')}</${Badge}>`
                    : html`<${Badge} tone="warn" dot>${t('wifi.backoff')}</${Badge}>`
                : null;
              return html`<li key=${'s:' + ssid} class="row">
                <div class="row-main">
                  <${Ssid} ssid=${ssid} class="row-title" />
                </div>
                <div class="row-actions">
                  ${status}
                  <button
                    type="button"
                    class="btn btn-icon btn-ghost btn-sm"
                    onClick=${() => onForget(ssid)}
                    disabled=${removing === ssid}
                    aria-label=${t('net.forget_label', { ssid: displaySsid(ssid) })}
                    title=${t('net.forget_confirm')}
                  >
                    ${removing === ssid ? html`<span class="spinner" aria-hidden="true"></span>` : html`<${IconTrash} size=${17} />`}
                  </button>
                </div>
              </li>`;
            })}
          </ul>`}
    ${wifi && saved.length >= MAX_SAVED ? html`<p class="muted saved-full">${t('net.saved_full', { max: MAX_SAVED })}</p>` : null}
  </${Card}>`;
}
