# Wisp UI style guide

This guide applies to new and changed device layouts. Use the shared tokens in
`components/wisp_ui/include/wisp/ui/theme_tokens.hpp` and the Wisp widget theme.

## Spacing

- Independent controls must have a visible gap between their bounds, vertically
  and horizontally. Never place standalone buttons or inputs edge to edge.
- Use at least `theme::kGapSm` (8 px) between independent controls; use
  `theme::kGap` (16 px) between an information block and its actions, and
  `theme::kGapLg` (24 px) between distinct groups or secondary entry points.
- Set spacing on the container with row/column gaps or explicit margins. Internal
  padding does not create space between adjacent controls. Preserve these gaps
  when controls appear, disappear or wrap, and when the keyboard opens.
- Rows belonging to one grouped list may share a surface and use dividers.
  Labels and icons inside a composite control belong to that control. These
  exceptions do not apply to separate action buttons.

## Alignment and touch targets

- Centre trailing arrows, status icons and accessory buttons vertically within
  the full list row, including rows with a two-line label. Centre each glyph in
  its own button as well.
- Keep interactive targets at least 48 × 48 px. Derive usable space from the
  display shape and safe area; preserve control spacing on small round screens
  by scrolling rather than squeezing actions together.
- Check final widget bounds after layout. With LVGL flex layouts, `min_height`
  on a content-sized row can leave accessories above the row's visual centre;
  use a definite row height or an equivalent layout that centres the content.

## Verification

Check spacing and alignment on rectangular, rounded and round displays, in
Chinese, English and Japanese. Include expanded items, detail views, long labels
and keyboard states. Use actual rendered screenshots and geometry assertions for
layout regressions; declarations of flex alignment alone are insufficient.
