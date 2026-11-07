// Memory (mvp spec 5.3): the agent's long-term memory notes and its todos,
// each in its own card with a delete button that asks first
// (confirmDialog). Deletes cannot be undone, and a mistaken tap on a touch
// screen should not lose data.

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError, errorDetail } from '../api.js';
import { locale, t } from '../i18n/index.js';
import { subscribe } from '../ws.js';
import { Card } from '../components/card.js';
import { confirmDialog } from '../components/dialog.js';
import { toast } from '../components/toast.js';
import { IconAlert, IconChecklist, IconNote, IconRefresh, IconTrash } from '../icons.js';

/**
 * @typedef {Object} MemoryNote  one entry of GET /memory
 * @property {number} id
 * @property {string} text
 * @property {number} updated  unix seconds
 *
 * @typedef {Object} TodoRow  one entry of GET /todos
 * @property {number} id
 * @property {string} title
 * @property {boolean} done
 * @property {string} [due_local]     "YYYY-MM-DDTHH:MM"
 * @property {'none'|'daily'|{weekly: number[]}} repeat
 * @property {string} [snooze_until]  "YYYY-MM-DDTHH:MM"
 */

const WEEKDAY_KEYS = ['', 'day.mon', 'day.tue', 'day.wed', 'day.thu', 'day.fri', 'day.sat', 'day.sun'];

function formatUpdated(unixSeconds) {
  if (!Number.isFinite(unixSeconds) || unixSeconds <= 0) return '';
  return new Intl.DateTimeFormat(locale(), { dateStyle: 'medium', timeStyle: 'short' }).format(unixSeconds * 1000);
}

/** "YYYY-MM-DDTHH:MM" -> a locale-neutral, readable "YYYY-MM-DD HH:MM". */
function formatLocalMinute(s) {
  return typeof s === 'string' ? s.replace('T', ' ') : '';
}

function repeatLabel(repeat) {
  if (repeat === 'daily') return t('mem.repeat_daily');
  if (repeat && typeof repeat === 'object' && Array.isArray(repeat.weekly)) {
    const days = [...repeat.weekly].sort((a, b) => a - b).map((d) => t(WEEKDAY_KEYS[d] || ''));
    return t('mem.repeat_weekly', { days: days.join(t('mem.day_sep')) });
  }
  return '';
}

export function MemoryPage() {
  return html`<div class="page">
    <header class="page-head">
      <div>
        <h1 class="page-title" tabindex="-1">${t('nav.memory')}</h1>
        <p class="page-sub">${t('mem.subtitle')}</p>
      </div>
    </header>
    <${NotesCard} />
    <${TodosCard} />
  </div>`;
}

function NotesCard() {
  const [notes, setNotes] = useState(/** @type {MemoryNote[]|null} */ (null));
  const [error, setError] = useState(null);
  const [loading, setLoading] = useState(false);
  const [removing, setRemoving] = useState(0);
  const alive = useRef(true);
  const reloadTimer = useRef(0);

  async function load() {
    setLoading(true);
    try {
      const r = await api('GET', '/memory');
      if (!alive.current) return;
      setNotes(Array.isArray(r?.notes) ? r.notes : []);
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
    const unsubscribe = subscribe(['events'], (msg) => {
      if (msg.t !== 'event' || msg.topic !== 'memory.changed') return;
      clearTimeout(reloadTimer.current);
      reloadTimer.current = setTimeout(load, 300);
    });
    return () => {
      alive.current = false;
      clearTimeout(reloadTimer.current);
      unsubscribe();
    };
  }, []);

  async function remove(note) {
    const ok = await confirmDialog({
      title: t('mem.note_delete'),
      body: t('mem.note_delete_body', { text: note.text }),
      okText: t('mem.note_delete'),
      danger: true,
    });
    if (!ok) return;
    setRemoving(note.id);
    try {
      await api('DELETE', `/memory/${note.id}`);
      setNotes((list) => (list || []).filter((n) => n.id !== note.id));
      toast(t('mem.note_deleted'), 'ok');
    } catch (e) {
      if (e.status === 404) {
        // Already gone (the agent removed it first): the page agreed with
        // the device anyway, so drop the row without an error toast.
        setNotes((list) => (list || []).filter((n) => n.id !== note.id));
      } else if (e.status !== 401) {
        toast(describeError(e), 'err');
        // A delete that timed out waiting on the flash thread can still
        // land afterwards (routes_memory.cpp), so re-fetch rather than
        // trust that the row is still there.
        load();
      }
    } finally {
      if (alive.current) setRemoving(0);
    }
  }

  const list = notes || [];
  return html`<${Card} title=${t('mem.notes_title')} icon=${IconNote}>
    ${error && !notes
      ? html`<div class="alert tone-err" role="alert">
          <${IconAlert} size=${18} />
          <span class="alert-body">
            ${describeError(error)}
            ${errorDetail(error) ? html`<br /><span class="err-detail mono">${errorDetail(error)}</span>` : null}
          </span>
        </div>`
      : null}
    ${!notes
      ? html`<div class="skel skel-line" aria-busy="true" aria-label=${t('common.loading')}></div>`
      : list.length === 0
        ? html`<p class="stat-empty">${t('mem.notes_empty')}</p>`
        : html`<ul class="rows">
            ${list.map((n) => {
              const updated = formatUpdated(n.updated);
              return html`<li key=${n.id} class="row">
                <div class="row-main">
                  <span class="row-title">${n.text}</span>
                  ${updated ? html`<span class="row-sub">${t('mem.updated', { when: updated })}</span>` : null}
                </div>
                <div class="row-actions">
                  <button
                    type="button"
                    class="btn btn-icon btn-ghost"
                    disabled=${removing === n.id}
                    onClick=${() => remove(n)}
                    aria-label=${t('mem.note_delete') + ' · ' + n.text}
                    title=${t('mem.note_delete')}
                  >
                    <${IconTrash} size=${16} />
                  </button>
                </div>
              </li>`;
            })}
          </ul>`}
    <div class="form-actions">
      <button type="button" class="btn btn-ghost btn-sm" onClick=${load} disabled=${loading}>
        <${IconRefresh} size=${15} class=${loading ? 'spin' : ''} />${t('common.refresh')}
      </button>
    </div>
  </${Card}>`;
}

function TodosCard() {
  const [todos, setTodos] = useState(/** @type {TodoRow[]|null} */ (null));
  const [error, setError] = useState(null);
  const [loading, setLoading] = useState(false);
  const [removing, setRemoving] = useState(0);
  const alive = useRef(true);
  const reloadTimer = useRef(0);

  async function load() {
    setLoading(true);
    try {
      const r = await api('GET', '/todos');
      if (!alive.current) return;
      setTodos(Array.isArray(r?.todos) ? r.todos : []);
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
    const unsubscribe = subscribe(['events'], (msg) => {
      if (msg.t !== 'event' || msg.topic !== 'todo.changed') return;
      clearTimeout(reloadTimer.current);
      reloadTimer.current = setTimeout(load, 300);
    });
    return () => {
      alive.current = false;
      clearTimeout(reloadTimer.current);
      unsubscribe();
    };
  }, []);

  async function remove(todo) {
    const ok = await confirmDialog({
      title: t('mem.todo_delete'),
      body: t('mem.todo_delete_body', { title: todo.title }),
      okText: t('mem.todo_delete'),
      danger: true,
    });
    if (!ok) return;
    setRemoving(todo.id);
    try {
      await api('DELETE', `/todos/${todo.id}`);
      setTodos((list) => (list || []).filter((x) => x.id !== todo.id));
      toast(t('mem.todo_deleted'), 'ok');
    } catch (e) {
      if (e.status === 404) {
        // Already gone (the agent removed it first): the page agreed with
        // the device anyway, so drop the row without an error toast.
        setTodos((list) => (list || []).filter((x) => x.id !== todo.id));
      } else if (e.status !== 401) {
        toast(describeError(e), 'err');
        // A delete that timed out waiting on the flash thread can still
        // land afterwards (routes_memory.cpp), so re-fetch rather than
        // trust that the row is still there.
        load();
      }
    } finally {
      if (alive.current) setRemoving(0);
    }
  }

  const list = todos || [];
  return html`<${Card} title=${t('mem.todos_title')} icon=${IconChecklist}>
    ${error && !todos
      ? html`<div class="alert tone-err" role="alert">
          <${IconAlert} size=${18} />
          <span class="alert-body">
            ${describeError(error)}
            ${errorDetail(error) ? html`<br /><span class="err-detail mono">${errorDetail(error)}</span>` : null}
          </span>
        </div>`
      : null}
    ${!todos
      ? html`<div class="skel skel-line" aria-busy="true" aria-label=${t('common.loading')}></div>`
      : list.length === 0
        ? html`<p class="stat-empty">${t('mem.todos_empty')}</p>`
        : html`<ul class="rows">
            ${list.map((row) => {
              const repeat = repeatLabel(row.repeat);
              const sub = [
                row.done ? t('mem.todo_done') : '',
                row.due_local ? t('mem.due', { when: formatLocalMinute(row.due_local) }) : '',
                repeat,
                row.snooze_until ? t('mem.snoozed', { when: formatLocalMinute(row.snooze_until) }) : '',
              ]
                .filter(Boolean)
                .join(' · ');
              return html`<li key=${row.id} class="row">
                <div class="row-main">
                  <span class=${row.done ? 'row-title is-done' : 'row-title'}>${row.title}</span>
                  ${sub ? html`<span class="row-sub">${sub}</span>` : null}
                </div>
                <div class="row-actions">
                  <button
                    type="button"
                    class="btn btn-icon btn-ghost"
                    disabled=${removing === row.id}
                    onClick=${() => remove(row)}
                    aria-label=${t('mem.todo_delete') + ' · ' + row.title}
                    title=${t('mem.todo_delete')}
                  >
                    <${IconTrash} size=${16} />
                  </button>
                </div>
              </li>`;
            })}
          </ul>`}
    <div class="form-actions">
      <button type="button" class="btn btn-ghost btn-sm" onClick=${load} disabled=${loading}>
        <${IconRefresh} size=${15} class=${loading ? 'spin' : ''} />${t('common.refresh')}
      </button>
    </div>
  </${Card}>`;
}
