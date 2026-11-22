// "Now playing" (media spec 3.7): the current track's title and artist with
// previous / pause-resume / next, shown while the player is not idle.
//
// The state comes from GET /media/player; every media.player and
// media.library event, a dropped-messages notice and each (re)connect only
// mean "ask again" -- consecutive events may carry equal payloads, so none
// is compared with the last one. A 503 (no audio output for music) hides
// the card. Controls post a command and then ask again, so the card also
// follows them without a live socket. The title, artist and album are the
// file's own tags: rendered as text only.

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeMediaError } from '../api.js';
import { t } from '../i18n/index.js';
import { subscribe } from '../ws.js';
import { useNow, useVisible } from '../util.js';
import { Badge } from './badge.js';
import { Card } from './card.js';
import { toast } from './toast.js';
import { IconMusic, IconPause, IconPlay, IconSkipBack, IconSkipForward } from '../icons.js';

/**
 * @typedef {Object} PlayerState  GET /media/player
 * @property {boolean} available    a card is mounted
 * @property {boolean} scanning
 * @property {number} track_count
 * @property {'idle'|'playing'|'paused'} status
 * @property {'none'|'user'|'system'} pause_reason
 * @property {{id: string, title: string, artist: string, album: string,
 *             duration: number}|null} track
 * @property {number} position      seconds
 * @property {number} index
 * @property {number} queue_length
 * @property {'sequence'|'repeat_all'|'repeat_one'|'shuffle'} mode
 */

/** Seconds as "m:ss" or "h:mm:ss". */
export function formatDuration(s) {
  if (!Number.isFinite(s) || s < 0) return '';
  const total = Math.floor(s);
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const sec = String(total % 60).padStart(2, '0');
  return h > 0 ? `${h}:${String(m).padStart(2, '0')}:${sec}` : `${m}:${sec}`;
}

/** The last segment of a library path, for a track without a title tag. */
export function baseName(path) {
  const name = String(path || '').split('/').pop() || '';
  const dot = name.lastIndexOf('.');
  return dot > 0 ? name.slice(0, dot) : name;
}

/**
 * The card; renders nothing while the player is idle, missing or not yet
 * known.
 */
export function NowPlayingCard() {
  const visible = useVisible();
  const [state, setState] = useState(/** @type {PlayerState|null} */ (null));
  const [at, setAt] = useState(0);
  const [busy, setBusy] = useState(false);
  const alive = useRef(true);
  const inFlight = useRef(false);
  const again = useRef(false);

  async function load() {
    if (inFlight.current) {
      // An answer already on its way may predate what prompted this call.
      again.current = true;
      return;
    }
    inFlight.current = true;
    again.current = false;
    try {
      const r = await api('GET', '/media/player');
      if (!alive.current) return;
      setState(r && typeof r === 'object' ? r : null);
      setAt(Date.now());
    } catch (e) {
      // No player (503) or no such route (older firmware): no card. Any
      // other failure keeps what was shown; the next event asks again.
      if (alive.current && (e.status === 503 || e.status === 404)) setState(null);
    } finally {
      inFlight.current = false;
    }
    if (again.current && alive.current) load();
  }

  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
    };
  }, []);

  useEffect(() => {
    if (!visible) return undefined;
    load();
    const unsubscribe = subscribe(
      ['events'],
      (msg) => {
        if (msg.t === 'dropped') load();
        else if (msg.t === 'event' && (msg.topic === 'media.player' || msg.topic === 'media.library')) load();
      },
      {
        onState: (st) => {
          if (st === 'open') load();
        },
      },
    );
    return unsubscribe;
  }, [visible]);

  async function control(action) {
    setBusy(true);
    try {
      await api('POST', '/media/player/' + action);
    } catch (e) {
      if (e.status !== 401) toast(describeMediaError(e), 'err');
    } finally {
      if (alive.current) setBusy(false);
    }
    load();
  }

  if (!state || state.status === 'idle' || !state.track) return null;
  const track = state.track;
  const playing = state.status === 'playing';
  const title = track.title || baseName(track.id);
  const badge = playing
    ? html`<${Badge} tone="ok" dot pulse>${t('np.playing')}</${Badge}>`
    : html`<${Badge} tone=${state.pause_reason === 'system' ? 'warn' : 'muted'}>
        ${state.pause_reason === 'system' ? t('np.paused_system') : t('np.paused')}
      </${Badge}>`;

  return html`<${Card} title=${t('np.title')} icon=${IconMusic} actions=${badge} class="np-card">
    <div class="np-body">
      <div class="np-main">
        <span class="np-title" title=${title}>${title}</span>
        <span class="np-artist" title=${track.artist || ''}>${track.artist || t('media.unknown_artist')}</span>
      </div>
      <div class="np-controls">
        <button
          type="button"
          class="btn btn-icon btn-secondary"
          disabled=${busy}
          onClick=${() => control('prev')}
          aria-label=${t('np.prev')}
          title=${t('np.prev')}
        >
          <${IconSkipBack} size=${18} />
        </button>
        <button
          type="button"
          class="btn btn-icon btn-primary"
          disabled=${busy}
          onClick=${() => control(playing ? 'pause' : 'resume')}
          aria-label=${playing ? t('np.pause') : t('np.resume')}
          title=${playing ? t('np.pause') : t('np.resume')}
        >
          ${playing ? html`<${IconPause} size=${18} />` : html`<${IconPlay} size=${18} />`}
        </button>
        <button
          type="button"
          class="btn btn-icon btn-secondary"
          disabled=${busy}
          onClick=${() => control('next')}
          aria-label=${t('np.next')}
          title=${t('np.next')}
        >
          <${IconSkipForward} size=${18} />
        </button>
      </div>
    </div>
    <${Progress} state=${state} at=${at} />
  </${Card}>`;
}

// The position, advanced locally while playing: the device reports it only
// with a state change, not every second.
function Progress({ state, at }) {
  const playing = state.status === 'playing';
  const now = useNow(playing ? 1000 : 60000);
  const duration = Number(state.track?.duration) || 0;
  let pos = Number(state.position) || 0;
  if (playing) pos += Math.max(0, (now - at) / 1000);
  if (duration > 0) pos = Math.min(pos, duration);
  const pct = duration > 0 ? (pos / duration) * 100 : 0;
  const queue =
    state.queue_length > 1 ? t('np.queue', { index: state.index + 1, count: state.queue_length }) : '';
  return html`<div class="np-progress">
    ${duration > 0
      ? html`<div class="meter tone-accent" role="presentation"><div class="meter-fill np-fill" style=${{ width: pct + '%' }}></div></div>`
      : null}
    <div class="np-times">
      <span class="mono">${formatDuration(pos)}${duration > 0 ? ' / ' + formatDuration(duration) : ''}</span>
      ${queue ? html`<span>${queue}</span>` : null}
    </div>
  </div>`;
}
