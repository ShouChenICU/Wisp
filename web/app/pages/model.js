// Model (spec 7.3, 5.3): the OpenAI-compatible endpoint, the API key and a
// connection test.
//
// The key is write-only. The device only ever says whether one is set
// (has_key); this page never shows, stores or logs what is typed beyond
// the input itself, clears the input once it is saved, and sends api_key
// only when the input has something in it -- an empty input keeps the
// key. "Clear" is a separate, confirmed action that sends api_key "".
//
// The key belongs to the origin (scheme://host:port) it was saved for: a
// base_url on another origin saved without a new key makes the device
// forget the key (spec 5.3). The page warns before saving and, when that
// happens, asks for the key again instead of running the test.
//
// The test (POST /llm/test) runs against the saved settings, so with
// unsaved edits the button saves them first and says so.

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError, errorDetail } from '../api.js';
import { t } from '../i18n/index.js';
import { Card } from '../components/card.js';
import { Badge } from '../components/badge.js';
import { confirmDialog } from '../components/dialog.js';
import { toast } from '../components/toast.js';
import { IconAlert, IconCheck, IconKey, IconLink, IconModel, IconRefresh } from '../icons.js';
import { formatCount } from '../util.js';

const URL_PLACEHOLDER = 'https://api.openai.com/v1';
// LlmConfigService's limit for the model id.
const MODEL_MAX_BYTES = 128;
const MANUAL = '\u0000manual';

/**
 * @typedef {Object} LlmConfig  GET/PUT /llm
 * @property {string} base_url
 * @property {string} model
 * @property {boolean} has_key
 *
 * @typedef {Object} LlmTest  POST /llm/test
 * @property {boolean} ok
 * @property {null|'dns'|'connect'|'tls'|'timeout'|'other'|'http'} error
 * @property {number} http_status  0 when no HTTP response arrived
 * @property {number} latency_ms
 * @property {string[]} models
 */

function validUrl(s) {
  return /^https?:\/\/[^\s/?#]+/i.test(s);
}

// The URL's origin as the device compares it (lowercase host, default port
// left out, user info ignored); '' when it does not parse.
function originOf(s) {
  try {
    return new URL(s).origin;
  } catch {
    return '';
  }
}

// Printable ASCII without spaces, as LlmConfigService requires.
function validKey(s) {
  return /^[\x21-\x7e]*$/.test(s);
}

export function ModelPage() {
  const [config, setConfig] = useState(/** @type {LlmConfig|null} */ (null));
  const [loadError, setLoadError] = useState(null);
  const [url, setUrl] = useState('');
  const [model, setModel] = useState('');
  const [key, setKey] = useState('');
  const [problem, setProblem] = useState(null); // {field, text}
  // The last save moved the endpoint to another origin and the device
  // forgot the key.
  const [keyDropped, setKeyDropped] = useState(false);
  const [saving, setSaving] = useState(false);
  const [testing, setTesting] = useState(false);
  const [result, setResult] = useState(/** @type {LlmTest|null} */ (null));
  const [testError, setTestError] = useState(null);
  const [models, setModels] = useState(/** @type {string[]} */ ([]));
  const [manualModel, setManualModel] = useState(false);
  const alive = useRef(true);

  function adopt(c) {
    setConfig(c);
    setUrl(c.base_url || '');
    setModel(c.model || '');
  }

  async function load() {
    try {
      const c = await api('GET', '/llm');
      if (!alive.current) return;
      adopt(c);
      setLoadError(null);
    } catch (e) {
      if (alive.current && e.status !== 401) setLoadError(e);
    }
  }

  useEffect(() => {
    alive.current = true;
    load();
    return () => {
      alive.current = false;
    };
  }, []);

  const dirty = !!config && (url.trim() !== config.base_url || model !== config.model || key !== '');
  // Saving now would drop the stored key.
  const willDropKey =
    !!config && config.has_key && key === '' && url.trim() !== config.base_url && originOf(url.trim()) !== originOf(config.base_url);

  function check() {
    const u = url.trim();
    if (!u) return { field: 'url', text: t('model.err_url_empty') };
    if (!validUrl(u)) return { field: 'url', text: t('model.err_url') };
    if (new TextEncoder().encode(model).length > MODEL_MAX_BYTES) {
      return { field: 'model', text: t('model.err_model_long', { max: MODEL_MAX_BYTES }) };
    }
    if (!validKey(key)) return { field: 'key', text: t('model.err_key') };
    return null;
  }

  // Saves the form; true on success. Only changed fields are sent, and
  // api_key only when something was typed. `quiet` skips the toast when
  // a test follows and reports for itself. Returns 'key_dropped' when the
  // device forgot the key because the endpoint moved to another origin.
  async function save(quiet = false) {
    const bad = check();
    setProblem(bad);
    if (bad) return false;
    const body = {};
    const u = url.trim();
    if (u !== config.base_url) body.base_url = u;
    if (model !== config.model) body.model = model;
    if (key) body.api_key = key;
    if (!Object.keys(body).length) return true;
    setSaving(true);
    try {
      const hadKey = config.has_key;
      const c = await api('PUT', '/llm', body);
      if (!alive.current) return false;
      setKey('');
      adopt(c);
      const dropped = hadKey && !c.has_key && !('api_key' in body);
      setKeyDropped(dropped);
      // A different endpoint has a different model list.
      if ('base_url' in body) {
        setModels([]);
        setResult(null);
      }
      if (dropped) return 'key_dropped';
      if (!quiet) toast(t('model.saved'), 'ok');
      return true;
    } catch (e) {
      if (e.status !== 401) setProblem({ field: 'form', text: t('model.save_failed', { reason: describeError(e) }) });
      return false;
    } finally {
      if (alive.current) setSaving(false);
    }
  }

  async function test() {
    if (dirty) {
      const saved = await save(true);
      // Without a key the test would only report 401; ask for the key.
      if (saved !== true) return;
    }
    setTesting(true);
    setTestError(null);
    setResult(null);
    try {
      const r = await api('POST', '/llm/test');
      if (!alive.current) return;
      setResult(r);
      if (r.ok) {
        const list = [...new Set((r.models || []).filter((m) => typeof m === 'string' && m))].sort();
        setModels(list);
        // A model the list does not offer stays editable as typed.
        setManualModel(list.length === 0 || (model !== '' && !list.includes(model)));
      }
    } catch (e) {
      if (alive.current && e.status !== 401) setTestError(e);
    } finally {
      if (alive.current) setTesting(false);
    }
  }

  async function clearKey() {
    const ok = await confirmDialog({
      title: t('model.clear_title'),
      body: t('model.clear_body'),
      okText: t('model.clear_confirm'),
      danger: true,
    });
    if (!ok) return;
    setSaving(true);
    try {
      const c = await api('PUT', '/llm', { api_key: '' });
      if (!alive.current) return;
      setConfig(c);
      setKey('');
      toast(t('model.cleared'), 'ok');
    } catch (e) {
      if (e.status !== 401) toast(t('model.save_failed', { reason: describeError(e) }), 'err');
    } finally {
      if (alive.current) setSaving(false);
    }
  }

  const busy = saving || testing;
  const fieldError = (f) => (problem && problem.field === f ? problem.text : '');
  const useSelect = models.length > 0 && !manualModel;
  const selectValue = models.includes(model) ? model : '';

  if (loadError && !config) {
    return html`<div class="page">
      <${Head} />
      <${Card} title=${t('model.load_failed')} icon=${IconAlert} tone="err">
        <p class="muted">${describeError(loadError)}</p>
        ${errorDetail(loadError) ? html`<p class="err-detail mono">${errorDetail(loadError)}</p>` : null}
        <button type="button" class="btn btn-secondary" onClick=${load}>${t('common.retry')}</button>
      </${Card}>
    </div>`;
  }

  return html`<div class="page">
    <${Head} />
    <div class="grid grid-2 grid-wide-first">
      <${Card} title=${t('model.endpoint_title')} icon=${IconLink}>
        ${!config
          ? html`<div aria-busy="true" aria-label=${t('common.loading')}>
              <div class="skel skel-line"></div><br /><div class="skel skel-line"></div>
            </div>`
          : html`<form
              class="form"
              novalidate
              onSubmit=${(e) => {
                e.preventDefault();
                save();
              }}
            >
              <div class="field">
                <label class="field-label" for="llm-url">${t('model.url')}</label>
                <input
                  id="llm-url"
                  class="input mono-input"
                  type="url"
                  inputmode="url"
                  autocomplete="off"
                  autocapitalize="off"
                  spellcheck=${false}
                  placeholder=${URL_PLACEHOLDER}
                  value=${url}
                  onInput=${(e) => {
                    setUrl(e.currentTarget.value);
                    setProblem(null);
                  }}
                  disabled=${busy}
                  aria-invalid=${fieldError('url') ? 'true' : 'false'}
                  aria-describedby="llm-url-hint"
                />
                <p id="llm-url-hint" class=${fieldError('url') ? 'field-hint is-error' : 'field-hint'}>
                  ${fieldError('url') || t('model.url_hint')}
                </p>
              </div>

              <div class="field">
                <div class="field-row">
                  <label class="field-label" for="llm-key">${t('model.key')}</label>
                  ${config.has_key
                    ? html`<${Badge} tone="ok"><${IconKey} size=${13} />${t('model.key_set')}</${Badge}>`
                    : html`<${Badge} tone="muted">${t('model.key_unset')}</${Badge}>`}
                </div>
                <div class="input-group">
                  <input
                    id="llm-key"
                    class="input mono-input"
                    type="password"
                    autocomplete="off"
                    autocapitalize="off"
                    spellcheck=${false}
                    placeholder=${config.has_key ? t('model.key_keep_ph') : t('model.key_ph')}
                    value=${key}
                    onInput=${(e) => {
                      setKey(e.currentTarget.value);
                      setProblem(null);
                      setKeyDropped(false);
                    }}
                    disabled=${busy}
                    aria-invalid=${fieldError('key') ? 'true' : 'false'}
                    aria-describedby="llm-key-hint"
                  />
                  ${config.has_key
                    ? html`<button type="button" class="btn btn-ghost input-addon-text" onClick=${clearKey} disabled=${busy}>
                        ${t('model.clear')}
                      </button>`
                    : null}
                </div>
                <p id="llm-key-hint" class=${fieldError('key') ? 'field-hint is-error' : willDropKey ? 'field-hint is-warn' : 'field-hint'}>
                  ${fieldError('key') ||
                  (willDropKey ? t('model.key_hint_origin') : config.has_key ? t('model.key_hint_set') : t('model.key_hint'))}
                </p>
              </div>

              ${keyDropped && !config.has_key
                ? html`<div class="alert tone-warn" role="alert"><${IconAlert} size=${18} /><span>${t('model.key_dropped')}</span></div>`
                : null}

              <div class="field">
                <label class="field-label" for="llm-model">${t('model.model')}</label>
                ${useSelect
                  ? html`<select
                      id="llm-model"
                      class="input select"
                      value=${selectValue}
                      onChange=${(e) => {
                        const v = e.currentTarget.value;
                        if (v === MANUAL) setManualModel(true);
                        else setModel(v);
                        setProblem(null);
                      }}
                      disabled=${busy}
                      aria-describedby="llm-model-hint"
                    >
                      ${selectValue === '' ? html`<option value="" disabled>${t('model.pick')}</option>` : null}
                      ${models.map((m) => html`<option key=${m} value=${m}>${m}</option>`)}
                      <option value=${MANUAL}>${t('model.manual_option')}</option>
                    </select>`
                  : html`<input
                      id="llm-model"
                      class="input mono-input"
                      type="text"
                      autocomplete="off"
                      autocapitalize="off"
                      spellcheck=${false}
                      placeholder=${t('model.model_ph')}
                      value=${model}
                      onInput=${(e) => {
                        setModel(e.currentTarget.value);
                        setProblem(null);
                      }}
                      disabled=${busy}
                      aria-invalid=${fieldError('model') ? 'true' : 'false'}
                      aria-describedby="llm-model-hint"
                    />`}
                <p id="llm-model-hint" class=${fieldError('model') ? 'field-hint is-error' : 'field-hint'}>
                  ${fieldError('model') ||
                  (useSelect
                    ? t('model.model_hint_list', { count: formatCount(models.length) })
                    : models.length
                      ? html`${t('model.model_hint_manual')}
                          <button type="button" class="btn btn-link btn-inline" onClick=${() => setManualModel(false)}>
                            ${t('model.back_to_list')}
                          </button>`
                      : t('model.model_hint'))}
                </p>
              </div>

              ${problem?.field === 'form'
                ? html`<div class="alert tone-err" role="alert"><${IconAlert} size=${18} /><span>${problem.text}</span></div>`
                : null}

              <div class="form-actions">
                <button type="submit" class="btn btn-primary" disabled=${busy || !dirty}>
                  ${saving ? html`<span class="spinner" aria-hidden="true"></span>` : null}${t('model.save')}
                </button>
              </div>
            </form>`}
      </${Card}>

      <${Card} title=${t('model.test_title')} icon=${IconModel}>
        <p class="muted">${t('model.test_desc')}</p>
        <div class="test-actions">
          <button
            type="button"
            class="btn btn-secondary"
            onClick=${test}
            disabled=${busy || !config || (!dirty && !config.base_url)}
          >
            ${testing ? html`<span class="spinner" aria-hidden="true"></span>` : html`<${IconRefresh} size=${16} />`}
            ${dirty ? t('model.save_and_test') : t('model.test')}
          </button>
        </div>
        ${testing ? html`<p class="muted" role="status">${t('model.testing')}</p>` : null}
        ${testError
          ? html`<div class="alert tone-err" role="alert">
              <${IconAlert} size=${18} />
              <span>${testError.code === 'unavailable' ? t('model.test_unavailable') : describeError(testError)}</span>
            </div>`
          : null}
        ${result ? html`<${TestResult} result=${result} />` : null}
      </${Card}>
    </div>
  </div>`;
}

function Head() {
  return html`<header class="page-head">
    <div>
      <h1 class="page-title" tabindex="-1">${t('nav.model')}</h1>
      <p class="page-sub">${t('model.sub')}</p>
    </div>
  </header>`;
}

/** What a failed test means, in words (spec 5.3 error categories). */
function failureText(r) {
  if (r.error === 'http') {
    const s = r.http_status;
    if (s === 200) return t('model.fail_not_list');
    if (s === 401 || s === 403) return t('model.fail_auth', { status: s });
    if (s === 404) return t('model.fail_404');
    if (s === 429) return t('model.fail_429');
    if (s >= 500) return t('model.fail_5xx', { status: s });
    return t('model.fail_http', { status: s });
  }
  if (['dns', 'connect', 'tls', 'timeout'].includes(r.error)) return t('model.fail_' + r.error);
  return t('model.fail_other');
}

function TestResult({ result: r }) {
  const latency = html`<span class="mono">${formatCount(r.latency_ms)} ms</span>`;
  if (r.ok) {
    const n = (r.models || []).length;
    return html`<div class="test-result tone-ok" role="status">
      <div class="test-head"><${IconCheck} size=${18} /><strong>${t('model.ok')}</strong></div>
      <dl class="kv">
        <div><dt>${t('model.latency')}</dt><dd>${latency}</dd></div>
        <div><dt>${t('model.models_found')}</dt><dd class="mono">${formatCount(n)}</dd></div>
      </dl>
    </div>`;
  }
  return html`<div class="test-result tone-err" role="alert">
    <div class="test-head"><${IconAlert} size=${18} /><strong>${t('model.failed')}</strong></div>
    <p>${failureText(r)}</p>
    <dl class="kv">
      <div><dt>${t('model.category')}</dt><dd class="mono">${r.error || '—'}${r.http_status ? ' · HTTP ' + r.http_status : ''}</dd></div>
      <div><dt>${t('model.latency')}</dt><dd>${latency}</dd></div>
    </dl>
  </div>`;
}
