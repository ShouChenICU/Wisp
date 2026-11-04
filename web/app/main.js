// Entry point of the admin UI: registers the pages and renders the shell.
//
// Layout (spec 7.2): below 768 px a top bar plus a bottom tab bar with
// five tabs, the scene and log pages being reached from the system page;
// from 768 px a left sidebar that lists those two on their own. Both are rendered and
// CSS shows the one that fits, so resizing never remounts the page.

import { html, render, useEffect, useRef, useState } from '../vendor/htm-preact-3.1.1.mjs';
import { getInfo } from './api.js';
import { DEFAULT_PATH, currentPath, lookup, navigate, route, useRoute } from './router.js';
import { Toaster } from './components/toast.js';
import { LangSwitch } from './components/lang.js';
import { t, useLang } from './i18n/index.js';
import {
  IconChevronLeft,
  IconChip,
  IconLogout,
  IconLogs,
  IconModel,
  IconOverview,
  IconPackage,
  IconSystem,
  IconWifi,
  WispGlyph,
} from './icons.js';
import { PairPage } from './pages/pair.js';
import { OverviewPage } from './pages/overview.js';
import { DevicesPage } from './pages/devices.js';
import { LogsPage } from './pages/logs.js';
import { NetworkPage } from './pages/network.js';
import { ModelsPage } from './pages/models.js';
import { SystemPage, confirmLogout } from './pages/system.js';
import { ScenesPage } from './pages/scenes.js';

route('/pair', PairPage);
route('/overview', OverviewPage);
route('/devices', DevicesPage);
route('/network', NetworkPage);
route('/models', ModelsPage);
route('/system', SystemPage);
route('/logs', LogsPage);
route('/scenes', ScenesPage);

/**
 * Navigation entries; `wide` ones only appear in the sidebar. `href` (a
 * path) is where the link goes when that is not the page's own path.
 */
const NAV = [
  { path: '/overview', label: 'nav.overview', icon: IconOverview },
  { path: '/devices', label: 'nav.devices', icon: IconChip },
  { path: '/network', label: 'nav.network', icon: IconWifi },
  { path: '/models', href: '/models/llm', label: 'nav.model', icon: IconModel },
  { path: '/system', label: 'nav.system', icon: IconSystem },
  { path: '/scenes', label: 'nav.scenes', icon: IconPackage, wide: true },
  { path: '/logs', label: 'nav.logs', icon: IconLogs, wide: true },
];
const TITLE_KEYS = Object.fromEntries(NAV.map((n) => [n.path, n.label]));
const title = (path) => (TITLE_KEYS[path] ? t(TITLE_KEYS[path]) : 'Wisp');
// Pages the tab bar does not list, and the tab they live under.
const PARENT = { '/logs': '/system', '/scenes': '/system' };

function App() {
  const path = useRoute();
  // Subscribing here re-renders the whole tree on a language switch.
  const lang = useLang();
  const entry = lookup(path);
  // The page's own path: "#/models/asr" is the models page.
  const base = entry ? entry.path : path;

  useEffect(() => {
    if (!entry) navigate(DEFAULT_PATH);
    document.title = path === '/pair' ? `${t('nav.pair')} · Wisp` : `${title(base)} · ${t('app.title')}`;
  }, [path, lang]);

  if (!entry) return null;
  const Page = entry.component;
  if (path === '/pair') {
    return html`<${PairPage} /><${Toaster} />`;
  }
  return html`<${Shell} path=${base}><${Page} key=${base} /></${Shell}><${Toaster} />`;
}

function Shell({ path, children }) {
  const [info, setInfo] = useState(null);
  const first = useRef(true);
  const tab = PARENT[path] || path;

  useEffect(() => {
    getInfo()
      .then(setInfo)
      .catch(() => {});
  }, []);

  // After in-app navigation, move focus to the new page's heading so
  // keyboard and screen-reader users land on the content.
  useEffect(() => {
    if (first.current) {
      first.current = false;
      return;
    }
    window.scrollTo(0, 0);
    const h = document.querySelector('#main .page-title');
    if (h) h.focus({ preventScroll: true });
  }, [path]);

  return html`<div class="shell">
    <a class="skip-link" href="#main" onClick=${(e) => {
      e.preventDefault();
      document.querySelector('#main .page-title')?.focus();
    }}>${t('app.skip')}</a>

    <aside class="sidebar" aria-label=${t('app.nav_label')}>
      <div class="brand">
        <${WispGlyph} size=${34} />
        <div class="brand-text">
          <span class="brand-name">Wisp</span>
          <span class="brand-sub">${t('app.brand_sub')}</span>
        </div>
      </div>
      <nav class="side-nav">
        ${NAV.map(
          (n) => html`<a
            key=${n.path}
            href=${'#' + (n.href || n.path)}
            class=${path === n.path ? 'side-link is-active' : 'side-link'}
            aria-current=${path === n.path ? 'page' : undefined}
          >
            <${n.icon} size=${19} /><span>${t(n.label)}</span>
          </a>`,
        )}
      </nav>
      <div class="side-foot">
        <div class="side-device">
          <span class="side-device-name">${info?.name || 'Wisp'}</span>
          <span class="side-device-fw mono">${info ? 'v' + (info.fw || '—') : ''}</span>
        </div>
        <button
          type="button"
          class="btn btn-icon btn-ghost"
          onClick=${confirmLogout}
          aria-label=${t('app.logout')}
          title=${t('app.logout')}
        >
          <${IconLogout} size=${18} />
        </button>
      </div>
      <${LangSwitch} class="side-lang" />
    </aside>

    <header class="topbar">
      ${PARENT[path]
        ? html`<a class="topbar-back" href=${'#' + PARENT[path]} aria-label=${t('app.back', { page: title(PARENT[path]) })}>
            <${IconChevronLeft} size=${22} />
          </a>`
        : html`<span class="topbar-glyph"><${WispGlyph} size=${26} /></span>`}
      <span class="topbar-title">${title(path)}</span>
      <${LangSwitch} class="topbar-lang" />
    </header>

    <main id="main" class="main">${children}</main>

    <nav class="tabbar" aria-label=${t('app.nav_label')}>
      ${NAV.filter((n) => !n.wide).map(
        (n) => html`<a
          key=${n.path}
          href=${'#' + (n.href || n.path)}
          class=${tab === n.path ? 'tab is-active' : 'tab'}
          aria-current=${path === n.path ? 'page' : undefined}
        >
          <${n.icon} size=${22} /><span>${t(n.label)}</span>
        </a>`,
      )}
    </nav>
  </div>`;
}

async function boot() {
  // Decide where to start before the first paint, so an unpaired browser
  // does not flash an overview full of errors.
  try {
    const info = await getInfo();
    if (info && info.paired === false) navigate('/pair');
    else if (currentPath() === '/pair') navigate(DEFAULT_PATH);
  } catch {
    // Unreachable device: render anyway; the pages show their own errors.
  }
  const root = document.getElementById('app');
  root.replaceChildren();
  render(html`<${App} />`, root);
}

boot();
