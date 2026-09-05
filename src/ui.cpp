// The frame. One function, called once per frame, that reads the state and
// draws it -- there is nothing to keep in sync because there is nothing that
// holds a second copy.
#include "ui.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "imgui.h"
#include "implot.h"
#include "misc/cpp/imgui_stdlib.h"
#include "num.hpp"
#include "palette.hpp"

namespace ech {
namespace {

// The palette from theme.py, so the app and the figures it exports stay one
// object rather than two designs that ship together.
constexpr ImVec4 kTeal{0.094f, 0.416f, 0.325f, 1.0f};      // #186a53
constexpr ImVec4 kCrimson{0.659f, 0.196f, 0.275f, 1.0f};   // #a83246
constexpr ImVec4 kDim{0.427f, 0.427f, 0.427f, 1.0f};
constexpr ImVec4 kInk{0.102f, 0.102f, 0.102f, 1.0f};

// Sampled from the shared ramp, so n series get n distinct colours and the
// figure on screen matches the one the export writes.
ImVec4 series_col(std::size_t i, std::size_t n) {
    const Rgb c = series_colour(i, n);
    return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);
}

// The cell being edited, if any. Immediate mode has no widget to own this, so
// it is one place in the frame's own state.
int g_edit_row = -1, g_edit_col = -1;
std::string g_edit_buf;

const char* kTests[] = {"describe", "t test (unpaired)", "t test (paired)",
                        "one-sample t", "one-way ANOVA", "Pearson r",
                        "Spearman rho", "Mann-Whitney U"};

const char* kNotes[] = {
    "Open several tables. Double-click a cell to edit it. Select rows and "
    "press Exclude to leave them out of every plot, fit and test.",
    "Pick x, and Ctrl-click for more than one y. The table beside the plot is "
    "editable -- double-click a cell. The declaration is what gets drawn and "
    "can be edited by hand: y=A1,B1 is the same as Ctrl-clicking both.",
    "Linear or nonlinear regression on the included points. Interpolate reads "
    "a value off the fitted curve.",
    "Tests run on the included rows of the selected table.",
};

// A caption BEFORE its control. ImGui puts a widget's label on the right, so
// the natural spelling reads "scatter [v] kind  time [v] x axis" -- every
// caption sitting against the control after it rather than its own. The label
// is hidden with ## and drawn ahead instead.
void caption(const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine();
}

// A quiet line of instruction. For the things a control can do that looking at
// it will not tell you -- which is exactly the case that needs saying, and the
// case most likely to go unwritten.
void hint(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// A column picker. Reads the table when it draws, so it cannot show a column
// the table does not have.
bool column_combo(const char* label, const Table& t, int& sel, bool numeric_only,
                  bool allow_none = false) {
    const char* preview = sel >= 0 && sel < static_cast<int>(t.ncols())
                              ? t.cols()[static_cast<std::size_t>(sel)].name.c_str()
                              : "(none)";
    bool changed = false;
    caption(label);
    char id[64];
    std::snprintf(id, sizeof id, "##%s", label);
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo(id, preview)) {
        if (allow_none && ImGui::Selectable("(none)", sel < 0)) {
            sel = -1;
            changed = true;
        }
        for (std::size_t i = 0; i < t.ncols(); ++i) {
            if (numeric_only && !t.cols()[i].numeric) continue;
            const bool on = sel == static_cast<int>(i);
            if (ImGui::Selectable(t.cols()[i].name.c_str(), on)) {
                sel = static_cast<int>(i);
                changed = true;
            }
            if (on) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

// The data grid. CLIPPED: only the rows actually on screen are built, so a
// 50,000-row table costs the same as a 50-row one. This is the immediate-mode
// answer to the 50,052 canvas objects the Tk version created.
void draw_grid(App& app, Doc& d, const char* id, ImVec2 size) {
    const Table& t = d.table;
    const int ncol = static_cast<int>(t.ncols());
    if (ncol == 0) return;
    constexpr ImGuiTableFlags kFlags =
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable(id, ncol + 1, kFlags, size)) return;

    ImGui::TableSetupScrollFreeze(1, 1);
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 52.0f);
    for (int c = 0; c < ncol; ++c)
        ImGui::TableSetupColumn(t.cols()[static_cast<std::size_t>(c)].name.c_str());
    ImGui::TableHeadersRow();

    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(t.rows()));
    while (clip.Step()) {
        for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
            const std::size_t row = static_cast<std::size_t>(r);
            ImGui::TableNextRow();
            // An excluded row stays VISIBLE and greyed. Removing it would hide
            // the decision; this way the table still shows what was set aside.
            const bool out = t.excluded(row);
            if (out)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       ImGui::GetColorU32(ImVec4(0.90f, 0.90f, 0.90f, 1.0f)));
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(r);
            char lbl[24];
            std::snprintf(lbl, sizeof lbl, "%d", r);
            const bool sel = d.row_selected.count(row) != 0;
            if (ImGui::Selectable(lbl, sel,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowOverlap)) {
                if (!ImGui::GetIO().KeyCtrl) d.row_selected.clear();
                if (sel) d.row_selected.erase(row); else d.row_selected.insert(row);
            }
            for (int c = 0; c < ncol; ++c) {
                ImGui::TableSetColumnIndex(c + 1);
                if (g_edit_row == r && g_edit_col == c) {
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::SetKeyboardFocusHere();
                    const bool done = ImGui::InputText(
                        "##edit", &g_edit_buf,
                        ImGuiInputTextFlags_EnterReturnsTrue |
                            ImGuiInputTextFlags_AutoSelectAll);
                    if (done || ImGui::IsItemDeactivated()) {
                        if (done) {
                            // The edit lands in the table, so the next plot,
                            // fit and test use it. A value you can change on
                            // screen but not in the analysis is worse than one
                            // you cannot change at all.
                            d.table.set_cell(row, static_cast<std::size_t>(c),
                                             g_edit_buf);
                            d.series_dirty = true;
                            app.status = "edited row " + std::to_string(r) +
                                         ", " + t.cols()[static_cast<std::size_t>(c)].name;
                        }
                        g_edit_row = g_edit_col = -1;
                    }
                } else {
                    const std::string txt =
                        t.cell_text(row, static_cast<std::size_t>(c));
                    if (out) ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                    ImGui::TextUnformatted(txt.c_str());
                    if (out) ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered() &&
                        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        g_edit_row = r;
                        g_edit_col = c;
                        g_edit_buf = txt;
                    }
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void exclude_bar(App& app, Doc& d) {
    if (ImGui::Button("Exclude selected")) {
        for (std::size_t r : d.row_selected) d.table.set_excluded(r, true);
        d.series_dirty = true;
        app.status = d.table.name() + ": " +
                     std::to_string(d.table.n_excluded()) + " of " +
                     std::to_string(d.table.rows()) + " row(s) excluded";
    }
    ImGui::SameLine();
    if (ImGui::Button("Include")) {
        for (std::size_t r : d.row_selected) d.table.set_excluded(r, false);
        d.series_dirty = true;
        app.status = d.table.name() + ": " +
                     std::to_string(d.table.n_excluded()) + " of " +
                     std::to_string(d.table.rows()) + " row(s) excluded";
    }
    ImGui::SameLine();
    if (ImGui::Button("Suggest outliers")) {
        std::vector<std::size_t> hits;
        app.suggest_outliers(hits);
        d.row_selected.clear();
        for (std::size_t r : hits) d.row_selected.insert(r);
    }
    ImGui::SameLine();
    hint(d.row_selected.empty()
             ? "(Ctrl-click rows to select several)"
             : ("(" + std::to_string(d.row_selected.size()) +
                " row(s) selected)").c_str());
}

void draw_plot(Doc& d) {
    if (!d.series_err.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kCrimson);
        ImGui::TextWrapped("%s", d.series_err.c_str());
        ImGui::PopStyleColor();
        return;
    }
    if (!ImPlot::BeginPlot("##figure", ImVec2(-1, -1))) return;

    const std::string xlab = d.spec.x;
    const std::string ylab = d.spec.kind == Kind::Hist
                                 ? "count"
                                 : (d.spec.y.empty() ? "" : d.spec.y[0]);
    ImPlot::SetupAxes(xlab.c_str(), ylab.c_str());
    if (d.logx) ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
    if (d.logy) ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Log10);

    for (std::size_t i = 0; i < d.series.size(); ++i) {
        const Series& s = d.series[i];
        if (s.xs.empty()) continue;
        const ImVec4 col = series_col(i, d.series.size());
        const int n = static_cast<int>(s.xs.size());
        // THE POINT OF THE PORT. s.xs.data() is the vector that was filled
        // when the series was built; ImPlot walks it directly. No per-point
        // object is created, nothing is copied, and the cost of a redraw does
        // not grow with the number of points the way one canvas item each did.
        switch (d.spec.kind) {
            case Kind::Line:
                ImPlot::SetNextLineStyle(col, 2.0f);
                ImPlot::PlotLine(s.label.c_str(), s.xs.data(), s.ys.data(), n);
                break;
            case Kind::Hist:
            case Kind::Bars:
                ImPlot::SetNextFillStyle(col, 0.9f);
                ImPlot::PlotBars(s.label.c_str(), s.xs.data(), s.ys.data(), n,
                                 s.xs.size() > 1 ? (s.xs[1] - s.xs[0]) * 0.85 : 0.67);
                break;
            case Kind::Scatter:
                ImPlot::SetNextMarkerStyle(ImPlotMarker_Circle, 3.0f, col, 0.0f, col);
                ImPlot::PlotScatter(s.label.c_str(), s.xs.data(), s.ys.data(), n);
                break;
        }
    }

    // The fitted curve, in ink rather than a series colour: the curve is a
    // statement ABOUT the data rather than more data, and giving it a place in
    // the same colour cycle invites it to be read as another measurement.
    if (d.has_fit && d.draw_fit && d.fit.ok) {
        std::vector<double> cx, cy;
        curve(d.fit, d.fit.x_lo, d.fit.x_hi, 200, cx, cy);
        ImPlot::SetNextLineStyle(kInk, 2.0f);
        ImPlot::PlotLine("fit", cx.data(), cy.data(), static_cast<int>(cx.size()));
    }
    ImPlot::EndPlot();
}

void section_data(App& app, Doc& d) {
    exclude_bar(app, d);
    ImGui::Separator();
    draw_grid(app, d, "##datagrid", ImVec2(-1, -1));
}

void section_plot(App& app, Doc& d) {
    bool dirty = false;
    const char* kinds[] = {"scatter", "line", "hist", "bars"};
    int ki = static_cast<int>(d.spec.kind);
    caption("kind");
    ImGui::SetNextItemWidth(110);
    if (ImGui::Combo("##kind", &ki, kinds, 4)) {
        d.spec.kind = static_cast<Kind>(ki);
        dirty = true;
    }
    ImGui::SameLine();
    dirty |= column_combo("x axis", d.table, d.x, true);
    ImGui::SameLine();
    dirty |= column_combo("colour by", d.table, d.colour, false, true);
    ImGui::SameLine();
    caption("where");
    ImGui::SetNextItemWidth(200);
    if (ImGui::InputText("##where", &d.where,
                         ImGuiInputTextFlags_EnterReturnsTrue))
        dirty = true;

    // y is multi-select, so it is a list rather than a combo. Nothing about a
    // list of names says that though, so it is written down: a modifier you
    // have to already know about is a feature that does not exist.
    caption("y axis");
    if (ImGui::BeginChild("##ylist", ImVec2(240, 84), ImGuiChildFlags_Border)) {
        for (std::size_t i = 0; i < d.table.ncols(); ++i) {
            if (!d.table.cols()[i].numeric) continue;
            const int ci = static_cast<int>(i);
            const bool on = std::find(d.ys.begin(), d.ys.end(), ci) != d.ys.end();
            if (ImGui::Selectable(d.table.cols()[i].name.c_str(), on)) {
                if (!ImGui::GetIO().KeyCtrl) d.ys.clear();
                if (on) d.ys.erase(std::find(d.ys.begin(), d.ys.end(), ci));
                else d.ys.push_back(ci);
                dirty = true;
            }
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginGroup();
    dirty |= ImGui::Checkbox("log x", &d.logx);
    dirty |= ImGui::Checkbox("log y", &d.logy);
    ImGui::Checkbox("draw fitted curve", &d.draw_fit);
    ImGui::EndGroup();
    hint("Ctrl-click in the y list to plot several columns against the same x. "
         "Each becomes its own series; colour= is ignored then, because the "
         "series already ARE the columns.");

    if (dirty) {
        app.sync_spec_from_pickers();
        app.refresh_series();
    }

    caption("declaration");
    ImGui::SetNextItemWidth(-260);
    const bool entered = ImGui::InputText("##declaration", &app.spec_text_buf,
                                          ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Plot") || entered) app.apply_spec_text();
    ImGui::SameLine();
    if (ImGui::Button("Export .gp + data")) app.export_figure("echelle-figure");
    ImGui::Separator();

    // The table beside the plot, not on another tab: a number you have to
    // change somewhere else is a number you change without seeing what it does.
    const float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::BeginChild("##left", ImVec2(w * 0.34f, -1))) {
        exclude_bar(app, d);
        draw_grid(app, d, "##plotgrid", ImVec2(-1, -1));
    }
    ImGui::EndChild();
    ImGui::SameLine();
    if (ImGui::BeginChild("##right", ImVec2(0, -1))) draw_plot(d);
    ImGui::EndChild();

    if (d.series_dirty) app.refresh_series();
}

void section_fit(App& app, Doc& d) {
    int mi = 0;
    const auto& ms = models();
    for (std::size_t i = 0; i < ms.size(); ++i)
        if (ms[i].id == d.fit_model) mi = static_cast<int>(i);
    std::vector<const char*> names;
    for (const auto& m : ms) names.push_back(m.name);
    caption("model");
    ImGui::SetNextItemWidth(170);
    if (ImGui::Combo("##model", &mi, names.data(), static_cast<int>(names.size())))
        d.fit_model = ms[static_cast<std::size_t>(mi)].id;
    ImGui::SameLine();
    column_combo("fit x", d.table, d.fit_x, true);
    ImGui::SameLine();
    column_combo("fit y", d.table, d.fit_y, true);
    ImGui::SameLine();
    if (ImGui::Button("Fit")) app.run_fit();

    caption("interpolate at x =");
    ImGui::SetNextItemWidth(120);
    ImGui::InputText("##interp", &app.interp_buf);
    ImGui::SameLine();
    if (ImGui::Button("Interpolate")) app.run_interp();
    ImGui::Separator();
    ImGui::PushFont(nullptr);
    ImGui::TextUnformatted(app.fit_report.c_str());
    ImGui::PopFont();
}

void section_stats(App& app, Doc& d) {
    caption("test");
    ImGui::SetNextItemWidth(180);
    ImGui::Combo("##test", &d.test_kind, kTests,
                 static_cast<int>(sizeof kTests / sizeof *kTests));
    ImGui::SameLine();
    column_combo("a", d.table, d.test_a, true);
    ImGui::SameLine();
    column_combo("b", d.table, d.test_b, true, true);
    ImGui::SameLine();
    column_combo("group by", d.table, d.test_by, false, true);
    ImGui::SameLine();
    if (ImGui::Button("Run")) app.run_test();
    ImGui::Separator();
    ImGui::TextUnformatted(app.test_report.c_str());
}

}  // namespace

bool draw(App& app) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    constexpr ImGuiWindowFlags kRoot =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("##root", nullptr, kRoot);

    // --- the rail, which is ours: Windows has no equivalent -----------------
    if (ImGui::BeginChild("##rail", ImVec2(200, -28), ImGuiChildFlags_Border)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kTeal);
        ImGui::TextUnformatted("Echelle");
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("graph, fit, test");
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();

        const char* labels[] = {"Data", "Plot", "Fit", "Stats"};
        for (int i = 0; i < 4; ++i)
            if (ImGui::Selectable(labels[i],
                                  app.section == static_cast<Section>(i)))
                app.section = static_cast<Section>(i);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("TABLES");
        ImGui::PopStyleColor();
        for (int i = 0; i < static_cast<int>(app.docs.size()); ++i) {
            ImGui::PushID(i);
            if (ImGui::Selectable(app.docs[static_cast<std::size_t>(i)]
                                      ->table.name().c_str(),
                                  app.cur == i))
                app.select(i);
            ImGui::PopID();
        }
        ImGui::Spacing();
        if (ImGui::Button("Close table")) app.close_current();

        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 96);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", kNotes[static_cast<int>(app.section)]);
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    if (ImGui::BeginChild("##body", ImVec2(0, -28))) {
        Doc* d = app.doc();
        if (!d) {
            ImGui::TextUnformatted(
                "No table open. Pass a .csv on the command line, or drop one "
                "on the window.");
        } else {
            switch (app.section) {
                case Section::Data:  section_data(app, *d); break;
                case Section::Plot:  section_plot(app, *d); break;
                case Section::Fit:   section_fit(app, *d); break;
                case Section::Stats: section_stats(app, *d); break;
            }
        }
    }
    ImGui::EndChild();

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(app.status.c_str());
    ImGui::PopStyleColor();
    ImGui::End();
    return true;
}

}  // namespace ech
