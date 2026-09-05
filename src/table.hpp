// One opened file: its columns, and which rows are excluded.
//
// COLUMNAR, not a list of rows. The Python this replaces held `list[dict]` --
// a hash lookup and a boxed float per cell -- which is why reading a 50,000
// row file cost 57 ms and grouping it cost another 22. Here a numeric column
// IS a `std::vector<double>`, contiguous and cache-friendly, so a plot can
// hand ImPlot a raw pointer with no copy at all when nothing is filtered out.
// That zero-copy path is the whole reason for the layout.
//
// Numbers are parsed once, at load, and never again -- a redraw reads doubles,
// not text. Group ids are the exception and are interned lazily, because only
// a `colour=` or `group by` column ever needs them.
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ech {

// Distinct strings, stored once. A grouping column of 50,000 cells over four
// batches is four strings and 50,000 uint32s, not 50,000 strings.
//
// The hash and equality are TRANSPARENT so a lookup can be done on a
// string_view. Without that, `ids_.find(std::string(s))` builds and destroys a
// std::string for every cell examined -- an allocation per lookup, in the one
// loop that runs once per cell in the file.
struct SvHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept {
        return std::hash<std::string_view>{}(s);
    }
};
struct SvEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        return a == b;
    }
};

class Arena {
  public:
    std::uint32_t intern(std::string_view s);
    const std::string& operator[](std::uint32_t id) const { return *vals_[id]; }
    std::size_t size() const { return vals_.size(); }

  private:
    // deque, not vector: the map's keys are views onto these strings, and a
    // vector reallocating would leave every key in the map dangling.
    std::deque<std::string> store_;
    std::vector<const std::string*> vals_;
    std::unordered_map<std::string_view, std::uint32_t, SvHash, SvEq> ids_;
};

struct Column {
    std::string name;
    std::vector<double> num;        // NaN where the cell is not a number
    // A column counts as numeric when at least half its sampled cells parse.
    // Half, not all: a real instrument file has blanks and the odd "n/a", and
    // refusing the column for those would hide the measurement.
    bool numeric = false;

    // Interned ids, for grouping. Built ON DEMAND, not at load: only a
    // `colour=` or `group by` column is ever grouped, and interning every
    // cell of every column up front meant a 50,000-row numeric column paid
    // 50,000 hash inserts to produce 50,000 distinct ids that nothing reads.
    std::vector<std::uint32_t> cat;
    bool cat_built = false;
};

class Table {
  public:
    // Throws std::runtime_error on an unreadable or empty file.
    static Table load(const std::string& path, char delim = '\0');

    // The same parse, over text already in memory. A session file carries its
    // tables' CONTENT rather than their paths, so restoring one has to build a
    // table without touching the disk -- and a path that still exists is not
    // the same thing as a file that still holds what it held.
    static Table from_text(std::string text, std::string path, std::string name,
                           char delim = '\0');

    // Every cell as it stands NOW, header first, in the delimiter it was read
    // with. Edits included -- this is what a session writes, so what reopens
    // is what was on screen and not what the file said before it was touched.
    std::string to_csv() const;

    // Delimiter by COUNTING the header, not by extension: a .csv written in a
    // comma-decimal locale is semicolon-delimited and the suffix lies.
    static char sniff(std::string_view header);

    std::size_t rows() const { return nrows_; }
    std::size_t ncols() const { return cols_.size(); }
    const std::vector<Column>& cols() const { return cols_; }
    const std::string& name() const { return name_; }
    const std::string& path() const { return path_; }
    const Arena& arena() const { return arena_; }
    char delim() const { return delim_; }

    // Group ids for one column, interning on first use.
    const std::vector<std::uint32_t>& groups(int c) const;

    int index_of(std::string_view col) const;
    const Column* find(std::string_view col) const;
    std::vector<std::string> numeric_names() const;

    // --- exclusion. Recorded, never silent: an excluded row stays in the
    // table and every fit and test reports how many were left out.
    bool excluded(std::size_t i) const { return excl_[i] != 0; }
    void set_excluded(std::size_t i, bool on) {
        if (excl_[i] != static_cast<std::uint8_t>(on)) {
            excl_[i] = static_cast<std::uint8_t>(on);
            nexcl_ += on ? 1 : -1;
        }
    }
    std::size_t n_excluded() const { return nexcl_; }
    void clear_exclusions();

    // Edit a cell. Re-parses that one value so the next fit sees it -- a value you can change on screen but not in the analysis is
    // worse than one you cannot change at all.
    void set_cell(std::size_t row, std::size_t col, std::string_view text);
    std::string cell_text(std::size_t row, std::size_t col) const;

    // (xs, ys) over INCLUDED rows, numeric pairs only.
    void series(int xc, int yc, std::vector<double>& xs,
                std::vector<double>& ys) const;
    // One column's included, numeric values.
    std::vector<double> values(int c) const;

  private:
    std::string path_, name_;
    char delim_ = ',';
    std::size_t nrows_ = 0, nexcl_ = 0;
    mutable std::vector<Column> cols_;   // mutable: groups() interns lazily
    std::vector<std::uint8_t> excl_;
    mutable Arena arena_;

    // The original text of every cell, so the grid can show what the
    // instrument actually wrote rather than a double round-tripped back to
    // digits -- "1.10" and "0.30" are the record; printing 1.1 and 0.3 is a
    // quiet edit nobody authorised.
    //
    // A slice into one buffer, not a string per cell. 50,000 rows over four
    // columns is 200,000 cells: as std::string that is 200,000 heap blocks
    // and ~6 MB of headers to hold about 1 MB of digits. As {offset, length}
    // it is 8 bytes each over the file buffer that was read anyway, and an
    // edit appends to the same buffer and repoints the slice.
    struct Slice {
        std::uint32_t off = 0, len = 0;
    };
    std::string buf_;               // the file, then any edited values
    std::vector<Slice> raw_;        // column-major: raw_[c * nrows_ + r]

    std::string_view raw_at(std::size_t r, std::size_t c) const {
        const Slice& s = raw_[c * nrows_ + r];
        return std::string_view(buf_).substr(s.off, s.len);
    }
};

}  // namespace ech
