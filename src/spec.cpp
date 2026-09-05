#include "spec.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

#include "num.hpp"

namespace ech {
namespace {

constexpr std::string_view kKeys[] = {"kind", "x", "y", "colour", "color",
                                      "where", "bins", "limit", "title",
                                      "logx", "logy"};
// Longest first, so `<=` is not read as `<` followed by a stray `=`.
constexpr std::string_view kOps[] = {"<=", ">=", "!=", "==", "~", "<", ">"};

std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    return o;
}

std::vector<std::string_view> split_on(std::string_view s, char c) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i <= s.size()) {
        const std::size_t j = std::min(s.find(c, i), s.size());
        if (j > i) out.push_back(s.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

}  // namespace

bool parse_spec(std::string_view text, Spec& out, std::string& err) {
    out = Spec{};
    bool saw_kind = false;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ') ++i;
        std::size_t j = text.find(' ', i);
        if (j == std::string_view::npos) j = text.size();
        std::string_view tok = text.substr(i, j - i);
        i = j;
        if (tok.empty()) continue;

        const std::size_t eq = tok.find('=');
        if (eq == std::string_view::npos) {
            err = "not a key=value token: '" + std::string(tok) + "'";
            return false;
        }
        const std::string k = lower(trim(tok.substr(0, eq)));
        const std::string v(trim(tok.substr(eq + 1)));
        if (std::find(std::begin(kKeys), std::end(kKeys), k) == std::end(kKeys)) {
            err = "unknown key '" + k + "'";
            return false;
        }
        if (k == "kind") {
            saw_kind = true;
            if (v == "scatter") out.kind = Kind::Scatter;
            else if (v == "line") out.kind = Kind::Line;
            else if (v == "hist") out.kind = Kind::Hist;
            else if (v == "bars") out.kind = Kind::Bars;
            else {
                err = "kind must be one of scatter, line, hist, bars";
                return false;
            }
        } else if (k == "x") out.x = v;
        else if (k == "y") {
            for (auto p : split_on(v, ',')) out.y.emplace_back(p);
        } else if (k == "colour" || k == "color") out.colour = v;
        else if (k == "where") out.where = v;
        else if (k == "title") out.title = v;
        else if (k == "bins") out.bins = std::max(2, static_cast<int>(to_num(v)));
        else if (k == "limit") out.limit = static_cast<int>(to_num(v));
        else if (k == "logx") out.logx = (v != "0" && !v.empty());
        else if (k == "logy") out.logy = (v != "0" && !v.empty());
    }
    (void)saw_kind;
    return true;
}

std::string spec_text(const Spec& s) {
    std::string o = "kind=";
    switch (s.kind) {
        case Kind::Scatter: o += "scatter"; break;
        case Kind::Line:    o += "line"; break;
        case Kind::Hist:    o += "hist"; break;
        case Kind::Bars:    o += "bars"; break;
    }
    if (!s.x.empty()) o += " x=" + s.x;
    if (!s.y.empty()) {
        o += " y=";
        for (std::size_t i = 0; i < s.y.size(); ++i) {
            if (i) o += ',';
            o += s.y[i];
        }
    }
    if (!s.colour.empty()) o += " colour=" + s.colour;
    if (!s.where.empty()) o += " where=" + s.where;
    if (!s.title.empty()) o += " title=" + s.title;
    if (s.logx) o += " logx=1";
    if (s.logy) o += " logy=1";
    return o;
}

bool make_filter(const Table& t, std::string_view where, Filter& out,
                 std::string& err) {
    out = Filter{};
    if (trim(where).empty()) return true;
    std::string_view w = trim(where);
    std::size_t at = std::string_view::npos;
    std::string_view op;
    for (auto cand : kOps) {
        const std::size_t p = w.find(cand);
        if (p != std::string_view::npos && (at == std::string_view::npos || p < at)) {
            at = p;
            op = cand;
        }
    }
    if (at == std::string_view::npos) {
        err = "where needs one of <= >= != == ~ < >";
        return false;
    }
    const std::string col(trim(w.substr(0, at)));
    const std::string val(trim(w.substr(at + op.size())));
    const int ci = t.index_of(col);
    // A column that is not in the header must be NAMED. Reporting "no rows
    // matched" for a typo sends you looking at the data instead of the spec.
    if (ci < 0) {
        err = "no column '" + col + "' in where=";
        return false;
    }
    out.active = true;
    out.col = ci;
    out.op = std::string(op);
    out.val = val;
    const double n = to_num(val);
    out.numeric = is_num(n) && t.cols()[static_cast<std::size_t>(ci)].numeric;
    out.num = n;
    return true;
}

bool keep_row(const Table& t, const Filter& f, std::size_t row) {
    if (!f.active) return true;
    const auto& col = t.cols()[static_cast<std::size_t>(f.col)];
    if (f.numeric) {
        const double v = col.num[row];
        if (!is_num(v)) return false;
        if (f.op == "==") return v == f.num;
        if (f.op == "!=") return v != f.num;
        if (f.op == "<")  return v < f.num;
        if (f.op == "<=") return v <= f.num;
        if (f.op == ">")  return v > f.num;
        if (f.op == ">=") return v >= f.num;
        return false;  // '~' on a numeric column falls through to the text path
    }
    const std::string cell = t.cell_text(row, static_cast<std::size_t>(f.col));
    std::string a = cell, b = f.val;
    for (char& c : a) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    for (char& c : b) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
    if (f.op == "~")  return a.find(b) != std::string::npos;
    if (f.op == "==") return a == b;
    if (f.op == "!=") return a != b;
    if (f.op == "<")  return a < b;
    if (f.op == "<=") return a <= b;
    if (f.op == ">")  return a > b;
    if (f.op == ">=") return a >= b;
    return false;
}

bool build_series(const Table& t, const Spec& s, std::vector<Series>& out,
                  std::string& err) {
    out.clear();
    if (s.x.empty()) { err = "spec needs x="; return false; }
    const int xc = t.index_of(s.x);
    if (xc < 0) { err = "no column '" + s.x + "' (x=)"; return false; }
    if (s.kind != Kind::Hist && s.y.empty()) {
        err = "spec needs y= for this kind";
        return false;
    }
    for (const auto& y : s.y)
        if (t.index_of(y) < 0) { err = "no column '" + y + "' (y=)"; return false; }
    int cc = -1;
    if (!s.colour.empty()) {
        cc = t.index_of(s.colour);
        if (cc < 0) { err = "no column '" + s.colour + "' (colour=)"; return false; }
    }
    Filter f;
    if (!make_filter(t, s.where, f, err)) return false;

    const auto& X = t.cols()[static_cast<std::size_t>(xc)].num;
    const std::size_t nrows = t.rows();
    const std::size_t cap = s.limit > 0 ? static_cast<std::size_t>(s.limit) : nrows;

    if (s.kind == Kind::Hist) {
        std::vector<double> vals;
        vals.reserve(nrows);
        for (std::size_t i = 0; i < nrows && vals.size() < cap; ++i) {
            if (t.excluded(i) || !keep_row(t, f, i)) continue;
            if (is_num(X[i])) vals.push_back(X[i]);
        }
        if (vals.empty()) { err = "no numeric values in " + s.x; return false; }
        const auto [lo, hi] = std::minmax_element(vals.begin(), vals.end());
        const int nb = std::max(2, s.bins);
        double w = (*hi - *lo) / nb;
        if (w == 0.0) w = 1.0;
        Series ser;
        ser.label = s.x;
        ser.xs.resize(static_cast<std::size_t>(nb));
        ser.ys.assign(static_cast<std::size_t>(nb), 0.0);
        for (int i = 0; i < nb; ++i) ser.xs[static_cast<std::size_t>(i)] =
            *lo + (i + 0.5) * w;
        for (double v : vals) {
            int b = static_cast<int>((v - *lo) / w);
            b = std::clamp(b, 0, nb - 1);
            ser.ys[static_cast<std::size_t>(b)] += 1.0;
        }
        out.push_back(std::move(ser));
        return true;
    }

    // Several y columns: the series ARE the columns, and colouring by a third
    // field on top of that produces a legend nobody can read, so colour= is
    // ignored here rather than combined.
    if (s.y.size() > 1) {
        for (const auto& yname : s.y) {
            const auto& Y = t.cols()[static_cast<std::size_t>(t.index_of(yname))].num;
            Series ser;
            ser.label = yname;
            for (std::size_t i = 0; i < nrows && ser.xs.size() < cap; ++i) {
                if (t.excluded(i) || !keep_row(t, f, i)) continue;
                if (is_num(X[i]) && is_num(Y[i])) {
                    ser.xs.push_back(X[i]);
                    ser.ys.push_back(Y[i]);
                }
            }
            if (!ser.xs.empty()) out.push_back(std::move(ser));
        }
        if (out.empty()) { err = "no numeric pairs in the y columns"; return false; }
    } else {
        const auto& Y = t.cols()[static_cast<std::size_t>(t.index_of(s.y[0]))].num;
        if (cc < 0) {
            Series ser;
            ser.label = s.y[0];   // never "" -- a blank legend entry is a
                                  // swatch beside nothing
            for (std::size_t i = 0; i < nrows && ser.xs.size() < cap; ++i) {
                if (t.excluded(i) || !keep_row(t, f, i)) continue;
                if (is_num(X[i]) && is_num(Y[i])) {
                    ser.xs.push_back(X[i]);
                    ser.ys.push_back(Y[i]);
                }
            }
            if (ser.xs.empty()) { err = "no numeric pairs in " + s.y[0]; return false; }
            out.push_back(std::move(ser));
        } else {
            const auto& g = t.groups(cc);
            std::unordered_map<std::uint32_t, std::size_t> seen;
            for (std::size_t i = 0; i < nrows; ++i) {
                if (t.excluded(i) || !keep_row(t, f, i)) continue;
                if (!is_num(X[i]) || !is_num(Y[i])) continue;
                auto it = seen.find(g[i]);
                if (it == seen.end()) {
                    if (seen.size() >= kMaxGroups) {
                        // Count them all so the message can say how many.
                        std::size_t distinct = seen.size();
                        std::vector<std::uint32_t> more;
                        for (std::size_t k = i; k < nrows; ++k)
                            if (!t.excluded(k) && keep_row(t, f, k) &&
                                !seen.count(g[k]) &&
                                std::find(more.begin(), more.end(), g[k]) == more.end())
                                more.push_back(g[k]);
                        distinct += more.size();
                        err = "colour=" + s.colour + " has " +
                              std::to_string(distinct) +
                              " distinct values -- that is an identifier, not a "
                              "grouping (at most " + std::to_string(kMaxGroups) + ")";
                        out.clear();
                        return false;
                    }
                    it = seen.emplace(g[i], out.size()).first;
                    Series ser;
                    ser.label = t.arena()[g[i]];
                    if (ser.label.empty()) ser.label = s.y[0];
                    out.push_back(std::move(ser));
                }
                out[it->second].xs.push_back(X[i]);
                out[it->second].ys.push_back(Y[i]);
            }
            if (out.empty()) { err = "no numeric pairs in " + s.y[0]; return false; }
            std::sort(out.begin(), out.end(), [](const Series& a, const Series& b) {
                return a.label < b.label;
            });
        }
    }

    // Sorted for line, so a file in arbitrary row order does not draw a
    // scribble; left alone for scatter, where order is not a variable.
    if (s.kind == Kind::Line) {
        std::vector<std::size_t> idx;
        for (auto& ser : out) {
            idx.resize(ser.xs.size());
            std::iota(idx.begin(), idx.end(), std::size_t{0});
            std::sort(idx.begin(), idx.end(),
                      [&](std::size_t a, std::size_t b) { return ser.xs[a] < ser.xs[b]; });
            std::vector<double> nx(ser.xs.size()), ny(ser.ys.size());
            for (std::size_t k = 0; k < idx.size(); ++k) {
                nx[k] = ser.xs[idx[k]];
                ny[k] = ser.ys[idx[k]];
            }
            ser.xs.swap(nx);
            ser.ys.swap(ny);
        }
    }
    return true;
}

}  // namespace ech
