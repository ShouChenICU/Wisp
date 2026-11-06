// fold_tree() (ui_inspector.hpp): fitting a control tree into the agent's
// budget.

#include <algorithm>
#include <string>

#include "wisp/os/large.hpp"
#include "wisp/scene/ui_inspector.hpp"

namespace wisp::scene {

namespace {

constexpr std::string_view kCut = "…已截断，用 ui.tree(ref) 查看子树";

std::string more_rows(size_t n) { return "…还有 " + std::to_string(n) + " 行"; }

std::string collapsed(size_t n) { return " …（" + std::to_string(n) + " 个子控件）"; }

// The lines as a forest, and what folding has done to it so far.
class Folder {
public:
    explicit Folder(std::span<const TreeLine> lines) : lines_(lines) {
        const size_t n = lines.size();
        kid_begin_.assign(n + 1, 0);
        fold_lo_.assign(n, 0);
        fold_hi_.assign(n, 0);
        collapsed_.assign(n, false);
        descendants_.assign(n, 0);
        // Children grouped by parent, each group in line order.
        os::LargeVector<uint32_t> count(n, 0);
        for (size_t i = 0; i < n; ++i) {
            const uint32_t p = parent(i);
            if (p == kNoParent) {
                roots_.push_back(static_cast<uint32_t>(i));
            } else {
                ++count[p];
            }
        }
        for (size_t i = 0; i < n; ++i) kid_begin_[i + 1] = kid_begin_[i] + count[i];
        kids_.assign(kid_begin_[n], 0);
        os::LargeVector<uint32_t> fill(kid_begin_.begin(), kid_begin_.end() - 1);
        for (size_t i = 0; i < n; ++i) {
            const uint32_t p = parent(i);
            if (p != kNoParent) kids_[fill[p]++] = static_cast<uint32_t>(i);
        }
        // Pre-order: every child comes after its parent.
        for (size_t i = n; i-- > 0;) {
            const uint32_t p = parent(i);
            if (p != kNoParent) descendants_[p] += 1 + descendants_[i];
        }
    }

    size_t kid_count(uint32_t i) const { return kid_begin_[i + 1] - kid_begin_[i]; }

    // Step 2 for one container.
    void fold(uint32_t i) {
        fold_lo_[i] = static_cast<uint32_t>(kTreeKeepEnds);
        fold_hi_[i] = static_cast<uint32_t>(kid_count(i) - kTreeKeepEnds);
    }
    // Step 3 for one subtree.
    void collapse(uint32_t i) { collapsed_[i] = true; }

    // Renders the visible lines through out(line).
    template <class Out>
    void render(Out&& out) const {
        struct Item {
            uint32_t node;
            uint32_t hidden;  // > 0: the "more rows" line for this many
            uint16_t depth;
        };
        os::LargeVector<Item> stack;
        for (size_t r = roots_.size(); r-- > 0;) stack.push_back({roots_[r], 0, 0});
        os::LargeString line;
        while (!stack.empty()) {
            const Item it = stack.back();
            stack.pop_back();
            line.assign(static_cast<size_t>(it.depth) * 2, ' ');
            if (it.hidden > 0) {
                line += more_rows(it.hidden);
                out(line);
                continue;
            }
            const uint32_t i = it.node;
            line.assign(static_cast<size_t>(lines_[i].depth) * 2, ' ');
            line += lines_[i].text;
            if (collapsed_[i] && kid_count(i) > 0) {
                line += collapsed(descendants_[i]);
                out(line);
                continue;
            }
            out(line);
            const uint32_t b = kid_begin_[i];
            const uint32_t k = static_cast<uint32_t>(kid_count(i));
            for (uint32_t j = k; j-- > 0;) {
                if (fold_hi_[i] > fold_lo_[i] && j >= fold_lo_[i] && j < fold_hi_[i]) {
                    if (j == fold_lo_[i]) {
                        stack.push_back({i, fold_hi_[i] - fold_lo_[i], lines_[kids_[b + j]].depth});
                    }
                    continue;
                }
                stack.push_back({kids_[b + j], 0, 0});
            }
        }
    }

    // Bytes of the rendered text, lines joined with '\n'.
    size_t length() const {
        size_t total = 0;
        size_t count = 0;
        render([&](const os::LargeString& l) {
            total += l.size();
            ++count;
        });
        return count == 0 ? 0 : total + count - 1;
    }

    std::string text() const {
        std::string out;
        render([&](const os::LargeString& l) {
            if (!out.empty()) out.push_back('\n');
            out.append(l.data(), l.size());
        });
        return out;
    }

    // Containers with more than 2 * kTreeKeepEnds children, most children
    // first.
    os::LargeVector<uint32_t> foldable() const {
        os::LargeVector<uint32_t> out;
        for (uint32_t i = 0; i < lines_.size(); ++i)
            if (kid_count(i) > 2 * kTreeKeepEnds) out.push_back(i);
        std::stable_sort(out.begin(), out.end(),
                         [this](uint32_t a, uint32_t b) { return kid_count(a) > kid_count(b); });
        return out;
    }

    // Nodes with children, deepest first.
    os::LargeVector<uint32_t> collapsible() const {
        os::LargeVector<uint32_t> out;
        for (uint32_t i = 0; i < lines_.size(); ++i)
            if (kid_count(i) > 0) out.push_back(i);
        std::stable_sort(out.begin(), out.end(), [this](uint32_t a, uint32_t b) {
            return lines_[a].depth > lines_[b].depth;
        });
        return out;
    }

private:
    uint32_t parent(size_t i) const {
        const uint32_t p = lines_[i].parent_index;
        return p < i ? p : kNoParent;
    }

    std::span<const TreeLine> lines_;
    os::LargeVector<uint32_t> roots_;
    os::LargeVector<uint32_t> kid_begin_;
    os::LargeVector<uint32_t> kids_;
    os::LargeVector<uint32_t> fold_lo_, fold_hi_;
    os::LargeVector<bool> collapsed_;
    os::LargeVector<uint32_t> descendants_;
};

// Whole lines of `text` while they and kCut fit in max_chars.
std::string cut(const std::string& text, size_t max_chars) {
    std::string out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const size_t added = (out.empty() ? 0 : 1) + (end - pos);
        if (out.size() + added + 1 + kCut.size() > max_chars) break;
        if (!out.empty()) out.push_back('\n');
        out.append(text, pos, end - pos);
        pos = end + 1;
    }
    if (!out.empty()) out.push_back('\n');
    if (out.size() + kCut.size() > max_chars) return out.substr(0, 0);
    out.append(kCut);
    return out;
}

}  // namespace

std::string fold_tree(std::span<const TreeLine> lines, size_t max_chars) {
    Folder f(lines);
    if (f.length() <= max_chars) return f.text();
    for (uint32_t i : f.foldable()) {
        f.fold(i);
        if (f.length() <= max_chars) return f.text();
    }
    for (uint32_t i : f.collapsible()) {
        f.collapse(i);
        if (f.length() <= max_chars) return f.text();
    }
    return cut(f.text(), max_chars);
}

}  // namespace wisp::scene
