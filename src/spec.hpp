// The declaration: `kind=scatter x=dose y=response colour=batch where=day==3`
//
// It is what the pickers write and it is also editable by hand, which is the
// point. A picker can only offer combinations someone anticipated; the text
// can express `where=residual>2.5` the first time anyone wants it.
//
// Unknown keys are REFUSED rather than ignored: a typo that plots the wrong
// column is worse than one that plots nothing.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "table.hpp"

namespace ech {

enum class Kind { Scatter, Line, Hist, Bars };

struct Spec {
    Kind kind = Kind::Scatter;
    std::string x;
    std::vector<std::string> y;   // several y against one x
    std::string colour;
    std::string where;
    std::string title;
    int bins = 30;
    int limit = 0;
    bool logx = false, logy = false;
};

// Returns false and fills `err` rather than throwing: a bad declaration is
// something the user typed, not an exceptional condition.
bool parse_spec(std::string_view text, Spec& out, std::string& err);
std::string spec_text(const Spec& s);

// --- filtering -------------------------------------------------------------
// ONE comparison, not an expression language: col==v, col!=v, col<v, col<=v,
// col>v, col>=v, col~substring. Numeric when both sides are numbers, string
// otherwise -- as strings "10" < "9", and a plot drawn on that is wrong and
// looks fine.
struct Filter {
    bool active = false;
    int col = -1;
    std::string op, val;
    double num = 0;
    bool numeric = false;
};
bool make_filter(const Table& t, std::string_view where, Filter& out,
                 std::string& err);
bool keep_row(const Table& t, const Filter& f, std::size_t row);

// --- series ----------------------------------------------------------------
// One drawable series: a label and two contiguous arrays. `xs`/`ys` are what
// ImPlot is handed directly -- no per-point object, no copy at draw time.
struct Series {
    std::string label;
    std::vector<double> xs, ys;
};

// A colour column with a distinct value per row is not a grouping, it is an
// identifier -- and one legend entry per row covers the whole figure. Refused,
// with the column named.
inline constexpr std::size_t kMaxGroups = 12;

// Builds the series for a spec. `err` is set and false returned on any of the
// refusals above.
bool build_series(const Table& t, const Spec& s, std::vector<Series>& out,
                  std::string& err);

}  // namespace ech
