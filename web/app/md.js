// A deliberately small Markdown renderer for the agent's replies (spec
// 7.3): paragraphs, lists, inline and fenced code, bold and links only --
// no images, headings, tables or raw HTML. Everything is escaped before
// any tag is added, so a reply cannot inject markup: a link's target is
// checked after escaping and only http(s) URLs are turned into <a>,
// anything else (including a javascript: URL or an <img> the model typed
// as text) stays literal, escaped text.
//
// renderMarkdown()'s result is only ever meant for
// dangerouslySetInnerHTML on a container the page controls; nothing here
// reads back untrusted HTML.

const ESCAPE_MAP = { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' };

/** Escapes text for safe insertion into HTML (text or an attribute value). */
export function escapeHtml(text) {
  return String(text).replace(/[&<>"']/g, (c) => ESCAPE_MAP[c]);
}

const SAY_TAG_RE = /<\/?say>/g;

/**
 * Removes the device's `<say>`/`</say>` markers (agent-core spec 4.3),
 * keeping their content, the same way the device's own `split_say` does:
 * the tags themselves are dropped, a stray `</say>` is dropped too, and
 * an unclosed `<say>` leaves its content in place.
 *
 * A single left-to-right scan over the original text, matching the
 * device: removing every `<say>` first and then every `</say>` (two
 * passes over each other's output) can turn text that only coincidentally
 * spells one tag once the other is gone into a match that was never
 * there to begin with, e.g. "</sa" + "<say>" + "y>" -- stripping "<say>"
 * first leaves "</sa" + "y>" = "</say>", which a second pass would then
 * also strip, dropping text the device's single pass keeps.
 *
 * @param {string} text
 * @returns {string}
 */
export function stripSay(text) {
  return String(text).replace(SAY_TAG_RE, '');
}

// A fenced block: ``` (optionally with a language tag), a body, then ```
// on its own. Body may be empty and may itself contain blank lines.
const FENCE_RE = /```[^\n]*\n([\s\S]*?)```/g;

/**
 * Renders a limited Markdown subset to an HTML string: paragraphs
 * (blank-line separated, soft breaks become <br>), bullet (`-`/`*`) and
 * numbered (`1.`) lists, fenced and inline code, `**bold**` and
 * `[text](url)` links (http/https only).
 *
 * @param {string} text
 * @returns {string} HTML, safe to assign to innerHTML
 */
export function renderMarkdown(text) {
  const src = String(text);
  let out = '';
  let last = 0;
  let m;
  FENCE_RE.lastIndex = 0;
  while ((m = FENCE_RE.exec(src))) {
    if (m.index > last) out += renderBlocks(src.slice(last, m.index));
    out += renderCodeBlock(m[1]);
    last = FENCE_RE.lastIndex;
  }
  if (last < src.length) out += renderBlocks(src.slice(last));
  return out;
}

function renderCodeBlock(raw) {
  const body = raw.endsWith('\n') ? raw.slice(0, -1) : raw;
  return `<pre><code>${escapeHtml(body)}</code></pre>`;
}

function renderBlocks(text) {
  return text
    .split(/\n{2,}/)
    .map(renderBlock)
    .join('');
}

const BULLET_RE = /^[-*]\s+(.*)$/;
const ORDERED_RE = /^\d+\.\s+(.*)$/;

function renderBlock(block) {
  const lines = block
    .split('\n')
    .map((l) => l.trim())
    .filter((l) => l.length > 0);
  if (lines.length === 0) return '';

  const asBullet = lines.map((l) => l.match(BULLET_RE));
  if (asBullet.every(Boolean)) return renderList('ul', asBullet.map((m) => m[1]));

  const asOrdered = lines.map((l) => l.match(ORDERED_RE));
  if (asOrdered.every(Boolean)) return renderList('ol', asOrdered.map((m) => m[1]));

  return `<p>${lines.map(renderInline).join('<br>')}</p>`;
}

function renderList(tag, items) {
  return `<${tag}>${items.map((it) => `<li>${renderInline(it)}</li>`).join('')}</${tag}>`;
}

const CODE_SPAN_RE = /`([^`\n]+)`/g;
const BOLD_RE = /\*\*([^*\n]+)\*\*/g;
const LINK_RE = /\[([^\]\n]+)\]\(([^)\s]+)\)/g;
const SAFE_URL_RE = /^https?:\/\//i;

// Escapes `line`, then layers code spans, bold and links on top -- in
// that order, so `**` or `[...]` inside a code span is never reinterpreted,
// and the escaping happens before any tag exists to be mistaken for markup.
function renderInline(line) {
  const escaped = escapeHtml(line);
  const codes = [];
  let out = escaped.replace(CODE_SPAN_RE, (_, content) => {
    codes.push(`<code>${content}</code>`);
    return `\u0000${codes.length - 1}\u0000`;
  });
  out = out.replace(BOLD_RE, '<strong>$1</strong>');
  out = out.replace(LINK_RE, (whole, text, url) => {
    if (!SAFE_URL_RE.test(url)) return whole;
    return `<a class="link" href="${url}" target="_blank" rel="noopener noreferrer">${text}</a>`;
  });
  out = out.replace(/\u0000(\d+)\u0000/g, (_, i) => codes[Number(i)]);
  return out;
}
