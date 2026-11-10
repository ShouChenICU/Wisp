// Models (spec 7.3, 5.3): the saved model profiles, on three strictly
// separate tabs -- chat (llm), speech recognition (asr) and speech
// synthesis (tts). Each tab lists, creates, edits, tests, activates and
// deletes only its own kind; "set as current" moves only that kind's
// active profile. The tab lives in the hash (#/models/asr).
//
// API keys are write-only. The device only ever says whether a profile
// has one (has_key); this page never shows, stores or logs what is typed
// beyond the input itself, clears the input once it is saved, and sends
// api_key only when the input has something in it -- an empty input
// keeps the key. "Clear" is a separate, confirmed action that sends
// api_key "". "Use the key of another profile" sends api_key_from: the
// device copies the key itself, and only between profiles whose base_url
// is on the same origin, so the page offers only those.
//
// A key belongs to the origin (scheme://host:port) it was saved for: a
// base_url on another origin saved without a new key makes the device
// forget that profile's key (spec 5.3). The form warns before saving and,
// when it happens, asks for the key again instead of running the test.
//
// The test (POST /ai/profiles/{id}/test) runs against what is saved, so
// with unsaved edits the button saves first and says so; a new profile is
// created first and the form stays open on it so a model can be picked
// from the list the test returns. "Fetch" next to the model field is the
// same save-then-test.
//
// What the list says about a model (`details`) turns typing into picking:
// the models of this tab's kind (a model the server gives no kind shows on
// every tab), the chosen TTS model's voices, the ASR model's languages, and
// an LLM's context window and image input, filled in when a model is
// picked and those fields were not touched. Without details the fields
// stay text inputs, and every list keeps a "type it in" option.
//
// The TTS sample rate is never filled in from the model: a set rate is
// sent with every request, and servers that check strictly (speech.cpp)
// refuse it. The model's rate only shows, as the placeholder and in the
// hint.

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError, errorDetail } from '../api.js';
import { getLang, locale, t } from '../i18n/index.js';
import { currentPath, useRoute } from '../router.js';
import { Card } from '../components/card.js';
import { Badge } from '../components/badge.js';
import { confirmDialog } from '../components/dialog.js';
import { toast } from '../components/toast.js';
import { IconAlert, IconCheck, IconKey, IconMic, IconModel, IconPlus, IconRefresh, IconSpeaker, IconTrash } from '../icons.js';
import { formatCount } from '../util.js';

/** @typedef {'llm'|'asr'|'tts'} Kind */

/**
 * @typedef {Object} Preset  one of GET /ai/presets "presets"
 * @property {string} id
 * @property {string} name      the platform's brand name (not translated)
 * @property {Kind[]} kinds
 * @property {string} base_url  '' for custom; may hold a '<…>' placeholder
 * @property {boolean} key_required
 *
 * @typedef {Object} Profile  GET /ai/profiles "profiles", POST/PUT result
 * @property {string} id
 * @property {Kind} kind
 * @property {string} name
 * @property {string} preset
 * @property {string} base_url
 * @property {string} model
 * @property {Object<string, string>} options
 * @property {boolean} has_key
 *
 * @typedef {Object} ProfilesState  GET /ai/profiles, POST /ai/active
 * @property {{llm: string, asr: string, tts: string}} active  '' = none
 * @property {Profile[]} profiles
 *
 * @typedef {Object} Voice  one of ModelDetail "voices"
 * @property {string} name
 * @property {string} [language]
 * @property {string} [gender]
 * @property {string} [description]
 *
 * @typedef {Object} ModelDetail  one of ProbeResult "details"; only what the list said
 * @property {string} id
 * @property {Kind} [kind]
 * @property {number} [sample_rate]
 * @property {number} [context_window]
 * @property {boolean} [vision]
 * @property {string[]} [languages]
 * @property {Voice[]} [voices]
 *
 * @typedef {Object} ProbeResult  POST /ai/profiles/{id}/test
 * @property {boolean} ok
 * @property {null|'dns'|'connect'|'tls'|'timeout'|'other'|'http'|'no_model_list'} error
 * @property {number} http_status  0 when no HTTP response arrived
 * @property {number} latency_ms
 * @property {string[]} models
 * @property {ModelDetail[]} details
 */

const KINDS = [
  { id: 'llm', icon: IconModel },
  { id: 'asr', icon: IconMic },
  { id: 'tts', icon: IconSpeaker },
];
// ModelProfileService's limits (spec 5.3).
const MAX_PROFILES = 16;
const NAME_MAX_BYTES = 32;
const MODEL_MAX_BYTES = 128;
const OPTION_MAX_BYTES = 64;
const MANUAL = '\u0000manual';
// llm-only options the agent reads (agent-core spec 1.5): kept in the MVP
// (MVP spec 3.1) even though the tools that would use "vision" (a
// screenshot) are deferred, so this form is the only way to set it --
// "test connection" elsewhere never writes it back.
const MIN_CONTEXT_WINDOW = 4096;
const MAX_CONTEXT_WINDOW = 1048576;
const DEFAULT_CONTEXT_WINDOW = 65536;
// asr.language and tts.sample_rate/speed (spec 2, ModelProfileService).
const LANGUAGE_RE = /^[A-Za-z-]{1,8}$/;
const MIN_SAMPLE_RATE = 8000;
const MAX_SAMPLE_RATE = 48000;
const DEFAULT_SAMPLE_RATE = 24000;
const MIN_SPEED = 0.25;
const MAX_SPEED = 4.0;
// What ModelProfileService's parse_speed accepts: digits with at most one
// '.', not necessarily on both sides ("1", "1.5", "1.", ".5" all parse; a
// lone "." does not, since there is no digit at all).
const SPEED_RE = /^(?:\d+\.\d*|\.\d+|\d+)$/;
// Offered for ASR when the model does not list its languages.
const COMMON_LANGUAGES = ['zh', 'en', 'ja', 'ko', 'yue', 'fr', 'de', 'es', 'it', 'pt', 'ru'];
const NO_CATALOG = { models: [], details: {} };
// Presets whose spec name is a description rather than a brand.
const PRESET_LABEL_KEYS = { custom: 'models.preset_custom', ollama: 'models.preset_ollama' };

const bytes = (s) => new TextEncoder().encode(s).length;

/** The kind in "#/models/<kind>", or null. */
function kindOf(path) {
  const k = path.split('/')[2] || '';
  return KINDS.some((x) => x.id === k) ? k : null;
}

/** A language code's name in the page's language ("zh" -> "中文"), or the code. */
function langName(code) {
  try {
    const name = new Intl.DisplayNames([locale()], { type: 'language' }).of(code);
    return name && name !== code ? name : code;
  } catch {
    return code;
  }
}

function voiceLabel(v) {
  return [v.name, v.language ? langName(v.language) : '', v.description || ''].filter(Boolean).join(' · ');
}

/** The models of the last test fit for `kind`: those the server gives that kind or none. */
function catalogFor(catalog, kind) {
  if (!catalog) return NO_CATALOG;
  return { models: catalog.models.filter((m) => !catalog.details[m]?.kind || catalog.details[m].kind === kind), details: catalog.details };
}

function presetLabel(preset) {
  if (!preset) return '';
  const key = PRESET_LABEL_KEYS[preset.id];
  return key ? t(key) : preset.name;
}

// The preset's address as a value for the URL field: a placeholder
// address cannot be saved, so it only goes into the field's placeholder.
function presetUrl(preset) {
  return preset && !preset.base_url.includes('<') ? preset.base_url : '';
}

function urlPlaceholder(preset) {
  if (!preset || !preset.base_url) return 'https://api.example.com/v1';
  return preset.base_url.replace(/<[^>]*>/g, `<${t('models.host')}>`);
}

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

// Printable ASCII without spaces, as ModelProfileService requires.
function validKey(s) {
  return /^[\x21-\x7e]*$/.test(s);
}

export function ModelsPage() {
  const path = useRoute();
  const kind = kindOf(path);
  const [state, setState] = useState(/** @type {ProfilesState|null} */ (null));
  const [presets, setPresets] = useState(/** @type {Preset[]|null} */ (null));
  const [loadError, setLoadError] = useState(null);
  // Per profile id: {testing, result, error} of the last test.
  const [tests, setTests] = useState({});
  // Per profile id: {models, details} of its last successful test, the
  // model ids sorted and the details by model id.
  const [catalogs, setCatalogs] = useState({});
  const alive = useRef(true);

  // A bare "#/models" opens the chat tab; replacing the history entry
  // keeps Back from bouncing into this redirect again.
  useEffect(() => {
    if (!kind && currentPath().startsWith('/models')) location.replace('#/models/llm');
  }, [kind]);

  async function load() {
    try {
      const [s, p] = await Promise.all([api('GET', '/ai/profiles'), presets ? null : api('GET', '/ai/presets')]);
      if (!alive.current) return;
      setState(s);
      if (p) setPresets(p.presets || []);
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

  async function runTest(id) {
    setTests((m) => ({ ...m, [id]: { testing: true } }));
    try {
      /** @type {ProbeResult} */
      const r = await api('POST', `/ai/profiles/${encodeURIComponent(id)}/test`);
      if (!alive.current) return;
      setTests((m) => ({ ...m, [id]: { result: r } }));
      if (r.ok) {
        const list = [...new Set((r.models || []).filter((m) => typeof m === 'string' && m))].sort();
        const details = {};
        for (const d of r.details || []) if (d && typeof d.id === 'string') details[d.id] = d;
        setCatalogs((m) => ({ ...m, [id]: { models: list, details } }));
      }
    } catch (e) {
      if (alive.current) setTests((m) => ({ ...m, [id]: e.status === 401 ? undefined : { error: e } }));
    }
  }

  if (!kind) return null;

  if (loadError && !state) {
    return html`<div class="page">
      <${Head} />
      <${Card} title=${t('models.load_failed')} icon=${IconAlert} tone="err">
        <p class="muted">${describeError(loadError)}</p>
        ${errorDetail(loadError) ? html`<p class="err-detail mono">${errorDetail(loadError)}</p>` : null}
        <button type="button" class="btn btn-secondary" onClick=${load}>${t('common.retry')}</button>
      </${Card}>
    </div>`;
  }

  const counts = Object.fromEntries(KINDS.map((k) => [k.id, state ? state.profiles.filter((p) => p.kind === k.id).length : 0]));

  return html`<div class="page">
    <${Head} />
    <nav class="segmented model-tabs" role="tablist" aria-label=${t('models.tabs_label')}>
      ${KINDS.map(
        (k) => html`<a
          key=${k.id}
          role="tab"
          href=${'#/models/' + k.id}
          class=${kind === k.id ? 'seg is-active' : 'seg'}
          aria-selected=${kind === k.id ? 'true' : 'false'}
          aria-controls="model-panel"
        >
          <${k.icon} size=${16} /><span>${t('models.kind.' + k.id)}</span>
          ${state ? html`<span class="count-pill">${formatCount(counts[k.id])}</span>` : null}
        </a>`,
      )}
    </nav>
    <${KindPanel}
      key=${kind}
      kind=${kind}
      state=${state}
      presets=${presets}
      tests=${tests}
      catalogs=${catalogs}
      onState=${setState}
      onReload=${load}
      onTest=${runTest}
    />
  </div>`;
}

function Head() {
  return html`<header class="page-head">
    <div>
      <h1 class="page-title" tabindex="-1">${t('nav.model')}</h1>
      <p class="page-sub">${t('models.sub')}</p>
    </div>
  </header>`;
}

/** One tab: the profiles of one kind, and the form that edits them. */
function KindPanel({ kind, state, presets, tests, catalogs, onState, onReload, onTest }) {
  // null, 'new' or the id of the profile being edited.
  const [editing, setEditing] = useState(/** @type {string|null} */ (null));
  const [busy, setBusy] = useState(''); // id with an activate/delete in flight
  const kindInfo = KINDS.find((k) => k.id === kind);
  const loading = !state || !presets;
  const mine = state ? state.profiles.filter((p) => p.kind === kind) : [];
  const activeId = state ? state.active[kind] : '';
  const full = !!state && state.profiles.length >= MAX_PROFILES;

  async function activate(p) {
    setBusy(p.id);
    try {
      const s = await api('POST', '/ai/active', { id: p.id });
      onState(s);
      toast(t('models.activated', { name: p.name }), 'ok');
    } catch (e) {
      if (e.status !== 401) toast(describeError(e), 'err');
    } finally {
      setBusy('');
    }
  }

  async function remove(p) {
    if (p.id === activeId && mine.length > 1) {
      toast(t('models.delete_active'), 'warn');
      return;
    }
    const ok = await confirmDialog({
      title: t('models.delete_title', { name: p.name }),
      body: t('models.delete_body'),
      okText: t('models.delete_confirm'),
      danger: true,
    });
    if (!ok) return;
    setBusy(p.id);
    try {
      await api('DELETE', `/ai/profiles/${encodeURIComponent(p.id)}`);
      if (editing === p.id) setEditing(null);
      toast(t('models.deleted'), 'ok');
    } catch (e) {
      if (e.status !== 401) toast(describeError(e), 'err');
    } finally {
      setBusy('');
      onReload();
    }
  }

  const form = (profile) => html`<${ProfileForm}
    kind=${kind}
    profile=${profile}
    presets=${presets}
    all=${state.profiles}
    catalog=${profile ? catalogFor(catalogs[profile.id], kind) : NO_CATALOG}
    testing=${profile ? !!tests[profile.id]?.testing : false}
    onSaved=${async (saved, { keepOpen, test }) => {
      await onReload();
      setEditing(keepOpen ? saved.id : null);
      if (test) onTest(saved.id);
    }}
    onCancel=${() => setEditing(null)}
  />`;

  return html`<section id="model-panel" role="tabpanel" aria-label=${t('models.kind.' + kind)}>
    <${Card}
      title=${t('models.kind.' + kind)}
      icon=${kindInfo.icon}
      actions=${html`<button
        type="button"
        class="btn btn-primary btn-sm"
        onClick=${() => setEditing('new')}
        disabled=${loading || full || editing === 'new'}
      >
        <${IconPlus} size=${16} />${t('models.new')}
      </button>`}
    >
      <p class="muted model-desc">${t('models.desc.' + kind)}</p>
      ${full ? html`<p class="inline-note tone-warn"><${IconAlert} size=${16} /><span>${t('models.full', { max: MAX_PROFILES })}</span></p>` : null}
      ${loading
        ? html`<div aria-busy="true" aria-label=${t('common.loading')}>
            <div class="skel skel-line"></div><br /><div class="skel skel-line"></div>
          </div>`
        : html`
            ${editing === 'new' ? html`<div class="model-new">${form(null)}</div>` : null}
            ${mine.length === 0 && editing !== 'new'
              ? html`<div class="empty model-empty">
                  <span class="empty-icon"><${kindInfo.icon} size=${22} /></span>
                  <p class="empty-title">${t('models.empty_title')}</p>
                  <p class="muted">${t('models.empty_hint')}</p>
                </div>`
              : null}
            ${mine.length
              ? html`<ul class="rows model-rows">
                  ${mine.map(
                    (p) => html`<${ProfileRow}
                      key=${p.id}
                      profile=${p}
                      preset=${presets.find((x) => x.id === p.preset)}
                      active=${p.id === activeId}
                      busy=${busy === p.id}
                      test=${tests[p.id]}
                      models=${catalogFor(catalogs[p.id], kind).models}
                      editing=${editing === p.id}
                      form=${editing === p.id ? form(p) : null}
                      onActivate=${() => activate(p)}
                      onTest=${() => onTest(p.id)}
                      onEdit=${() => setEditing(editing === p.id ? null : p.id)}
                      onDelete=${() => remove(p)}
                    />`,
                  )}
                </ul>`
              : null}
          `}
      ${state ? html`<p class="muted model-count">${t('models.count', { count: formatCount(state.profiles.length), max: MAX_PROFILES })}</p>` : null}
    </${Card}>
  </section>`;
}

function ProfileRow({ profile: p, preset, active, busy, test, models, editing, form, onActivate, onTest, onEdit, onDelete }) {
  const testing = !!test?.testing;
  return html`<li class=${active ? 'row row-saved model-row is-current' : 'row row-saved model-row'}>
    <div class="row-line">
      <div class="row-main">
        <span class="row-title">
          ${p.name}${active ? html`<${Badge} tone="accent" dot>${t('models.active')}</${Badge}>` : null}
        </span>
        <span class="row-sub">${presetLabel(preset) || p.preset} · <span class="mono model-url">${p.base_url || '—'}</span></span>
        <span class="row-sub">
          <span class=${p.model ? 'mono' : ''}>${p.model || t('models.no_model')}</span>
          ${p.kind === 'tts' && p.options?.voice ? html` · ${t('models.voice')} <span class="mono">${p.options.voice}</span>` : null}
          ${' · '}${p.has_key ? html`<span class="text-ok"><${IconKey} size=${12} /> ${t('models.key_set')}</span>` : t('models.key_unset')}
        </span>
      </div>
      <div class="row-actions">
        ${active
          ? null
          : html`<button type="button" class="btn btn-secondary btn-sm" onClick=${onActivate} disabled=${busy}>
              ${t('models.activate')}
            </button>`}
        <button type="button" class="btn btn-ghost btn-sm" onClick=${onTest} disabled=${testing || busy}>
          ${testing ? html`<span class="spinner" aria-hidden="true"></span>` : html`<${IconRefresh} size=${15} />`}${t('models.test')}
        </button>
        <button type="button" class="btn btn-ghost btn-sm" onClick=${onEdit} aria-expanded=${editing ? 'true' : 'false'} disabled=${busy}>
          ${t('models.edit')}
        </button>
        <button
          type="button"
          class="btn btn-icon btn-ghost btn-sm"
          onClick=${onDelete}
          disabled=${busy}
          aria-label=${t('models.delete_label', { name: p.name })}
          title=${t('models.delete_confirm')}
        >
          <${IconTrash} size=${17} />
        </button>
      </div>
    </div>
    ${test ? html`<${TestOutcome} test=${test} profile=${p} models=${models} onPick=${editing ? null : onEdit} />` : null}
    ${form}
  </li>`;
}

/** What a failed test means, in words (spec 5.3 error categories). */
function failureText(r) {
  if (r.error === 'no_model_list') return t('models.fail_no_list');
  if (r.error === 'http') {
    const s = r.http_status;
    if (s === 200) return t('models.fail_not_list');
    if (s === 401 || s === 403) return t('models.fail_auth', { status: s });
    if (s === 429) return t('models.fail_429');
    if (s >= 500) return t('models.fail_5xx', { status: s });
    return t('models.fail_http', { status: s });
  }
  if (['dns', 'connect', 'tls', 'timeout'].includes(r.error)) return t('models.fail_' + r.error);
  return t('models.fail_other');
}

function TestOutcome({ test, profile, models, onPick }) {
  if (test.testing) return html`<p class="muted model-testing" role="status">${t('models.testing')}</p>`;
  if (test.error) {
    return html`<div class="alert tone-err model-result" role="alert">
      <${IconAlert} size=${18} />
      <span>${test.error.code === 'unavailable' ? t('models.test_unavailable') : describeError(test.error)}</span>
    </div>`;
  }
  const r = test.result;
  if (!r) return null;
  const latency = html`<span class="mono">${formatCount(r.latency_ms)} ms</span>`;
  if (r.ok) {
    const pick = onPick && models.length > 0 && !models.includes(profile.model);
    return html`<div class="test-result tone-ok model-result" role="status">
      <div class="test-head"><${IconCheck} size=${18} /><strong>${t('models.ok')}</strong></div>
      <dl class="kv">
        <div><dt>${t('models.latency')}</dt><dd>${latency}</dd></div>
        <div><dt>${t('models.models_found')}</dt><dd class="mono">${formatCount(models.length)}</dd></div>
      </dl>
      ${pick
        ? html`<div><button type="button" class="btn btn-secondary btn-sm" onClick=${onPick}>${t('models.pick_model')}</button></div>`
        : null}
    </div>`;
  }
  // No model list is not a broken connection: the model is typed in.
  const tone = r.error === 'no_model_list' ? 'tone-warn' : 'tone-err';
  return html`<div class=${'test-result model-result ' + tone} role="alert">
    <div class="test-head">
      <${IconAlert} size=${18} /><strong>${r.error === 'no_model_list' ? t('models.no_list') : t('models.failed')}</strong>
    </div>
    <p>${failureText(r)}</p>
    <dl class="kv">
      <div><dt>${t('models.category')}</dt><dd class="mono">${r.error || '—'}${r.http_status ? ' · HTTP ' + r.http_status : ''}</dd></div>
      <div><dt>${t('models.latency')}</dt><dd>${latency}</dd></div>
    </dl>
  </div>`;
}

/**
 * Creates (profile null) or edits one profile of `kind`.
 *
 * `onSaved(profile, {keepOpen, test})` runs after a successful save:
 * `keepOpen` keeps the form on that profile (a just-created profile being
 * tested, or a key the device dropped), `test` asks for its test.
 */
function ProfileForm({ kind, profile, presets, all, catalog, testing, onSaved, onCancel }) {
  const { models, details } = catalog;
  const choices = presets.filter((p) => p.kinds.includes(kind));
  const [presetId, setPresetId] = useState(profile ? profile.preset : '');
  const [name, setName] = useState(profile ? profile.name : '');
  const [url, setUrl] = useState(profile ? profile.base_url : '');
  const [key, setKey] = useState('');
  const [keySource, setKeySource] = useState('new'); // 'new' | 'reuse'
  const [keyFrom, setKeyFrom] = useState('');
  const [model, setModel] = useState(profile ? profile.model : '');
  const [manualModel, setManualModel] = useState(false);
  const [language, setLanguage] = useState(profile?.options?.language || '');
  const [voice, setVoice] = useState(profile?.options?.voice || '');
  const [sampleRate, setSampleRate] = useState(profile?.options?.sample_rate || '');
  const [speed, setSpeed] = useState(profile?.options?.speed || '');
  const [vision, setVision] = useState(profile?.options?.vision === 'true');
  const [contextWindow, setContextWindow] = useState(profile?.options?.context_window || '');
  const [manualVoice, setManualVoice] = useState(false);
  const [manualLanguage, setManualLanguage] = useState(false);
  const [advancedOpen, setAdvancedOpen] = useState(!!profile?.options?.sample_rate);
  // Fields the user changed by hand: picking a model no longer fills them.
  const touched = useRef({ vision: false, context_window: false });
  const [problem, setProblem] = useState(null); // {field, text}
  const [saving, setSaving] = useState(false);
  // The last save moved the address to another origin and the device
  // forgot the key.
  const [keyDropped, setKeyDropped] = useState(false);
  const alive = useRef(true);
  useEffect(() => () => (alive.current = false), []);

  const preset = presets.find((p) => p.id === presetId);
  const idp = (profile ? profile.id : 'new') + '-';
  const u = url.trim();
  const origin = validUrl(u) ? originOf(u) : '';
  // Profiles (of any kind) whose key the device would copy: same origin,
  // key set.
  const donors = all.filter((p) => (!profile || p.id !== profile.id) && p.has_key && origin && originOf(p.base_url) === origin);
  const reuse = keySource === 'reuse' && donors.length > 0;
  const hasKey = !!profile?.has_key;

  // A changed preset fills in what still holds the previous preset's
  // defaults (or nothing).
  function pickPreset(id) {
    const next = presets.find((p) => p.id === id);
    if (!u || u === presetUrl(preset)) setUrl(presetUrl(next));
    if (!name.trim() || name === presetLabel(preset)) setName(presetLabel(next));
    setPresetId(id);
    setProblem(null);
  }

  const languageNow = profile?.options?.language || '';
  const voiceNow = profile?.options?.voice || '';
  const sampleRateNow = profile?.options?.sample_rate || '';
  const speedNow = profile?.options?.speed || '';
  const visionNow = profile?.options?.vision === 'true';
  const contextWindowNow = profile?.options?.context_window || '';
  const dirty =
    !profile ||
    presetId !== profile.preset ||
    name !== profile.name ||
    u !== profile.base_url ||
    model !== profile.model ||
    (kind === 'asr' && language.trim() !== languageNow) ||
    (kind === 'tts' &&
      (voice.trim() !== voiceNow || sampleRate.trim() !== sampleRateNow || speed.trim() !== speedNow)) ||
    (kind === 'llm' && (vision !== visionNow || contextWindow.trim() !== contextWindowNow)) ||
    key !== '' ||
    reuse;
  // Saving now would drop the stored key.
  const willDropKey = hasKey && key === '' && !reuse && u !== profile.base_url && originOf(u) !== originOf(profile.base_url);

  // `forTest`: saved to run the test, which needs no voice yet (it may come
  // from the list the test returns).
  function check(forTest = false) {
    if (!preset) return { field: 'preset', text: t('models.err_preset') };
    if (!name.trim() || bytes(name.trim()) > NAME_MAX_BYTES) return { field: 'name', text: t('models.err_name', { max: NAME_MAX_BYTES }) };
    if (!u) return { field: 'url', text: t('models.err_url_empty') };
    if (u.includes('<')) return { field: 'url', text: t('models.err_url_placeholder', { host: t('models.host') }) };
    if (!validUrl(u)) return { field: 'url', text: t('models.err_url') };
    if (keySource === 'reuse' && donors.length > 0 && !keyFrom) return { field: 'key', text: t('models.err_key_from') };
    if (!reuse && !validKey(key)) return { field: 'key', text: t('models.err_key') };
    if (bytes(model) > MODEL_MAX_BYTES) return { field: 'model', text: t('models.err_model_long', { max: MODEL_MAX_BYTES }) };
    if (kind === 'asr' && language.trim() && !LANGUAGE_RE.test(language.trim())) {
      return { field: 'language', text: t('models.err_language') };
    }
    if (bytes(voice.trim()) > OPTION_MAX_BYTES) return { field: 'voice', text: t('models.err_voice_long', { max: OPTION_MAX_BYTES }) };
    // The synthesis endpoint needs a voice name (OpenAI's /audio/speech
    // 400s without one); the server itself stays lenient.
    if (kind === 'tts' && !voice.trim() && !forTest) return { field: 'voice', text: t('models.err_voice_required') };
    if (kind === 'tts' && sampleRate.trim()) {
      const n = Number(sampleRate.trim());
      if (!/^\d+$/.test(sampleRate.trim()) || n < MIN_SAMPLE_RATE || n > MAX_SAMPLE_RATE) {
        return {
          field: 'sample_rate',
          text: t('models.err_sample_rate', { min: formatCount(MIN_SAMPLE_RATE), max: formatCount(MAX_SAMPLE_RATE) }),
        };
      }
    }
    if (kind === 'tts' && speed.trim()) {
      const n = Number(speed.trim());
      if (!SPEED_RE.test(speed.trim()) || n < MIN_SPEED || n > MAX_SPEED) {
        return { field: 'speed', text: t('models.err_speed', { min: MIN_SPEED, max: MAX_SPEED }) };
      }
    }
    if (kind === 'llm' && contextWindow.trim()) {
      const n = Number(contextWindow.trim());
      if (!/^\d+$/.test(contextWindow.trim()) || n < MIN_CONTEXT_WINDOW || n > MAX_CONTEXT_WINDOW) {
        return { field: 'context_window', text: t('models.err_context_window', { min: formatCount(MIN_CONTEXT_WINDOW), max: formatCount(MAX_CONTEXT_WINDOW) }) };
      }
    }
    return null;
  }

  // Creates or updates; with `test`, the test follows (and reports for
  // itself, so no toast).
  async function save(test = false) {
    const bad = check(test);
    setProblem(bad);
    if (bad) {
      if (bad.field === 'sample_rate') setAdvancedOpen(true);
      return;
    }
    const body = {};
    if (!profile || presetId !== profile.preset) body.preset = presetId;
    if (!profile || name.trim() !== profile.name) body.name = name.trim();
    if (!profile || u !== profile.base_url) body.base_url = u;
    if (profile ? model !== profile.model : model) body.model = model;
    if (kind === 'asr' && language.trim() !== languageNow) {
      // options is replaced as a whole; keep the keys this page does not edit.
      const options = { ...(profile?.options || {}) };
      if (language.trim()) options.language = language.trim();
      else delete options.language;
      body.options = options;
    }
    if (
      kind === 'tts' &&
      (voice.trim() !== voiceNow || sampleRate.trim() !== sampleRateNow || speed.trim() !== speedNow)
    ) {
      // options is replaced as a whole; keep the keys this page does not edit.
      const options = { ...(profile?.options || {}) };
      if (voice.trim()) options.voice = voice.trim();
      else delete options.voice;
      if (sampleRate.trim()) options.sample_rate = sampleRate.trim();
      else delete options.sample_rate;
      if (speed.trim()) options.speed = speed.trim();
      else delete options.speed;
      body.options = options;
    }
    if (kind === 'llm' && (vision !== visionNow || contextWindow.trim() !== contextWindowNow)) {
      const options = { ...(profile?.options || {}) };
      options.vision = vision ? 'true' : 'false';
      if (contextWindow.trim()) options.context_window = contextWindow.trim();
      else delete options.context_window;
      body.options = options;
    }
    if (reuse) body.api_key_from = keyFrom;
    else if (key) body.api_key = key;
    if (profile && !Object.keys(body).length) {
      if (test) onSaved(profile, { keepOpen: true, test: true });
      return;
    }

    setSaving(true);
    try {
      const saved = profile
        ? await api('PUT', `/ai/profiles/${encodeURIComponent(profile.id)}`, body)
        : await api('POST', '/ai/profiles', { kind, ...body });
      if (!alive.current) return;
      // The device normalizes what it stores (trailing slashes, spaces);
      // show that, or the form would stay "changed" after a save.
      setUrl(saved.base_url);
      setName(saved.name);
      setKey('');
      setKeySource('new');
      const dropped = hasKey && !saved.has_key && !('api_key' in body) && !('api_key_from' in body);
      setKeyDropped(dropped);
      // Without its key the test would only report 401; ask for the key.
      if (dropped) {
        onSaved(saved, { keepOpen: true, test: false });
        return;
      }
      if (!test) toast(profile ? t('models.saved') : t('models.created'), 'ok');
      onSaved(saved, { keepOpen: test, test });
    } catch (e) {
      if (!alive.current || e.status === 401) return;
      const field = e.code === 'key_origin_mismatch' ? 'key' : 'form';
      setProblem({ field, text: t('models.save_failed', { reason: describeError(e) }) });
    } finally {
      if (alive.current) setSaving(false);
    }
  }

  async function clearKey() {
    const ok = await confirmDialog({
      title: t('models.clear_title'),
      body: t('models.clear_body'),
      okText: t('models.clear_confirm'),
      danger: true,
    });
    if (!ok) return;
    setSaving(true);
    try {
      const saved = await api('PUT', `/ai/profiles/${encodeURIComponent(profile.id)}`, { api_key: '' });
      if (!alive.current) return;
      setKey('');
      toast(t('models.cleared'), 'ok');
      onSaved(saved, { keepOpen: true, test: false });
    } catch (e) {
      if (e.status !== 401) toast(t('models.save_failed', { reason: describeError(e) }), 'err');
    } finally {
      if (alive.current) setSaving(false);
    }
  }

  const busy = saving || testing;
  const fieldError = (f) => (problem && problem.field === f ? problem.text : '');
  const hint = (f, text, warn = false) =>
    html`<p id=${idp + f + '-hint'} class=${fieldError(f) ? 'field-hint is-error' : warn ? 'field-hint is-warn' : 'field-hint'}>
      ${fieldError(f) || text}
    </p>`;
  const useSelect = models.length > 0 && !manualModel;
  const selectValue = models.includes(model) ? model : '';
  const info = details[model];

  // A picked model brings what the list says about it into the fields the
  // user has not changed by hand.
  function pickModel(v) {
    setModel(v);
    const d = details[v];
    if (kind !== 'llm' || !d) return;
    if (!touched.current.context_window && d.context_window >= MIN_CONTEXT_WINDOW) {
      setContextWindow(String(Math.min(d.context_window, MAX_CONTEXT_WINDOW)));
    }
    if (!touched.current.vision && typeof d.vision === 'boolean') setVision(d.vision);
  }

  const voices = (kind === 'tts' && info?.voices) || [];
  const voiceInList = voices.some((v) => v.name === voice.trim());
  // A saved voice the list does not have stays typed.
  const useVoiceSelect = voices.length > 0 && !manualVoice && (!voice.trim() || voiceInList);
  const listedLanguages = info?.languages?.length ? info.languages : COMMON_LANGUAGES;
  const useLanguageSelect = !manualLanguage && (!language.trim() || listedLanguages.includes(language.trim()));
  const modelRate = kind === 'tts' && info?.sample_rate >= MIN_SAMPLE_RATE && info.sample_rate <= MAX_SAMPLE_RATE ? info.sample_rate : 0;
  const speedValue = SPEED_RE.test(speed.trim()) ? Math.min(MAX_SPEED, Math.max(MIN_SPEED, Number(speed.trim()))) : 1;

  // A new profile starts with the platform; the rest follows once chosen.
  const presetField = html`<div class="field">
    <label class="field-label" for=${idp + 'preset'}>${t('models.preset')}</label>
    <select
      id=${idp + 'preset'}
      class="input select"
      value=${presetId}
      onChange=${(e) => pickPreset(e.currentTarget.value)}
      disabled=${busy}
      aria-describedby=${idp + 'preset-hint'}
    >
      ${presetId === '' ? html`<option value="" disabled>${t('models.pick_preset')}</option>` : null}
      ${choices.map((p) => html`<option key=${p.id} value=${p.id}>${presetLabel(p)}</option>`)}
    </select>
    ${hint('preset', t('models.preset_hint'))}
  </div>`;

  return html`<form
    class="form model-form"
    novalidate
    aria-label=${profile ? t('models.edit_title', { name: profile.name }) : t('models.new_title', { kind: t('models.kind.' + kind) })}
    onSubmit=${(e) => {
      e.preventDefault();
      save(false);
    }}
  >
    ${!profile ? html`<p class="model-form-title">${t('models.new_title', { kind: t('models.kind.' + kind) })}</p>` : null}
    ${presetField}
    ${!preset
      ? html`<div class="form-actions">
          <button type="button" class="btn btn-ghost" onClick=${onCancel}>${t('common.cancel')}</button>
        </div>`
      : html`
          <div class="field">
            <label class="field-label" for=${idp + 'name'}>${t('models.name')}</label>
            <input
              id=${idp + 'name'}
              class="input"
              type="text"
              autocomplete="off"
              value=${name}
              onInput=${(e) => {
                setName(e.currentTarget.value);
                setProblem(null);
              }}
              disabled=${busy}
              aria-invalid=${fieldError('name') ? 'true' : 'false'}
              aria-describedby=${idp + 'name-hint'}
            />
            ${hint('name', t('models.name_hint'))}
          </div>

          <div class="field">
            <label class="field-label" for=${idp + 'url'}>${t('models.url')}</label>
            <input
              id=${idp + 'url'}
              class="input mono-input"
              type="url"
              inputmode="url"
              autocomplete="off"
              autocapitalize="off"
              spellcheck=${false}
              placeholder=${urlPlaceholder(preset)}
              value=${url}
              onInput=${(e) => {
                setUrl(e.currentTarget.value);
                setProblem(null);
              }}
              disabled=${busy}
              aria-invalid=${fieldError('url') ? 'true' : 'false'}
              aria-describedby=${idp + 'url-hint'}
            />
            ${hint('url', t('models.url_hint'))}
          </div>

          <div class="field">
            <div class="field-row">
              <span class="field-label" id=${idp + 'key-label'}>${t('models.key')}</span>
              ${hasKey
                ? html`<${Badge} tone="ok"><${IconKey} size=${13} />${t('models.key_set')}</${Badge}>`
                : html`<${Badge} tone="muted">${t('models.key_unset')}</${Badge}>`}
            </div>
            ${donors.length > 0
              ? html`<div class="segmented model-key-source" role="radiogroup" aria-labelledby=${idp + 'key-label'}>
                  ${['new', 'reuse'].map(
                    (s) => html`<button
                      key=${s}
                      type="button"
                      role="radio"
                      aria-checked=${(s === 'reuse') === reuse ? 'true' : 'false'}
                      class=${(s === 'reuse') === reuse ? 'seg is-active' : 'seg'}
                      onClick=${() => {
                        setKeySource(s);
                        if (s === 'reuse' && !donors.some((d) => d.id === keyFrom)) setKeyFrom(donors[0].id);
                        setProblem(null);
                        setKeyDropped(false);
                      }}
                      disabled=${busy}
                    >
                      ${t(s === 'new' ? 'models.key_source_new' : 'models.key_source_reuse')}
                    </button>`,
                  )}
                </div>`
              : null}
            ${reuse
              ? html`<select
                  id=${idp + 'key'}
                  class="input select"
                  value=${keyFrom}
                  onChange=${(e) => setKeyFrom(e.currentTarget.value)}
                  disabled=${busy}
                  aria-labelledby=${idp + 'key-label'}
                  aria-describedby=${idp + 'key-hint'}
                >
                  ${donors.map(
                    (d) => html`<option key=${d.id} value=${d.id}>
                      ${t('models.key_from_option', { name: d.name, kind: t('models.kind.' + d.kind) })}
                    </option>`,
                  )}
                </select>`
              : html`<div class="input-group">
                  <input
                    id=${idp + 'key'}
                    class="input mono-input"
                    type="password"
                    autocomplete="new-password"
                    autocapitalize="off"
                    spellcheck=${false}
                    placeholder=${hasKey ? t('models.key_keep_ph') : t('models.key_ph')}
                    value=${key}
                    onInput=${(e) => {
                      setKey(e.currentTarget.value);
                      setProblem(null);
                      setKeyDropped(false);
                    }}
                    disabled=${busy}
                    aria-labelledby=${idp + 'key-label'}
                    aria-invalid=${fieldError('key') ? 'true' : 'false'}
                    aria-describedby=${idp + 'key-hint'}
                  />
                  ${hasKey
                    ? html`<button type="button" class="btn btn-ghost input-addon-text" onClick=${clearKey} disabled=${busy}>
                        ${t('models.clear')}
                      </button>`
                    : null}
                </div>`}
            ${hint(
              'key',
              reuse
                ? t('models.key_from_hint')
                : willDropKey
                  ? t('models.key_hint_origin')
                  : hasKey
                    ? t('models.key_hint_set')
                    : preset.key_required
                      ? t('models.key_required')
                      : t('models.key_hint'),
              willDropKey,
            )}
          </div>

          ${keyDropped && !hasKey
            ? html`<div class="alert tone-warn" role="alert"><${IconAlert} size=${18} /><span>${t('models.key_dropped')}</span></div>`
            : null}

          <div class="field">
            <label class="field-label" for=${idp + 'model'}>${t('models.model')}</label>
            <div class="input-group">
            ${useSelect
              ? html`<select
                  id=${idp + 'model'}
                  class="input select"
                  value=${selectValue}
                  onChange=${(e) => {
                    const v = e.currentTarget.value;
                    if (v === MANUAL) setManualModel(true);
                    else pickModel(v);
                    setProblem(null);
                  }}
                  disabled=${busy}
                  aria-describedby=${idp + 'model-hint'}
                >
                  ${selectValue === '' ? html`<option value="" disabled>${t('models.pick')}</option>` : null}
                  ${models.map((m) => html`<option key=${m} value=${m}>${m}</option>`)}
                  <option value=${MANUAL}>${t('models.manual_option')}</option>
                </select>`
              : html`<input
                  id=${idp + 'model'}
                  class="input mono-input"
                  type="text"
                  autocomplete="off"
                  autocapitalize="off"
                  spellcheck=${false}
                  placeholder=${t('models.model_ph.' + kind)}
                  value=${model}
                  onInput=${(e) => {
                    setModel(e.currentTarget.value);
                    setProblem(null);
                  }}
                  disabled=${busy}
                  aria-invalid=${fieldError('model') ? 'true' : 'false'}
                  aria-describedby=${idp + 'model-hint'}
                />`}
              <button
                type="button"
                class="btn btn-secondary model-fetch"
                onClick=${() => save(true)}
                disabled=${busy}
                title=${t('models.fetch_label')}
              >
                ${testing ? html`<span class="spinner" aria-hidden="true"></span>` : html`<${IconRefresh} size=${16} />`}
                <span>${t('models.fetch')}</span>
              </button>
            </div>
            <p id=${idp + 'model-hint'} class=${fieldError('model') ? 'field-hint is-error' : 'field-hint'}>
              ${fieldError('model') ||
              (useSelect
                ? t('models.model_hint_list', { count: formatCount(models.length) })
                : models.length
                  ? html`${t('models.model_hint_manual')}
                      <button type="button" class="btn btn-link btn-inline" onClick=${() => setManualModel(false)}>
                        ${t('models.back_to_list')}
                      </button>`
                  : t('models.model_hint'))}
            </p>
          </div>

          ${kind === 'asr'
            ? html`<div class="field">
                <label class="field-label" for=${idp + 'language'}>${t('models.language')}</label>
                ${useLanguageSelect
                  ? html`<select
                      id=${idp + 'language'}
                      class="input select"
                      value=${language.trim()}
                      onChange=${(e) => {
                        const v = e.currentTarget.value;
                        if (v === MANUAL) setManualLanguage(true);
                        else setLanguage(v);
                        setProblem(null);
                      }}
                      disabled=${busy}
                      aria-describedby=${idp + 'language-hint'}
                    >
                      <option value="">${t('models.language_auto')}</option>
                      ${listedLanguages.map((c) => html`<option key=${c} value=${c}>${langName(c)} (${c})</option>`)}
                      <option value=${MANUAL}>${t('models.manual_option')}</option>
                    </select>`
                  : html`<input
                      id=${idp + 'language'}
                      class="input mono-input"
                      type="text"
                      autocomplete="off"
                      autocapitalize="off"
                      spellcheck=${false}
                      maxlength="8"
                      placeholder=${t('models.language_ph')}
                      value=${language}
                      onInput=${(e) => {
                        setLanguage(e.currentTarget.value);
                        setProblem(null);
                      }}
                      disabled=${busy}
                      aria-invalid=${fieldError('language') ? 'true' : 'false'}
                      aria-describedby=${idp + 'language-hint'}
                    />`}
                <p id=${idp + 'language-hint'} class=${fieldError('language') ? 'field-hint is-error' : 'field-hint'}>
                  ${fieldError('language') ||
                  (useLanguageSelect
                    ? t('models.language_hint_list')
                    : html`${t('models.language_hint_manual')}
                        <button
                          type="button"
                          class="btn btn-link btn-inline"
                          onClick=${() => {
                            setManualLanguage(false);
                            if (!listedLanguages.includes(language.trim())) setLanguage('');
                            setProblem(null);
                          }}
                        >
                          ${t('models.back_to_list')}
                        </button>`)}
                </p>
              </div>`
            : null}

          ${kind === 'tts'
            ? html`<div class="field">
                <label class="field-label" for=${idp + 'voice'}>${t('models.voice')}</label>
                ${useVoiceSelect
                  ? html`<select
                      id=${idp + 'voice'}
                      class="input select"
                      value=${voiceInList ? voice.trim() : ''}
                      onChange=${(e) => {
                        const v = e.currentTarget.value;
                        if (v === MANUAL) setManualVoice(true);
                        else setVoice(v);
                        setProblem(null);
                      }}
                      disabled=${busy}
                      aria-invalid=${fieldError('voice') ? 'true' : 'false'}
                      aria-describedby=${idp + 'voice-hint'}
                    >
                      ${voiceInList ? null : html`<option value="" disabled>${t('models.voice_pick')}</option>`}
                      ${voices.map((v) => html`<option key=${v.name} value=${v.name}>${voiceLabel(v)}</option>`)}
                      <option value=${MANUAL}>${t('models.manual_option')}</option>
                    </select>`
                  : html`<input
                      id=${idp + 'voice'}
                      class="input mono-input"
                      type="text"
                      autocomplete="off"
                      autocapitalize="off"
                      spellcheck=${false}
                      placeholder=${t('models.voice_ph')}
                      value=${voice}
                      onInput=${(e) => {
                        setVoice(e.currentTarget.value);
                        setProblem(null);
                      }}
                      disabled=${busy}
                      aria-invalid=${fieldError('voice') ? 'true' : 'false'}
                      aria-describedby=${idp + 'voice-hint'}
                    />`}
                <p id=${idp + 'voice-hint'} class=${fieldError('voice') ? 'field-hint is-error' : 'field-hint'}>
                  ${fieldError('voice') ||
                  (useVoiceSelect
                    ? t('models.voice_hint_list', { count: formatCount(voices.length) })
                    : voices.length
                      ? html`${t('models.voice_hint_manual')}
                          <button
                            type="button"
                            class="btn btn-link btn-inline"
                            onClick=${() => {
                              setManualVoice(false);
                              if (!voiceInList) setVoice('');
                              setProblem(null);
                            }}
                          >
                            ${t('models.back_to_list')}
                          </button>`
                      : t('models.voice_hint'))}
                </p>
              </div>
              <div class="field range-field">
                <div class="field-row">
                  <label class="field-label" for=${idp + 'speed'}>${t('models.speed')}</label>
                  <span class="model-speed-value">
                    <output class=${speed.trim() ? 'range-value mono' : 'range-value'} for=${idp + 'speed'}>
                      ${speed.trim() ? speedValue.toFixed(2) + '×' : t('models.speed_default')}
                    </output>
                    <button
                      type="button"
                      class="btn btn-link btn-inline"
                      onClick=${() => {
                        setSpeed('');
                        setProblem(null);
                      }}
                      disabled=${busy || !speed.trim()}
                    >
                      ${t('models.speed_reset')}
                    </button>
                  </span>
                </div>
                <input
                  id=${idp + 'speed'}
                  class="range"
                  type="range"
                  min=${MIN_SPEED}
                  max=${MAX_SPEED}
                  step="0.05"
                  value=${speedValue}
                  style=${{ '--fill': ((speedValue - MIN_SPEED) / (MAX_SPEED - MIN_SPEED)) * 100 + '%' }}
                  onInput=${(e) => {
                    setSpeed(e.currentTarget.value);
                    setProblem(null);
                  }}
                  disabled=${busy}
                  aria-valuetext=${speed.trim() ? speedValue.toFixed(2) + '×' : t('models.speed_default')}
                  aria-describedby=${idp + 'speed-hint'}
                />
                ${hint('speed', t('models.speed_hint', { min: MIN_SPEED, max: MAX_SPEED }))}
              </div>
              <details class="model-advanced" open=${advancedOpen} onToggle=${(e) => setAdvancedOpen(e.currentTarget.open)}>
                <summary>${t('models.advanced')}</summary>
                <div class="field">
                  <label class="field-label" for=${idp + 'sample_rate'}>${t('models.sample_rate')}</label>
                  <input
                    id=${idp + 'sample_rate'}
                    class="input mono-input"
                    type="number"
                    inputmode="numeric"
                    min=${MIN_SAMPLE_RATE}
                    max=${MAX_SAMPLE_RATE}
                    step="1"
                    placeholder=${String(modelRate || DEFAULT_SAMPLE_RATE)}
                    value=${sampleRate}
                    onInput=${(e) => {
                      setSampleRate(e.currentTarget.value);
                      setProblem(null);
                    }}
                    disabled=${busy}
                    aria-invalid=${fieldError('sample_rate') ? 'true' : 'false'}
                    aria-describedby=${idp + 'sample_rate-hint'}
                  />
                  <p id=${idp + 'sample_rate-hint'} class=${fieldError('sample_rate') ? 'field-hint is-error' : 'field-hint'}>
                    ${fieldError('sample_rate') ||
                    t('models.sample_rate_hint', { default: formatCount(DEFAULT_SAMPLE_RATE) }) +
                      (modelRate ? (getLang() === 'en' ? ' ' : '') + t('models.sample_rate_model', { rate: formatCount(modelRate) }) : '')}
                  </p>
                </div>
              </details>`
            : null}

          ${kind === 'llm'
            ? html`<div class="field">
                <label class="field-row field-label" for=${idp + 'vision'}>
                  <span>${t('models.vision')}</span>
                  <input
                    id=${idp + 'vision'}
                    type="checkbox"
                    checked=${vision}
                    onChange=${(e) => {
                      setVision(e.currentTarget.checked);
                      touched.current.vision = true;
                      setProblem(null);
                    }}
                    disabled=${busy}
                    aria-describedby=${idp + 'vision-hint'}
                  />
                </label>
                ${hint('vision', t('models.vision_hint'))}
              </div>
              <div class="field">
                <label class="field-label" for=${idp + 'context_window'}>${t('models.context_window')}</label>
                <input
                  id=${idp + 'context_window'}
                  class="input mono-input"
                  type="number"
                  inputmode="numeric"
                  min=${MIN_CONTEXT_WINDOW}
                  max=${MAX_CONTEXT_WINDOW}
                  step="1"
                  placeholder=${String(DEFAULT_CONTEXT_WINDOW)}
                  value=${contextWindow}
                  onInput=${(e) => {
                    setContextWindow(e.currentTarget.value);
                    touched.current.context_window = true;
                    setProblem(null);
                  }}
                  disabled=${busy}
                  aria-invalid=${fieldError('context_window') ? 'true' : 'false'}
                  aria-describedby=${idp + 'context_window-hint'}
                />
                ${hint('context_window', t('models.context_window_hint', { default: formatCount(DEFAULT_CONTEXT_WINDOW) }))}
              </div>`
            : null}

          ${problem?.field === 'form'
            ? html`<div class="alert tone-err" role="alert"><${IconAlert} size=${18} /><span>${problem.text}</span></div>`
            : null}

          <div class="form-actions">
            <button type="button" class="btn btn-ghost" onClick=${onCancel} disabled=${saving}>${t('common.cancel')}</button>
            <button type="button" class="btn btn-secondary" onClick=${() => save(true)} disabled=${busy}>
              ${testing ? html`<span class="spinner" aria-hidden="true"></span>` : html`<${IconRefresh} size=${16} />`}
              ${dirty ? t('models.save_and_test') : t('models.test')}
            </button>
            <button type="submit" class="btn btn-primary" disabled=${busy || !dirty}>
              ${saving ? html`<span class="spinner" aria-hidden="true"></span>` : null}${profile ? t('models.save') : t('models.create')}
            </button>
          </div>
        `}
  </form>`;
}
