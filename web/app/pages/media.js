// Media library (media spec 3.7): the SD card's media folder by folder,
// with upload, download, delete and new folder, the track index with a
// search, a rescan, and the "Now playing" card.
//
// The card's media root lists only music/, download/ and document/; files
// and folders can be added only below them (the device refuses anything
// else with invalid_path), so the root offers neither upload nor new
// folder. One upload at a time: the device answers 409 to a second one,
// and refuses an empty body (411), so empty files are not offered.
//
// Live updates: each media.library event ({available, scanning,
// track_count}) is the library's state at that moment and also means the
// folder and the index may have changed, so both are fetched again;
// consecutive events may carry equal payloads and none is skipped for it.
// Track tags are the files' own text and are rendered as text only.

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeMediaError, errorDetail, mediaFileUrl, mediaPath, upload } from '../api.js';
import { locale, t } from '../i18n/index.js';
import { subscribe } from '../ws.js';
import { formatBytes, formatCount, useVisible } from '../util.js';
import { Card } from '../components/card.js';
import { confirmDialog } from '../components/dialog.js';
import { toast } from '../components/toast.js';
import { NowPlayingCard, formatDuration } from '../components/now_playing.js';
import {
  IconAlert,
  IconArrowDown,
  IconChevronRight,
  IconClose,
  IconFile,
  IconFolder,
  IconFolderPlus,
  IconMusic,
  IconPlay,
  IconRefresh,
  IconSearch,
  IconStorage,
  IconTrash,
  IconUpload,
} from '../icons.js';

/**
 * @typedef {Object} Entry  one entry of GET /media/files
 * @property {string} name
 * @property {boolean} dir
 * @property {number} size
 * @property {number} mtime  unix seconds
 *
 * @typedef {Object} Track  one item of GET /media/tracks
 * @property {string} id     library path
 * @property {string} title
 * @property {string} artist
 * @property {string} album
 * @property {number} duration  seconds
 *
 * @typedef {{available: boolean, scanning: boolean, track_count: number}} Library
 */

// Clocks before 2020 mean the device had no real time when it wrote.
const MIN_REAL_UNIX = 1577836800;
const PAGE = 50;
const PLAYABLE = /\.(mp3|wav)$/i;
// A burst of library events (a rescan touching many files) costs one load.
const RELOAD_DELAY_MS = 300;
const SEARCH_DELAY_MS = 300;
// A rescan's own events end the "scanning" shown after the button; without
// a card no event may come, so it also ends after this long.
const RESCAN_HOLD_MS = 10000;

const join = (dir, name) => (dir ? dir + '/' + name : name);
// What the device accepts as one path segment (media spec 3.2 in short:
// the device checks the rest and answers invalid_path).
const validName = (name) => name !== '' && !name.startsWith('.') && !/[/\\]/.test(name);

function formatMtime(unix) {
  if (!Number.isFinite(unix) || unix < MIN_REAL_UNIX) return '';
  return new Intl.DateTimeFormat(locale(), { dateStyle: 'medium', timeStyle: 'short' }).format(new Date(unix * 1000));
}

export function MediaPage() {
  const visible = useVisible();
  const [dir, setDir] = useState('');
  const [library, setLibrary] = useState(/** @type {Library|null} */ (null));
  const [rescanning, setRescanning] = useState(false);
  // Bumped on every library event; the folder and track lists reload on it.
  const [epoch, setEpoch] = useState(0);
  const alive = useRef(true);
  const reloadTimer = useRef(0);
  const holdTimer = useRef(0);

  // The library's state when there is no event yet. GET /media/player
  // carries it; without an audio output that route is 503 and the state
  // stays unknown until the first media.library event.
  async function loadLibrary() {
    try {
      const r = await api('GET', '/media/player');
      if (alive.current && r) {
        setLibrary({ available: !!r.available, scanning: !!r.scanning, track_count: Number(r.track_count) || 0 });
      }
    } catch {
      // No player, or unreachable: the folder listing shows its own error.
    }
  }

  function bump() {
    clearTimeout(reloadTimer.current);
    reloadTimer.current = setTimeout(() => {
      if (alive.current) setEpoch((n) => n + 1);
    }, RELOAD_DELAY_MS);
  }

  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
      clearTimeout(reloadTimer.current);
      clearTimeout(holdTimer.current);
    };
  }, []);

  useEffect(() => {
    if (!visible) return undefined;
    loadLibrary();
    const unsubscribe = subscribe(
      ['events'],
      (msg) => {
        if (msg.t === 'dropped') {
          loadLibrary();
          bump();
          return;
        }
        if (msg.t !== 'event' || msg.topic !== 'media.library') return;
        const p = msg.payload || {};
        setLibrary({ available: !!p.available, scanning: !!p.scanning, track_count: Number(p.track_count) || 0 });
        if (!p.scanning) {
          clearTimeout(holdTimer.current);
          setRescanning(false);
        }
        bump();
      },
      {
        onState: (st) => {
          if (st === 'open') {
            loadLibrary();
            bump();
          }
        },
      },
    );
    return unsubscribe;
  }, [visible]);

  async function rescan() {
    setRescanning(true);
    clearTimeout(holdTimer.current);
    holdTimer.current = setTimeout(() => {
      if (alive.current) setRescanning(false);
      loadLibrary();
    }, RESCAN_HOLD_MS);
    try {
      await api('POST', '/media/rescan');
    } catch (e) {
      clearTimeout(holdTimer.current);
      setRescanning(false);
      if (e.status !== 401) toast(describeMediaError(e), 'err');
    }
  }

  const scanning = rescanning || !!library?.scanning;
  let status = '';
  if (library && !library.available) status = t('media.no_card_title');
  else if (scanning) status = t('media.scanning');
  else if (library) status = t('media.tracks_count', { count: formatCount(library.track_count) });

  return html`<div class="page">
    <header class="page-head">
      <div>
        <h1 class="page-title" tabindex="-1">${t('media.title')}</h1>
        ${status ? html`<p class="page-sub" role="status">${status}</p>` : null}
      </div>
      <div class="page-head-actions">
        <button type="button" class="btn btn-secondary" onClick=${rescan} disabled=${scanning}>
          <${IconRefresh} size=${16} class=${scanning ? 'spin' : ''} />${scanning ? t('media.scanning') : t('media.rescan')}
        </button>
      </div>
    </header>

    <${NowPlayingCard} />
    <${FolderCard} dir=${dir} onDir=${setDir} epoch=${epoch} onChanged=${bump} />
    <${TracksCard} epoch=${epoch} />
  </div>`;
}

function Breadcrumbs({ dir, onDir }) {
  const parts = dir ? dir.split('/') : [];
  return html`<nav class="crumbs" aria-label=${t('media.breadcrumb')}>
    <ol>
      <li>
        ${parts.length
          ? html`<button type="button" class="btn btn-link" onClick=${() => onDir('')}>${t('media.root')}</button>`
          : html`<span aria-current="page">${t('media.root')}</span>`}
      </li>
      ${parts.map((p, i) => {
        const last = i === parts.length - 1;
        const to = parts.slice(0, i + 1).join('/');
        return html`<li key=${to}>
          <${IconChevronRight} size=${14} class="crumb-sep" />
          ${last
            ? html`<span aria-current="page">${p}</span>`
            : html`<button type="button" class="btn btn-link" onClick=${() => onDir(to)}>${p}</button>`}
        </li>`;
      })}
    </ol>
  </nav>`;
}

function FolderCard({ dir, onDir, epoch, onChanged }) {
  const [entries, setEntries] = useState(/** @type {Entry[]|null} */ (null));
  const [error, setError] = useState(null);
  const [loading, setLoading] = useState(false);
  const [removing, setRemoving] = useState('');
  const [newFolder, setNewFolder] = useState(/** @type {string|null} */ (null));
  const [creating, setCreating] = useState(false);
  // null | {phase: 'uploading', name, pct} | {phase: 'failed', reason, detail?}
  const [job, setJob] = useState(null);
  const input = useRef(null);
  const abort = useRef(null);
  const alive = useRef(true);
  const shown = useRef(dir);
  const atRoot = dir === '';

  async function load(target = dir) {
    setLoading(true);
    try {
      const r = await api('GET', '/media/files?dir=' + encodeURIComponent(target));
      if (!alive.current || shown.current !== target) return;
      setEntries(Array.isArray(r?.entries) ? r.entries : []);
      setError(null);
    } catch (e) {
      if (!alive.current || shown.current !== target || e.status === 401) return;
      // A folder that went away (deleted elsewhere, card swapped) leads
      // back to the root instead of a dead end.
      if (e.code === 'not_found' && target !== '') {
        onDir('');
        return;
      }
      setError(e);
      setEntries(null);
    } finally {
      if (alive.current && shown.current === target) setLoading(false);
    }
  }

  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
      abort.current?.();
    };
  }, []);

  useEffect(() => {
    shown.current = dir;
    setEntries(null);
    setError(null);
    setNewFolder(null);
    // A failed upload's notice belongs to the folder it was meant for.
    setJob((j) => (j?.phase === 'uploading' ? j : null));
    load(dir);
  }, [dir]);

  useEffect(() => {
    if (epoch > 0) load(dir);
  }, [epoch]);

  async function pick(e) {
    const file = e.currentTarget.files?.[0] || null;
    e.currentTarget.value = '';
    if (!file) return;
    if (file.size === 0) {
      setJob({ phase: 'failed', reason: t('media.upload_empty') });
      return;
    }
    const exists = (entries || []).some((x) => !x.dir && x.name === file.name);
    if (exists) {
      const ok = await confirmDialog({
        title: t('media.replace_title', { name: file.name }),
        body: t('media.replace_body'),
        okText: t('media.replace'),
      });
      if (!ok) return;
    }
    start(file, dir);
  }

  async function start(file, target) {
    setJob({ phase: 'uploading', name: file.name, pct: 0 });
    const req = upload('/media/files/' + mediaPath(join(target, file.name)), file, {
      method: 'PUT',
      onProgress: (sent, total) => {
        const pct = total > 0 ? Math.min(100, Math.round((sent / total) * 100)) : 0;
        setJob((j) => (j?.phase === 'uploading' ? { ...j, pct } : j));
      },
    });
    abort.current = req.abort;
    try {
      await req.done;
      if (!alive.current) return;
      setJob(null);
      toast(t('media.uploaded', { name: file.name }), 'ok');
    } catch (e) {
      if (!alive.current || e.status === 401) return;
      if (abort.current === null) {
        // Cancelled by the user: the device drops the partial file.
        setJob(null);
        return;
      }
      const reason = e.status === 409 ? t('media.upload_busy') : describeMediaError(e);
      setJob({ phase: 'failed', reason, detail: errorDetail(e) });
    } finally {
      abort.current = null;
      if (alive.current && shown.current === target) load(target);
      onChanged();
    }
  }

  function cancelUpload() {
    const stop = abort.current;
    abort.current = null;
    stop?.();
  }

  async function createFolder() {
    const name = (newFolder || '').trim();
    if (!validName(name)) return;
    setCreating(true);
    try {
      await api('POST', '/media/folders/' + mediaPath(join(dir, name)));
      if (!alive.current) return;
      setNewFolder(null);
      toast(t('media.folder_created', { name }), 'ok');
      load(dir);
    } catch (err) {
      if (err.status !== 401) toast(describeMediaError(err), 'err');
    } finally {
      if (alive.current) setCreating(false);
    }
  }

  async function remove(entry) {
    const path = join(dir, entry.name);
    if (entry.dir) {
      // The device deletes only empty folders; asking first spares the
      // user a confirmation that can only fail.
      try {
        const r = await api('GET', '/media/files?dir=' + encodeURIComponent(path));
        if (Array.isArray(r?.entries) && r.entries.length > 0) {
          toast(t('media.folder_not_empty'), 'warn');
          return;
        }
      } catch (e) {
        if (e.status !== 401) toast(describeMediaError(e), 'err');
        return;
      }
    }
    const ok = await confirmDialog({
      title: `${t('media.delete')} · ${entry.name}`,
      body: entry.dir ? t('media.delete_folder_body', { name: entry.name }) : t('media.delete_file_body', { name: entry.name }),
      okText: t('media.delete'),
      danger: true,
    });
    if (!ok) return;
    setRemoving(entry.name);
    try {
      await api('DELETE', '/media/files/' + mediaPath(path));
      toast(t('media.deleted', { name: entry.name }), 'ok');
    } catch (e) {
      if (e.status === 401) return;
      toast(entry.dir && e.code === 'invalid_path' ? t('media.folder_not_empty') : describeMediaError(e), 'err');
    } finally {
      if (alive.current) setRemoving('');
      load(dir);
      onChanged();
    }
  }

  async function play(entry) {
    // The folder's playable files as the queue, starting at this one.
    const files = (entries || []).filter((x) => !x.dir && PLAYABLE.test(x.name));
    const start = Math.max(0, files.findIndex((x) => x.name === entry.name));
    try {
      await api('POST', '/media/player/play', { items: files.map((x) => join(dir, x.name)), start });
    } catch (e) {
      if (e.status !== 401) toast(describeMediaError(e), 'err');
    }
  }

  const uploading = job?.phase === 'uploading';
  const noCard = error?.code === 'media_unavailable';
  const list = entries || [];

  const actions = html`
    <button
      type="button"
      class="btn btn-secondary btn-sm"
      disabled=${atRoot || noCard || newFolder !== null}
      onClick=${() => setNewFolder('')}
    >
      <${IconFolderPlus} size=${16} /><span class="btn-label">${t('media.new_folder')}</span>
    </button>
    <button
      type="button"
      class="btn btn-primary btn-sm"
      disabled=${atRoot || noCard || uploading}
      onClick=${() => input.current?.click()}
    >
      <${IconUpload} size=${16} /><span class="btn-label">${t('media.upload')}</span>
    </button>`;

  return html`<${Card} title=${t('media.files')} icon=${IconFolder} actions=${actions} class="media-card">
    <input ref=${input} type="file" class="sr-only" tabindex="-1" aria-hidden="true" onChange=${pick} />
    <div class="media-bar">
      <${Breadcrumbs} dir=${dir} onDir=${onDir} />
      <button
        type="button"
        class="btn btn-icon btn-ghost"
        onClick=${() => load(dir)}
        disabled=${loading}
        aria-label=${t('common.refresh')}
        title=${t('common.refresh')}
      >
        <${IconRefresh} size=${18} class=${loading ? 'spin' : ''} />
      </button>
    </div>
    ${atRoot && !noCard ? html`<p class="field-hint">${t('media.upload_root_hint')}</p>` : null}

    ${newFolder !== null
      ? html`<${NewFolderForm}
          value=${newFolder}
          busy=${creating}
          onInput=${setNewFolder}
          onSubmit=${createFolder}
          onCancel=${() => setNewFolder(null)}
        />`
      : null}

    ${uploading
      ? html`<div class="fw-progress media-upload" role="status">
          <div class="field-row">
            <span class="media-upload-name">${t('media.uploading', { name: job.name })}</span>
            <span class="mono">${new Intl.NumberFormat(locale(), { style: 'percent' }).format(job.pct / 100)}</span>
            <button
              type="button"
              class="btn btn-icon btn-ghost btn-sm"
              onClick=${cancelUpload}
              aria-label=${t('media.upload_cancel')}
              title=${t('media.upload_cancel')}
            >
              <${IconClose} size=${16} />
            </button>
          </div>
          <div
            class="meter tone-accent"
            role="progressbar"
            aria-label=${t('media.uploading', { name: job.name })}
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
          <button
            type="button"
            class="btn btn-icon btn-ghost btn-sm"
            onClick=${() => setJob(null)}
            aria-label=${t('media.dismiss')}
            title=${t('media.dismiss')}
          >
            <${IconClose} size=${16} />
          </button>
        </div>`
      : null}

    ${noCard
      ? html`<div class="empty">
          <span class="empty-icon"><${IconStorage} size=${26} /></span>
          <p class="empty-title">${t('media.no_card_title')}</p>
          <p class="muted">${t('media.no_card')}</p>
        </div>`
      : error
        ? html`<div class="alert tone-err" role="alert">
            <${IconAlert} size=${18} />
            <span class="alert-body">
              ${t('media.load_failed')} · ${describeMediaError(error)}
              ${errorDetail(error) ? html`<br /><span class="err-detail mono">${errorDetail(error)}</span>` : null}
            </span>
          </div>`
        : !entries
          ? html`<div aria-busy="true" aria-label=${t('common.loading')}>
              ${[0, 1, 2].map(
                (i) => html`<div key=${i} class="skeleton-row media-skel">
                  <div class="skel skel-icon"></div>
                  <div class="skel skel-line"></div>
                </div>`,
              )}
            </div>`
          : list.length === 0
            ? html`<p class="stat-empty">${t('media.empty_folder')}</p>`
            : html`<ul class="rows media-rows">
                ${list.map((x) => {
                  const when = formatMtime(x.mtime);
                  const playable = !x.dir && PLAYABLE.test(x.name);
                  const Icon = x.dir ? IconFolder : playable ? IconMusic : IconFile;
                  const main = html`<span class=${'dev-icon tone-' + (x.dir ? 'accent' : 'muted')}><${Icon} size=${18} /></span>
                    <span class="row-main">
                      <span class="row-title">${x.name}</span>
                      <span class="row-sub">${x.dir ? t('media.folder') : formatBytes(x.size)}${when ? ' · ' + when : ''}</span>
                    </span>`;
                  return html`<li key=${x.name} class="row media-row">
                    ${x.dir
                      ? html`<button type="button" class="media-open" onClick=${() => onDir(join(dir, x.name))}>
                          ${main}<${IconChevronRight} size=${18} class="media-chevron" />
                        </button>`
                      : html`<div class="media-open is-static">${main}</div>`}
                    <div class="media-actions">
                      ${playable
                        ? html`<button
                            type="button"
                            class="btn btn-icon btn-ghost"
                            onClick=${() => play(x)}
                            aria-label=${t('media.play') + ' ' + x.name}
                            title=${t('media.play')}
                          >
                            <${IconPlay} size=${16} />
                          </button>`
                        : null}
                      ${x.dir
                        ? null
                        : html`<a
                            class="btn btn-icon btn-ghost"
                            href=${mediaFileUrl(join(dir, x.name))}
                            download=${x.name}
                            aria-label=${t('media.download') + ' ' + x.name}
                            title=${t('media.download')}
                          >
                            <${IconArrowDown} size=${16} />
                          </a>`}
                      ${atRoot
                        ? null
                        : html`<button
                            type="button"
                            class="btn btn-icon btn-ghost"
                            disabled=${removing === x.name}
                            onClick=${() => remove(x)}
                            aria-label=${t('media.delete') + ' ' + x.name}
                            title=${t('media.delete')}
                          >
                            <${IconTrash} size=${16} />
                          </button>`}
                    </div>
                  </li>`;
                })}
              </ul>`}
  </${Card}>`;
}

function NewFolderForm({ value, busy, onInput, onSubmit, onCancel }) {
  const field = useRef(null);
  useEffect(() => field.current?.focus(), []);
  const name = value.trim();
  const ok = name === '' || validName(name);
  return html`<form
    class="media-form"
    onSubmit=${(e) => {
      e.preventDefault();
      if (name && ok) onSubmit();
    }}
  >
    <label class="sr-only" for="media-folder">${t('media.folder_name')}</label>
    <input
      id="media-folder"
      ref=${field}
      class="input"
      type="text"
      autocomplete="off"
      placeholder=${t('media.folder_name')}
      value=${value}
      maxlength="255"
      aria-invalid=${ok ? undefined : 'true'}
      aria-describedby=${ok ? undefined : 'media-folder-hint'}
      onInput=${(e) => onInput(e.currentTarget.value)}
      onKeyDown=${(e) => e.key === 'Escape' && onCancel()}
    />
    <div class="media-form-actions">
      <button type="button" class="btn btn-ghost" onClick=${onCancel} disabled=${busy}>${t('common.cancel')}</button>
      <button type="submit" class="btn btn-primary" disabled=${busy || !name || !ok}>${t('media.create')}</button>
    </div>
    ${ok ? null : html`<p id="media-folder-hint" class="field-hint is-error">${t('media.bad_name')}</p>`}
  </form>`;
}

function TracksCard({ epoch }) {
  const [query, setQuery] = useState('');
  const [applied, setApplied] = useState('');
  const [items, setItems] = useState(/** @type {Track[]|null} */ (null));
  const [total, setTotal] = useState(0);
  const [error, setError] = useState(null);
  const [more, setMore] = useState(false);
  const alive = useRef(true);
  const seq = useRef(0);

  async function load(q, count) {
    const mine = ++seq.current;
    try {
      const r = await api('GET', `/media/tracks?q=${encodeURIComponent(q)}&offset=0&limit=${count}`);
      if (!alive.current || mine !== seq.current) return;
      setItems(Array.isArray(r?.items) ? r.items : []);
      setTotal(Number(r?.total) || 0);
      setError(null);
    } catch (e) {
      if (!alive.current || mine !== seq.current || e.status === 401) return;
      setError(e);
    }
  }

  async function loadMore() {
    const have = items?.length || 0;
    const mine = ++seq.current;
    setMore(true);
    try {
      const r = await api('GET', `/media/tracks?q=${encodeURIComponent(applied)}&offset=${have}&limit=${PAGE}`);
      if (!alive.current || mine !== seq.current) return;
      setItems((list) => [...(list || []), ...(Array.isArray(r?.items) ? r.items : [])]);
      setTotal(Number(r?.total) || 0);
    } catch (e) {
      if (alive.current && e.status !== 401) toast(describeMediaError(e), 'err');
    } finally {
      if (alive.current) setMore(false);
    }
  }

  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
    };
  }, []);

  useEffect(() => {
    const id = setTimeout(() => setApplied(query.trim()), SEARCH_DELAY_MS);
    return () => clearTimeout(id);
  }, [query]);

  useEffect(() => {
    load(applied, PAGE);
  }, [applied]);

  // A library change keeps as many rows as are shown (up to the device's
  // page cap), so "show more" is not undone by an upload elsewhere.
  useEffect(() => {
    if (epoch > 0) load(applied, Math.min(200, Math.max(PAGE, items?.length || 0)));
  }, [epoch]);

  async function play(index) {
    try {
      await api('POST', '/media/player/play', { items: items.map((x) => x.id), start: index });
    } catch (e) {
      if (e.status !== 401) toast(describeMediaError(e), 'err');
    }
  }

  const list = items || [];
  const noCard = error?.code === 'media_unavailable';
  return html`<${Card} title=${t('media.tracks')} icon=${IconMusic} class="media-card">
    <label class="search media-search">
      <${IconSearch} size=${16} />
      <span class="sr-only">${t('media.search')}</span>
      <input
        type="search"
        placeholder=${t('media.search')}
        value=${query}
        onInput=${(e) => setQuery(e.currentTarget.value)}
        autocomplete="off"
      />
    </label>
    ${error
      ? noCard
        ? html`<p class="stat-empty">${t('media.no_card')}</p>`
        : html`<p class="field-hint is-error">${describeMediaError(error)}</p>`
      : !items
        ? html`<div class="skel skel-line"></div>`
        : list.length === 0
          ? html`<p class="stat-empty">${applied ? t('media.tracks_none_match', { q: applied }) : t('media.tracks_empty')}</p>`
          : html`<ul class="rows media-rows">
              ${list.map((x, i) => {
                const sub = [x.artist || t('media.unknown_artist'), x.album, x.duration > 0 ? formatDuration(x.duration) : '']
                  .filter(Boolean)
                  .join(' · ');
                return html`<li key=${x.id} class="row media-row">
                  <div class="media-open is-static">
                    <span class="dev-icon tone-accent"><${IconMusic} size=${18} /></span>
                    <span class="row-main">
                      <span class="row-title">${x.title || x.id.split('/').pop()}</span>
                      <span class="row-sub">${sub}</span>
                    </span>
                  </div>
                  <div class="media-actions">
                    <button
                      type="button"
                      class="btn btn-icon btn-ghost"
                      onClick=${() => play(i)}
                      aria-label=${t('media.play') + ' ' + (x.title || x.id)}
                      title=${t('media.play')}
                    >
                      <${IconPlay} size=${16} />
                    </button>
                  </div>
                </li>`;
              })}
            </ul>
            <div class="media-foot">
              <span class="muted">${t('media.shown', { shown: formatCount(list.length), total: formatCount(total) })}</span>
              ${list.length < total
                ? html`<button type="button" class="btn btn-secondary btn-sm" onClick=${loadMore} disabled=${more}>
                    ${t('media.load_more')}
                  </button>`
                : null}
            </div>`}
  </${Card}>`;
}
