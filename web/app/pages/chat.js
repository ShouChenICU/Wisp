// The "对话" page (agent-core spec 7.3, MVP spec 3): the web half of the
// one device-wide conversation with the Agent.
//
// State comes from GET /conversation on mount, then follows the device's
// "agent" WebSocket frames (spec 7.2) in place: "delta" accumulates into a
// not-yet-settled reply shown at the bottom of the list, "msg" inserts the
// settled message (and clears that buffer -- the server flushes any
// pending delta right before it sends "msg", so nothing is lost), "state"
// and "confirm" update the strip above the composer, and "cleared"
// resolves the clear flow below. A reconnect (onState "open", the same
// signal a dropped WS frame or a missed one would need to recover from)
// or a frame reporting drops just re-fetches, which is simpler than
// reasoning about what exactly was missed and also covers a changed
// epoch or boot id (MVP spec: cache reset on either).
//
// Messages with "hidden" (compaction summaries, turn markers) are never
// shown as dialogue; a "dropped" summary (the hidden one with no text)
// shows one muted line instead, meaning some earlier turns were summarized
// away without being kept at all.
//
// A tool call's one-line summary and a message's text/args/result are cut
// to 200 code points by the server; expanding a tool row fetches the
// uncut message(s) with GET /messages/{seq} -- only possible for the
// current conversation, so archived tool calls stay at whatever length
// GET /archive/{n} already gave them.

import { html, useEffect, useRef, useState } from '../../vendor/htm-preact-3.1.1.mjs';
import { api, describeError, errorDetail } from '../api.js';
import { t, locale } from '../i18n/index.js';
import { subscribe } from '../ws.js';
import { Badge } from '../components/badge.js';
import { confirmDialog } from '../components/dialog.js';
import { toast } from '../components/toast.js';
import { IconAlert, IconChevronLeft, IconChevronRight, IconMic, IconMore, IconStop } from '../icons.js';
import { renderMarkdown, stripSay } from '../md.js';
import { formatCount } from '../util.js';

// A socket (re)opening right after a load just saw the current state.
const RELOAD_GAP_MS = 2000;
// Turn states (agent-core spec 3.8) in which a turn is running: the
// composer's button is "stop" and a new message only queues.
const RUNNING_STATES = new Set(['thinking', 'tool', 'output', 'waiting_confirm']);
const MAX_MESSAGE_CHARS = 4000;

/**
 * @typedef {Object} ToolCall  an assistant message's call record
 * @property {string} id
 * @property {string} name
 * @property {string} args
 * @property {number} args_len
 *
 * @typedef {Object} ChatMessage  agent-core spec 7.1's message shape
 * @property {number} seq
 * @property {number} ts
 * @property {number} turn
 * @property {'user'|'assistant'|'tool'|'note'|'summary'|'marker'} role
 * @property {string} [text]
 * @property {string} [source]
 * @property {ToolCall[]} [calls]
 * @property {string} [call_id]
 * @property {string} [name]
 * @property {boolean} [ok]
 * @property {string} [result]
 * @property {number} [result_len]
 * @property {boolean} [hidden]
 * @property {boolean} [dropped]
 *
 * @typedef {Object} Conversation  GET /conversation
 * @property {number} epoch
 * @property {string} state
 * @property {string|null} tool
 * @property {{name: string, model: string, vision: boolean}|null} model
 * @property {number|null} turn
 * @property {boolean} queued
 * @property {string|null} confirm
 * @property {number} total
 * @property {ChatMessage[]} messages
 */

export function ChatPage() {
  const [conv, setConv] = useState(/** @type {Conversation|null} */ (null));
  const [loadError, setLoadError] = useState(null);
  const [draft, setDraft] = useState('');
  const [sending, setSending] = useState(false);
  const [stopping, setStopping] = useState(false);
  const [composerError, setComposerError] = useState('');
  const [clearPending, setClearPending] = useState(false);
  const [menuOpen, setMenuOpen] = useState(false);
  const [view, setView] = useState({ kind: 'current' }); // {kind:'current'|'archives'|'archive', n?}
  const [streamText, setStreamText] = useState('');
  const [expanded, setExpanded] = useState(/** @type {Set<number>} */ (new Set()));
  const [fullCache, setFullCache] = useState(/** @type {Map<number, any>} */ (new Map()));
  const alive = useRef(true);
  const inFlight = useRef(false);
  const again = useRef(false);
  const lastDone = useRef(0);
  const scrollRef = useRef(null);
  const stickToBottom = useRef(true);
  // onAgentFrame is wired up once (below) and so always closes over the
  // clearPending of that first render; a ref gives it the current value.
  // The device also sends "cleared" for a plain new conversation (spec
  // 7.2: it carries the epoch bump both share), so only clearAll's own
  // request shows the clear-specific toast.
  const clearPendingRef = useRef(false);
  // seq restarts at 1 in every conversation (spec 7.1), so fullCache and
  // expanded -- both keyed by seq -- must not survive an epoch change or
  // they would show a stale seq-3's content under the new conversation's
  // seq-3. null means "no GET /conversation has landed yet", so the first
  // one never counts as a change. A reconnect's catchUp() also calls
  // load(), so this covers a changed boot_id (epoch restarts at 1 on
  // every boot) the same way, without a separate check.
  const lastEpoch = useRef(/** @type {number|null} */ (null));

  function resetMessageCaches() {
    setFullCache(new Map());
    setExpanded(new Set());
  }

  async function load() {
    if (inFlight.current) {
      again.current = true;
      return;
    }
    inFlight.current = true;
    again.current = false;
    try {
      const c = await api('GET', '/agent/conversation');
      if (!alive.current) return;
      if (lastEpoch.current !== null && c.epoch !== lastEpoch.current) resetMessageCaches();
      lastEpoch.current = c.epoch;
      // Idle with nothing to confirm means no clear can genuinely be
      // in flight on the device, so a clearPendingRef stuck true (its
      // "cleared" frame lost to a dropped WS message, say) cannot
      // otherwise unstick the "Clear all" menu item.
      if (c.confirm == null && c.state === 'idle' && clearPendingRef.current) {
        clearPendingRef.current = false;
        setClearPending(false);
      }
      setConv(c);
      setStreamText('');
      setLoadError(null);
    } catch (e) {
      if (alive.current && e.status !== 401) setLoadError(e);
    } finally {
      inFlight.current = false;
      lastDone.current = Date.now();
    }
    if (again.current && alive.current) load();
  }

  // Only for onState("open"): a reconnect's own hello/replay already
  // brings the socket current, so a catch-up load right after is only
  // insurance and can wait out a load already in flight or just done.
  function catchUp() {
    if (inFlight.current || Date.now() - lastDone.current < RELOAD_GAP_MS) return;
    load();
  }

  function onAgentFrame(msg) {
    if (msg.t === 'dropped') {
      // Unlike catchUp(), this must never be skipped: the device just
      // told us a frame (maybe a state change we need) was lost, so
      // throttling or an in-flight load here could leave the page stuck.
      // load() itself coalesces -- a load already running sets `again`
      // and runs once more right after -- so calling it unconditionally
      // is still at most one extra request.
      load();
      return;
    }
    if (msg.t !== 'agent') return;
    switch (msg.k) {
      case 'delta':
        setStreamText((s) => s + msg.text);
        break;
      case 'state':
        setConv((c) => (c ? { ...c, state: msg.state, tool: msg.state === 'tool' ? msg.tool : null } : c));
        break;
      case 'confirm':
        setConv((c) => (c ? { ...c, confirm: msg.text ?? null } : c));
        break;
      case 'msg':
        setStreamText('');
        setConv((c) => {
          if (!c) return c;
          const messages = c.messages.some((m) => m.seq === msg.msg.seq)
            ? c.messages.map((m) => (m.seq === msg.msg.seq ? msg.msg : m))
            : [...c.messages, msg.msg].sort((a, b) => a.seq - b.seq);
          return { ...c, messages, total: Math.max(c.total, messages.length) };
        });
        break;
      case 'cleared': {
        const wasClearing = clearPendingRef.current;
        clearPendingRef.current = false;
        setClearPending(false);
        if (msg.ok) {
          if (wasClearing) toast(t('chat.cleared'), 'ok');
          // Don't wait for load() to notice the epoch moved: a tool row
          // left expanded from the old conversation would otherwise flash
          // its stale content for one more render.
          resetMessageCaches();
          load();
        } else if (wasClearing) {
          toast(t('chat.clear_denied'), 'warn');
        }
        break;
      }
      default:
        break;
    }
  }

  useEffect(() => {
    alive.current = true;
    return () => {
      alive.current = false;
    };
  }, []);

  useEffect(() => {
    load();
    const unsubscribe = subscribe(['agent'], onAgentFrame, {
      onState: (st) => {
        if (st === 'open') catchUp();
      },
    });
    return unsubscribe;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Keeps the newest message in view unless the user scrolled up to read
  // something older.
  useEffect(() => {
    const el = scrollRef.current;
    if (el && stickToBottom.current) el.scrollTop = el.scrollHeight;
  }, [conv?.messages, streamText]);

  function onScroll(e) {
    const el = e.currentTarget;
    stickToBottom.current = el.scrollHeight - el.scrollTop - el.clientHeight < 48;
  }

  function onFullLoaded(seq, full) {
    setFullCache((m) => {
      const next = new Map(m);
      next.set(seq, full);
      return next;
    });
  }

  function toggleExpanded(seq) {
    setExpanded((s) => {
      const next = new Set(s);
      if (next.has(seq)) next.delete(seq);
      else next.add(seq);
      return next;
    });
  }

  async function send() {
    const text = draft.trim();
    if (!text) return;
    if (Array.from(text).length > MAX_MESSAGE_CHARS) {
      setComposerError(t('err.too_long'));
      return;
    }
    setSending(true);
    setComposerError('');
    try {
      const r = await api('POST', '/agent/messages', { text });
      if (!alive.current) return;
      setDraft('');
      setConv((c) => (c ? { ...c, queued: !!r?.queued } : c));
      if (r?.queued) toast(t('chat.queued'), 'accent');
    } catch (e) {
      if (!alive.current || e.status === 401) return;
      setComposerError(describeError(e));
    } finally {
      if (alive.current) setSending(false);
    }
  }

  async function stop() {
    setStopping(true);
    try {
      await api('POST', '/agent/stop');
    } catch (e) {
      if (e.status !== 401) toast(describeError(e), 'err');
    } finally {
      if (alive.current) setStopping(false);
    }
  }

  async function newConversation() {
    setMenuOpen(false);
    try {
      await api('POST', '/agent/conversation/new');
      setView({ kind: 'current' });
      toast(t('chat.new_started'), 'ok');
      load();
    } catch (e) {
      if (e.status !== 401) toast(describeError(e), 'err');
    }
  }

  async function clearAll() {
    setMenuOpen(false);
    const ok = await confirmDialog({
      title: t('chat.clear_title'),
      body: t('chat.clear_body'),
      okText: t('chat.clear_confirm'),
      danger: true,
    });
    if (!ok) return;
    // Set before the request, not after: the device's "cleared" WS frame
    // (the denial from a device-side timeout, say) can arrive while this
    // await is still pending, and onAgentFrame must see it as ours.
    clearPendingRef.current = true;
    setClearPending(true);
    try {
      await api('DELETE', '/agent/conversations');
    } catch (e) {
      clearPendingRef.current = false;
      setClearPending(false);
      if (e.status !== 401) toast(describeError(e), 'err');
    }
  }

  if (loadError && !conv) {
    return html`<div class="page page-chat">
      <${Head} menuDisabled=${true} />
      <div class="chat-panel chat-panel-error">
        <${IconAlert} size=${22} />
        <p class="chat-error-title">${t('chat.load_failed')}</p>
        <p class="muted">${describeError(loadError)}</p>
        ${errorDetail(loadError) ? html`<p class="err-detail mono">${errorDetail(loadError)}</p>` : null}
        <button type="button" class="btn btn-secondary" onClick=${load}>${t('common.retry')}</button>
      </div>
    </div>`;
  }

  if (!conv) {
    return html`<div class="page page-chat">
      <${Head} menuDisabled=${true} />
      <div class="chat-panel" aria-busy="true" aria-label=${t('common.loading')}>
        <div class="chat-scroll">
          <div class="skel skel-line" style="width:60%"></div>
          <div class="skel skel-line" style="width:40%;margin-left:auto"></div>
          <div class="skel skel-line" style="width:70%"></div>
        </div>
      </div>
    </div>`;
  }

  const running = RUNNING_STATES.has(conv.state);

  return html`<div class="page page-chat">
    <${Head}
      menuDisabled=${false}
      menuOpen=${menuOpen}
      onToggleMenu=${() => setMenuOpen((o) => !o)}
      onClose=${() => setMenuOpen(false)}
      onNew=${newConversation}
      onHistory=${() => {
        setMenuOpen(false);
        setView({ kind: 'archives' });
      }}
      onClear=${clearAll}
      clearDisabled=${clearPending}
    />
    <${ModelBar} model=${conv.model} />
    ${conv.confirm ? html`<div class="alert tone-warn chat-confirm" role="alert">
          <${IconAlert} size=${18} />
          <div><strong>${t('chat.confirm_title')}</strong><br />${conv.confirm}</div>
        </div>`
      : null}

    ${view.kind === 'archives'
      ? html`<${ArchiveList} onOpen=${(n) => setView({ kind: 'archive', n })} onBack=${() => setView({ kind: 'current' })} />`
      : view.kind === 'archive'
        ? html`<${ArchiveView} n=${view.n} onBack=${() => setView({ kind: 'archives' })} />`
        : html`
            <div class="chat-panel">
              <div class="chat-scroll" ref=${scrollRef} onScroll=${onScroll}>
                ${conv.messages.length === 0 && !streamText
                  ? html`<div class="empty chat-empty">
                      <p class="empty-title">${t('chat.empty_title')}</p>
                      <p class="muted">${t('chat.empty_hint')}</p>
                    </div>`
                  : conv.messages.map((m) => html`<${MessageRow}
                        key=${m.seq}
                        m=${m}
                        messages=${conv.messages}
                        expanded=${expanded.has(m.seq)}
                        onToggle=${toggleExpanded}
                        fullCache=${fullCache}
                        onFullLoaded=${onFullLoaded}
                      />`)}
                ${streamText
                  ? html`<div class="msg msg-assistant msg-streaming md" dangerouslySetInnerHTML=${{ __html: renderMarkdown(stripSay(streamText)) }}></div>`
                  : null}
              </div>
              ${running
                ? html`<p class="chat-status muted" role="status">
                    ${conv.state === 'tool' && conv.tool
                      ? t('chat.tool_running', { name: conv.tool })
                      : conv.state === 'thinking'
                        ? t('chat.thinking')
                        : conv.state === 'waiting_confirm'
                          ? t('chat.confirm_title')
                          : ''}
                  </p>`
                : null}
              ${conv.queued ? html`<p class="chat-status muted">${t('chat.has_queued')}</p>` : null}
              <form
                class="chat-composer"
                onSubmit=${(e) => {
                  e.preventDefault();
                  send();
                }}
              >
                <textarea
                  class="input chat-input"
                  rows="1"
                  placeholder=${t('chat.placeholder')}
                  value=${draft}
                  disabled=${sending}
                  onInput=${(e) => {
                    setDraft(e.currentTarget.value);
                    setComposerError('');
                  }}
                  onKeyDown=${(e) => {
                    if (e.key === 'Enter' && !e.shiftKey) {
                      e.preventDefault();
                      send();
                    }
                  }}
                  aria-invalid=${composerError ? 'true' : 'false'}
                  aria-label=${t('chat.placeholder')}
                />
                ${running
                  ? html`<button type="button" class="btn btn-danger chat-send" onClick=${stop} disabled=${stopping}>
                      <${IconStop} size=${16} />${t('chat.stop')}
                    </button>`
                  : html`<button type="submit" class="btn btn-primary chat-send" disabled=${sending || !draft.trim()}>
                      ${sending ? html`<span class="spinner" aria-hidden="true"></span>` : null}${t('chat.send')}
                    </button>`}
              </form>
              ${composerError ? html`<p class="field-hint is-error chat-composer-error">${composerError}</p>` : null}
            </div>
          `}
  </div>`;
}

function Head({ menuDisabled, menuOpen, onToggleMenu, onClose, onNew, onHistory, onClear, clearDisabled }) {
  return html`<header class="page-head">
    <div>
      <h1 class="page-title" tabindex="-1">${t('nav.chat')}</h1>
      <p class="page-sub">${t('chat.sub')}</p>
    </div>
    <div class="page-head-actions">
      <${ChatMenu}
        disabled=${menuDisabled}
        open=${!!menuOpen}
        onToggle=${onToggleMenu}
        onClose=${onClose}
        onNew=${onNew}
        onHistory=${onHistory}
        onClear=${onClear}
        clearDisabled=${clearDisabled}
      />
    </div>
  </header>`;
}

function ChatMenu({ disabled, open, onToggle, onClose, onNew, onHistory, onClear, clearDisabled }) {
  const ref = useRef(null);
  useEffect(() => {
    if (!open) return undefined;
    const onDoc = (e) => {
      if (ref.current && !ref.current.contains(e.target)) onClose();
    };
    const onKey = (e) => {
      if (e.key === 'Escape') onClose();
    };
    document.addEventListener('mousedown', onDoc);
    document.addEventListener('keydown', onKey);
    return () => {
      document.removeEventListener('mousedown', onDoc);
      document.removeEventListener('keydown', onKey);
    };
  }, [open, onClose]);

  return html`<div class="chat-menu" ref=${ref}>
    <button
      type="button"
      class="btn btn-icon btn-ghost"
      disabled=${disabled}
      aria-haspopup="true"
      aria-expanded=${open ? 'true' : 'false'}
      aria-label=${t('chat.menu_label')}
      onClick=${onToggle}
    >
      <${IconMore} size=${20} />
    </button>
    ${open
      ? html`<div class="chat-menu-pop" role="menu">
          <button type="button" role="menuitem" class="chat-menu-item" onClick=${onNew}>${t('chat.menu_new')}</button>
          <button type="button" role="menuitem" class="chat-menu-item" onClick=${onHistory}>${t('chat.menu_history')}</button>
          <button type="button" role="menuitem" class="chat-menu-item is-danger" disabled=${clearDisabled} onClick=${onClear}>
            ${t('chat.menu_clear')}
          </button>
        </div>`
      : null}
  </div>`;
}

function ModelBar({ model }) {
  if (!model) {
    return html`<p class="chat-model-bar muted">
      <${IconAlert} size=${15} /><span>${t('chat.no_model')}</span>
      <a href="#/models/llm" class="link card-link">${t('chat.model_link')}<${IconChevronRight} size=${13} /></a>
    </p>`;
  }
  return html`<p class="chat-model-bar muted">
    <span class="chat-model-name">${model.name}</span>
    ${model.vision ? html`<${Badge} tone="accent">${t('chat.vision_on')}</${Badge}>` : null}
  </p>`;
}

function MessageRow({ m, messages, expanded, onToggle, fullCache, onFullLoaded, canFetch = true }) {
  if (m.role === 'user') return html`<${UserMsg} m=${m} />`;
  if (m.role === 'assistant') return m.text ? html`<${AssistantMsg} m=${m} />` : null;
  if (m.role === 'note') return html`<${NoteMsg} m=${m} />`;
  if (m.role === 'tool') {
    const call = findCall(messages, m.call_id);
    return html`<${ToolRow}
      m=${m}
      call=${call}
      expanded=${expanded}
      onToggle=${onToggle}
      fullCache=${fullCache}
      onFullLoaded=${onFullLoaded}
      canFetch=${canFetch}
    />`;
  }
  if (m.role === 'summary' && m.dropped) {
    return html`<p class="chat-dropped muted">${t('chat.dropped_note')}</p>`;
  }
  return null; // hidden: other summaries and every marker
}

function findCall(messages, callId) {
  for (const m of messages) {
    if (m.role !== 'assistant' || !m.calls) continue;
    const c = m.calls.find((x) => x.id === callId);
    if (c) return { seq: m.seq, ...c };
  }
  return null;
}

function UserMsg({ m }) {
  return html`<div class="msg msg-user">
    ${m.source === 'voice' ? html`<${IconMic} size=${14} class="msg-mic" />` : null}
    <${Lines} text=${m.text} />
  </div>`;
}

function AssistantMsg({ m }) {
  return html`<div
    class="msg msg-assistant md"
    dangerouslySetInnerHTML=${{ __html: renderMarkdown(stripSay(m.text)) }}
  ></div>`;
}

function NoteMsg({ m }) {
  return html`<p class="msg msg-note muted"><${Lines} text=${m.text} /></p>`;
}

/** Plain text as safe text nodes, newlines kept as line breaks. */
function Lines({ text }) {
  // A plain array of text nodes and <br>s, not a tagged template: Preact
  // renders a component's array return the same as a fragment, and a
  // bare string in it becomes a safely-escaped text node.
  const out = [];
  String(text)
    .split('\n')
    .forEach((line, i) => {
      if (i > 0) out.push(html`<br />`);
      out.push(line);
    });
  return out;
}

// A heuristic one-line summary of a tool call's JSON arguments: the first
// string value, if there is one -- enough to tell "scene.install
// counter" apart from "scene.install pomodoro" without knowing every
// tool's schema.
function argSummary(argsText) {
  if (!argsText) return '';
  try {
    const v = JSON.parse(argsText);
    if (v && typeof v === 'object' && !Array.isArray(v)) {
      for (const k of Object.keys(v)) {
        if (typeof v[k] === 'string' && v[k]) return v[k];
      }
    }
  } catch {
    // Not JSON (or the cut text does not parse); show the tool name alone.
  }
  return '';
}

function pretty(text) {
  try {
    return JSON.stringify(JSON.parse(text), null, 2);
  } catch {
    return text;
  }
}

function ToolRow({ m, call, expanded, onToggle, fullCache, onFullLoaded, canFetch = true }) {
  const [loading, setLoading] = useState(false);
  const [err, setErr] = useState(null);
  const summary = argSummary(call?.args || '');
  const line = `🔧 ${m.name}${summary ? ' ' + summary : ''} ${m.ok ? '✓' : '✗'}`;

  const fullResult = fullCache.get(m.seq);
  const fullAssistant = call ? fullCache.get(call.seq) : null;
  const resultText = fullResult ? fullResult.result : m.result;
  const argsText = fullAssistant ? fullAssistant.calls?.find((c) => c.id === m.call_id)?.args ?? call?.args ?? '' : call?.args || '';

  async function toggle() {
    const wasExpanded = expanded;
    onToggle(m.seq);
    if (wasExpanded || !canFetch) return;
    const needResult = m.result_len > m.result.length && !fullCache.has(m.seq);
    const needArgs = call && call.args_len > call.args.length && !fullCache.has(call.seq);
    if (!needResult && !needArgs) return;
    setLoading(true);
    setErr(null);
    try {
      const jobs = [];
      if (needResult) jobs.push(api('GET', `/agent/messages/${m.seq}`).then((full) => onFullLoaded(m.seq, full)));
      if (needArgs) jobs.push(api('GET', `/agent/messages/${call.seq}`).then((full) => onFullLoaded(call.seq, full)));
      await Promise.all(jobs);
    } catch (e) {
      if (e.status !== 401) setErr(e);
    } finally {
      setLoading(false);
    }
  }

  return html`<div class="chat-tool">
    <button type="button" class="chat-tool-row" aria-expanded=${expanded ? 'true' : 'false'} onClick=${toggle}>
      ${line}
    </button>
    ${expanded
      ? html`<div class="chat-tool-detail">
          ${argsText
            ? html`<div class="chat-tool-field"><span class="chat-tool-label">${t('chat.tool_args')}</span><pre>${pretty(argsText)}</pre></div>`
            : null}
          <div class="chat-tool-field"><span class="chat-tool-label">${t('chat.tool_result')}</span><pre>${pretty(resultText)}</pre></div>
          ${loading ? html`<p class="muted">${t('common.loading')}</p>` : null}
          ${err ? html`<p class="field-hint is-error">${t('chat.tool_full_failed', { reason: describeError(err) })}</p>` : null}
        </div>`
      : null}
  </div>`;
}

function formatWhen(ts) {
  if (!ts) return '—';
  return new Intl.DateTimeFormat(locale(), { dateStyle: 'medium', timeStyle: 'short' }).format(ts * 1000);
}

function ArchiveList({ onOpen, onBack }) {
  const [list, setList] = useState(/** @type {{n:number,ts:number,first:string}[]|null} */ (null));
  const [error, setError] = useState(null);
  const alive = useRef(true);

  useEffect(() => {
    alive.current = true;
    api('GET', '/agent/archive')
      .then((r) => {
        if (alive.current) setList(r.archives || []);
      })
      .catch((e) => {
        if (alive.current && e.status !== 401) setError(e);
      });
    return () => {
      alive.current = false;
    };
  }, []);

  return html`<div class="chat-panel chat-archive-list">
    <div class="chat-archive-head">
      <button type="button" class="btn btn-ghost btn-sm" onClick=${onBack}>
        <${IconChevronLeft} size=${16} />${t('chat.archive_back')}
      </button>
    </div>
    ${error
      ? html`<p class="muted">${describeError(error)}</p>`
      : !list
        ? html`<div class="skel skel-line"></div>`
        : list.length === 0
          ? html`<p class="muted">${t('chat.archives_empty')}</p>`
          : html`<ul class="rows chat-archive-rows">
              ${list.map(
                (a) => html`<li key=${a.n} class="row">
                  <button type="button" class="row-main chat-archive-row" onClick=${() => onOpen(a.n)}>
                    <span class="row-title">${formatWhen(a.ts)}</span>
                    <span class="row-sub">${a.first || '—'}</span>
                  </button>
                </li>`,
              )}
            </ul>`}
  </div>`;
}

function ArchiveView({ n, onBack }) {
  const [data, setData] = useState(/** @type {{n:number,messages:ChatMessage[]}|null} */ (null));
  const [error, setError] = useState(null);
  // Tool rows can still be opened to see their (already cut) args/result,
  // just without GET /messages/{seq}: that endpoint only reaches the
  // current conversation (agent-core spec 7.1), not an archive.
  const [expanded, setExpanded] = useState(/** @type {Set<number>} */ (new Set()));
  const alive = useRef(true);

  useEffect(() => {
    alive.current = true;
    setData(null);
    setError(null);
    api('GET', `/agent/archive/${n}`)
      .then((r) => {
        if (alive.current) setData(r);
      })
      .catch((e) => {
        if (alive.current && e.status !== 401) setError(e);
      });
    return () => {
      alive.current = false;
    };
  }, [n]);

  function toggle(seq) {
    setExpanded((s) => {
      const next = new Set(s);
      if (next.has(seq)) next.delete(seq);
      else next.add(seq);
      return next;
    });
  }

  return html`<div class="chat-panel">
    <div class="chat-archive-head">
      <button type="button" class="btn btn-ghost btn-sm" onClick=${onBack}>
        <${IconChevronLeft} size=${16} />${t('chat.archives_back')}
      </button>
      <span class="chat-archive-title">${t('chat.archive_title', { n: formatCount(n) })}</span>
    </div>
    <div class="chat-scroll">
      ${error
        ? html`<p class="muted">${describeError(error)}</p>`
        : !data
          ? html`<div class="skel skel-line"></div>`
          : data.messages.map(
              (m) => html`<${MessageRow}
                key=${m.seq}
                m=${m}
                messages=${data.messages}
                expanded=${expanded.has(m.seq)}
                onToggle=${toggle}
                fullCache=${EMPTY_MAP}
                onFullLoaded=${() => {}}
                canFetch=${false}
              />`,
            )}
    </div>
  </div>`;
}

const EMPTY_MAP = new Map();
