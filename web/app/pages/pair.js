// Pairing (spec 7.3): ask the device to show a code, then type it in. On
// success the device sets the HttpOnly token cookie and the app moves on
// to the overview.

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError } from '../api.js';
import { navigate } from '../router.js';
import { t, useLang } from '../i18n/index.js';
import { IconAlert, IconLock, WispGlyph } from '../icons.js';
import { toast } from '../components/toast.js';
import { LangSwitch } from '../components/lang.js';
import { useNow } from '../util.js';

// Spec 4.4: a code is valid for 2 minutes. The device does not say when
// its code expires, so the page times it from the moment it asked.
const CODE_TTL_MS = 120000;
const CODE_LEN = 6;

export function PairPage() {
  useLang();
  const [info, setInfo] = useState(null);
  const [phase, setPhase] = useState('start'); // 'start' | 'code'
  const [code, setCode] = useState('');
  const [busy, setBusy] = useState(false);
  // {key, tone} or {text, tone}; a key is translated at render time so the
  // message follows a language switch.
  const [error, setError] = useState(null);
  const [expiresAt, setExpiresAt] = useState(0);
  const inputRef = useRef(null);
  const now = useNow(1000);

  useEffect(() => {
    let alive = true;
    api('GET', '/info')
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

  const remaining = Math.max(0, expiresAt - now);
  const expired = phase === 'code' && remaining === 0;

  function apiError(e) {
    return { err: e, tone: 'err' };
  }

  async function requestCode() {
    if (busy) return;
    setBusy(true);
    setError(null);
    try {
      await api('POST', '/pair/start');
      setCode('');
      setExpiresAt(Date.now() + CODE_TTL_MS);
      setPhase('code');
      // Re-requesting while already on the code step keeps focus useful.
      setTimeout(() => inputRef.current?.focus(), 0);
    } catch (e) {
      setError(e.status === 429 ? { key: 'pair.err_locked', tone: 'warn' } : apiError(e));
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
        setError({ key: 'pair.err_locked', tone: 'warn' });
      } else if (e.status === 403 && e.code === 'permission_denied') {
        // The device answers a wrong code and an expired one the same way.
        setError(
          Date.now() >= expiresAt
            ? { key: 'pair.err_expired', tone: 'warn' }
            : { key: 'pair.err_wrong', tone: 'err' },
        );
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
    if (v.length === CODE_LEN && !expired) confirm(v);
  }

  const mm = Math.floor(remaining / 60000);
  const ss = String(Math.floor((remaining % 60000) / 1000)).padStart(2, '0');
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
        ? html`<button type="button" class="btn btn-primary btn-lg btn-block" onClick=${requestCode} disabled=${busy}>
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
              disabled=${busy || expired}
              aria-describedby="pair-hint"
              aria-invalid=${error ? 'true' : 'false'}
              spellcheck=${false}
            />
            <p id="pair-hint" class=${expired ? 'pair-hint is-expired' : 'pair-hint'}>
              ${expired ? t('pair.expired') : t('pair.expires_in', { time: `${mm}:${ss}` })}
            </p>
            ${expired
              ? html`<button type="button" class="btn btn-primary btn-lg btn-block" onClick=${requestCode} disabled=${busy}>
                  ${t('pair.get_new')}
                </button>`
              : html`<button type="submit" class="btn btn-primary btn-lg btn-block" disabled=${busy || code.length !== CODE_LEN}>
                    ${busy ? html`<span class="spinner" aria-hidden="true"></span>` : null} ${t('pair.submit')}
                  </button>
                  <button type="button" class="btn btn-link" onClick=${requestCode} disabled=${busy}>
                    ${t('pair.resend')}
                  </button>`}
          </form>`}

      ${error
        ? html`<div class=${'alert tone-' + error.tone} role="alert">
            <${error.tone === 'warn' ? IconLock : IconAlert} size=${18} />
            <span>${errorText}</span>
          </div>`
        : null}
    </div>
    <p class="pair-foot">
      ${info ? `${name} · ${t('common.fw', { version: info.fw || '—' })}` : t('app.title')}
    </p>
  </main>`;
}
