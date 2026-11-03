// Placeholder pages for the routes whose real pages are not built yet, so
// the navigation is complete and the real page only has to replace the
// route registration.

import { html } from '../../vendor/htm-preact-3.1.1.mjs';
import { logout } from '../api.js';
import { t } from '../i18n/index.js';
import { confirmDialog } from '../components/dialog.js';
import { IconChevronRight, IconLogout, IconLogs } from '../icons.js';

function Soon({ title, sub, icon: Icon, children }) {
  return html`<div class="page">
    <header class="page-head">
      <div>
        <h1 class="page-title" tabindex="-1">${title}</h1>
        <p class="page-sub">${sub}</p>
      </div>
    </header>
    ${children}
    <div class="empty">
      <span class="empty-icon"><${Icon} size=${28} /></span>
      <p class="empty-title">${t('stub.soon_title')}</p>
      <p class="muted">${t('stub.soon_body')}</p>
    </div>
  </div>`;
}

export const networkStub = (Icon) => () =>
  html`<${Soon} title=${t('nav.network')} sub=${t('stub.network_sub')} icon=${Icon} />`;

export const modelStub = (Icon) => () =>
  html`<${Soon} title=${t('nav.model')} sub=${t('stub.model_sub')} icon=${Icon} />`;

/** Asks first, then revokes this browser's pairing. */
export async function confirmLogout() {
  const ok = await confirmDialog({
    title: t('logout.title'),
    body: t('logout.body'),
    okText: t('logout.confirm'),
    danger: true,
  });
  if (ok) await logout().catch(() => {});
}

// The system page is also where the phone layout keeps the way to the logs.
export const systemStub = (Icon) => () =>
  html`<${Soon} title=${t('nav.system')} sub=${t('stub.system_sub')} icon=${Icon}>
    <ul class="card list-card link-list">
      <li>
        <a class="link-row" href="#/logs">
          <span class="dev-icon tone-accent"><${IconLogs} size=${18} /></span>
          <span class="dev-main">
            <span class="row-title">${t('nav.logs')}</span>
            <span class="row-sub">${t('stub.logs_desc')}</span>
          </span>
          <${IconChevronRight} size=${18} />
        </a>
      </li>
      <li>
        <button type="button" class="link-row" onClick=${confirmLogout}>
          <span class="dev-icon tone-err"><${IconLogout} size=${18} /></span>
          <span class="dev-main">
            <span class="row-title">${t('app.logout')}</span>
            <span class="row-sub">${t('logout.desc')}</span>
          </span>
        </button>
      </li>
    </ul>
  </${Soon}>`;
