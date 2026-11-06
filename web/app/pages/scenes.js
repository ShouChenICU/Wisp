// Scenes (scene-runtime-core spec 7.2): the installed scene packages, a
// zip upload, and per scene open/close/reload, export and delete. A row
// expands to show the hosts the scene may call, its last error and its
// files (scene-device spec 7.2).
//
// Close always ends the scene (POST /exit), whether it is in front or in
// the background (scene-device spec 3.7); the device's own back gesture is
// what sends a background-capable scene to the background.
//
// Files: where they live, how much of the quota they use, and per file a
// download link and a delete that asks first. A scene that declares
// "files": "sd" is marked as needing an SD card while no card is in, or
// while its files cannot be reached. Nothing here deletes a file unless
// the user asks for that one.
//
// The device validates and compiles an upload before writing anything; a
// rejected package comes back as invalid_scene with the compiler's
// "file:line: message". Deleting needs a yes on the device's screen, so the
// request only answers "pending"; the outcome, like every open, close,
// crash and install, arrives as a scene.state event, which reloads the list.

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError, errorDetail, exportUrl, fileUrl, upload } from '../api.js';
import { locale, t } from '../i18n/index.js';
import { subscribe } from '../ws.js';
import { formatBytes } from '../util.js';
import { Badge } from '../components/badge.js';
import { Card } from '../components/card.js';
import { confirmDialog } from '../components/dialog.js';
import { toast } from '../components/toast.js';
import {
  IconAlert,
  IconArrowDown,
  IconChevronDown,
  IconGlobe,
  IconLogs,
  IconPackage,
  IconPlay,
  IconRefresh,
  IconStorage,
  IconStop,
  IconTrash,
  IconUpload,
} from '../icons.js';

/**
 * @typedef {Object} SceneRow  one entry of GET /scenes
 * @property {string} id
 * @property {string} name
 * @property {string} version
 * @property {number} code_bytes
 * @property {number} data_bytes
 * @property {'stopped'|'running'|'background'|'error'} state
 * @property {string[]} hosts   permissions.http from scene.json
 * @property {string|null} error  the last error while state is "error"
 * @property {'auto'|'sd'|'int'} files  where scene.json wants its files
 *
 * @typedef {Object} SceneFiles  GET /scenes/{id}/files
 * @property {'sd'|'int'|null} location  null until the scene first writes
 * @property {'auto'|'sd'|'int'} want
 * @property {number} quota
 * @property {number} used
 * @property {boolean} truncated  more files than the list holds
 * @property {{name: string, size: number, mtime: number}[]} files
 */

const STATE_TONE = { running: 'ok', background: 'accent', error: 'err', stopped: 'muted' };
// Clocks before 2020 mean the device had no real time when it wrote.
const MIN_REAL_UNIX = 1577836800;
// A scene zip is at most this large (the device answers 413 beyond it).
const MAX_UPLOAD = 576 * 1024;

/** A sentence for a failed scene request, preferring this page's wording. */
function sceneError(e) {
  if (e?.code === 'unavailable') return t('scenes.unavailable');
  if (e?.code === 'dev_mode') return t('scenes.dev_mode');
  return describeError(e);
}

export function ScenesPage() {
  const [scenes, setScenes] = useState(/** @type {SceneRow[]|null} */ (null));
  const [sdCard, setSdCard] = useState(true);
  const [error, setError] = useState(null);
  const [loading, setLoading] = useState(false);
  const [open, setOpen] = useState(() => new Set());
  const alive = useRef(true);
  const reloadTimer = useRef(0);

  async function load() {
    setLoading(true);
    try {
      const r = await api('GET', '/scenes');
      if (!alive.current) return;
      setScenes(Array.isArray(r?.scenes) ? r.scenes : []);
      setSdCard(r?.sd_card !== false);
      setError(null);
    } catch (e) {
      if (alive.current && e.status !== 401) setError(e);
    } finally {
      if (alive.current) setLoading(false);
    }
  }

  useEffect(() => {
    alive.current = true;
    load();
    // A burst of state changes (a reload is stopped + running) costs one
    // request.
    const unsubscribe = subscribe(['events'], (msg) => {
      if (msg.t !== 'event' || msg.topic !== 'scene.state') return;
      clearTimeout(reloadTimer.current);
      reloadTimer.current = setTimeout(load, 300);
    });
    return () => {
      alive.current = false;
      clearTimeout(reloadTimer.current);
      unsubscribe();
    };
  }, []);

  function toggle(id) {
    setOpen((prev) => {
      const next = new Set(prev);
      if (next.has(id)) next.delete(id);
      else next.add(id);
      return next;
    });
  }

  const list = scenes || [];
  return html`<div class="page">
    <header class="page-head">
      <div>
        <h1 class="page-title" tabindex="-1">${t('scenes.title')}</h1>
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

    <${UploadCard} onInstalled=${load} />

    ${error && !scenes
      ? html`<${Card} title=${t('scenes.title')} icon=${IconAlert} tone="err">
          <p class="muted">${sceneError(error)}</p>
          ${errorDetail(error) ? html`<p class="err-detail mono">${errorDetail(error)}</p>` : null}
          <button type="button" class="btn btn-secondary" onClick=${load}>${t('common.retry')}</button>
        </${Card}>`
      : !scenes
        ? html`<div class="card list-card" aria-busy="true" aria-label=${t('common.loading')}>
            ${[0, 1, 2].map(
              (i) => html`<div key=${i} class="dev-row skeleton-row">
                <div class="skel skel-icon"></div>
                <div class="skel skel-line"></div>
              </div>`,
            )}
          </div>`
        : list.length === 0
          ? html`<div class="empty"><p class="muted">${t('scenes.empty')}</p></div>`
          : html`<ul class="card list-card dev-list">
              ${list.map(
                (s) =>
                  html`<${SceneItem}
                    key=${s.id}
                    scene=${s}
                    sdCard=${sdCard}
                    open=${open.has(s.id)}
                    onToggle=${toggle}
                    onChanged=${load}
                  />`,
              )}
            </ul>`}
  </div>`;
}

function UploadCard({ onInstalled }) {
  const [file, setFile] = useState(/** @type {File|null} */ (null));
  // null | {phase: 'uploading', pct} | {phase: 'failed', reason, detail?}
  const [job, setJob] = useState(null);
  const input = useRef(null);
  const abort = useRef(null);

  useEffect(() => () => abort.current?.(), []);

  function pick(e) {
    setJob(null);
    setFile(e.currentTarget.files?.[0] || null);
  }

  async function start() {
    if (!file) return;
    if (file.size > MAX_UPLOAD) {
      setJob({ phase: 'failed', reason: t('err.payload_too_large') });
      return;
    }
    setJob({ phase: 'uploading', pct: 0 });
    const req = upload('/scenes', file, {
      onProgress: (sent, total) => {
        const pct = total > 0 ? Math.min(100, Math.round((sent / total) * 100)) : 0;
        setJob((j) => (j?.phase === 'uploading' ? { phase: 'uploading', pct } : j));
      },
    });
    abort.current = req.abort;
    try {
      const scene = await req.done;
      setJob(null);
      setFile(null);
      if (input.current) input.current.value = '';
      toast(t('scenes.installed', { name: scene?.name || '', version: scene?.version || '' }), 'ok');
      onInstalled();
    } catch (e) {
      if (e.status === 401) return;
      if (e.code === 'invalid_scene') {
        setJob({ phase: 'failed', reason: t('scenes.invalid'), detail: e.message });
      } else {
        setJob({ phase: 'failed', reason: sceneError(e), detail: errorDetail(e) });
      }
    } finally {
      abort.current = null;
    }
  }

  const busy = job?.phase === 'uploading';
  return html`<${Card} title=${t('scenes.upload')} icon=${IconPackage}>
    <div class="form">
      <div class="field">
        <label class="field-label sr-only" for="scene-file">${t('scenes.upload')}</label>
        <input
          id="scene-file"
          ref=${input}
          class="input"
          type="file"
          accept=".zip,application/zip"
          disabled=${busy}
          onChange=${pick}
        />
      </div>
      <div class="form-actions">
        <button type="button" class="btn btn-primary" onClick=${start} disabled=${!file || busy}>
          <${IconUpload} size=${16} />${t('scenes.upload')}
        </button>
      </div>
      ${busy
        ? html`<div class="fw-progress" role="status">
            <div class="field-row">
              <span>${t('scenes.uploading')}</span>
              <span class="mono">${new Intl.NumberFormat(locale(), { style: 'percent' }).format(job.pct / 100)}</span>
            </div>
            <div
              class="meter tone-accent"
              role="progressbar"
              aria-label=${t('scenes.uploading')}
              aria-valuemin="0"
              aria-valuemax="100"
              aria-valuenow=${job.pct}
            >
              <div class="meter-fill" style=${{ width: job.pct + '%' }}></div>
            </div>
          </div>`
        : null}
      ${job?.phase === 'failed'
        ? html`<div class="alert tone-err" role="alert">
            <${IconAlert} size=${18} />
            <span class="alert-body">
              ${job.reason}
              ${job.detail ? html`<br /><span class="err-detail mono">${job.detail}</span>` : null}
            </span>
          </div>`
        : null}
    </div>
  </${Card}>`;
}

function SceneItem({ scene, sdCard, open, onToggle, onChanged }) {
  const [busy, setBusy] = useState(false);
  const [exportData, setExportData] = useState(false);
  const [deleteData, setDeleteData] = useState(false);
  const panelId = 'scene-' + scene.id;
  const tone = STATE_TONE[scene.state] || 'muted';
  const hosts = Array.isArray(scene.hosts) ? scene.hosts : [];
  const live = scene.state === 'running' || scene.state === 'background';
  const needsCard = scene.files === 'sd' && !sdCard;

  async function act(action) {
    setBusy(true);
    try {
      await api('POST', `/scenes/${encodeURIComponent(scene.id)}/${action}`);
      onChanged();
    } catch (e) {
      if (e.status !== 401) toast(sceneError(e), 'err');
    } finally {
      setBusy(false);
    }
  }

  // Fetched rather than followed, so a failed export shows its error
  // instead of being saved as a .zip holding a JSON error body.
  async function exportZip(e) {
    e.preventDefault();
    const url = e.currentTarget.getAttribute('href');
    try {
      const res = await fetch(url, { credentials: 'same-origin', cache: 'no-store' });
      if (!res.ok) {
        let code = 'http_' + res.status;
        try {
          code = (await res.json())?.error?.code || code;
        } catch {
          // not JSON: keep the status
        }
        toast(sceneError({ status: res.status, code }), 'err');
        return;
      }
      const name = /filename="([^"]+)"/.exec(res.headers.get('Content-Disposition') || '')?.[1];
      const link = document.createElement('a');
      link.href = URL.createObjectURL(await res.blob());
      link.download = name || scene.id + '.zip';
      link.click();
      setTimeout(() => URL.revokeObjectURL(link.href), 1000);
    } catch {
      toast(sceneError({ status: 0, code: 'network' }), 'err');
    }
  }

  async function remove() {
    const ok = await confirmDialog({
      title: `${t('scenes.delete')} · ${scene.name}`,
      body: (deleteData ? t('scenes.delete_data') + ' · ' : '') + t('scenes.delete_pending'),
      okText: t('scenes.delete'),
      danger: true,
    });
    if (!ok) return;
    setBusy(true);
    try {
      await api('DELETE', `/scenes/${encodeURIComponent(scene.id)}?data=${deleteData ? 1 : 0}`);
      toast(t('scenes.delete_pending'), 'accent');
    } catch (e) {
      if (e.status !== 401) toast(sceneError(e), 'err');
    } finally {
      setBusy(false);
    }
  }

  return html`<li class=${open ? 'dev-item is-open' : 'dev-item'}>
    <button
      type="button"
      class="dev-row"
      aria-expanded=${open ? 'true' : 'false'}
      aria-controls=${panelId}
      onClick=${() => onToggle(scene.id)}
    >
      <span class=${'dev-icon tone-' + tone}><${IconPackage} size=${18} /></span>
      <span class="dev-main">
        <span class="dev-id">${scene.name}</span>
        <span class="dev-sub"><span class="mono">${scene.id}</span> · v${scene.version}</span>
      </span>
      ${needsCard ? html`<${Badge} tone="warn">${t('scenes.needs_sd')}</${Badge}>` : null}
      <${Badge} tone=${tone}>${STATE_TONE[scene.state] ? t('scenes.state.' + scene.state) : scene.state}</${Badge}>
      <span class="dev-chevron"><${IconChevronDown} size=${18} /></span>
    </button>
    ${open
      ? html`<div class="dev-panel" id=${panelId}>
          <dl class="kv">
            <div><dt>${t('scenes.code_size')}</dt><dd class="mono">${formatBytes(scene.code_bytes)}</dd></div>
            <div><dt>${t('scenes.data_size')}</dt><dd class="mono">${formatBytes(scene.data_bytes)}</dd></div>
            <div>
              <dt>${t('scenes.hosts')}</dt>
              <dd>
                ${hosts.length
                  ? html`<span class="chips">${hosts.map((h) => html`<span key=${h} class="chip mono"><${IconGlobe} size=${12} /> ${h}</span>`)}</span>`
                  : html`<span class="muted">${t('scenes.no_hosts')}</span>`}
              </dd>
            </div>
          </dl>
          ${scene.error
            ? html`<div class="alert tone-err scene-error" role="status">
                <${IconAlert} size=${18} />
                <span class="alert-body">
                  <strong>${t('scenes.last_error')}</strong><br />
                  <span class="err-detail mono">${scene.error}</span>
                </span>
              </div>`
            : null}
          <div class="scene-actions">
            ${scene.state !== 'running'
              ? html`<button type="button" class="btn btn-primary btn-sm" disabled=${busy} onClick=${() => act('open')}>
                  <${IconPlay} size=${15} />${t('scenes.open')}
                </button>`
              : null}
            ${live
              ? html`<button type="button" class="btn btn-secondary btn-sm" disabled=${busy} onClick=${() => act('exit')}>
                  <${IconStop} size=${15} />${t('scenes.close')}
                </button>`
              : null}
            <button type="button" class="btn btn-secondary btn-sm" disabled=${busy} onClick=${() => act('reload')}>
              <${IconRefresh} size=${15} />${t('scenes.reload')}
            </button>
            <a class="btn btn-ghost btn-sm" href=${'#/logs?tag=scene.' + scene.id}>
              <${IconLogs} size=${15} />${t('scenes.logs')}
            </a>
          </div>
          <${FilesSection} scene=${scene} sdCard=${sdCard} />
          <div class="scene-actions">
            <a class="btn btn-secondary btn-sm" href=${exportUrl(scene.id, exportData)} download onClick=${exportZip}>
              ${t('scenes.export')}
            </a>
            <label class="scene-check">
              <input type="checkbox" checked=${exportData} onChange=${(e) => setExportData(e.currentTarget.checked)} />
              ${t('scenes.export_data')}
            </label>
          </div>
          <div class="scene-actions">
            <button type="button" class="btn btn-danger btn-sm" disabled=${busy} onClick=${remove}>
              <${IconTrash} size=${15} />${t('scenes.delete')}
            </button>
            <label class="scene-check">
              <input type="checkbox" checked=${deleteData} onChange=${(e) => setDeleteData(e.currentTarget.checked)} />
              ${t('scenes.delete_data')}
            </label>
          </div>
        </div>`
      : null}
  </li>`;
}

function formatMtime(unix) {
  if (!Number.isFinite(unix) || unix < MIN_REAL_UNIX) return '';
  return new Intl.DateTimeFormat(locale(), { dateStyle: 'medium', timeStyle: 'short' }).format(new Date(unix * 1000));
}

/** The scene's files: location, usage, and per file download and delete. */
function FilesSection({ scene, sdCard }) {
  const [info, setInfo] = useState(/** @type {SceneFiles|null} */ (null));
  const [error, setError] = useState(null);
  const [removing, setRemoving] = useState('');
  const alive = useRef(true);
  const base = `/scenes/${encodeURIComponent(scene.id)}/files`;

  async function load() {
    try {
      const r = await api('GET', base);
      if (!alive.current) return;
      setInfo(r);
      setError(null);
    } catch (e) {
      if (alive.current && e.status !== 401) setError(e);
    }
  }

  useEffect(() => {
    alive.current = true;
    load();
    return () => {
      alive.current = false;
    };
  }, [scene.id]);

  async function remove(name) {
    const ok = await confirmDialog({
      title: `${t('scenes.file_delete')} · ${name}`,
      body: t('scenes.file_delete_body', { name }),
      okText: t('scenes.file_delete'),
      danger: true,
    });
    if (!ok) return;
    setRemoving(name);
    try {
      await api('DELETE', `${base}/${encodeURIComponent(name)}`);
      toast(t('scenes.file_deleted', { name }), 'ok');
    } catch (e) {
      if (e.status !== 401) toast(sceneError(e), 'err');
    } finally {
      if (alive.current) setRemoving('');
      load();
    }
  }

  const want = info?.want || scene.files;
  const unreachable = error?.code === 'unavailable';
  const needsCard = want === 'sd' && (!sdCard || unreachable);
  const files = Array.isArray(info?.files) ? info.files : [];
  const where = info?.location ? t('scenes.files_at.' + info.location) : t('scenes.files_none_yet');

  return html`<section class="scene-files" aria-label=${t('scenes.files')}>
    <h3 class="field-label with-icon"><${IconStorage} size=${16} />${t('scenes.files')}</h3>
    ${needsCard
      ? html`<div class="alert tone-warn" role="status">
          <${IconAlert} size=${18} /><span class="alert-body">${t('scenes.needs_sd_body')}</span>
        </div>`
      : null}
    ${error && !needsCard
      ? html`<p class="field-hint is-error">${unreachable ? t('scenes.files_unavailable') : sceneError(error)}</p>`
      : null}
    ${info
      ? html`<dl class="kv">
            <div><dt>${t('scenes.files_where')}</dt><dd>${where}</dd></div>
            <div>
              <dt>${t('scenes.files_used')}</dt>
              <dd class="mono">${formatBytes(info.used)} / ${formatBytes(info.quota)}</dd>
            </div>
          </dl>
          ${files.length === 0
            ? html`<p class="stat-empty">${t('scenes.files_empty')}</p>`
            : html`<ul class="rows">
                ${files.map((f) => {
                  const when = formatMtime(f.mtime);
                  return html`<li key=${f.name} class="row">
                    <div class="row-main">
                      <span class="row-title mono">${f.name}</span>
                      <span class="row-sub">${formatBytes(f.size)}${when ? ' · ' + when : ''}</span>
                    </div>
                    <div class="row-actions">
                      <a
                        class="btn btn-ghost btn-sm"
                        href=${fileUrl(scene.id, f.name)}
                        download=${f.name}
                        aria-label=${t('scenes.file_download') + ' ' + f.name}
                      >
                        <${IconArrowDown} size=${15} />${t('scenes.file_download')}
                      </a>
                      <button
                        type="button"
                        class="btn btn-ghost btn-sm"
                        disabled=${removing === f.name}
                        onClick=${() => remove(f.name)}
                        aria-label=${t('scenes.file_delete') + ' ' + f.name}
                      >
                        <${IconTrash} size=${15} />${t('scenes.file_delete')}
                      </button>
                    </div>
                  </li>`;
                })}
              </ul>`}
          ${info.truncated ? html`<p class="field-hint">${t('scenes.files_truncated', { count: files.length })}</p>` : null}`
      : !error
        ? html`<div class="skel skel-line"></div>`
        : null}
  </section>`;
}
