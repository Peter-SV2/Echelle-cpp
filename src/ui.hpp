// The panel, as state plus one draw call per frame.
//
// Immediate mode changes what a "widget" is. There are no Tk objects to
// create, configure and keep in sync with the data -- the Python's `_switch`
// existed only to push a table's columns into six comboboxes and a listbox and
// pull them back out again, and every bug in it was the two drifting apart.
// Here the combobox reads the table when it draws. There is one copy of the
// state and it is the table.
//
// This header is deliberately free of ImGui: it is the app's state and the
// operations on it, so the parts that decide WHAT to show can be compiled and
// exercised without a window. Only ui.cpp includes imgui.h.
#pragma once

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "fitstat.hpp"
#include "spec.hpp"
#include "table.hpp"

namespace ech {

// One open file and everything the panel remembers about it, so the rail
// switches between ANALYSES rather than between files: each table keeps the
// plot it was showing, the fit that was run on it and the rows set aside.
struct Doc {
    Table table;
    Spec spec;
    Fit fit;
    bool has_fit = false;
    std::vector<Series> series;
    std::string series_err;
    bool series_dirty = true;

    // Picker state, as indices into the table's own columns. Indices, not
    // names: a name that no longer exists silently plots the previous table's
    // column, which is the bug `_switch` kept re-introducing.
    int x = -1, split = -1;
    std::vector<int> ys;
    int fit_x = -1, fit_y = -1;
    Model fit_model = Model::Linear;
    int test_a = -1, test_b = -1, test_by = -1;
    int test_kind = 0;
    std::string where;
    bool logx = false, logy = false, draw_fit = true;

    // Which rows the pointer has picked out. A set, not a flag per row: a
    // selection is almost always a handful out of tens of thousands.
    std::set<std::size_t> row_selected;

    explicit Doc(Table&& t) : table(std::move(t)) {}
};

// Add, remove or replace one y column in a doc's selection.
//
// It lives here rather than inside the y-list widget because it is state, not
// drawing -- and because the version that lived in the widget cleared the
// vector and then erased an iterator found in the emptied one, which is
// undefined behaviour and crashed the program. A rule with two branches
// deserves a test, and nothing in a draw call can have one.
//
// `additive` is Ctrl held: toggle this column in the selection. Without it the
// selection becomes exactly this column.
void toggle_y(Doc& d, int col, bool additive);

enum class Section { Data, Plot, Fit, Stats };

struct App {
    std::vector<std::unique_ptr<Doc>> docs;
    int cur = -1;
    Section section = Section::Data;
    std::string status = "open a file";
    std::string spec_text_buf;      // the editable declaration
    std::string interp_buf;
    std::string fit_report, test_report;

    Doc* doc() { return cur >= 0 && cur < static_cast<int>(docs.size())
                            ? docs[static_cast<std::size_t>(cur)].get()
                            : nullptr; }

    bool open(const std::string& path);
    void close_current();
    void select(int i);

    // Rebuilds the series from the declaration. Called when something that
    // affects the plot changes -- NOT every frame: at 50,000 points the build
    // is 1.2 ms, which is fine on a change and wasteful at 60 Hz.
    void refresh_series();
    void sync_spec_from_pickers();
    bool apply_spec_text();

    // Where this session was last written, so Save does not ask again. Empty
    // until it has been saved once.
    std::string session_path;

    void run_fit();
    void run_interp();
    void run_test();
    void suggest_outliers(std::vector<std::size_t>& select_rows);
    bool export_figure(const std::string& stem);
};

// Asking the user for a path is the one thing the panel cannot do for itself:
// a file dialog belongs to the platform. Declared here and DEFINED BY THE HOST
// (main.cpp), so ui.cpp stays free of windows.h and the core stays free of
// both. Returns an empty string when the user cancels.
enum class Ask { OpenTable, OpenSession, SaveSession, SaveFigure };
std::string ask_path(Ask what);

// The one place a frame is drawn. Returns false when the user has asked to
// close the window.
bool draw(App& app);

}  // namespace ech
