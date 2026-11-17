// MCP servers (spec 4.3 of docs/superpowers/specs/2026-11-15-llm-tool-providers-design.md):
// the servers whose tools the Agent finds with tools.find, on their own page
// (#/mcp) under the Models tab. Lists, creates, edits, enables, tests and
// deletes them through /mcp/servers.
//
// Header values are write-only, as the profiles' API keys are (models.js):
// the device only says whether a header has one (has_value). A value is
// never shown, pre-filled, logged or put in localStorage; a stored one
// shows as "set" and its input stays empty with the placeholder
// "unchanged". An entry is sent without `value` to keep what is stored
// (decision 11 of the spec), with one only when something was typed, and
// every typed value is dropped from the form once a save succeeds. Removing
// a header is how its value is cleared: the device refuses an empty value.
//
// A stored value belongs to the server's origin: an update to a URL on
// another origin drops every value not typed again. The form marks those
// rows before saving, and after the save the row names the headers the
// answer came back without.
//
// The test (POST /mcp/servers/{id}/test) holds the request until the
// device's probe ends, up to about 30 s, so the button stays disabled and
// the row says so meanwhile.

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError, errorDetail } from '../api.js';
import { t } from '../i18n/index.js';
import { Card } from '../components/card.js';
import { Badge } from '../components/badge.js';
import { confirmDialog } from '../components/dialog.js';
import { toast } from '../components/toast.js';
import { IconAlert, IconCheck, IconKey, IconLink, IconPlus, IconRefresh, IconTrash } from '../icons.js';
import { formatCount } from '../util.js';

/**
 * @typedef {Object} McpHeader  one of McpServer "headers"
 * @property {string} name
 * @property {boolean} has_value
 *
 * @typedef {Object} McpServer  GET /mcp/servers "servers", POST/PUT result
 * @property {string} id
 * @property {string} name
 * @property {string} url
 * @property {boolean} enabled
 * @property {number} timeout_ms
 * @property {McpHeader[]} headers
 *
 * @typedef {Object} McpProbeResult  POST /mcp/servers/{id}/test
 * @property {boolean} ok
 * @property {null|'dns'|'connect'|'tls'|'timeout'|'http'|'unsupported_protocol'|'bad_response'} error
 * @property {number} latency_ms
 * @property {string} protocol
 * @property {string[]} tools
 */

// McpServerService's limits (wisp/core/mcp_servers.hpp). The server count
// comes from the list's "max".
const NAME_MAX_BYTES = 32;
const URL_MAX_BYTES = 2048;
const MAX_HEADERS = 8;
const HEADER_NAME_MAX_BYTES = 64;
const HEADER_VALUE_MAX_BYTES = 1024;
const MIN_TIMEOUT_S = 1;
const MAX_TIMEOUT_S = 120;
const DEFAULT_TIMEOUT_S = 60;
// The client sets these itself (mcp_servers.cpp kReservedHeaders).
const RESERVED_HEADERS = ['accept', 'content-type', 'mcp-session-id', 'mcp-protocol-version', 'host', 'content-length', 'transfer-encoding', 'connection'];
// An RFC 9110 token, as core::http_header_syntax_ok() requires.
const TOKEN_RE = /^[!#$%&'*+\-.^_`|~0-9A-Za-z]+$/;
// Control bytes and DEL, which the device refuses in a value.
const CONTROL_RE = /[\u0000-\u001f\u007f]/;
const FAIL_CATEGORIES = ['dns', 'connect', 'tls', 'timeout', 'http', 'unsupported_protocol', 'bad_response'];

const bytes = (s) => new TextEncoder().encode(s).length;
const lower = (s) => s.toLowerCase();

function validUrl(s) {
  return /^https?:\/\/[^\s/?#]+/i.test(s) && !/[\s\u0000-\u001f\u007f]/.test(s);
}

// The URL's origin as the device compares it; '' when it does not parse.
function originOf(s) {
  try {
    return new URL(s).origin;
  } catch {
    return '';
  }
}

function hasUserinfo(s) {
  const m = /^https?:\/\/([^/?#]*)/i.exec(s);
  return !!m && m[1].includes('@');
}

let nextRowKey = 1;

/** The form's header rows for a server: names only, never values. */
function rowsOf(server) {
  return (server?.headers || []).map((h) => ({ key: nextRowKey++, name: h.name, stored: h.has_value ? h.name : '', value: '' }));
}

export function McpServersPage() {
  const [list, setList] = useState(/** @type {{servers: McpServer[], max: number}|null} */ (null));
  const [loadError, setLoadError] = useState(null);
  // null, 'new' or the id of the server being edited.
  const [editing, setEditing] = useState(/** @type {string|null} */ (null));
  const [busy, setBusy] = useState(''); // id with an enable/delete in flight
  // Per server id: {testing} | {result} | {error} of its last test.
  const [tests, setTests] = useState({});
  // Per server id: names of the headers its last save dropped.
  const [dropped, setDropped] = useState({});
  const alive = useRef(true);

  async function load() {
    try {
      const r = await api('GET', '/mcp/servers');
      if (!alive.current) return;
      setList({ servers: r.servers || [], max: r.max || 0 });
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

  // A probe still in flight keeps its entry, so the Test button stays
  // disabled and the answer lands on the right server.
  function forget(id) {
    setTests((m) => (m[id] && m[id].testing ? m : { ...m, [id]: undefined }));
  }

  async function runTest(s) {
    setTests((m) => ({ ...m, [s.id]: { testing: true } }));
    try {
      /** @type {McpProbeResult} */
      const r = await api('POST', `/mcp/servers/${encodeURIComponent(s.id)}/test`);
      if (alive.current) setTests((m) => ({ ...m, [s.id]: { result: r } }));
    } catch (e) {
      if (alive.current) setTests((m) => ({ ...m, [s.id]: e.status === 401 ? undefined : { error: e } }));
    }
  }

  async function setEnabled(s, enabled) {
    setBusy(s.id);
    try {
      await api('PUT', `/mcp/servers/${encodeURIComponent(s.id)}`, { enabled });
      if (!alive.current) return;
      toast(t(enabled ? 'mcp.enabled_on' : 'mcp.enabled_off', { name: s.name }), 'ok');
    } catch (e) {
      if (e.status !== 401) toast(describeError(e), 'err');
    } finally {
      if (alive.current) setBusy('');
      load();
    }
  }

  async function remove(s) {
    const ok = await confirmDialog({
      title: t('mcp.delete_title', { name: s.name }),
      body: t('mcp.delete_body'),
      okText: t('mcp.delete_confirm'),
      danger: true,
    });
    if (!ok) return;
    setBusy(s.id);
    try {
      await api('DELETE', `/mcp/servers/${encodeURIComponent(s.id)}`);
      if (!alive.current) return;
      if (editing === s.id) setEditing(null);
      forget(s.id);
      setDropped((m) => ({ ...m, [s.id]: undefined }));
      toast(t('mcp.deleted'), 'ok');
    } catch (e) {
      if (e.status !== 401) toast(describeError(e), 'err');
    } finally {
      if (alive.current) setBusy('');
      load();
    }
  }

  async function onSaved(saved, droppedNames) {
    forget(saved.id);
    setDropped((m) => ({ ...m, [saved.id]: droppedNames.length ? droppedNames : undefined }));
    setEditing(null);
    await load();
  }

  const loading = !list && !loadError;
  const servers = list ? list.servers : [];
  const max = list ? list.max : 0;
  const full = !!list && servers.length >= max;
  const form = (server) => html`<${ServerForm}
    server=${server}
    max=${max}
    onSaved=${onSaved}
    onCancel=${() => setEditing(null)}
  />`;

  let body;
  if (loadError && !list) {
    const unavailable = loadError.code === 'unavailable';
    body = html`<div class="alert tone-err" role="alert">
      <${IconAlert} size=${18} />
      <div class="alert-body">
        <strong>${unavailable ? t('mcp.unavailable') : t('mcp.load_failed')}</strong>
        ${unavailable ? null : html`<p>${describeError(loadError)}</p>`}
        ${!unavailable && errorDetail(loadError) ? html`<p class="err-detail mono">${errorDetail(loadError)}</p>` : null}
        ${unavailable ? null : html`<p><button type="button" class="btn btn-secondary btn-sm" onClick=${load}>${t('common.retry')}</button></p>`}
      </div>
    </div>`;
  } else if (loading) {
    body = html`<div aria-busy="true" aria-label=${t('common.loading')}>
      <div class="skel skel-line"></div><br /><div class="skel skel-line"></div>
    </div>`;
  } else {
    body = html`
      ${full ? html`<p class="inline-note tone-warn"><${IconAlert} size=${16} /><span>${t('mcp.full', { max })}</span></p>` : null}
      ${editing === 'new' ? html`<div class="model-new">${form(null)}</div>` : null}
      ${servers.length === 0 && editing !== 'new'
        ? html`<div class="empty model-empty">
            <span class="empty-icon"><${IconLink} size=${22} /></span>
            <p class="empty-title">${t('mcp.empty_title')}</p>
            <p class="muted">${t('mcp.empty_hint')}</p>
          </div>`
        : null}
      ${servers.length
        ? html`<ul class="rows model-rows">
            ${servers.map(
              (s) => html`<${ServerRow}
                key=${s.id}
                server=${s}
                busy=${busy === s.id}
                test=${tests[s.id]}
                dropped=${dropped[s.id]}
                editing=${editing === s.id}
                form=${editing === s.id ? form(s) : null}
                onEnable=${(on) => setEnabled(s, on)}
                onTest=${() => runTest(s)}
                onEdit=${() => setEditing(editing === s.id ? null : s.id)}
                onDelete=${() => remove(s)}
              />`,
            )}
          </ul>`
        : null}
    `;
  }

  return html`<div class="page">
    <header class="page-head mcp-head">
      <div>
        <h1 class="page-title" tabindex="-1">${t('nav.mcp')}</h1>
        <p class="page-sub">${t('mcp.desc')}</p>
      </div>
      <div class="page-head-actions">
        ${list
          ? html`<span class="muted mcp-count">${t('mcp.count', { count: formatCount(servers.length), max: formatCount(max) })}</span>`
          : null}
        <button
          type="button"
          class="btn btn-primary btn-sm"
          onClick=${() => setEditing('new')}
          disabled=${!list || full || editing === 'new'}
        >
          <${IconPlus} size=${16} />${t('mcp.add')}
        </button>
      </div>
    </header>
    <${Card}>${body}</${Card}>
  </div>`;
}

function ServerRow({ server: s, busy, test, dropped, editing, form, onEnable, onTest, onEdit, onDelete }) {
  const testing = !!test?.testing;
  const switchId = 'mcp-on-' + s.id;
  return html`<li class="row row-saved model-row mcp-row">
    <div class="row-line">
      <div class="row-main">
        <span class="row-title">
          ${s.name} <span class="mono mcp-id">${s.id}</span>${s.enabled ? null : html`<${Badge} tone="muted">${t('mcp.disabled_badge')}</${Badge}>`}
        </span>
        <span class="row-sub mono">${s.url}</span>
        <span class="row-sub">
          ${s.headers.length
            ? s.headers.map(
                (h, i) => html`${i ? ' · ' : ''}<span class="mono">${h.name}</span>${' '}${h.has_value
                    ? html`<span class="text-ok"><${IconKey} size=${12} /> ${t('mcp.header_set')}</span>`
                    : t('mcp.header_unset')}`,
              )
            : t('mcp.headers_none')}
        </span>
      </div>
      <div class="row-actions">
        <label class="mcp-switch" for=${switchId}>
          <input
            id=${switchId}
            type="checkbox"
            role="switch"
            checked=${s.enabled}
            onChange=${(e) => onEnable(e.currentTarget.checked)}
            disabled=${busy}
            aria-label=${t('mcp.enabled_label', { name: s.name })}
          />
          <span>${t('mcp.enabled')}</span>
        </label>
        <button type="button" class="btn btn-ghost btn-sm" onClick=${onTest} disabled=${testing || busy}>
          ${testing ? html`<span class="spinner" aria-hidden="true"></span>` : html`<${IconRefresh} size=${15} />`}${t('mcp.test')}
        </button>
        <button type="button" class="btn btn-ghost btn-sm" onClick=${onEdit} aria-expanded=${editing ? 'true' : 'false'} disabled=${busy}>
          ${t('mcp.edit')}
        </button>
        <button
          type="button"
          class="btn btn-icon btn-ghost btn-sm"
          onClick=${onDelete}
          disabled=${busy}
          aria-label=${t('mcp.delete_label', { name: s.name })}
          title=${t('mcp.delete_confirm')}
        >
          <${IconTrash} size=${17} />
        </button>
      </div>
    </div>
    ${dropped
      ? html`<div class="alert tone-warn model-result" role="alert">
          <${IconAlert} size=${18} /><span>${t('mcp.headers_dropped', { names: dropped.join(', ') })}</span>
        </div>`
      : null}
    ${test ? html`<${TestOutcome} test=${test} />` : null}
    ${form}
  </li>`;
}

function TestOutcome({ test }) {
  if (test.testing) {
    return html`<p class="muted model-testing" role="status">${t('mcp.testing')}</p>`;
  }
  if (test.error) {
    return html`<div class="alert tone-err model-result" role="alert">
      <${IconAlert} size=${18} />
      <span>${test.error.code === 'unavailable' ? t('mcp.test_unavailable') : describeError(test.error)}</span>
    </div>`;
  }
  const r = test.result;
  if (!r) return null;
  const latency = html`<span class="mono">${formatCount(r.latency_ms)} ms</span>`;
  if (r.ok) {
    const tools = r.tools || [];
    return html`<div class="test-result tone-ok model-result" role="status">
      <div class="test-head"><${IconCheck} size=${18} /><strong>${t('mcp.ok')}</strong></div>
      <dl class="kv">
        <div><dt>${t('mcp.protocol')}</dt><dd class="mono">${r.protocol || '—'}</dd></div>
        <div><dt>${t('mcp.latency')}</dt><dd>${latency}</dd></div>
        <div><dt>${t('mcp.tools_found')}</dt><dd class="mono">${formatCount(tools.length)}</dd></div>
      </dl>
      ${tools.length
        ? html`<p class="mono mcp-tools">${tools.join(', ')}</p>`
        : html`<p>${t('mcp.no_tools')}</p>`}
    </div>`;
  }
  const known = FAIL_CATEGORIES.includes(r.error);
  return html`<div class="test-result tone-err model-result" role="alert">
    <div class="test-head"><${IconAlert} size=${18} /><strong>${t('mcp.failed')}</strong></div>
    <p>${t(known ? 'mcp.fail_' + r.error : 'mcp.fail_other')}</p>
    <dl class="kv">
      <div><dt>${t('mcp.category')}</dt><dd class="mono">${r.error || '—'}</dd></div>
      <div><dt>${t('mcp.latency')}</dt><dd>${latency}</dd></div>
    </dl>
  </div>`;
}

/**
 * Creates (server null) or edits one server. `onSaved(server, dropped)`
 * runs after a successful save, with the names of the headers sent to keep
 * their stored value that came back without one.
 */
function ServerForm({ server, max, onSaved, onCancel }) {
  const [name, setName] = useState(server ? server.name : '');
  const [url, setUrl] = useState(server ? server.url : '');
  const [enabled, setEnabled] = useState(server ? server.enabled : true);
  // What the form opened with: the row's switch can change the server
  // meanwhile, and only a change made here is the form's to send.
  const enabledAtOpen = useRef(enabled).current;
  const timeoutNow = server ? String(Math.round(server.timeout_ms / 1000)) : String(DEFAULT_TIMEOUT_S);
  const [timeout, setTimeoutS] = useState(timeoutNow);
  const [rows, setRows] = useState(() => rowsOf(server));
  const [problem, setProblem] = useState(null); // {field, text}
  const [saving, setSaving] = useState(false);
  const alive = useRef(true);
  useEffect(() => () => (alive.current = false), []);

  const idp = 'mcp-' + (server ? server.id : 'new') + '-';
  const u = url.trim();
  const movesOrigin = !!server && u !== server.url && originOf(u) !== originOf(server.url);
  // A row keeps its stored value when it is sent without one, under the
  // name it was stored with, to the same origin.
  const sameName = (r) => !!r.stored && lower(r.name.trim()) === lower(r.stored);
  const keeps = (r) => sameName(r) && !r.value;
  const headersNow = server ? server.headers.map((h) => h.name).join('\n') : '';
  const headersDirty = rows.map((r) => r.name.trim()).join('\n') !== headersNow || rows.some((r) => r.value !== '');
  const dirty =
    !server ||
    name.trim() !== server.name ||
    u !== server.url ||
    enabled !== enabledAtOpen ||
    timeout.trim() !== timeoutNow ||
    headersDirty;

  function edit(key, patch) {
    setRows((rs) => rs.map((r) => (r.key === key ? { ...r, ...patch } : r)));
    setProblem(null);
  }

  function check() {
    const n = name.trim();
    if (!n || bytes(n) > NAME_MAX_BYTES) return { field: 'name', text: t('mcp.err_name', { max: NAME_MAX_BYTES }) };
    if (!u) return { field: 'url', text: t('mcp.err_url_empty') };
    if (!validUrl(u) || bytes(u) > URL_MAX_BYTES) return { field: 'url', text: t('mcp.err_url', { max: formatCount(URL_MAX_BYTES) }) };
    if (hasUserinfo(u)) return { field: 'url', text: t('mcp.err_url_userinfo') };
    const s = timeout.trim();
    if (!/^\d+$/.test(s) || Number(s) < MIN_TIMEOUT_S || Number(s) > MAX_TIMEOUT_S) {
      return { field: 'timeout', text: t('mcp.err_timeout', { min: MIN_TIMEOUT_S, max: MAX_TIMEOUT_S }) };
    }
    const seen = new Set();
    for (const r of rows) {
      const hn = r.name.trim();
      const field = 'header-' + r.key;
      if (!TOKEN_RE.test(hn) || bytes(hn) > HEADER_NAME_MAX_BYTES) {
        return { field, text: t('mcp.err_header_name', { max: HEADER_NAME_MAX_BYTES }) };
      }
      if (RESERVED_HEADERS.includes(lower(hn))) return { field, text: t('mcp.err_header_reserved', { name: hn }) };
      if (seen.has(lower(hn))) return { field, text: t('mcp.err_header_dup', { name: hn }) };
      seen.add(lower(hn));
      if (r.value) {
        if (bytes(r.value) > HEADER_VALUE_MAX_BYTES || CONTROL_RE.test(r.value)) {
          return { field, text: t('mcp.err_header_value_bad', { max: formatCount(HEADER_VALUE_MAX_BYTES) }) };
        }
      } else if (!keeps(r)) {
        return { field, text: t('mcp.err_header_value', { name: hn }) };
      }
    }
    return null;
  }

  async function save() {
    const bad = check();
    setProblem(bad);
    if (bad) return;
    const body = {};
    if (!server || name.trim() !== server.name) body.name = name.trim();
    if (!server || u !== server.url) body.url = u;
    if (!server || enabled !== enabledAtOpen) body.enabled = enabled;
    if (!server || timeout.trim() !== timeoutNow) body.timeout_ms = Number(timeout.trim()) * 1000;
    // headers replaces the whole list. It goes with any URL change too, so
    // which entries were meant to keep their value is known on the way back.
    if (!server || headersDirty || 'url' in body) {
      body.headers = rows.map((r) => (r.value ? { name: r.name.trim(), value: r.value } : { name: r.name.trim() }));
    }
    if (server && !Object.keys(body).length) {
      onCancel();
      return;
    }
    const kept = server ? rows.filter((r) => !r.value).map((r) => r.name.trim()) : [];

    setSaving(true);
    try {
      /** @type {McpServer} */
      const saved = server
        ? await api('PUT', `/mcp/servers/${encodeURIComponent(server.id)}`, body)
        : await api('POST', '/mcp/servers', body);
      if (!alive.current) return;
      // No typed value outlives the request that carried it.
      setRows(rowsOf(saved));
      const lost = kept.filter((hn) => !saved.headers.some((h) => h.has_value && lower(h.name) === lower(hn)));
      if (lost.length) toast(t('mcp.headers_dropped', { names: lost.join(', ') }), 'warn');
      else toast(server ? t('mcp.saved') : t('mcp.created'), 'ok');
      onSaved(saved, lost);
    } catch (e) {
      if (!alive.current || e.status === 401) return;
      const reason = e.code === 'limit_reached' ? t('mcp.limit', { max }) : describeError(e);
      setProblem({ field: 'form', text: t('mcp.save_failed', { reason }), detail: e.code === 'invalid_argument' ? e.message : '' });
    } finally {
      if (alive.current) setSaving(false);
    }
  }

  const fieldError = (f) => (problem && problem.field === f ? problem.text : '');
  const hint = (f, text, warn = false) =>
    html`<p id=${idp + f + '-hint'} class=${fieldError(f) ? 'field-hint is-error' : warn ? 'field-hint is-warn' : 'field-hint'}>
      ${fieldError(f) || text}
    </p>`;
  const title = server ? t('mcp.edit_title', { name: server.name }) : t('mcp.new_title');

  return html`<form
    class="form model-form"
    novalidate
    aria-label=${title}
    onSubmit=${(e) => {
      e.preventDefault();
      save();
    }}
  >
    ${!server ? html`<p class="model-form-title">${title}</p>` : null}
    <div class="field">
      <label class="field-label" for=${idp + 'name'}>${t('mcp.name')}</label>
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
        disabled=${saving}
        aria-invalid=${fieldError('name') ? 'true' : 'false'}
        aria-describedby=${idp + 'name-hint'}
      />
      ${hint('name', t('mcp.name_hint'))}
    </div>

    <div class="field">
      <label class="field-label" for=${idp + 'url'}>${t('mcp.url')}</label>
      <input
        id=${idp + 'url'}
        class="input mono-input"
        type="url"
        inputmode="url"
        autocomplete="off"
        autocapitalize="off"
        spellcheck=${false}
        placeholder="https://example.com/mcp"
        value=${url}
        onInput=${(e) => {
          setUrl(e.currentTarget.value);
          setProblem(null);
        }}
        disabled=${saving}
        aria-invalid=${fieldError('url') ? 'true' : 'false'}
        aria-describedby=${idp + 'url-hint'}
      />
      ${hint('url', t('mcp.url_hint'))}
    </div>

    <div class="field">
      <label class="field-row field-label" for=${idp + 'enabled'}>
        <span>${t('mcp.enabled')}</span>
        <input
          id=${idp + 'enabled'}
          type="checkbox"
          role="switch"
          checked=${enabled}
          onChange=${(e) => {
            setEnabled(e.currentTarget.checked);
            setProblem(null);
          }}
          disabled=${saving}
        />
      </label>
    </div>

    <div class="field">
      <label class="field-label" for=${idp + 'timeout'}>${t('mcp.timeout')}</label>
      <input
        id=${idp + 'timeout'}
        class="input mono-input mcp-timeout"
        type="number"
        inputmode="numeric"
        min=${MIN_TIMEOUT_S}
        max=${MAX_TIMEOUT_S}
        step="1"
        placeholder=${String(DEFAULT_TIMEOUT_S)}
        value=${timeout}
        onInput=${(e) => {
          setTimeoutS(e.currentTarget.value);
          setProblem(null);
        }}
        disabled=${saving}
        aria-invalid=${fieldError('timeout') ? 'true' : 'false'}
        aria-describedby=${idp + 'timeout-hint'}
      />
      ${hint('timeout', t('mcp.timeout_hint', { min: MIN_TIMEOUT_S, max: MAX_TIMEOUT_S, default: DEFAULT_TIMEOUT_S }))}
    </div>

    <fieldset class="field mcp-headers">
      <legend class="field-label">${t('mcp.headers')}</legend>
      <p class="field-hint">${t('mcp.headers_hint')}</p>
      ${rows.map((r) => {
        const clears = movesOrigin && keeps(r);
        const err = fieldError('header-' + r.key);
        return html`<div key=${r.key} class="mcp-header-row">
          <div class="input-group">
            <input
              class="input mono-input"
              type="text"
              autocomplete="off"
              autocapitalize="off"
              spellcheck=${false}
              placeholder=${t('mcp.header_name_ph')}
              maxlength=${HEADER_NAME_MAX_BYTES}
              value=${r.name}
              onInput=${(e) => edit(r.key, { name: e.currentTarget.value })}
              disabled=${saving}
              aria-label=${t('mcp.header_name')}
              aria-invalid=${err ? 'true' : 'false'}
            />
            <input
              class="input mono-input"
              type="password"
              autocomplete="new-password"
              autocapitalize="off"
              spellcheck=${false}
              placeholder=${sameName(r) ? t('mcp.header_keep_ph') : t('mcp.header_value_ph')}
              value=${r.value}
              onInput=${(e) => edit(r.key, { value: e.currentTarget.value })}
              disabled=${saving}
              aria-label=${t('mcp.header_value')}
              aria-invalid=${err ? 'true' : 'false'}
            />
            <button
              type="button"
              class="btn btn-ghost input-addon-text"
              onClick=${() => {
                setRows((rs) => rs.filter((x) => x.key !== r.key));
                setProblem(null);
              }}
              disabled=${saving}
            >
              ${t('mcp.header_remove')}
            </button>
          </div>
          ${sameName(r) && !r.value
            ? html`<p class=${err ? 'field-hint is-error' : clears ? 'field-hint is-warn' : 'field-hint'}>
                ${err || (clears ? t('mcp.header_will_clear') : html`<span class="text-ok"><${IconKey} size=${12} /> ${t('mcp.header_set')}</span>`)}
              </p>`
            : err
              ? html`<p class="field-hint is-error">${err}</p>`
              : null}
        </div>`;
      })}
      <div>
        <button
          type="button"
          class="btn btn-secondary btn-sm"
          onClick=${() => {
            setRows((rs) => [...rs, { key: nextRowKey++, name: '', stored: '', value: '' }]);
            setProblem(null);
          }}
          disabled=${saving || rows.length >= MAX_HEADERS}
        >
          <${IconPlus} size=${16} />${t('mcp.header_add')}
        </button>
        ${rows.length >= MAX_HEADERS ? html`<span class="field-hint mcp-headers-full">${t('mcp.headers_full', { max: MAX_HEADERS })}</span>` : null}
      </div>
    </fieldset>

    ${problem?.field === 'form'
      ? html`<div class="alert tone-err" role="alert">
          <${IconAlert} size=${18} />
          <div class="alert-body">
            <span>${problem.text}</span>
            ${problem.detail ? html`<p class="err-detail mono">${problem.detail}</p>` : null}
          </div>
        </div>`
      : null}

    <div class="form-actions">
      <button type="button" class="btn btn-ghost" onClick=${onCancel} disabled=${saving}>${t('common.cancel')}</button>
      <button type="submit" class="btn btn-primary" disabled=${saving || !dirty}>
        ${saving ? html`<span class="spinner" aria-hidden="true"></span>` : null}${server ? t('mcp.save') : t('mcp.create')}
      </button>
    </div>
  </form>`;
}
