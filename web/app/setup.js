// The Wi-Fi setup wizard (setup.html; spec 4.3, 7.3), which a phone on the
// device's setup hotspot is sent to by the captive-portal redirect.
//
// It pairs first: POST /pair/ap pairs any browser on the hotspot without a
// code. Anywhere else the device refuses that with 403 (the simulator,
// whose interface is loopback, always does), and the wizard falls back to
// the normal pairing code. Then: pick a network -> password -> connecting
// (following net.wifi.state for up to a minute) -> the device's new
// address, or the reason it failed with a way to try again. A saved
// network skips the password and connects with the one the device keeps,
// unless the device has parked that password or rejects it now; then the
// password form opens (spec 7.3).
//
// From the network list on, the wizard holds an `events` subscription, so
// the shared socket is already open when a join is sent and the join's own
// net.wifi.state events are not missed.
//
// Only files from the device's own pack are loaded: on the hotspot the
// phone has no internet.

import { html, render, useEffect, useRef, useState } from '../vendor/htm-preact-3.1.1.mjs';
import { api, describeError, errorDetail, setUnauthorizedHandler } from './api.js';
import { t, useLang } from './i18n/index.js';
import { subscribe } from './ws.js';
import { Toaster } from './components/toast.js';
import { LangSwitch } from './components/lang.js';
import {
  ADMIN_PORT,
  JOIN_TIMEOUT_MS,
  MDNS_URL,
  JoinForm,
  ScanList,
  Ssid,
  displaySsid,
  joinFailureText,
  joinNetwork,
  prepareScan,
  rejectedKeys,
} from './components/wifi.js';
import { PairPage } from './pages/pair.js';
import { IconAlert, IconCheck, IconChevronLeft, IconPlus, IconRefresh, WispGlyph } from './icons.js';
import { useNow } from './util.js';

const IPV4 = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})$/;

function adminUrl(ip) {
  return IPV4.test(ip) ? `http://${ip}:${ADMIN_PORT}` : '';
}

const STEPS = ['setup.step_pick', 'setup.step_password', 'setup.step_connect'];
const STEP_OF = { pick: 0, password: 1, connecting: 2, failed: 2, done: 3 };
// Phases that hold the events subscription (see the file comment).
const LIVE = new Set(['pick', 'password', 'connecting', 'failed']);

function Wizard() {
  useLang();
  // 'auth' | 'pair' | 'pick' | 'password' | 'connecting' | 'done' | 'failed'
  const [phase, setPhase] = useState('auth');
  const [name, setName] = useState('');
  const [authError, setAuthError] = useState(null);
  const [scan, setScan] = useState(null);
  const [scanning, setScanning] = useState(false);
  const [scanError, setScanError] = useState(null);
  // ssidKey()s of saved networks whose stored password is parked.
  const [rejected, setRejected] = useState(() => new Set());
  // The network being joined: a scan entry, or {ssid, secured, manual}.
  const [network, setNetwork] = useState(null);
  const [manual, setManual] = useState(false);
  const [result, setResult] = useState(null);
  const [joinStart, setJoinStart] = useState(0);
  const [stopJoin, setStopJoin] = useState(null);
  // Shown on the password form when the stored password was rejected.
  const [formError, setFormError] = useState('');
  // Kept only for "try again" after a failure that was not the password's
  // fault; never rendered.
  const lastPassword = useRef('');
  // The current join uses the device's stored password.
  const savedJoin = useRef(false);

  async function authenticate() {
    setPhase('auth');
    setAuthError(null);
    try {
      const info = await api('GET', '/info');
      setName(info?.name || '');
      if (!info?.paired) await api('POST', '/pair/ap');
      setPhase('pick');
    } catch (e) {
      // Not on the hotspot: pair with a code from the device's screen.
      if (e.status === 403 && e.code === 'permission_denied') setPhase('pair');
      else setAuthError(e);
    }
  }

  useEffect(() => {
    // A token revoked mid-way (or never set) starts over from pairing.
    const restore = setUnauthorizedHandler(() => authenticate());
    authenticate();
    return restore;
  }, []);

  useEffect(() => {
    document.title = `${t('setup.title')} · Wisp`;
  });

  useEffect(() => {
    if (phase === 'pick' && !scan && !scanning) runScan();
  }, [phase]);

  const live = LIVE.has(phase);
  useEffect(() => (live ? subscribe(['events'], () => {}) : undefined), [live]);

  async function runScan() {
    setScanning(true);
    setScanError(null);
    try {
      const [r, wifi] = await Promise.all([
        api('GET', '/wifi/scan'),
        // Without it every saved network is tried with its stored password.
        api('GET', '/wifi').catch(() => null),
      ]);
      setRejected(rejectedKeys(wifi));
      setScan(prepareScan(r?.networks));
    } catch (e) {
      if (e.status !== 401) setScanError(e);
    } finally {
      setScanning(false);
    }
  }

  function choose(n) {
    setManual(false);
    setNetwork(n);
    setFormError('');
    if (n.saved && !rejected.has(n.ssid_hex)) connect(n, '', true);
    else if (n.secured) setPhase('password');
    else connect(n, '');
  }

  // Leaves a join for the password form of the same network ("改密码").
  function changePassword() {
    stopJoin?.();
    setStopJoin(null);
    setManual(false);
    setFormError('');
    setPhase('password');
  }

  // `target`: a scan entry, or the typed hidden network (JoinForm).
  // `saved`: join with the device's stored password.
  function connect(target, password, saved = false) {
    lastPassword.current = password;
    savedJoin.current = saved;
    setNetwork(target);
    setResult(null);
    setFormError('');
    setJoinStart(Date.now());
    setPhase('connecting');
    const stop = joinNetwork(target, password, (r) => {
      setStopJoin(null);
      setResult(r);
      // The stored password no longer works, or the network is no longer
      // saved: ask for the password as for a new network.
      if (saved && !r.ok && (r.reason === 'auth_failed' || r.error?.code === 'not_found')) {
        if (r.reason === 'auth_failed') setFormError(t('join.saved_rejected'));
        setPhase('password');
        return;
      }
      setPhase(r.ok ? 'done' : 'failed');
      // The device closes its hotspot a few seconds after this page has
      // read the success over it (GET /wifi showing "connected", spec 3).
      // A result taken from the event socket was not such a read, so make
      // one; if it never arrives the hotspot still closes on its own.
      if (r.ok) api('GET', '/wifi').catch(() => {});
    }, { saved });
    setStopJoin(() => stop);
  }

  function backToList() {
    stopJoin?.();
    setStopJoin(null);
    setManual(false);
    setResult(null);
    setFormError('');
    setPhase('pick');
  }

  if (phase === 'pair') {
    return html`<${PairPage} onPaired=${() => setPhase('pick')} notice=${t('setup.pair_notice')} />`;
  }

  const step = STEP_OF[phase] ?? -1;
  let body;
  if (phase === 'auth') {
    body = authError
      ? html`<div class="alert tone-err" role="alert">
            <${IconAlert} size=${18} />
            <span>
              ${describeError(authError)}
              ${errorDetail(authError) ? html`<span class="err-detail mono">${errorDetail(authError)}</span>` : null}
            </span>
          </div>
          <button type="button" class="btn btn-primary btn-lg btn-block" onClick=${authenticate}>${t('common.retry')}</button>`
      : html`<p class="setup-wait" role="status"><span class="spinner" aria-hidden="true"></span>${t('setup.connecting_device')}</p>`;
  } else if (phase === 'pick') {
    body = html`<div class="setup-section-head">
        <h2 class="setup-h2">${t('setup.pick_title')}</h2>
        <button type="button" class="btn btn-ghost btn-sm" onClick=${runScan} disabled=${scanning}>
          ${scanning ? html`<span class="spinner" aria-hidden="true"></span>` : html`<${IconRefresh} size=${16} />`}
          ${t('net.rescan')}
        </button>
      </div>
      ${scanError
        ? html`<div class="alert tone-err" role="alert">
            <${IconAlert} size=${18} /><span>${t('net.scan_failed', { reason: describeError(scanError) })}</span>
          </div>`
        : null}
      ${scanning && !scan
        ? html`<p class="setup-wait" role="status"><span class="spinner" aria-hidden="true"></span>${t('net.scanning')}</p>`
        : null}
      ${scan && scan.list.length === 0 && !scanning ? html`<p class="muted">${t('net.scan_empty')}</p>` : null}
      ${scan && scan.list.length
        ? html`<div class="setup-scroll">
            <${ScanList} networks=${scan.list} hidden=${scan.hidden} onPick=${choose} disabled=${scanning} />
          </div>`
        : null}
      <button
        type="button"
        class="btn btn-link setup-manual"
        onClick=${() => {
          setManual(true);
          setNetwork(null);
          setPhase('password');
        }}
      >
        <${IconPlus} size=${16} />${t('net.manual')}
      </button>`;
  } else if (phase === 'password') {
    body = html`<div class="setup-section-head">
        <button type="button" class="btn btn-ghost btn-sm setup-back" onClick=${backToList}>
          <${IconChevronLeft} size=${16} />${t('setup.back')}
        </button>
      </div>
      ${manual
        ? html`<h2 class="setup-h2">${t('net.manual_title')}</h2>`
        : html`<h2 class="setup-h2 setup-ssid"><${Ssid} ssid=${network.ssid} /></h2>`}
      <${JoinForm}
        key=${manual ? 'manual' : network.ssid_hex}
        network=${manual ? undefined : network}
        idPrefix="setup-join"
        error=${formError}
        submitText=${t('join.connect')}
        onSubmit=${connect}
      />`;
  } else if (phase === 'connecting') {
    body = html`<${Connecting}
      ssid=${network?.ssid || ''}
      since=${joinStart}
      onBack=${backToList}
      onChangePassword=${savedJoin.current && network?.secured ? changePassword : null}
    />`;
  } else if (phase === 'done') {
    body = html`<${Done} ssid=${network?.ssid || ''} ip=${result?.ip || ''} />`;
  } else if (phase === 'failed') {
    const canRetype = network && (network.secured || network.manual);
    const badPassword = result?.reason === 'auth_failed';
    const retype = () => {
      setManual(!!network.manual);
      setPhase('password');
    };
    body = html`<div class="setup-result">
        <span class="setup-result-icon tone-err"><${IconAlert} size=${30} /></span>
        <h2 class="setup-h2">${t('setup.failed_title', { ssid: displaySsid(network?.ssid || '') })}</h2>
        <p class="muted" role="alert">${result ? joinFailureText(result) : ''}</p>
      </div>
      <div class="setup-actions">
        ${canRetype && badPassword
          ? html`<button type="button" class="btn btn-primary btn-lg btn-block" onClick=${retype}>${t('setup.retype')}</button>`
          : html`<button
              type="button"
              class="btn btn-primary btn-lg btn-block"
              onClick=${() => connect(network, lastPassword.current, savedJoin.current)}
            >
              ${t('common.retry')}
            </button>`}
        <button type="button" class="btn btn-secondary btn-lg btn-block" onClick=${backToList}>${t('setup.other_network')}</button>
        ${canRetype && !badPassword
          ? html`<button type="button" class="btn btn-link setup-manual" onClick=${retype}>${t('setup.retype')}</button>`
          : null}
      </div>`;
  }

  return html`<main class="pair-page setup-page" id="main">
    <${LangSwitch} class="pair-lang" />
    <div class="pair-card setup-card">
      <div class="setup-head">
        <div class="setup-glyph"><${WispGlyph} size=${40} /></div>
        <div>
          <h1 class="setup-title">${t('setup.title')}</h1>
          <p class="pair-sub setup-sub">${t('setup.intro', { name: name || 'Wisp' })}</p>
        </div>
      </div>
      ${step >= 0
        ? html`<ol class="pair-steps setup-steps" aria-label=${t('setup.steps')}>
            ${STEPS.map(
              (key, i) => html`<li
                key=${key}
                class=${i === step ? 'is-current' : i < step ? 'is-done' : ''}
                aria-current=${i === step ? 'step' : undefined}
              >
                <span class="step-num">${i < step ? html`<${IconCheck} size=${12} />` : i + 1}</span><span>${t(key)}</span>
              </li>`,
            )}
          </ol>`
        : null}
      ${body}
    </div>
    <p class="pair-foot">${t('app.title')}</p>
  </main>`;
}

// `onChangePassword`: set for a join with the stored password.
function Connecting({ ssid, since, onBack, onChangePassword }) {
  const now = useNow(500);
  const pct = Math.min(100, ((now - since) / JOIN_TIMEOUT_MS) * 100);
  return html`<div class="setup-result" role="status">
      <span class="setup-orb" aria-hidden="true"><${WispGlyph} size=${44} /></span>
      <h2 class="setup-h2">${t('setup.connecting', { ssid: displaySsid(ssid) })}</h2>
      <p class="muted">${t('setup.connecting_hint')}</p>
      ${onChangePassword ? html`<p class="muted">${t('join.saved_note')}</p>` : null}
      <div class="meter tone-accent setup-meter" aria-hidden="true"><div class="meter-fill" style=${{ width: pct + '%' }}></div></div>
    </div>
    <button type="button" class="btn btn-ghost btn-block" onClick=${onBack}>${t('setup.cancel')}</button>
    ${onChangePassword
      ? html`<button type="button" class="btn btn-link setup-manual" onClick=${onChangePassword}>${t('join.change_password')}</button>`
      : null}`;
}

function Done({ ssid, ip }) {
  const url = adminUrl(ip);
  return html`<div class="setup-result">
      <span class="setup-result-icon tone-ok"><${IconCheck} size=${30} /></span>
      <h2 class="setup-h2">${t('setup.done_title', { ssid: displaySsid(ssid) })}</h2>
      <p class="muted">${t('setup.done_hint', { ssid: displaySsid(ssid) })}</p>
    </div>
    <div class="url-box">
      <span class="field-label">${t('setup.address')}</span>
      ${url ? html`<a class="url-link mono" href=${url}>${url}</a>` : html`<span class="muted">${t('setup.no_ip')}</span>`}
      <a class="url-link mono" href=${MDNS_URL}>${MDNS_URL}</a>
      <p class="field-hint">${t('setup.mdns_hint')}</p>
    </div>
    ${url ? html`<a class="btn btn-primary btn-lg btn-block" href=${url}>${t('setup.open_admin')}</a>` : null}
    <p class="muted setup-note">${t('setup.ap_closing')}</p>`;
}

const root = document.getElementById('app');
root.replaceChildren();
render(html`<${Wizard} /><${Toaster} />`, root);
