#include "table.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include "num.hpp"

namespace ech {

std::uint32_t Arena::intern(std::string_view s) {
    if (auto it = ids_.find(s); it != ids_.end()) return it->second;
    const auto id = static_cast<std::uint32_t>(vals_.size());
    const std::string& owned = store_.emplace_back(s);
    vals_.push_back(&owned);
    ids_.emplace(std::string_view(owned), id);
    return id;
}

const std::vector<std::uint32_t>& Table::groups(int c) const {
    Column& col = cols_[static_cast<std::size_t>(c)];
    if (!col.cat_built) {
        col.cat.reserve(nrows_);
        for (std::size_t r = 0; r < nrows_; ++r)
            col.cat.push_back(arena_.intern(raw_at(r, static_cast<std::size_t>(c))));
        col.cat_built = true;
    }
    return col.cat;
}

char Table::sniff(std::string_view header) {
    // Count, do not guess. The winner is whichever candidate appears most in
    // the header line; a tie falls to comma because that is what the extension
    // usually claims and being wrong there is the least surprising.
    static constexpr char kCands[] = {',', '\t', ';', '|'};
    char best = ',';
    std::size_t most = 0;
    for (char c : kCands) {
        const std::size_t n =
            static_cast<std::size_t>(std::count(header.begin(), header.end(), c));
        if (n > most) {
            most = n;
            best = c;
        }
    }
    return best;
}

namespace {

// One line of a delimited file into fields, by pointer. No allocation, no
// copy: every field is a view into the buffer the caller already holds.
//
// Quoted fields are honoured because a label column with a comma in it is
// ordinary ("Tris, pH 7.4"), and splitting it would shift every later column
// on that row by one -- a corruption that lands silently in the middle of a
// table and looks like bad data rather than a bad parse.
void split(std::string_view line, char delim,
           std::vector<std::string_view>& out) {
    out.clear();
    std::size_t i = 0;
    const std::size_t n = line.size();
    while (true) {
        if (i < n && line[i] == '"') {
            const std::size_t start = ++i;
            while (i < n && !(line[i] == '"' &&
                              (i + 1 >= n || line[i + 1] != '"')))
                ++i;
            out.push_back(line.substr(start, i - start));
            while (i < n && line[i] != delim) ++i;
        } else {
            const std::size_t start = i;
            while (i < n && line[i] != delim) ++i;
            out.push_back(trim(line.substr(start, i - start)));
        }
        if (i >= n) break;
        ++i;  // step over the delimiter
        if (i == n) {                 // a trailing delimiter is an empty field
            out.push_back({});
            break;
        }
    }
}

std::string read_file(const std::string& path) {
    std::FILE* fh = std::fopen(path.c_str(), "rb");
    if (!fh) throw std::runtime_error("cannot open " + path);
    std::fseek(fh, 0, SEEK_END);
    const long len = std::ftell(fh);
    std::fseek(fh, 0, SEEK_SET);
    std::string buf;
    if (len > 0) {
        buf.resize(static_cast<std::size_t>(len));
        const std::size_t got = std::fread(buf.data(), 1, buf.size(), fh);
        buf.resize(got);
    }
    std::fclose(fh);
    return buf;
}

std::string basename_of(const std::string& p) {
    const std::size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

}  // namespace

Table Table::load(const std::string& path, char delim) {
    std::string text = read_file(path);
    if (text.empty()) throw std::runtime_error("empty file: " + path);
    return from_text(std::move(text), path, basename_of(path), delim);
}

Table Table::from_text(std::string text, std::string path, std::string name,
                       char delim) {
    Table t;
    t.path_ = std::move(path);
    t.name_ = std::move(name);
    t.buf_ = std::move(text);
    if (t.buf_.empty()) throw std::runtime_error("empty table: " + t.name_);

    // Drop a leading UTF-8 byte order mark. Excel writes one on every CSV it
    // exports, and so does PowerShell's -Encoding utf8, so this is the common
    // case rather than an exotic one. Left in place it becomes part of the
    // FIRST COLUMN'S NAME -- the header reads "?dose", and `x=dose` then finds
    // no such column while the grid appears to show one. Stripped here, before
    // anything takes an offset into the buffer.
    if (t.buf_.size() >= 3 && static_cast<unsigned char>(t.buf_[0]) == 0xEF &&
        static_cast<unsigned char>(t.buf_[1]) == 0xBB &&
        static_cast<unsigned char>(t.buf_[2]) == 0xBF)
        t.buf_.erase(0, 3);

    std::string_view all(t.buf_);
    std::size_t pos = all.find('\n');
    std::string_view header = all.substr(0, pos == std::string_view::npos
                                                ? all.size()
                                                : pos);
    t.delim_ = delim ? delim : sniff(header);

    std::vector<std::string_view> fields;
    split(header, t.delim_, fields);
    if (fields.empty()) throw std::runtime_error("no columns in " + t.name_);
    t.cols_.resize(fields.size());
    for (std::size_t c = 0; c < fields.size(); ++c)
        t.cols_[c].name = std::string(trim(fields[c]));

    // Count the lines before filling anything. One pass over memory to buy
    // exact reserves is far cheaper than the handful of reallocations and
    // copies that growing four parallel 50,000-element vectors would cost.
    const std::size_t nl =
        static_cast<std::size_t>(std::count(all.begin(), all.end(), '\n'));
    const std::size_t guess = nl ? nl : 1;
    const std::size_t ncol = t.cols_.size();
    for (auto& col : t.cols_) col.num.reserve(guess);
    std::vector<std::vector<Slice>> raw(ncol);
    for (auto& v : raw) v.reserve(guess);

    std::size_t line_start = (pos == std::string_view::npos) ? all.size() : pos + 1;
    std::size_t nrows = 0;
    while (line_start < all.size()) {
        std::size_t nlpos = all.find('\n', line_start);
        if (nlpos == std::string_view::npos) nlpos = all.size();
        std::string_view line = all.substr(line_start, nlpos - line_start);
        line_start = nlpos + 1;
        if (trim(line).empty()) continue;   // a blank line is not a row of blanks

        split(line, t.delim_, fields);
        for (std::size_t c = 0; c < ncol; ++c) {
            std::string_view f = c < fields.size() ? fields[c] : std::string_view{};
            raw[c].push_back(Slice{
                static_cast<std::uint32_t>(f.data() ? f.data() - t.buf_.data() : 0),
                static_cast<std::uint32_t>(f.size())});
            t.cols_[c].num.push_back(to_num(f));
        }
        ++nrows;
    }
    t.nrows_ = nrows;
    t.excl_.assign(nrows, 0);

    t.raw_.resize(ncol * nrows);
    for (std::size_t c = 0; c < ncol; ++c)
        std::copy(raw[c].begin(), raw[c].end(), t.raw_.begin() + c * nrows);

    // Numeric on a SAMPLE, not the whole column: the answer only decides
    // which pickers offer the column, and scanning 50,000 cells to learn what
    // the first 400 already said is work with no reader.
    const std::size_t sample = std::min<std::size_t>(nrows, 400);
    for (auto& col : t.cols_) {
        std::size_t good = 0;
        for (std::size_t i = 0; i < sample; ++i)
            if (is_num(col.num[i])) ++good;
        col.numeric = sample > 0 && good * 2 >= sample;
    }
    return t;
}

int Table::index_of(std::string_view col) const {
    for (std::size_t i = 0; i < cols_.size(); ++i)
        if (cols_[i].name == col) return static_cast<int>(i);
    return -1;
}

const Column* Table::find(std::string_view col) const {
    const int i = index_of(col);
    return i < 0 ? nullptr : &cols_[static_cast<std::size_t>(i)];
}

std::vector<std::string> Table::numeric_names() const {
    std::vector<std::string> out;
    for (const auto& c : cols_)
        if (c.numeric) out.push_back(c.name);
    return out;
}

void Table::clear_exclusions() {
    std::fill(excl_.begin(), excl_.end(), 0);
    nexcl_ = 0;
}

std::string Table::cell_text(std::size_t row, std::size_t col) const {
    return std::string(raw_at(row, col));
}

std::string Table::to_csv() const {
    // Quote only what would otherwise re-read as something else. A cell
    // holding the delimiter, a quote or a newline has to be quoted, or the
    // row silently gains a column when this is read back -- the same shifted
    // -by-one corruption the quoted-field handling on the way IN exists to
    // stop, arriving instead on the way out.
    auto field = [this](std::string_view v) {
        const bool needs = v.find(delim_) != std::string_view::npos ||
                           v.find('\"') != std::string_view::npos ||
                           v.find('\n') != std::string_view::npos ||
                           v.find('\r') != std::string_view::npos;
        if (!needs) return std::string(v);
        std::string o = "\"";
        for (char c : v) {
            if (c == '\"') o += '\"';
            o += c;
        }
        o += '\"';
        return o;
    };
    std::string o;
    o.reserve(buf_.size() + nrows_ * 2);
    for (std::size_t c = 0; c < cols_.size(); ++c) {
        if (c) o += delim_;
        o += field(cols_[c].name);
    }
    o += '\n';
    for (std::size_t r = 0; r < nrows_; ++r) {
        for (std::size_t c = 0; c < cols_.size(); ++c) {
            if (c) o += delim_;
            o += field(raw_at(r, c));
        }
        o += '\n';
    }
    return o;
}

void Table::set_cell(std::size_t row, std::size_t col, std::string_view text) {
    // Appending to buf_ can reallocate it, and every other slice is an offset
    // rather than a pointer precisely so that is harmless. Storing char* here
    // would leave the whole table dangling on the first edit.
    const std::uint32_t off = static_cast<std::uint32_t>(buf_.size());
    buf_.append(text);
    raw_[col * nrows_ + row] = Slice{off, static_cast<std::uint32_t>(text.size())};
    cols_[col].num[row] = to_num(text);
    if (cols_[col].cat_built) cols_[col].cat[row] = arena_.intern(text);
}

void Table::series(int xc, int yc, std::vector<double>& xs,
                   std::vector<double>& ys) const {
    xs.clear();
    ys.clear();
    if (xc < 0 || yc < 0) return;
    const auto& X = cols_[static_cast<std::size_t>(xc)].num;
    const auto& Y = cols_[static_cast<std::size_t>(yc)].num;
    xs.reserve(nrows_ - nexcl_);
    ys.reserve(nrows_ - nexcl_);
    for (std::size_t i = 0; i < nrows_; ++i) {
        if (excl_[i]) continue;
        const double a = X[i], b = Y[i];
        if (is_num(a) && is_num(b)) {
            xs.push_back(a);
            ys.push_back(b);
        }
    }
}

std::vector<double> Table::values(int c) const {
    std::vector<double> out;
    if (c < 0) return out;
    const auto& V = cols_[static_cast<std::size_t>(c)].num;
    out.reserve(nrows_ - nexcl_);
    for (std::size_t i = 0; i < nrows_; ++i)
        if (!excl_[i] && is_num(V[i])) out.push_back(V[i]);
    return out;
}

}  // namespace ech
