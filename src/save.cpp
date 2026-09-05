#include "save.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

#include "ui.hpp"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace ech {

std::uint64_t checksum(std::string_view bytes) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : bytes) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

bool write_atomic(const std::string& path, std::string_view body,
                  std::string& err) {
    const std::string tmp = path + ".part";
    std::FILE* fh = std::fopen(tmp.c_str(), "wb");
    if (!fh) {
        err = "cannot write " + tmp;
        return false;
    }
    bool ok = body.empty() ||
              std::fwrite(body.data(), 1, body.size(), fh) == body.size();
    ok = ok && std::fflush(fh) == 0;
    // fflush only moves the bytes out of this process. Without forcing them
    // to the device, a power loss between here and the rename can leave the
    // rename durable and its contents not -- the target then points at a file
    // of zeros, which is a worse outcome than not having written at all.
    if (ok) {
#ifdef _WIN32
        ok = _commit(_fileno(fh)) == 0;
#else
        ok = ::fsync(::fileno(fh)) == 0;
#endif
    }
    std::fclose(fh);
    if (!ok) {
        std::remove(tmp.c_str());
        err = "could not write all of " + tmp + " (disk full?)";
        return false;
    }

    // The replace itself is one step, so `path` holds the whole old file or
    // the whole new one and never a prefix of either.
#ifdef _WIN32
    if (!MoveFileExA(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::remove(tmp.c_str());
        err = "could not replace " + path + " (open in another program?)";
        return false;
    }
#else
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        err = "could not replace " + path;
        return false;
    }
#endif
    return true;
}

namespace {

constexpr const char* kMagic = "echelle-session 1";

std::string read_all(const std::string& path, bool& ok) {
    ok = false;
    std::FILE* fh = std::fopen(path.c_str(), "rb");
    if (!fh) return {};
    std::fseek(fh, 0, SEEK_END);
    const long len = std::ftell(fh);
    std::fseek(fh, 0, SEEK_SET);
    std::string buf;
    if (len > 0) {
        buf.resize(static_cast<std::size_t>(len));
        buf.resize(std::fread(buf.data(), 1, buf.size(), fh));
    }
    std::fclose(fh);
    ok = true;
    return buf;
}

void put(std::string& o, const char* key, const std::string& v) {
    o += key;
    o += ' ';
    o += v;
    o += '\n';
}

void put(std::string& o, const char* key, long long v) {
    put(o, key, std::to_string(v));
}

std::string join(const std::vector<int>& v) {
    std::string o;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) o += ',';
        o += std::to_string(v[i]);
    }
    return o;
}

std::vector<long long> split_ints(std::string_view s) {
    std::vector<long long> out;
    long long cur = 0;
    bool any = false, neg = false;
    for (char c : s) {
        if (c == '-' && !any) { neg = true; any = true; continue; }
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            any = true;
        } else if (any) {
            out.push_back(neg ? -cur : cur);
            cur = 0;
            any = false;
            neg = false;
        }
    }
    if (any) out.push_back(neg ? -cur : cur);
    return out;
}

// A cursor over the body. Lines for everything except the table text, which
// is length-prefixed: a CSV can contain any byte sequence at all, including a
// line that reads exactly like the next key, and length-prefixing is the one
// framing that cannot be confused by its own content.
struct Reader {
    std::string_view s;
    std::size_t p = 0;
    bool eof() const { return p >= s.size(); }
    std::string_view line() {
        const std::size_t nl = s.find('\n', p);
        const std::size_t end = nl == std::string_view::npos ? s.size() : nl;
        std::string_view out = s.substr(p, end - p);
        p = end < s.size() ? end + 1 : s.size();
        return out;
    }
    bool take(std::size_t n, std::string_view& out) {
        if (s.size() - p < n) return false;
        out = s.substr(p, n);
        p += n;
        return true;
    }
};

std::string_view key_of(std::string_view line) {
    const std::size_t sp = line.find(' ');
    return sp == std::string_view::npos ? line : line.substr(0, sp);
}
std::string_view val_of(std::string_view line) {
    const std::size_t sp = line.find(' ');
    return sp == std::string_view::npos ? std::string_view{} : line.substr(sp + 1);
}

int clamp_col(long long v, std::size_t ncols) {
    return (v >= 0 && v < static_cast<long long>(ncols)) ? static_cast<int>(v) : -1;
}

}  // namespace

bool save_session(const App& app, const std::string& path, std::string& err) {
    if (app.docs.empty()) {
        err = "nothing open to save";
        return false;
    }
    std::string body;
    put(body, "cur", app.cur);
    put(body, "section", static_cast<long long>(app.section));
    for (const auto& up : app.docs) {
        const Doc& d = *up;
        put(body, "table", d.table.name());
        put(body, "path", d.table.path());
        put(body, "delim", static_cast<long long>(d.table.delim()));
        put(body, "spec", spec_text(d.spec));
        put(body, "ys", join(d.ys));
        // One line for the pickers, in a fixed order. Named lines would be
        // friendlier to read and would also let a partly-understood file load
        // with some pickers silently at their defaults; a fixed row either
        // parses or does not.
        std::vector<int> pick = {d.x,        d.split,
                                 d.fit_x,    d.fit_y,
                                 static_cast<int>(d.fit_model),
                                 d.test_kind, d.test_a,
                                 d.test_b,   d.test_by,
                                 d.logx,     d.logy,
                                 d.draw_fit, d.has_fit};
        put(body, "pick", join(pick));

        std::vector<int> excl, sel;
        for (std::size_t i = 0; i < d.table.rows(); ++i)
            if (d.table.excluded(i)) excl.push_back(static_cast<int>(i));
        for (std::size_t i : d.row_selected) sel.push_back(static_cast<int>(i));
        put(body, "excl", join(excl));
        put(body, "sel", join(sel));

        const std::string csv = d.table.to_csv();
        put(body, "data", static_cast<long long>(csv.size()));
        body += csv;
    }

    std::string out = kMagic;
    out += '\n';
    out += "len " + std::to_string(body.size()) + '\n';
    char sum[32];
    std::snprintf(sum, sizeof sum, "sum %016llx\n",
                  static_cast<unsigned long long>(checksum(body)));
    out += sum;
    out += body;
    return write_atomic(path, out, err);
}

bool load_session(App& app, const std::string& path, std::string& err) {
    bool read_ok = false;
    const std::string file = read_all(path, read_ok);
    if (!read_ok) {
        err = "cannot open " + path;
        return false;
    }

    Reader head{file};
    if (head.line() != kMagic) {
        err = "not an Echelle session file (or a newer version of one)";
        return false;
    }
    const std::string_view len_line = head.line();
    const std::string_view sum_line = head.line();
    if (key_of(len_line) != "len" || key_of(sum_line) != "sum") {
        err = "session header is damaged";
        return false;
    }
    const auto want_len = static_cast<std::size_t>(std::strtoull(
        std::string(val_of(len_line)).c_str(), nullptr, 10));
    const auto want_sum = std::strtoull(
        std::string(val_of(sum_line)).c_str(), nullptr, 16);

    const std::string_view body = std::string_view(file).substr(head.p);
    // LENGTH FIRST, then the checksum. A truncated file is the common damage
    // and the length says so exactly -- "1.2 MB short" is something a person
    // can act on, where a checksum mismatch alone is not.
    if (body.size() != want_len) {
        err = "session is " +
              std::to_string(body.size() < want_len ? want_len - body.size()
                                                    : body.size() - want_len) +
              (body.size() < want_len ? " byte(s) short -- the file is "
                                        "truncated and was not loaded"
                                      : " byte(s) too long -- the file is "
                                        "damaged and was not loaded");
        return false;
    }
    if (checksum(body) != want_sum) {
        err = "session checksum does not match: the file is damaged and was "
              "not loaded. Nothing on screen has changed.";
        return false;
    }

    // Built to the side and swapped in only at the end. A loader that filled
    // the app as it parsed would leave a half-restored session on the first
    // bad line, which looks like a working session with tables missing.
    std::vector<std::unique_ptr<Doc>> docs;
    int cur = 0, section = 0;
    Reader r{body};
    std::unique_ptr<Doc> pending;
    std::string pending_path, pending_name;
    long long pending_delim = ',';
    std::string pending_spec, pending_ys, pending_pick, pending_excl,
        pending_sel;

    while (!r.eof()) {
        const std::string_view ln = r.line();
        if (ln.empty()) continue;
        const std::string_view k = key_of(ln), v = val_of(ln);
        if (k == "cur") cur = static_cast<int>(std::strtol(std::string(v).c_str(), nullptr, 10));
        else if (k == "section") section = static_cast<int>(std::strtol(std::string(v).c_str(), nullptr, 10));
        else if (k == "table") pending_name = std::string(v);
        else if (k == "path") pending_path = std::string(v);
        else if (k == "delim") pending_delim = std::strtol(std::string(v).c_str(), nullptr, 10);
        else if (k == "spec") pending_spec = std::string(v);
        else if (k == "ys") pending_ys = std::string(v);
        else if (k == "pick") pending_pick = std::string(v);
        else if (k == "excl") pending_excl = std::string(v);
        else if (k == "sel") pending_sel = std::string(v);
        else if (k == "data") {
            const auto n = static_cast<std::size_t>(
                std::strtoull(std::string(v).c_str(), nullptr, 10));
            std::string_view csv;
            if (!r.take(n, csv)) {
                err = "session ends in the middle of table '" + pending_name +
                      "' -- nothing was loaded";
                return false;
            }
            try {
                Table t = Table::from_text(std::string(csv), pending_path,
                                           pending_name,
                                           static_cast<char>(pending_delim));
                pending = std::make_unique<Doc>(std::move(t));
            } catch (const std::exception& e) {
                err = std::string("table '") + pending_name +
                      "' in the session will not parse: " + e.what();
                return false;
            }
            Doc& d = *pending;
            const std::size_t nc = d.table.ncols(), nr = d.table.rows();
            std::string perr;
            parse_spec(pending_spec, d.spec, perr);   // a bad spec is not fatal
            for (long long y : split_ints(pending_ys)) {
                const int c = clamp_col(y, nc);
                if (c >= 0) d.ys.push_back(c);
            }
            const auto p = split_ints(pending_pick);
            if (p.size() >= 13) {
                d.x = clamp_col(p[0], nc);
                d.split = clamp_col(p[1], nc);
                d.fit_x = clamp_col(p[2], nc);
                d.fit_y = clamp_col(p[3], nc);
                d.fit_model = static_cast<Model>(
                    p[4] >= 0 && p[4] < static_cast<long long>(models().size())
                        ? p[4] : 0);
                d.test_kind = (p[5] >= 0 && p[5] < 8) ? static_cast<int>(p[5]) : 0;
                d.test_a = clamp_col(p[6], nc);
                d.test_b = clamp_col(p[7], nc);
                d.test_by = clamp_col(p[8], nc);
                d.logx = p[9] != 0;
                d.logy = p[10] != 0;
                d.draw_fit = p[11] != 0;
                d.has_fit = p[12] != 0;
            }
            d.where = d.spec.where;
            // Row numbers are checked against the table that came with them.
            // They cannot be stale -- the data is in the same file -- but a
            // damaged byte inside the checksummed body is still conceivable,
            // and an out-of-range index here would index off the end.
            for (long long i : split_ints(pending_excl))
                if (i >= 0 && i < static_cast<long long>(nr))
                    d.table.set_excluded(static_cast<std::size_t>(i), true);
            for (long long i : split_ints(pending_sel))
                if (i >= 0 && i < static_cast<long long>(nr))
                    d.row_selected.insert(static_cast<std::size_t>(i));
            docs.push_back(std::move(pending));
            pending_spec.clear();
            pending_ys.clear();
            pending_pick.clear();
            pending_excl.clear();
            pending_sel.clear();
        }
    }

    if (docs.empty()) {
        err = "session holds no tables";
        return false;
    }

    app.docs = std::move(docs);
    app.cur = (cur >= 0 && cur < static_cast<int>(app.docs.size())) ? cur : 0;
    app.section = (section >= 0 && section <= 3) ? static_cast<Section>(section)
                                                 : Section::Data;
    app.select(app.cur);
    // The fit is not stored: it is derived, and a stored one could disagree
    // with the data beside it. Re-running costs milliseconds and cannot.
    for (int i = 0; i < static_cast<int>(app.docs.size()); ++i) {
        if (!app.docs[static_cast<std::size_t>(i)]->has_fit) continue;
        const int keep = app.cur;
        app.cur = i;
        app.run_fit();
        app.cur = keep;
    }
    app.select(app.cur);
    app.status = "restored " + std::to_string(app.docs.size()) +
                 " table(s) from " + path;
    return true;
}

}  // namespace ech
