// Inline SVG icons (spec 7.1: no icon fonts or remote images). Stroke icons
// on a 24x24 grid that take the surrounding text colour; decorative, so they
// are hidden from assistive technology and the text next to them carries
// the meaning.

import { html, useState } from '../vendor/htm-preact-3.1.1.mjs';

function Svg({ size = 20, children, class: cls }) {
  return html`<svg
    class=${cls ? 'icon ' + cls : 'icon'}
    viewBox="0 0 24 24"
    width=${size}
    height=${size}
    fill="none"
    stroke="currentColor"
    stroke-width="1.8"
    stroke-linecap="round"
    stroke-linejoin="round"
    aria-hidden="true"
    focusable="false"
  >
    ${children}
  </svg>`;
}

const paths = (...ds) => ds.map((d) => html`<path d=${d} />`);

const make =
  (body) =>
  (props = {}) =>
    html`<${Svg} ...${props}>${body()}</${Svg}>`;

export const IconOverview = make(
  () => html`<rect x="3.5" y="3.5" width="7" height="7" rx="2" />
    <rect x="13.5" y="3.5" width="7" height="7" rx="2" />
    <rect x="3.5" y="13.5" width="7" height="7" rx="2" />
    <rect x="13.5" y="13.5" width="7" height="7" rx="2" />`,
);
export const IconChat = make(
  () => html`<path d="M4 5.5h16a1 1 0 0 1 1 1V15a1 1 0 0 1-1 1H9l-4.2 3.6A.8.8 0 0 1 3.5 19V16H4a1 1 0 0 1-1-1V6.5a1 1 0 0 1 1-1z" />
    ${paths('M7.5 10h9', 'M7.5 13.2h6')}`,
);
export const IconChip = make(
  () => html`<rect x="6" y="6" width="12" height="12" rx="2.5" />
    <rect x="9.5" y="9.5" width="5" height="5" rx="1" />
    ${paths('M9 3v3M15 3v3M9 18v3M15 18v3M3 9h3M3 15h3M18 9h3M18 15h3')}`,
);
export const IconWifi = make(
  () => html`${paths('M2.5 9a14 14 0 0 1 19 0', 'M5.5 12.5a9.5 9.5 0 0 1 13 0', 'M8.8 16a4.8 4.8 0 0 1 6.4 0')}
    <circle cx="12" cy="19.2" r="1" fill="currentColor" stroke="none" />`,
);
export const IconModel = make(() =>
  paths(
    'M11 3.5l1.7 4.8 4.8 1.7-4.8 1.7L11 16.5l-1.7-4.8-4.8-1.7 4.8-1.7z',
    'M18 14.5l.8 2.2 2.2.8-2.2.8-.8 2.2-.8-2.2-2.2-.8 2.2-.8z',
  ),
);
export const IconSystem = make(
  () => html`${paths('M4 6.5h9', 'M18 6.5h2', 'M4 12h3', 'M11 12h9', 'M4 17.5h11', 'M19 17.5h1')}
    <circle cx="15.5" cy="6.5" r="2.2" />
    <circle cx="8.5" cy="12" r="2.2" />
    <circle cx="17" cy="17.5" r="2.2" />`,
);
export const IconLogs = make(
  () => html`<rect x="3" y="4" width="18" height="16" rx="3" />
    ${paths('M7.5 9.5l3 2.5-3 2.5', 'M13 15h3.5')}`,
);
export const IconBattery = make(
  () => html`<rect x="2.5" y="7" width="17" height="10" rx="2.5" />
    ${paths('M22 10.5v3')}`,
);
export const IconBolt = make(() => paths('M13 2.5L5 13.5h6.2L10.5 21.5l8.5-11.5h-6.2z'));
export const IconStorage = make(
  () => html`<ellipse cx="12" cy="6" rx="7.5" ry="3" />
    ${paths('M4.5 6v12c0 1.7 3.4 3 7.5 3s7.5-1.3 7.5-3V6', 'M4.5 12c0 1.7 3.4 3 7.5 3s7.5-1.3 7.5-3')}`,
);
export const IconClock = make(
  () => html`<circle cx="12" cy="12" r="8.5" />
    ${paths('M12 7.5V12l3 2')}`,
);
export const IconPackage = make(() =>
  paths('M12 2.8l8 4.5v9.4l-8 4.5-8-4.5V7.3z', 'M4.2 7.4L12 11.8l7.8-4.4', 'M12 11.8v9.4'),
);
export const IconChevronDown = make(() => paths('M6 9l6 6 6-6'));
export const IconChevronRight = make(() => paths('M9 6l6 6-6 6'));
export const IconChevronLeft = make(() => paths('M15 6l-6 6 6 6'));
export const IconPlay = make(() => html`<path d="M7.5 5.2v13.6L18.5 12z" fill="currentColor" />`);
export const IconStop = make(
  () => html`<rect x="6.5" y="6.5" width="11" height="11" rx="2" fill="currentColor" />`,
);
export const IconPause = make(() => paths('M9 5.5v13', 'M15 5.5v13'));
export const IconTrash = make(() =>
  paths('M4 7h16', 'M9.5 7V4.5h5V7', 'M6.2 7l.9 12.5a1.5 1.5 0 0 0 1.5 1.5h6.8a1.5 1.5 0 0 0 1.5-1.5L17.8 7'),
);
export const IconArrowDown = make(() => paths('M12 5v14', 'M6 13l6 6 6-6'));
export const IconRefresh = make(() => paths('M20 12a8 8 0 1 1-2.4-5.7', 'M20 4.5v5h-5'));
export const IconLogout = make(() =>
  paths('M14.5 4h3a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2h-3', 'M10 16.5L5.5 12 10 7.5', 'M5.5 12H15'),
);
export const IconAlert = make(() =>
  paths(
    'M10.3 4.3L2.7 17.6a2 2 0 0 0 1.7 3h15.2a2 2 0 0 0 1.7-3L13.7 4.3a2 2 0 0 0-3.4 0z',
    'M12 9.5v4',
    'M12 17h.01',
  ),
);
export const IconCheck = make(() => paths('M5 12.5l4.5 4.5L19 7.5'));
export const IconInfo = make(
  () => html`<circle cx="12" cy="12" r="8.5" />
    ${paths('M12 11v5', 'M12 8h.01')}`,
);
export const IconLock = make(
  () => html`<rect x="5" y="10.5" width="14" height="10" rx="2.5" />
    ${paths('M8.5 10.5V7.5a3.5 3.5 0 0 1 7 0v3')}`,
);
export const IconSearch = make(
  () => html`<circle cx="11" cy="11" r="6.5" />
    ${paths('M20 20l-4.2-4.2')}`,
);
export const IconGlobe = make(
  () => html`<circle cx="12" cy="12" r="8.5" />
    ${paths('M3.5 12h17', 'M12 3.5c2.4 2.4 3.6 5.2 3.6 8.5s-1.2 6.1-3.6 8.5c-2.4-2.4-3.6-5.2-3.6-8.5s1.2-6.1 3.6-8.5z')}`,
);
export const IconClose = make(() => paths('M6 6l12 12', 'M18 6L6 18'));
export const IconDisplay = make(
  () => html`<rect x="3" y="4" width="18" height="12.5" rx="2.5" />
    ${paths('M8.5 20.5h7', 'M12 16.5v4')}`,
);
export const IconTouch = make(
  () => html`<circle cx="12" cy="10" r="2.5" />
    ${paths('M7.5 14.5a6 6 0 1 1 9 0', 'M12 15.5v5')}`,
);
export const IconImu = make(
  () => html`<circle cx="12" cy="12" r="1.8" />
    ${paths('M12 10.2V3.5', 'M13.6 12.9l6 3.4', 'M10.4 12.9l-6 3.4', 'M10 5.5L12 3.5l2 2')}`,
);
export const IconButton = make(
  () => html`<circle cx="12" cy="12" r="8.5" />
    <circle cx="12" cy="12" r="3.5" />`,
);
export const IconSpeaker = make(() =>
  paths('M4 9.5v5h3.5L12.5 19V5L7.5 9.5z', 'M16 9a4.2 4.2 0 0 1 0 6', 'M18.8 6.5a8 8 0 0 1 0 11'),
);
export const IconMic = make(
  () => html`<rect x="9" y="3" width="6" height="11" rx="3" />
    ${paths('M5.5 11a6.5 6.5 0 0 0 13 0', 'M12 17.5V21')}`,
);

export const IconPower = make(() => paths('M12 3.5v8', 'M7 6.4a7.5 7.5 0 1 0 10 0'));
export const IconReset = make(() => paths('M4 12a8 8 0 1 0 2.4-5.7', 'M4 4.5v5h5'));
export const IconUpload = make(() =>
  paths('M12 15V4.5', 'M7.5 9L12 4.5 16.5 9', 'M4.5 15v3.5A1.5 1.5 0 0 0 6 20h12a1.5 1.5 0 0 0 1.5-1.5V15'),
);
export const IconSun = make(
  () => html`<circle cx="12" cy="12" r="3.8" />
    ${paths('M12 2.5v2', 'M12 19.5v2', 'M2.5 12h2', 'M19.5 12h2', 'M5.3 5.3l1.4 1.4', 'M17.3 17.3l1.4 1.4', 'M5.3 18.7l1.4-1.4', 'M17.3 6.7l1.4-1.4')}`,
);
export const IconMoon = make(() => paths('M19.5 14.5A8 8 0 0 1 9.5 4.5a8 8 0 1 0 10 10z'));
export const IconKey = make(
  () => html`<circle cx="8" cy="15" r="4.2" />
    ${paths('M11 12l8.5-8.5', 'M16 7l2.5 2.5', 'M14 9l1.8 1.8')}`,
);
export const IconPlus = make(() => paths('M12 5v14', 'M5 12h14'));
export const IconEye = make(
  () => html`${paths('M2.5 12S6 5.5 12 5.5 21.5 12 21.5 12 18 18.5 12 18.5 2.5 12 2.5 12z')}
    <circle cx="12" cy="12" r="3" />`,
);
export const IconEyeOff = make(() =>
  paths(
    'M4 4l16 16',
    'M10 6a9.6 9.6 0 0 1 2-.5c6 0 9.5 6.5 9.5 6.5a17 17 0 0 1-2.6 3.4',
    'M6.6 7.6A16.6 16.6 0 0 0 2.5 12S6 18.5 12 18.5a9 9 0 0 0 4.4-1.1',
    'M9.9 9.9a3 3 0 0 0 4.2 4.2',
  ),
);
export const IconLink = make(() =>
  paths('M10 14a4.5 4.5 0 0 0 6.4 0l3-3a4.5 4.5 0 0 0-6.4-6.4l-1 1', 'M14 10a4.5 4.5 0 0 0-6.4 0l-3 3a4.5 4.5 0 0 0 6.4 6.4l1-1'),
);
export const IconMore = make(
  () => html`<circle cx="5" cy="12" r="1.7" fill="currentColor" stroke="none" />
    <circle cx="12" cy="12" r="1.7" fill="currentColor" stroke="none" />
    <circle cx="19" cy="12" r="1.7" fill="currentColor" stroke="none" />`,
);
export const IconBrowser = make(
  () => html`<rect x="3" y="4.5" width="18" height="15" rx="2.5" />
    ${paths('M3 9h18', 'M6.5 6.8h.01', 'M9 6.8h.01')}`,
);

let glyphCount = 0;

/**
 * The wisp itself (小鬼火): a small flame with two eyes. Filled, not
 * stroked, and the one place the accent colour glows.
 */
export function WispGlyph({ size = 40, class: cls }) {
  // Each instance needs its own gradient id: url(#id) resolves to the first
  // match in the document, and when that copy sits in a display:none
  // subtree (the sidebar on a phone) Chrome paints the flame empty.
  const [fill] = useState(() => 'wisp-fill-' + ++glyphCount);
  return html`<svg
    class=${cls ? 'wisp-glyph ' + cls : 'wisp-glyph'}
    viewBox="0 0 48 48"
    width=${size}
    height=${size}
    aria-hidden="true"
    focusable="false"
  >
    <defs>
      <linearGradient id=${fill} x1="0" y1="0" x2="0" y2="1">
        <stop offset="0" stop-color="#d9f0ff" />
        <stop offset="0.45" stop-color="#8fcbff" />
        <stop offset="1" stop-color="#3d8fe0" />
      </linearGradient>
    </defs>
    <path
      d="M24 4.5c2.2 5.4 7.6 8.6 10.6 13.6 2.1 3.4 3.4 7 3.4 10.6C38 36.6 31.8 43 24 43S10 36.6 10 28.7c0-5.1 2.6-9.1 5.6-12.2.3 3.2 1.7 5.6 3.9 6.6-.9-7.2 1.3-13.4 4.5-18.6z"
      fill=${'url(#' + fill + ')'}
    />
    <ellipse cx="19.6" cy="30" rx="2" ry="2.7" fill="#0b1626" />
    <ellipse cx="28.4" cy="30" rx="2" ry="2.7" fill="#0b1626" />
    <ellipse cx="20.3" cy="29" rx="0.6" ry="0.8" fill="#ffffff" />
    <ellipse cx="29.1" cy="29" rx="0.6" ry="0.8" fill="#ffffff" />
  </svg>`;
}

/** The icon for a device class, as named by the device registry. */
export function classIcon(cls) {
  switch (cls) {
    case 'display':
      return IconDisplay;
    case 'touch':
      return IconTouch;
    case 'imu':
      return IconImu;
    case 'power':
      return IconBattery;
    case 'clock':
      return IconClock;
    case 'button':
      return IconButton;
    case 'audio_out':
      return IconSpeaker;
    case 'audio_in':
      return IconMic;
    case 'storage':
      return IconStorage;
    case 'network':
      return IconWifi;
    default:
      return IconChip;
  }
}
