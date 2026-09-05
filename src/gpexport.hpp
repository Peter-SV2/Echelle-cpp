// Export a figure as a gnuplot script, its data, and a caption.
//
// The app DRAWS with ImPlot -- no subprocess, no SVG, no file round-trip, and
// a redraw costs microseconds instead of the 846 ms a gnuplot spawn cost. But
// the .gp script was never about drawing on screen: it is a plain-text record
// of how a figure was made that a reviewer can rerun, and dropping it to gain
// speed would have traded away the reproducibility for a cost that only ever
// applied to the interactive path.
//
// So this runs on Save, not on every redraw. gnuplot is optional now: if it is
// not installed the script and the numbers are still written, and they are
// what a reader actually needs.
#pragma once

#include <string>
#include <vector>

#include "fitstat.hpp"
#include "spec.hpp"
#include "table.hpp"

namespace ech {

struct ExportResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> written;   // every path this produced
};

// Writes <stem>.gp, <stem>.dN.dat per series, <stem>.csv and
// <stem>.caption.md beside `out_stem`.
//
// `fit` may be null. When present its curve is written as an extra series in
// ink rather than a series colour: the curve is a statement ABOUT the data
// rather than more data, and giving it a place in the same colour cycle
// invites it to be read as another measurement.
ExportResult write_gnuplot(const std::string& out_stem, const Table& t,
                           const Spec& s, const std::vector<Series>& series,
                           const Fit* fit);

}  // namespace ech
