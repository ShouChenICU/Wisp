// Interface language for the admin UI: Chinese, Japanese and English.
//
// How to use it (every page, including ones added later):
//   - Never put user-visible text in a component. Add a key to all three
//     dictionaries (zh.js, ja.js, en.js -- flat `'key': 'text'` maps, one
//     entry per line; tools/check_i18n.py fails CI when their key sets
//     differ) and render `t('key')`, or `t('key', {name})` for text with
//     `{name}` placeholders.
//   - Keys are grouped by a dotted prefix: `nav.*`, `common.*`, one prefix
//     per page, and `err.<code>` for the API's error codes (see
//     describeError() in api.js -- the server's English message is never
//     the primary text).
//   - Numbers, sizes and durations go through Intl with locale(), not
//     hand-made unit strings.
//   - A component that renders text calls useLang() (the shell already
//     does, and Preact re-renders its children), so switching language
//     re-renders in place without losing page state. Text built outside
//     render (toasts, imperative DOM) is translated when it is created.
//
// The first visit picks the first of navigator.languages that is zh*, ja*
// or en* (English otherwise); a choice made in the language switcher is
// remembered in localStorage. A missing key falls back to English, then to
// the key itself; with localStorage "wisp.debug" = "1" each missing key is
// reported once with console.warn.

import { useEffect, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import zh from './zh.js';
import ja from './ja.js';
import en from './en.js';

const DICTS = { zh, ja, en };

/** The languages offered, each named in its own language. */
export const LANGS = [
  { id: 'zh', label: '中文', html: 'zh-CN' },
  { id: 'ja', label: '日本語', html: 'ja' },
  { id: 'en', label: 'English', html: 'en' },
];

const STORE_KEY = 'wisp.lang';
const listeners = new Set();
const warned = new Set();

function readStore(key) {
  try {
    return localStorage.getItem(key);
  } catch {
    return null;
  }
}

function detect() {
  const saved = readStore(STORE_KEY);
  if (saved && DICTS[saved]) return saved;
  const prefs = navigator.languages && navigator.languages.length ? navigator.languages : [navigator.language || ''];
  for (const tag of prefs) {
    const base = String(tag).toLowerCase().split('-')[0];
    if (DICTS[base]) return base;
  }
  return 'en';
}

const debug = readStore('wisp.debug') === '1';
let lang = detect();

function applyHtmlLang() {
  document.documentElement.lang = LANGS.find((l) => l.id === lang).html;
}
applyHtmlLang();

/** The current language id: "zh", "ja" or "en". */
export function getLang() {
  return lang;
}

/** The BCP 47 locale for Intl formatting in the current language. */
export function locale() {
  return LANGS.find((l) => l.id === lang).html;
}

export function setLang(next) {
  if (!DICTS[next] || next === lang) return;
  lang = next;
  try {
    localStorage.setItem(STORE_KEY, next);
  } catch {
    // Private mode or storage disabled: the choice lasts for this visit.
  }
  applyHtmlLang();
  for (const fn of [...listeners]) fn(next);
}

/** Re-renders the caller when the language changes; returns the language. */
export function useLang() {
  const [current, setCurrent] = useState(lang);
  useEffect(() => {
    listeners.add(setCurrent);
    return () => listeners.delete(setCurrent);
  }, []);
  return current;
}

/** True when `key` exists (in English, the reference dictionary). */
export function has(key) {
  return Object.prototype.hasOwnProperty.call(en, key);
}

/**
 * Translates `key`, replacing each `{name}` with params[name].
 *
 * @param {string} key
 * @param {Object<string, any>} [params]
 * @returns {string}
 */
export function t(key, params) {
  let text = DICTS[lang][key];
  if (text === undefined) {
    if (debug && !warned.has(lang + ':' + key)) {
      warned.add(lang + ':' + key);
      console.warn(`i18n: missing "${key}" in ${lang}`);
    }
    text = en[key] ?? key;
  }
  if (!params) return text;
  return text.replace(/\{(\w+)\}/g, (m, name) => (name in params ? String(params[name]) : m));
}
