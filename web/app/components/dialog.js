// Confirmation dialog on the native <dialog> element, which brings modal
// focus trapping, Esc to cancel and the top layer for free.

import { html, render } from '../../vendor/htm-preact-3.1.1.mjs';
import { t } from '../i18n/index.js';

/**
 * Asks the user to confirm an action.
 *
 * @param {{title: string, body?: string, okText?: string,
 *          cancelText?: string, danger?: boolean}} opts
 *   `body` is plain text. `danger` paints the confirm button red.
 * @returns {Promise<boolean>} true when confirmed
 */
export function confirmDialog({ title, body, okText = t('common.ok'), cancelText = t('common.cancel'), danger = false }) {
  return new Promise((resolve) => {
    const host = document.createElement('div');
    document.body.appendChild(host);
    let settled = false;

    const finish = (ok) => {
      if (settled) return;
      settled = true;
      const dlg = host.querySelector('dialog');
      if (dlg && dlg.open) dlg.close();
      render(null, host);
      host.remove();
      resolve(ok);
    };

    render(
      html`<dialog
        class="dialog"
        aria-labelledby="dialog-title"
        onCancel=${(e) => {
          e.preventDefault();
          finish(false);
        }}
        onClick=${(e) => {
          // A click on the backdrop lands on the <dialog> itself.
          if (e.target === e.currentTarget) finish(false);
        }}
      >
        <div class="dialog-body">
          <h2 id="dialog-title" class="dialog-title">${title}</h2>
          ${body ? html`<p class="dialog-text">${body}</p>` : null}
          <div class="dialog-actions">
            <button type="button" class="btn btn-ghost" onClick=${() => finish(false)}>
              ${cancelText}
            </button>
            <button
              type="button"
              class=${danger ? 'btn btn-danger' : 'btn btn-primary'}
              data-autofocus
              onClick=${() => finish(true)}
            >
              ${okText}
            </button>
          </div>
        </div>
      </dialog>`,
      host,
    );

    const dlg = host.querySelector('dialog');
    dlg.showModal();
    // Destructive actions start on "cancel" so Enter does not confirm them.
    const first = danger ? dlg.querySelector('.btn-ghost') : dlg.querySelector('[data-autofocus]');
    first?.focus();
  });
}
