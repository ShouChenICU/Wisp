import { html } from '../../vendor/htm-preact-3.1.1.mjs';

/**
 * A titled surface. `icon` is an icon component from icons.js, `actions`
 * sits at the right of the title row, and `tone` tints the icon
 * ("ok"/"warn"/"err"; accent by default).
 *
 * @param {{title?: any, icon?: Function, actions?: any, tone?: string,
 *          class?: string, children?: any}} props
 */
export function Card({ title, icon: Icon, actions, tone, class: cls, children }) {
  const head =
    title || actions
      ? html`<header class="card-head">
          ${Icon ? html`<span class=${'card-icon tone-' + (tone || 'accent')}><${Icon} size=${18} /></span>` : null}
          ${title ? html`<h2 class="card-title">${title}</h2>` : null}
          ${actions ? html`<div class="card-actions">${actions}</div>` : null}
        </header>`
      : null;
  return html`<section class=${cls ? 'card ' + cls : 'card'}>${head}${children}</section>`;
}
