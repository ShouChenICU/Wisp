// Wi-Fi pieces shared by the network page and the setup wizard: signal
// bars, the scan list, the join form and the watcher that follows a join
// attempt to its result.
//
// SSIDs come from the air, so they are untrusted text: they are only ever
// rendered as text nodes (never as HTML), wrapped in <bdi> so their
// direction cannot reorder the text around them, with control and bidi
// override characters shown as U+FFFD. The raw string is what goes back
// to the device.

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError } from '../api.js';
import { t } from '../i18n/index.js';
import { subscribe } from '../ws.js';
import { IconAlert, IconChevronRight, IconEye, IconEyeOff, IconLock } from '../icons.js';
import { Badge } from './badge.js';

/** Join attempts give up after this long (spec 7.3 wizard). */
export const JOIN_TIMEOUT_MS = 60000;
// The event stream is the fast path; this poll covers a socket that is
// down or full (the device allows two).
const JOIN_POLL_MS = 3000;

// The device's limits (WifiService::validate): SSID 1-32 bytes, password
// empty (open network) or 8-63 bytes, counted in UTF-8.
export const SSID_MAX_BYTES = 32;
export const PASSWORD_MIN_BYTES = 8;
export const PASSWORD_MAX_BYTES = 63;

const encoder = new TextEncoder();

/** UTF-8 length of `s`. */
export function byteLength(s) {
  return encoder.encode(s).length;
}

/**
 * `ssid` made safe to show: control characters and bidi embedding /
 * override / isolate controls become U+FFFD, so a name cannot hide
 * characters or flip into a misleading order.
 */
export function displaySsid(ssid) {
  // eslint-disable-next-line no-control-regex
  return String(ssid).replace(/[\u0000-\u001f\u007f\u202a-\u202e\u2066-\u2069]/g, '\uFFFD');
}

/** An SSID as an isolated text run. */
export function Ssid({ ssid, class: cls }) {
  return html`<bdi class=${cls ? 'ssid ' + cls : 'ssid'}>${displaySsid(ssid)}</bdi>`;
}

export function signalLevel(rssi) {
  if (!Number.isFinite(rssi) || rssi === 0) return 0;
  if (rssi >= -55) return 4;
  if (rssi >= -67) return 3;
  if (rssi >= -75) return 2;
  return 1;
}

export function SignalBars({ rssi }) {
  const level = signalLevel(rssi);
  return html`<span class="bars" role="img" aria-label=${t('wifi.signal_bars', { level })}>
    ${[1, 2, 3, 4].map((i) => html`<i key=${i} class=${i <= level ? 'on' : ''}></i>`)}
  </span>`;
}

/** The localized cause of a failed join ("not_found", "auth_failed", ...). */
export function reasonText(reason) {
  return t('wifi.reason_' + (['not_found', 'auth_failed'].includes(reason) ? reason : 'other'));
}

/**
 * @typedef {Object} ScanEntry  one network from GET /wifi/scan
 * @property {string} ssid
 * @property {number} rssi
 * @property {boolean} secured
 * @property {boolean} saved
 */

/**
 * Cleans a scan result for display: entries without an SSID (hidden
 * networks, which can only be joined by name) are counted, not listed;
 * repeats of one SSID (several access points) keep the strongest; the
 * rest are sorted strongest first.
 *
 * @param {ScanEntry[]} networks
 * @returns {{list: ScanEntry[], hidden: number}}
 */
export function prepareScan(networks) {
  const best = new Map();
  let hidden = 0;
  for (const n of Array.isArray(networks) ? networks : []) {
    if (!n || typeof n.ssid !== 'string') continue;
    if (n.ssid === '') {
      hidden++;
      continue;
    }
    const rssi = Number(n.rssi);
    const entry = { ssid: n.ssid, rssi: Number.isFinite(rssi) ? rssi : -100, secured: !!n.secured, saved: !!n.saved };
    const seen = best.get(n.ssid);
    if (!seen) best.set(n.ssid, entry);
    else {
      seen.saved = seen.saved || entry.saved;
      seen.secured = seen.secured || entry.secured;
      if (entry.rssi > seen.rssi) seen.rssi = entry.rssi;
    }
  }
  const list = [...best.values()].sort((a, b) => b.rssi - a.rssi);
  return { list, hidden };
}

/**
 * The networks from a scan, each a button. `selected` (an SSID) marks the
 * one being joined; `current` the one the device is on.
 *
 * @param {{networks: ScanEntry[], hidden?: number, selected?: string,
 *          current?: string, onPick: (n: ScanEntry) => void,
 *          disabled?: boolean, children?: any}} props
 *   `children` renders right below the selected row (the join form).
 */
export function ScanList({ networks, hidden = 0, selected, current, onPick, disabled, children }) {
  return html`<ul class="scan-list" aria-label=${t('scan.list_label')}>
    ${networks.map((n) => {
      const isSel = selected === n.ssid;
      const isCur = current === n.ssid;
      return html`<li key=${'n:' + n.ssid} class=${isSel ? 'scan-item is-selected' : 'scan-item'}>
        <button
          type="button"
          class="scan-row"
          onClick=${() => onPick(n)}
          disabled=${disabled}
          aria-expanded=${isSel ? 'true' : 'false'}
        >
          <${SignalBars} rssi=${n.rssi} />
          <span class="scan-main">
            <${Ssid} ssid=${n.ssid} class="scan-ssid" />
            <span class="scan-sub">
              ${n.secured ? t('scan.secured') : t('scan.open')}
            </span>
          </span>
          ${isCur
            ? html`<${Badge} tone="ok">${t('scan.current')}</${Badge}>`
            : n.saved
              ? html`<${Badge} tone="accent">${t('scan.saved')}</${Badge}>`
              : null}
          ${n.secured ? html`<span class="scan-lock" title=${t('scan.secured')}><${IconLock} size=${15} /></span>` : null}
          <span class="scan-chevron"><${IconChevronRight} size=${16} /></span>
        </button>
        ${isSel ? children : null}
      </li>`;
    })}
    ${hidden
      ? html`<li key="hidden" class="scan-hidden muted">${t('scan.hidden', { count: hidden })}</li>`
      : null}
  </ul>`;
}

/**
 * Asks for what a join needs and validates it before anything is sent.
 * With `network` it asks only for the password (none for an open
 * network); without one it also asks for the SSID (a hidden network).
 *
 * @param {{network?: ScanEntry, busy?: boolean, error?: string,
 *          submitText?: string, onSubmit: (ssid: string, password: string) => void,
 *          onCancel?: () => void, idPrefix?: string}} props
 */
export function JoinForm({ network, busy, error, submitText, onSubmit, onCancel, idPrefix = 'join' }) {
  const manual = !network;
  const [ssid, setSsid] = useState('');
  const [password, setPassword] = useState('');
  const [reveal, setReveal] = useState(false);
  const [problem, setProblem] = useState('');
  const first = useRef(null);
  const needsPassword = manual || network.secured;

  useEffect(() => {
    first.current?.focus();
  }, [network?.ssid, manual]);

  function submit(e) {
    e.preventDefault();
    if (busy) return;
    const name = manual ? ssid : network.ssid;
    if (manual) {
      if (!name) return setProblem(t('join.err_ssid_empty'));
      if (byteLength(name) > SSID_MAX_BYTES) return setProblem(t('join.err_ssid_long', { max: SSID_MAX_BYTES }));
    }
    const pwBytes = byteLength(password);
    if (needsPassword && !(manual && pwBytes === 0)) {
      if (pwBytes < PASSWORD_MIN_BYTES || pwBytes > PASSWORD_MAX_BYTES) {
        return setProblem(t('join.err_password', { min: PASSWORD_MIN_BYTES, max: PASSWORD_MAX_BYTES }));
      }
    }
    setProblem('');
    onSubmit(name, needsPassword ? password : '');
  }

  const shown = problem || error;
  const errId = idPrefix + '-err';
  return html`<form class="join-form" onSubmit=${submit} novalidate>
    ${manual
      ? html`<div class="field">
          <label class="field-label" for=${idPrefix + '-ssid'}>${t('join.ssid')}</label>
          <input
            id=${idPrefix + '-ssid'}
            ref=${first}
            class="input"
            type="text"
            autocomplete="off"
            autocapitalize="off"
            spellcheck=${false}
            value=${ssid}
            onInput=${(e) => {
              setSsid(e.currentTarget.value);
              setProblem('');
            }}
            disabled=${busy}
            aria-describedby=${shown ? errId : undefined}
          />
        </div>`
      : null}
    ${needsPassword
      ? html`<div class="field">
          <label class="field-label" for=${idPrefix + '-pw'}>
            ${manual ? t('join.password_optional') : t('join.password_for', { ssid: displaySsid(network.ssid) })}
          </label>
          <div class="input-group">
            <input
              id=${idPrefix + '-pw'}
              ref=${manual ? undefined : first}
              class="input"
              type=${reveal ? 'text' : 'password'}
              autocomplete="off"
              autocapitalize="off"
              spellcheck=${false}
              value=${password}
              onInput=${(e) => {
                setPassword(e.currentTarget.value);
                setProblem('');
              }}
              disabled=${busy}
              aria-describedby=${shown ? errId : undefined}
              aria-invalid=${shown ? 'true' : 'false'}
            />
            <button
              type="button"
              class="btn btn-ghost btn-icon input-addon"
              onClick=${() => setReveal(!reveal)}
              aria-label=${reveal ? t('join.hide_password') : t('join.show_password')}
              aria-pressed=${reveal ? 'true' : 'false'}
              title=${reveal ? t('join.hide_password') : t('join.show_password')}
            >
              ${reveal ? html`<${IconEyeOff} size=${18} />` : html`<${IconEye} size=${18} />`}
            </button>
          </div>
        </div>`
      : html`<p class="muted">${t('join.open_note')}</p>`}
    ${shown
      ? html`<div class="alert tone-err" role="alert" id=${errId}>
          <${IconAlert} size=${18} /><span>${shown}</span>
        </div>`
      : null}
    <div class="form-actions">
      ${onCancel
        ? html`<button type="button" class="btn btn-ghost" onClick=${onCancel} disabled=${busy}>${t('common.cancel')}</button>`
        : null}
      <button type="submit" class="btn btn-primary" disabled=${busy}>
        ${busy ? html`<span class="spinner" aria-hidden="true"></span>` : null}${submitText || t('join.connect')}
      </button>
    </div>
  </form>`;
}

/**
 * Sends a join (POST /wifi/networks) and follows it to an outcome.
 *
 * The device answers 202 and connects on its own task, announcing each
 * step as net.wifi.state. Events for the same SSID published before the
 * request may still be in flight on the socket, so an event only counts
 * after this join's own "connecting" event has been seen; GET /wifi
 * (polled, and fetched whenever the socket reopens) is always current
 * and counts at once.
 *
 * @param {string} ssid
 * @param {string} password  "" for an open network
 * @param {(result: {ok: true, ip: string, ssid: string}
 *                 | {ok: false, reason: string}
 *                 | {ok: false, timeout: true}
 *                 | {ok: false, error: any}) => void} onResult
 *   called once; `error` is an ApiError from the request itself
 * @returns {() => void} stops following (no result is reported after)
 */
export function joinNetwork(ssid, password, onResult) {
  let done = false;
  let posted = false;
  let armed = false;
  let pollTimer = 0;
  let unsubscribe = () => {};

  const finish = (result) => {
    if (done) return;
    done = true;
    stop();
    onResult(result);
  };

  function judge(st, fromEvent) {
    if (!st || st.ssid !== ssid) return;
    if (st.state === 'connecting') {
      if (fromEvent) armed = true;
      return;
    }
    if (fromEvent && !armed) return;
    if (st.state === 'connected') finish({ ok: true, ip: st.ip || '', ssid });
    else if (st.state === 'backoff') finish({ ok: false, reason: st.reason || 'other' });
  }

  async function poll() {
    if (done || !posted) return;
    try {
      judge(await api('GET', '/wifi'), false);
    } catch {
      // A failed poll is not a failed join; the next one may get through.
    }
  }

  // Subscribe before sending, so this join's own events are not missed.
  unsubscribe = subscribe(
    ['events'],
    (msg) => {
      if (msg.t === 'dropped') poll();
      else if (msg.topic === 'net.wifi.state') judge(msg.payload || {}, true);
    },
    { onState: (st) => st === 'open' && poll() },
  );
  const timeout = setTimeout(() => finish({ ok: false, timeout: true }), JOIN_TIMEOUT_MS);

  function stop() {
    clearTimeout(timeout);
    clearInterval(pollTimer);
    unsubscribe();
  }

  const body = password ? { ssid, password } : { ssid };
  api('POST', '/wifi/networks', body)
    .then(() => {
      if (done) return;
      posted = true;
      pollTimer = setInterval(poll, JOIN_POLL_MS);
      poll();
    })
    .catch((e) => finish({ ok: false, error: e }));

  return () => {
    done = true;
    stop();
  };
}

/** A sentence for a failed join result from joinNetwork(). */
export function joinFailureText(result) {
  if (result.timeout) return t('join.err_timeout');
  if (result.error) {
    if (result.error.code === 'quota_exceeded') return t('join.err_quota');
    return describeError(result.error);
  }
  return t('join.failed', { reason: reasonText(result.reason) });
}
