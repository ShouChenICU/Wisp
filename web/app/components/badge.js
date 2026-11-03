import { html } from '../../vendor/htm-preact-3.1.1.mjs';

/**
 * A small status pill.
 *
 * @param {{tone?: 'ok'|'warn'|'err'|'muted'|'accent', dot?: boolean,
 *          pulse?: boolean, children?: any}} props
 *   `dot` adds a leading status dot; `pulse` makes it breathe (live state).
 */
export function Badge({ tone = 'muted', dot = false, pulse = false, children }) {
  return html`<span class=${'badge tone-' + tone}>
    ${dot ? html`<span class=${pulse ? 'dot pulse' : 'dot'} aria-hidden="true"></span>` : null}${children}
  </span>`;
}
