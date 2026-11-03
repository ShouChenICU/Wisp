// Pairing (spec 7.3): ask the device to show a code, then type it in. On
// success the device sets the HttpOnly token cookie and the app moves on
// to the overview.

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError, getInfo } from '../api.js';
import { navigate } from '../router.js';
import { t, useLang } from '../i18n/index.js';
import { IconAlert, IconLock, WispGlyph } from '../icons.js';
import { toast } from '../components/toast.js';
import { LangSwitch } from '../components/lang.js';
import { useNow } from '../util.js';

// The device tells a wrong code (permission_denied) from an expired one
// (code_expired), and a locked pairing (429) says how long it stays
// locked (spec 6.1), so the page keeps no clock of its own for the code.
const CODE_LEN = 6;

function clock(ms) {
  const total = Math.ceil(ms / 1000);
  return `${Math.floor(total / 60)}:${String(total % 60).padStart(2, '0')}`;
}

export function PairPage() {
  useLang();
  const [info, setInfo] = useState(null);
  const [phase, setPhase] = useState('start'); // 'start' | 'code'
  const [code, setCode] = useState('');
  const [busy, setBusy] = useState(false);
  // {key, tone} or {text, tone}; a key is translated at render time so the
  // message follows a language switch.
  const [error, setError] = useState(null);
  const [expired, setExpired] = useState(false);
  const [lockedUntil, setLockedUntil] = useState(0);
  const inputRef = useRef(null);
  // useNow() only re-renders every second; the countdown reads the clock
  // itself so it never shows more than the device said.
  useNow(1000);
  const lockLeft = Math.max(0, lockedUntil - Date.now());
  const locked = lockLeft > 0;

  useEffect(() => {
    let alive = true;
    getInfo()
      .then((i) => {
        if (!alive) return;
        setInfo(i);
        if (i && i.paired) navigate('/overview');
      })
      .catch(() => {});
    return () => {
      alive = false;
    };
  }, []);

  useEffect(() => {
    if (phase === 'code') inputRef.current?.focus();
  }, [phase]);

  // The lock has run out: the countdown message goes, the buttons return.
  useEffect(() => {
    if (lockedUntil && !locked) {
      setLockedUntil(0);
      setError((e) => (e && e.lock ? null : e));
    }
  }, [locked, lockedUntil]);

  function apiError(e) {
    return { err: e, tone: 'err' };
  }

  function lockError(e) {
    if (e.retryAfterS) {
      setLockedUntil(Date.now() + e.retryAfterS * 1000);
      return { key: 'pair.err_locked_now', tone: 'warn', lock: true };
    }
    return { key: 'pair.err_locked', tone: 'warn' };
  }

  async function requestCode() {
    if (busy) return;
    setBusy(true);
    setError(null);
    try {
      await api('POST', '/pair/start');
      setCode('');
      setExpired(false);
      setPhase('code');
      // Re-requesting while already on the code step keeps focus useful.
      setTimeout(() => inputRef.current?.focus(), 0);
    } catch (e) {
      setError(e.status === 429 ? lockError(e) : apiError(e));
    } finally {
      setBusy(false);
    }
  }

  async function confirm(value) {
    if (busy || value.length !== CODE_LEN) return;
    setBusy(true);
    setError(null);
    try {
      await api('POST', '/pair/confirm', { code: value });
      toast(t('pair.success'), 'ok');
      navigate('/overview');
    } catch (e) {
      setCode('');
      if (e.status === 429) {
        setError(lockError(e));
      } else if (e.code === 'code_expired') {
        setExpired(true);
        setError({ key: 'pair.err_expired', tone: 'warn' });
      } else if (e.status === 403 && e.code === 'permission_denied') {
        setError({ key: 'pair.err_wrong', tone: 'err' });
      } else {
        setError(apiError(e));
      }
      setTimeout(() => inputRef.current?.focus(), 0);
    } finally {
      setBusy(false);
    }
  }

  function onInput(e) {
    const v = e.currentTarget.value.replace(/\D/g, '').slice(0, CODE_LEN);
    e.currentTarget.value = v;
    setCode(v);
    if (error) setError(null);
    if (v.length === CODE_LEN && !expired && !locked) confirm(v);
  }

  const name = info?.name || 'Wisp';
  const errorText = error ? (error.key ? t(error.key) : describeError(error.err)) : '';

  return html`<main class="pair-page" id="main">
    <${LangSwitch} class="pair-lang" />
    <div class="pair-card">
      <div class="pair-glyph"><${WispGlyph} size=${68} /></div>
      <h1 class="pair-title">${t('pair.title', { name })}</h1>
      <p class="pair-sub">${t('pair.intro')}</p>

      <ol class="pair-steps" aria-label=${t('pair.steps')}>
        <li class=${phase === 'start' ? 'is-current' : 'is-done'}>
          <span class="step-num">1</span><span>${t('pair.step1')}</span>
        </li>
        <li class=${phase === 'code' ? 'is-current' : ''}>
          <span class="step-num">2</span><span>${t('pair.step2')}</span>
        </li>
      </ol>

      ${phase === 'start'
        ? html`<button type="button" class="btn btn-primary btn-lg btn-block" onClick=${requestCode} disabled=${busy || locked}>
            ${busy ? html`<span class="spinner" aria-hidden="true"></span>` : null}
            ${t('pair.get_code')}
          </button>`
        : html`<form
            class="pair-form"
            onSubmit=${(e) => {
              e.preventDefault();
              confirm(code);
            }}
          >
            <label class="field-label" for="pair-code">${t('pair.code_label')}</label>
            <input
              id="pair-code"
              ref=${inputRef}
              class="code-input"
              type="text"
              inputmode="numeric"
              autocomplete="one-time-code"
              pattern="[0-9]*"
              maxlength=${CODE_LEN}
              placeholder="••••••"
              value=${code}
              onInput=${onInput}
              disabled=${busy || expired || locked}
              aria-describedby="pair-hint"
              aria-invalid=${error ? 'true' : 'false'}
              spellcheck=${false}
            />
            <p id="pair-hint" class=${expired ? 'pair-hint is-expired' : 'pair-hint'}>
              ${expired ? t('pair.expired') : t('pair.code_ttl')}
            </p>
            ${expired
              ? html`<button type="button" class="btn btn-primary btn-lg btn-block" onClick=${requestCode} disabled=${busy || locked}>
                  ${t('pair.get_new')}
                </button>`
              : html`<button
                    type="submit"
                    class="btn btn-primary btn-lg btn-block"
                    disabled=${busy || locked || code.length !== CODE_LEN}
                  >
                    ${busy ? html`<span class="spinner" aria-hidden="true"></span>` : null} ${t('pair.submit')}
                  </button>
                  <button type="button" class="btn btn-link" onClick=${requestCode} disabled=${busy || locked}>
                    ${t('pair.resend')}
                  </button>`}
          </form>`}

      ${error
        ? html`<div class=${'alert tone-' + error.tone} role="alert">
            <${error.tone === 'warn' ? IconLock : IconAlert} size=${18} />
            <span>
              ${errorText}
              ${error.lock && locked
                ? html`<span class="alert-timer" role="timer">${t('pair.locked_retry_in', { time: clock(lockLeft) })}</span>`
                : null}
            </span>
          </div>`
        : null}
    </div>
    <p class="pair-foot">
      ${info ? `${name} · ${t('common.fw', { version: info.fw || '—' })}` : t('app.title')}
    </p>
  </main>`;
}
