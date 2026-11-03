// Language switcher: a native <select>, so it is keyboard- and
// screen-reader-friendly everywhere, including on the pairing page.

import { html } from '../../vendor/htm-preact-3.1.1.mjs';
import { LANGS, setLang, t, useLang } from '../i18n/index.js';
import { IconGlobe } from '../icons.js';

export function LangSwitch({ class: cls }) {
  const lang = useLang();
  return html`<label class=${cls ? 'lang-switch ' + cls : 'lang-switch'} title=${t('lang.label')}>
    <${IconGlobe} size=${16} />
    <span class="sr-only">${t('lang.label')}</span>
    <select value=${lang} onChange=${(e) => setLang(e.currentTarget.value)}>
      ${LANGS.map((l) => html`<option key=${l.id} value=${l.id} lang=${l.html}>${l.label}</option>`)}
    </select>
  </label>`;
}
