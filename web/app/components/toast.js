// Transient notices. toast() can be called from anywhere; the single
// <Toaster/> mounted by the shell renders them in a polite live region.

import { html, useEffect, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { IconAlert, IconCheck, IconInfo } from '../icons.js';

const DURATION_MS = 4000;
const MAX_VISIBLE = 3;

let nextId = 1;
/** @type {{id: number, message: string, tone: string}[]} */
let items = [];
const listeners = new Set();

function emit() {
  for (const fn of listeners) fn(items);
}

function dismiss(id) {
  items = items.filter((t) => t.id !== id);
  emit();
}

/**
 * Shows a short message for a few seconds.
 *
 * @param {string} message  plain text (rendered as text, never as HTML)
 * @param {'ok'|'warn'|'err'|'muted'|'accent'} [tone]
 */
export function toast(message, tone = 'accent') {
  const id = nextId++;
  items = [...items, { id, message: String(message), tone }].slice(-MAX_VISIBLE);
  emit();
  setTimeout(() => dismiss(id), DURATION_MS);
}

const ICONS = { ok: IconCheck, err: IconAlert, warn: IconAlert };

export function Toaster() {
  const [list, setList] = useState(items);
  useEffect(() => {
    listeners.add(setList);
    return () => listeners.delete(setList);
  }, []);
  return html`<div class="toaster" role="status" aria-live="polite">
    ${list.map((t) => {
      const Icon = ICONS[t.tone] || IconInfo;
      return html`<div key=${t.id} class=${'toast tone-' + t.tone}>
        <span class="toast-icon"><${Icon} size=${18} /></span>
        <span class="toast-text">${t.message}</span>
      </div>`;
    })}
  </div>`;
}
