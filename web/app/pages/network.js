// Network (spec 7.3): the current connection, the saved networks (up to
// five, each removable) and a scan to add one. While the tab is visible
// the page follows net.wifi.state / net.ap.state, so a join started here
// (or anywhere else) shows up as it happens. Picking a saved network in the
// scan list connects with the password the device keeps; "改密码" opens the
// password form instead, and so does a parked or rejected stored password
// (spec 7.3).
//
// Networks are identified by ssidKey() (their bytes in hex), never by the
// displayed name, which is lossy for SSIDs that are not UTF-8.

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError, errorDetail } from '../api.js';
import { t } from '../i18n/index.js';
import { subscribe } from '../ws.js';
import { Card } from '../components/card.js';
import { Badge } from '../components/badge.js';
import { confirmDialog } from '../components/dialog.js';
import { toast } from '../components/toast.js';
import {
  allRejected,
  JoinForm,
  ScanList,
  SignalBars,
  Ssid,
  displaySsid,
  joinFailureText,
  joinNetwork,
  networkPath,
  prepareScan,
  reasonText,
  rejectedKeys,
  ssidKey,
} from '../components/wifi.js';
import { IconAlert, IconCheck, IconKey, IconPlus, IconRefresh, IconSearch, IconTrash, IconWifi } from '../icons.js';
import { useVisible } from '../util.js';

// WifiService keeps at most this many networks (spec 4.2).
const MAX_SAVED = 5;

/**
 * @typedef {Object} SavedEntry  one of GET /wifi "saved"
 * @property {string} ssid
 * @property {string} ssid_hex
 * @property {boolean} auth_failed  rejected password; not retried until
 *   it is re-entered (spec 4.2)
 */

/**
 * @typedef {Object} WifiStatus  GET /wifi
 * @property {'off'|'connecting'|'backoff'|'connected'} state
 * @property {string} ssid
 * @property {string} ssid_hex
 * @property {string} ip
 * @property {number} rssi
 * @property {''|'not_found'|'auth_failed'|'other'} reason
 * @property {SavedEntry[]} saved
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
  // Where the join form is open: {where: 'scan'|'saved', key} or
  // {where: 'manual'} for a hidden network; null when closed. `stored`:
  // a saved scan entry, joined with its stored password, whose password
  // form stays closed until "改密码".
  const [selected, setSelected] = useState(null);
  // {ssid, key, phase: 'busy'|'ok'|'fail', text?, ip?}
  const [join, setJoin] = useState(null);
  const [removing, setRemoving] = useState(''); // ssidKey being forgotten
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
    if (msg.topic === 'net.wifi.state') {
      setWifi((w) => (w ? { ...w, ...p } : w));
      // A rejected password may have parked a network: the saved list's
      // flags are only in GET /wifi.
      if (p.state === 'backoff' && p.reason === 'auth_failed') load();
    }
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

  function pick(where, key = '') {
    if (join?.phase === 'busy') return;
    const same = selected && selected.where === where && selected.key === key;
    setSelected(same ? null : { where, key });
    setJoin(null);
  }

  // A scan entry: a saved one (not parked) connects with its stored
  // password straight away, unless the device is already on it.
  function pickScan(n) {
    if (join?.phase === 'busy') return;
    const same = selected?.where === 'scan' && selected.key === n.ssid_hex;
    if (same || !n.saved || rejectedKeys(wifi).has(n.ssid_hex)) {
      pick('scan', n.ssid_hex);
      return;
    }
    setSelected({ where: 'scan', key: n.ssid_hex, stored: true });
    if (n.ssid_hex === currentKey) setJoin(null);
    else startJoin(n, '', true);
  }

  // "改密码": the password form for the selected saved network.
  function changePassword() {
    stopJoin.current?.();
    stopJoin.current = null;
    setSelected((sel) => (sel ? { where: sel.where, key: sel.key } : sel));
    setJoin(null);
  }

  // `stored`: join with the device's stored password.
  function startJoin(target, password, stored = false) {
    stopJoin.current?.();
    const key = ssidKey(target);
    const ssid = target.ssid;
    setJoin({ ssid, key, phase: 'busy' });
    stopJoin.current = joinNetwork(target, password, (r) => {
      stopJoin.current = null;
      if (!alive.current) return;
      if (stored && !r.ok && (r.reason === 'auth_failed' || r.error?.code === 'not_found')) {
        // The stored password no longer works, or the network is no
        // longer saved: open the password form.
        setSelected((sel) => (sel && sel.key === key ? { where: sel.where, key } : sel));
        setJoin(r.reason === 'auth_failed' ? { ssid, key, phase: 'fail', text: t('join.saved_rejected') } : null);
        load();
        return;
      }
      if (r.ok) {
        setJoin({ ssid, key, phase: 'ok', ip: r.ip });
        setSelected(null);
        toast(t('net.join_ok_toast', { ssid: displaySsid(ssid) }), 'ok');
        setScan((s) => (s ? { ...s, list: s.list.map((n) => (n.ssid_hex === key ? { ...n, saved: true } : n)) } : s));
      } else {
        setJoin({ ssid, key, phase: 'fail', text: joinFailureText(r) });
      }
      load();
    }, { saved: stored });
  }

  /** @param {SavedEntry} entry */
  async function forget(entry) {
    const { ssid } = entry;
    const key = ssidKey(entry);
    const inUse = wifi && ssidKey(wifi) === key && (wifi.state === 'connected' || wifi.state === 'connecting');
    const ok = await confirmDialog({
      title: t('net.forget_title', { ssid: displaySsid(ssid) }),
      body: inUse ? t('net.forget_body_current') : t('net.forget_body'),
      okText: t('net.forget_confirm'),
      danger: true,
    });
    if (!ok) return;
    setRemoving(key);
    try {
      await api('DELETE', '/wifi/networks/' + networkPath(entry));
      if (!alive.current) return;
      toast(t('net.forgot_toast', { ssid: displaySsid(ssid) }), 'ok');
      setScan((s) => (s ? { ...s, list: s.list.map((n) => (n.ssid_hex === key ? { ...n, saved: false } : n)) } : s));
    } catch (e) {
      if (e.status !== 401) toast(t('net.forget_failed', { reason: describeError(e) }), 'err');
    } finally {
      if (alive.current) setRemoving('');
      load();
    }
  }

  const busy = join?.phase === 'busy';
  const joinError = join?.phase === 'fail' ? join.text : '';
  const currentKey = wifi?.state === 'connected' ? ssidKey(wifi) : '';
  // Joining another network takes the device off the one this page
  // reaches it through.
  const form = (network) =>
    selected?.stored && network
      ? storedJoin(network)
      : html`<div class="scan-join">
      ${currentKey && (!network || network.ssid_hex !== currentKey)
        ? html`<div class="inline-note tone-warn">
            <${IconAlert} size=${15} /><span>${t('net.switch_note')}</span>
          </div>`
        : null}
      ${busy
        ? html`<p class="join-progress" role="status">
            <span class="spinner" aria-hidden="true"></span>
            ${t('net.joining', { ssid: displaySsid(join.ssid) })}
          </p>`
        : null}
      <${JoinForm}
        key=${network ? network.ssid_hex : 'manual'}
        network=${network}
        busy=${busy}
        error=${joinError}
        idPrefix="net-join"
        onSubmit=${startJoin}
        onCancel=${() => {
          stopJoin.current?.();
          stopJoin.current = null;
          setSelected(null);
          setJoin(null);
        }}
      />
    </div>`;

  // A saved network joined with its stored password: progress or the
  // outcome, and "改密码" for the password form.
  const storedJoin = (network) => {
    const here = network.ssid_hex === currentKey && !join;
    return html`<div class="scan-join">
      ${here
        ? html`<p class="muted" role="status">${t('join.saved_current')}</p>`
        : currentKey
          ? html`<div class="inline-note tone-warn">
              <${IconAlert} size=${15} /><span>${t('net.switch_note')}</span>
            </div>`
          : null}
      ${busy
        ? html`<p class="join-progress" role="status">
            <span class="spinner" aria-hidden="true"></span>
            ${t('net.joining', { ssid: displaySsid(join.ssid) })}
          </p>`
        : null}
      ${!here && !busy && !joinError ? html`<p class="muted">${t('join.saved_note')}</p>` : null}
      ${joinError
        ? html`<div class="alert tone-err" role="alert">
            <${IconAlert} size=${18} /><span>${joinError}</span>
          </div>`
        : null}
      <div class="form-actions">
        ${network.secured
          ? html`<button type="button" class="btn btn-link" onClick=${changePassword}>${t('join.change_password')}</button>`
          : null}
        ${joinError
          ? html`<button type="button" class="btn btn-primary" onClick=${() => startJoin(network, '', true)}>${t('common.retry')}</button>`
          : null}
      </div>
    </div>`;
  };

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
      <${SavedCard}
        wifi=${wifi}
        removing=${removing}
        onForget=${forget}
        retyping=${selected?.where === 'saved' ? selected.key : ''}
        onRetype=${(entry) => pick('saved', ssidKey(entry))}
        form=${(entry) => form({ ssid: entry.ssid, ssid_hex: ssidKey(entry), secured: true, saved: true })}
        busy=${busy}
      />
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
            selected=${selected?.where === 'scan' ? selected.key : ''}
            current=${currentKey}
            onPick=${pickScan}
            disabled=${busy}
          >
            ${selected?.where === 'scan' ? form(scan.list.find((n) => n.ssid_hex === selected.key)) : null}
          </${ScanList}>`
        : null}
      <div class="scan-manual">
        ${selected?.where === 'manual'
          ? html`<div class="scan-item is-selected">
              <p class="scan-manual-title">${t('net.manual_title')}</p>
              ${form(undefined)}
            </div>`
          : html`<button type="button" class="btn btn-link" onClick=${() => pick('manual')} disabled=${busy}>
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
  } else if (wifi.state === 'backoff' && allRejected(wifi)) {
    badge = html`<${Badge} tone="err" dot>${t('wifi.rejected')}</${Badge}>`;
    body = html`<div class="stat-text">${wifi.ssid ? html`<${Ssid} ssid=${wifi.ssid} />` : t('wifi.off')}</div>
      <p class="muted">${t('net.rejected_hint')}</p>`;
  } else if (wifi.state === 'backoff') {
    badge = html`<${Badge} tone="warn" dot>${t('wifi.backoff')}</${Badge}>`;
    body = html`<div class="stat-text">${wifi.ssid ? html`<${Ssid} ssid=${wifi.ssid} />` : t('wifi.off')}</div>
      <p class="muted">${t('wifi.retry_hint', { reason: reasonText(wifi.reason) })}</p>`;
  } else {
    badge = html`<${Badge} tone="muted" dot>${t('wifi.off')}</${Badge}>`;
    body = html`<div class="stat-text dim">${t('wifi.off')}</div>
      <p class="muted">${wifi.saved?.length ? t('net.off_hint_saved') : t('net.off_hint_none')}</p>`;
  }
  const ok = join?.phase === 'ok' && wifi.state === 'connected' && ssidKey(wifi) === join.key;
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

function SavedCard({ wifi, removing, onForget, retyping, onRetype, form, busy }) {
  const saved = wifi?.saved || [];
  const hereKey = wifi && wifi.state !== 'off' ? ssidKey(wifi) : '';
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
            ${saved.map((entry) => {
              const key = ssidKey(entry);
              const here = key === hereKey;
              const open = retyping === key;
              let status = null;
              if (here && wifi.state === 'connected') {
                status = html`<${Badge} tone="ok" dot>${t('wifi.connected')}</${Badge}>`;
              } else if (entry.auth_failed) {
                status = html`<${Badge} tone="err">${t('net.wrong_password')}</${Badge}>`;
              } else if (here) {
                status =
                  wifi.state === 'connecting'
                    ? html`<${Badge} tone="warn" dot pulse>${t('wifi.connecting')}</${Badge}>`
                    : html`<${Badge} tone="warn" dot>${t('wifi.backoff')}</${Badge}>`;
              }
              return html`<li key=${'s:' + key} class=${open ? 'row row-saved is-open' : 'row row-saved'}>
                <div class="row-line">
                  <div class="row-main">
                    <${Ssid} ssid=${entry.ssid} class="row-title" />
                    ${entry.auth_failed ? html`<span class="row-sub">${t('net.wrong_password_hint')}</span>` : null}
                  </div>
                  <div class="row-actions">
                    ${status}
                    ${entry.auth_failed
                      ? html`<button
                          type="button"
                          class="btn btn-secondary btn-sm"
                          onClick=${() => onRetype(entry)}
                          disabled=${busy}
                          aria-expanded=${open ? 'true' : 'false'}
                          aria-label=${t('net.retype_label', { ssid: displaySsid(entry.ssid) })}
                        >
                          <${IconKey} size=${15} />${t('net.retype')}
                        </button>`
                      : null}
                    <button
                      type="button"
                      class="btn btn-icon btn-ghost btn-sm"
                      onClick=${() => onForget(entry)}
                      disabled=${removing === key}
                      aria-label=${t('net.forget_label', { ssid: displaySsid(entry.ssid) })}
                      title=${t('net.forget_confirm')}
                    >
                      ${removing === key ? html`<span class="spinner" aria-hidden="true"></span>` : html`<${IconTrash} size=${17} />`}
                    </button>
                  </div>
                </div>
                ${open ? form(entry) : null}
              </li>`;
            })}
          </ul>`}
    ${wifi && saved.length >= MAX_SAVED ? html`<p class="muted saved-full">${t('net.saved_full', { max: MAX_SAVED })}</p>` : null}
  </${Card}>`;
}
