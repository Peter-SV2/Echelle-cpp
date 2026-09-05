// Everything the panel does that is not drawing.
//
// Separate from ui.cpp so it compiles and runs without a window: these are the
// operations a test can call, and the drawing is a thin layer over them.
#include <algorithm>
#include <cstdio>
#include <utility>

#include "gpexport.hpp"
#include "num.hpp"
#include "ui.hpp"

namespace ech {
namespace {

std::string fmt(const char* f, double v) {
    char b[128];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

void line(std::string& out, const std::string& k, double v) {
    char b[160];
    std::snprintf(b, sizeof b, "%-22s %14.6g\n", k.c_str(), v);
    out += b;
}

}  // namespace

bool App::open(const std::string& path) {
    try {
        Table t = Table::load(path);
        auto d = std::make_unique<Doc>(std::move(t));
        // Sensible defaults so the first plot is one click, not six: the first
        // two numeric columns are what anyone would have picked.
        for (std::size_t i = 0; i < d->table.ncols(); ++i) {
            if (!d->table.cols()[i].numeric) continue;
            if (d->x < 0) d->x = static_cast<int>(i);
            else if (d->ys.empty()) d->ys.push_back(static_cast<int>(i));
        }
        d->fit_x = d->x;
        d->fit_y = d->ys.empty() ? -1 : d->ys[0];
        d->test_a = d->x;
        d->test_b = d->fit_y;
        docs.push_back(std::move(d));
        cur = static_cast<int>(docs.size()) - 1;
        sync_spec_from_pickers();
        refresh_series();
        status = docs.back()->table.name() + ": " +
                 std::to_string(docs.back()->table.rows()) + " row(s), " +
                 std::to_string(docs.back()->table.ncols()) + " column(s)";
        return true;
    } catch (const std::exception& e) {
        status = std::string("could not open: ") + e.what();
        return false;
    }
}

void App::close_current() {
    if (cur < 0) return;
    docs.erase(docs.begin() + cur);
    cur = docs.empty() ? -1 : std::min(cur, static_cast<int>(docs.size()) - 1);
    if (cur >= 0) select(cur);
}

void App::select(int i) {
    if (i < 0 || i >= static_cast<int>(docs.size())) return;
    cur = i;
    sync_spec_from_pickers();
    refresh_series();
}

void App::sync_spec_from_pickers() {
    Doc* d = doc();
    if (!d) return;
    Spec s;
    s.kind = d->spec.kind;
    s.bins = d->spec.bins;
    s.title = d->spec.title;
    const auto& cols = d->table.cols();
    if (d->x >= 0) s.x = cols[static_cast<std::size_t>(d->x)].name;
    for (int y : d->ys) s.y.push_back(cols[static_cast<std::size_t>(y)].name);
    if (d->split >= 0) s.split = cols[static_cast<std::size_t>(d->split)].name;
    s.where = d->where;
    s.logx = d->logx;
    s.logy = d->logy;
    d->spec = s;
    spec_text_buf = spec_text(s);
}

bool App::apply_spec_text() {
    Doc* d = doc();
    if (!d) return false;
    Spec s;
    std::string err;
    // Hand edits WIN. The declaration is what gets drawn, and a picker that
    // silently overwrote it would make the text box a decoration.
    if (!parse_spec(spec_text_buf, s, err)) {
        status = err;
        return false;
    }
    d->spec = s;
    d->x = d->table.index_of(s.x);
    d->ys.clear();
    for (const auto& y : s.y) {
        const int i = d->table.index_of(y);
        if (i >= 0) d->ys.push_back(i);
    }
    d->split = s.split.empty() ? -1 : d->table.index_of(s.split);
    d->where = s.where;
    d->logx = s.logx;
    d->logy = s.logy;
    refresh_series();
    return true;
}

void App::refresh_series() {
    Doc* d = doc();
    if (!d) return;
    d->series_err.clear();
    if (!build_series(d->table, d->spec, d->series, d->series_err)) {
        d->series.clear();
        status = d->series_err;
    } else {
        std::size_t n = 0;
        for (const auto& s : d->series) n += s.xs.size();
        status = std::to_string(n) + " point(s), " +
                 std::to_string(d->series.size()) + " series";
        if (d->table.n_excluded())
            status += "  (" + std::to_string(d->table.n_excluded()) +
                      " row(s) excluded)";
    }
    d->series_dirty = false;
}

void App::run_fit() {
    Doc* d = doc();
    fit_report.clear();
    if (!d) { status = "open a table first"; return; }
    if (d->fit_x < 0 || d->fit_y < 0) { status = "pick x and y to fit"; return; }
    std::vector<double> xs, ys;
    d->table.series(d->fit_x, d->fit_y, xs, ys);
    if (xs.size() < 2) { status = "not enough numeric pairs"; return; }

    d->fit = (d->fit_model == Model::Linear) ? linfit(xs, ys)
                                             : nlinfit(xs, ys, d->fit_model);
    d->has_fit = d->fit.ok;
    if (!d->fit.ok) {
        fit_report = "fit failed: " + d->fit.error + "\n";
        status = d->fit.error;
        return;
    }
    const Fit& f = d->fit;
    fit_report = f.model + "  on " + std::to_string(f.n) + " included point(s)";
    // EXCLUSION IS REPORTED, ALWAYS. A fit that quietly used fewer points than
    // the table shows is how a result stops being reproducible.
    if (d->table.n_excluded())
        fit_report += "  (" + std::to_string(d->table.n_excluded()) +
                      " row(s) excluded)";
    fit_report += "\n\n";
    for (std::size_t i = 0; i < f.names.size(); ++i) {
        char b[220];
        if (i < f.se.size() && i < f.ci_lo.size())
            std::snprintf(b, sizeof b, "%-12s %14.6g   SE %-12.6g  95%% CI %.6g to %.6g\n",
                          f.names[i].c_str(), f.values[i], f.se[i], f.ci_lo[i],
                          f.ci_hi[i]);
        else
            std::snprintf(b, sizeof b, "%-12s %14.6g   SE not identifiable\n",
                          f.names[i].c_str(), f.values[i]);
        fit_report += b;
    }
    fit_report += "\n";
    line(fit_report, "R squared", f.r2);
    line(fit_report, "Sy.x", f.sy_x);
    line(fit_report, "df", f.df);
    line(fit_report, "SSE", f.sse);
    if (f.id == Model::Linear) line(fit_report, "p (slope != 0)", f.p_slope);
    if (f.se.empty() && f.df > 0)
        fit_report +=
            "\nNo standard errors: J'J is singular, so these parameters are not\n"
            "separately identifiable from this data. Reporting one would be a lie.\n";
    status = f.model + " fitted, r2 = " + fmt("%.5g", f.r2);
    refresh_series();
}

void App::run_interp() {
    Doc* d = doc();
    if (!d || !d->has_fit) { status = "run a fit first"; return; }
    const double x = to_num(interp_buf);
    if (!is_num(x)) { status = "interpolate needs a number"; return; }
    const double y = predict(d->fit, x);
    char b[200];
    // Say when it is OUTSIDE the fitted range. A model evaluated past its data
    // is an extrapolation, and it looks exactly like an interpolation.
    const bool out = x < d->fit.x_lo || x > d->fit.x_hi;
    std::snprintf(b, sizeof b, "\ninterpolated: x = %.6g  ->  y = %.6g%s\n", x, y,
                  out ? "   EXTRAPOLATED -- outside the fitted x range" : "");
    fit_report += b;
    status = out ? "outside the fitted range -- extrapolated"
                 : "interpolated from the fit";
}

void App::run_test() {
    Doc* d = doc();
    test_report.clear();
    if (!d) { status = "open a table first"; return; }
    const auto A = d->test_a >= 0 ? d->table.values(d->test_a) : std::vector<double>{};
    const auto B = d->test_b >= 0 ? d->table.values(d->test_b) : std::vector<double>{};

    TestResult r;
    switch (d->test_kind) {
        case 0: {   // describe
            const Describe s = describe(A);
            r.ok = s.n > 0;
            r.name = "describe";
            r.stats = {{"n", static_cast<double>(s.n)}, {"mean", s.mean},
                       {"SD", s.sd}, {"SEM", s.sem}, {"95% CI low", s.ci_lo},
                       {"95% CI high", s.ci_hi}, {"min", s.min}, {"Q1", s.q1},
                       {"median", s.median}, {"Q3", s.q3}, {"max", s.max}};
            if (!r.ok) r.error = "no numeric values in that column";
            break;
        }
        case 1: r = ttest(A, B, false); break;
        case 2: r = ttest(A, B, true); break;
        case 3: r = one_sample_t(A, 0.0); break;
        case 4: {   // one-way ANOVA, grouped by a column
            if (d->test_by < 0) { r.error = "pick a column to group by"; break; }
            const auto& g = d->table.groups(d->test_by);
            const auto& V = d->table.cols()[static_cast<std::size_t>(d->test_a)].num;
            std::vector<std::vector<double>> buckets;
            std::vector<std::uint32_t> seen;
            for (std::size_t i = 0; i < d->table.rows(); ++i) {
                if (d->table.excluded(i) || !is_num(V[i])) continue;
                auto it = std::find(seen.begin(), seen.end(), g[i]);
                if (it == seen.end()) {
                    seen.push_back(g[i]);
                    buckets.emplace_back();
                    it = seen.end() - 1;
                }
                buckets[static_cast<std::size_t>(it - seen.begin())].push_back(V[i]);
            }
            r = anova1(buckets);
            break;
        }
        case 5: r = pearson(A, B); break;
        case 6: r = spearman(A, B); break;
        case 7: r = mannwhitney(A, B); break;
        default: r.error = "unknown test"; break;
    }

    if (!r.ok) {
        test_report = "cannot run: " + r.error + "\n";
        status = r.error;
        return;
    }
    test_report = r.name;
    if (d->table.n_excluded())
        test_report += "   (" + std::to_string(d->table.n_excluded()) +
                       " row(s) excluded)";
    test_report += "\n\n";
    for (const auto& [k, v] : r.stats) line(test_report, k, v);
    if (!r.note.empty()) test_report += "\n" + r.note + "\n";
    status = r.name + " done";
}

void App::suggest_outliers(std::vector<std::size_t>& select_rows) {
    select_rows.clear();
    Doc* d = doc();
    if (!d || d->ys.empty()) { status = "pick a y column first"; return; }
    const int c = d->ys[0];
    const auto& V = d->table.cols()[static_cast<std::size_t>(c)].num;
    std::vector<double> vals;
    std::vector<std::size_t> idx;
    for (std::size_t i = 0; i < d->table.rows(); ++i)
        if (is_num(V[i])) { vals.push_back(V[i]); idx.push_back(i); }
    for (std::size_t k : outliers(vals)) select_rows.push_back(idx[k]);
    // SUGGESTED, never applied. A point dropped without a trace is how a
    // result stops being reproducible, and Prism's reviewers ask for exactly
    // this.
    status = std::to_string(select_rows.size()) + " suggested outlier(s) in " +
             d->table.cols()[static_cast<std::size_t>(c)].name +
             " -- selected, not excluded; press Exclude to accept";
}

bool App::export_figure(const std::string& stem) {
    Doc* d = doc();
    if (!d || d->series.empty()) { status = "nothing plotted yet"; return false; }
    ExportResult r = write_gnuplot(stem, d->table, d->spec, d->series,
                                   (d->has_fit && d->draw_fit) ? &d->fit : nullptr);
    if (!r.ok) { status = r.error; return false; }
    status = "wrote " + std::to_string(r.written.size()) +
             " file(s): the script, its data and the numbers";
    return true;
}

}  // namespace ech
