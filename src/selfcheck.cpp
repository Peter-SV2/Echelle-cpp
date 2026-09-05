// The self-checks, carried over from the Python they replace.
//
// These are the SAME assertions: published critical values for t and F,
// parameters recovered from curves generated with known ones, the delimiter
// sniffed by counting, an excluded row leaving every consumer, and the two
// bugs that cost real data here -- the sidecar overwriting its own input, and
// a colour column with a distinct value per row drawing one legend entry per
// point. Porting a program without porting what proved it works is how a
// rewrite ships the same bugs with better performance.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "fitstat.hpp"
#include "gpexport.hpp"
#include "num.hpp"
#include "spec.hpp"
#include "table.hpp"
#include "ui.hpp"

using namespace ech;

namespace {

int failures = 0;

void check(bool cond, const std::string& what) {
    if (!cond) {
        std::printf("  FAIL  %s\n", what.c_str());
        ++failures;
    }
}

void near(double got, double want, double tol, const std::string& what) {
    if (!(std::fabs(got - want) <= tol)) {
        std::printf("  FAIL  %s: got %.10g want %.10g (tol %g)\n", what.c_str(),
                    got, want, tol);
        ++failures;
    }
}

std::filesystem::path tmpdir() {
    auto d = std::filesystem::temp_directory_path() / "echelle_selfcheck";
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}

void write(const std::filesystem::path& p, const std::string& s) {
    std::ofstream f(p, std::ios::binary);
    f << s;
}

std::string slurp(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// --- numbers ---------------------------------------------------------------

void check_parsing() {
    check(!is_num(to_num("")), "an empty field is not zero");
    check(!is_num(to_num("n/a")), "text is not a number");
    // THE ONE THAT MATTERS. Accepting a prefix reads "1,5" as 1 and "12abc" as
    // 12, which is how a comma-decimal file becomes a plot that is wrong and
    // looks fine.
    check(!is_num(to_num("12abc")), "a partial parse is refused");
    check(!is_num(to_num("1,5")), "a comma decimal is refused, not truncated");
    near(to_num(" 8.5 "), 8.5, 0, "surrounding space is trimmed");
    near(to_num("-3e2"), -300.0, 0, "exponent form");
    near(to_num("1.10\r"), 1.10, 0, "a CRLF file leaves a stray carriage return");
}

// --- distributions ---------------------------------------------------------

void check_distributions() {
    // Published two-tailed critical values.
    near(t_sf2(2.228139, 10), 0.05, 1e-6, "t_sf2 at the 5% point, df=10");
    near(t_sf2(1.959964, 1e9), 0.05, 1e-5, "t approaches normal at large df");
    near(t_crit(10, 0.95), 2.228139, 1e-5, "t_crit df=10");
    near(t_crit(1e9, 0.95), 1.959964, 1e-4, "t_crit at large df is z");
    near(f_sf(4.103, 2, 10), 0.05, 1e-3, "F_sf(4.103; 2,10)");
    near(f_sf(3.478, 3, 12), 0.05, 1e-3, "F_sf(3.478; 3,12)");
    near(z_sf2(1.959964), 0.05, 1e-6, "two-tailed normal at 1.96");
    near(betainc(2, 3, 0.5), 0.6875, 1e-12, "I_0.5(2,3) is exact");
    check(t_sf2(0, 5) == 1.0, "t=0 is p=1");
}

// --- regression ------------------------------------------------------------

void check_linfit() {
    std::vector<double> xs, ys;
    for (int i = 0; i < 20; ++i) {
        xs.push_back(i * 0.5);
        ys.push_back(3.0 * (i * 0.5) - 1.5);
    }
    Fit f = linfit(xs, ys);
    check(f.ok, "linfit runs");
    near(f.slope, 3.0, 1e-9, "slope recovered");
    near(f.intercept, -1.5, 1e-9, "intercept recovered");
    near(f.r2, 1.0, 1e-12, "an exact line is r2 = 1");
    near(predict(f, 10.0), 28.5, 1e-9, "interpolation off the line");

    Fit bad = linfit(std::vector<double>{1.0}, std::vector<double>{1.0});
    check(!bad.ok, "one point is not a regression");
    std::vector<double> flat = {2.0, 2.0, 2.0};
    Fit v = linfit(flat, flat);
    check(!v.ok && v.error == "all x are identical", "a vertical line is refused");
}

void check_nlinfit() {
    // Generated from KNOWN parameters, then recovered. This is the check that
    // a fit is a fit and not a curve that happens to pass near the points.
    std::vector<double> xs, ys;
    for (int i = 0; i < 40; ++i) {
        const double x = i * 0.25;
        xs.push_back(x);
        ys.push_back(5.0 * std::exp(-0.7 * x) + 1.0);
    }
    Fit f = nlinfit(xs, ys, Model::ExpDecay);
    check(f.ok, "exp_decay fits");
    near(f.r2, 1.0, 1e-6, "exp_decay r2");
    near(f.values[1], 0.7, 1e-3, "exp_decay recovers k");
    near(f.values[2], 1.0, 1e-2, "exp_decay recovers the plateau");

    std::vector<double> mx, my;
    for (int i = 1; i <= 30; ++i) {
        const double x = i * 2.0;
        mx.push_back(x);
        my.push_back(120.0 * x / (15.0 + x));
    }
    Fit mm = nlinfit(mx, my, Model::MichaelisMenten);
    check(mm.ok, "michaelis_menten fits");
    near(mm.values[0], 120.0, 0.5, "Vmax recovered");
    near(mm.values[1], 15.0, 0.5, "Km recovered");

    std::vector<double> gx, gy;
    for (int i = 0; i < 60; ++i) {
        const double x = -3.0 + i * 0.1;
        gx.push_back(x);
        gy.push_back(2.5 * std::exp(-((x - 0.4) * (x - 0.4)) / (2.0 * 0.8 * 0.8)));
    }
    Fit g = nlinfit(gx, gy, Model::Gaussian);
    check(g.ok, "gaussian fits");
    near(g.values[1], 0.4, 1e-2, "gaussian recovers the mean");
    near(std::fabs(g.values[2]), 0.8, 1e-2, "gaussian recovers the sd");

    Fit few = nlinfit(std::vector<double>{1, 2}, std::vector<double>{1, 2},
                      Model::ExpDecay);
    check(!few.ok, "fewer points than parameters is refused");

    // As many points as parameters is not a fit, it is an interpolation with
    // no residual degrees of freedom to judge it by, so it is refused rather
    // than reported with a meaningless r2 of 1.
    std::vector<double> px = {0, 1, 2}, py = {0, 1, 2};
    Fit p2 = nlinfit(px, py, Model::Poly2);
    check(!p2.ok, "three points and three parameters is refused");

    // With room to spare it fits and DOES claim standard errors.
    std::vector<double> qx, qy;
    for (int i = 0; i < 12; ++i) {
        const double x = i * 0.5;
        qx.push_back(x);
        qy.push_back(2.0 * x * x - 3.0 * x + 1.0);
    }
    Fit p3 = nlinfit(qx, qy, Model::Poly2);
    check(p3.ok, "poly2 fits with residual df");
    near(p3.values[0], 2.0, 1e-4, "poly2 recovers a");
    near(p3.values[1], -3.0, 1e-4, "poly2 recovers b");
    check(p3.df == 9, "df is n - k");
}

// --- tests -----------------------------------------------------------------

void check_tests() {
    std::vector<double> a = {5.1, 4.9, 5.6, 5.2, 5.0, 5.3};
    std::vector<double> b = {6.2, 6.0, 6.4, 6.1, 6.6, 6.3};
    TestResult t = ttest(a, b, false);
    check(t.ok, "unpaired t runs");
    check(t.stats[5].second < 0.001, "clearly separated groups give a small p");

    TestResult p = ttest(a, a, true);
    check(p.ok, "paired t runs");
    near(p.stats[0].second, 0.0, 1e-12, "a column against itself differs by 0");

    TestResult one = one_sample_t(a, 5.0);
    check(one.ok && one.stats[6].second == 6, "one-sample t reports n");

    TestResult av = anova1({{1, 2, 3}, {4, 5, 6}, {7, 8, 9}});
    check(av.ok, "one-way ANOVA runs");
    near(av.stats[3].second, 2.0, 1e-12, "df between is k-1");
    near(av.stats[4].second, 6.0, 1e-12, "df within is N-k");

    std::vector<double> xs = {1, 2, 3, 4, 5}, ys = {2, 4, 6, 8, 10};
    TestResult pr = pearson(xs, ys);
    near(pr.stats[0].second, 1.0, 1e-12, "a perfect line is r=1");

    // Spearman must be MONOTONIC, not linear: this is why it is Pearson on the
    // ranks rather than on the values.
    std::vector<double> mono = {1, 8, 27, 64, 125};
    TestResult sp = spearman(xs, mono);
    near(sp.stats[0].second, 1.0, 1e-12, "a monotone curve is rho=1");

    // Ties must be averaged. The 1 - 6*sum(d^2) shortcut is wrong here.
    std::vector<double> tie_a = {1, 2, 2, 3}, tie_b = {1, 2, 2, 3};
    TestResult st = spearman(tie_a, tie_b);
    near(st.stats[0].second, 1.0, 1e-12, "tied ranks still correlate perfectly");

    TestResult mw = mannwhitney(a, b);
    check(mw.ok && mw.stats[4].second < 0.05, "Mann-Whitney separates the groups");

    std::vector<double> withx = {1, 2, 3, 4, 5, 6, 7, 8, 100};
    auto hits = outliers(withx);
    check(hits.size() == 1 && hits[0] == 8, "Tukey finds the planted outlier");

    Describe d = describe(std::vector<double>{1, 2, 3, 4});
    near(d.mean, 2.5, 1e-12, "mean");
    near(d.median, 2.5, 1e-12, "median");
    near(d.sd, std::sqrt(5.0 / 3.0), 1e-12, "sample sd, n-1");
}

// --- table -----------------------------------------------------------------

void check_table() {
    auto d = tmpdir();
    const auto p = d / "t.csv";
    write(p, "x,y,g\n1,2,a\n2,4,a\n3,60,b\n4,8,b\n");
    Table t = Table::load(p.string());
    check(t.ncols() == 3 && t.rows() == 4, "shape");
    check(t.cols()[0].name == "x" && t.cols()[2].name == "g", "header");
    check(t.cols()[0].numeric && t.cols()[1].numeric && !t.cols()[2].numeric,
          "g is not a numeric column");

    std::vector<double> xs, ys;
    t.series(0, 1, xs, ys);
    check(xs.size() == 4 && ys[2] == 60, "series before exclusion");

    // EXCLUSION HAS TO REACH EVERYTHING. Dropping the outlier must change the
    // series, the column AND the fit, or a figure would disagree with itself.
    t.set_excluded(2, true);
    t.series(0, 1, xs, ys);
    check(xs.size() == 3 && ys[2] == 8, "the excluded row left the series");
    check(t.values(1).size() == 3, "and left the column");
    check(t.n_excluded() == 1, "and is counted");
    Fit f = linfit(xs, ys);
    near(f.slope, 2.0, 1e-9, "the excluded point no longer moves the fit");
    t.set_excluded(2, false);
    check(t.n_excluded() == 0, "including it back is symmetric");

    // Delimiter by counting, not by extension.
    check(Table::sniff("a;b;c") == ';', "semicolon file");
    check(Table::sniff("a\tb\tc") == '\t', "tab file");
    check(Table::sniff("a,b,c") == ',', "comma file");
    const auto sc = d / "semi.csv";
    write(sc, "a;b\n1;2\n3;4\n");
    Table st = Table::load(sc.string());
    check(st.ncols() == 2 && st.rows() == 2, "a .csv that is semicolon-delimited");

    // A quoted field with the delimiter inside must not shift the row.
    const auto q = d / "q.csv";
    write(q, "name,v\n\"Tris, pH 7.4\",3\nplain,4\n");
    Table qt = Table::load(q.string());
    check(qt.ncols() == 2 && qt.rows() == 2, "quoted shape");
    check(qt.cell_text(0, 0) == "Tris, pH 7.4", "the comma stayed inside the field");
    near(qt.cols()[1].num[0], 3.0, 0, "and the next column did not shift");

    // An edit reaches the analysis, and the raw text of every OTHER cell
    // survives the buffer reallocation that the edit causes.
    Table et = Table::load(p.string());
    const std::string before = et.cell_text(3, 1);
    et.set_cell(0, 1, "99.5");
    near(et.cols()[1].num[0], 99.5, 0, "the edit reached the numbers");
    check(et.cell_text(0, 1) == "99.5", "and the text");
    check(et.cell_text(3, 1) == before, "an unrelated cell survived the append");

    // Group ids are interned lazily and must be stable and few.
    const auto& g = et.groups(2);
    check(g.size() == 4 && g[0] == g[1] && g[2] == g[3] && g[0] != g[2],
          "two batches, two ids");
    check(et.arena()[g[0]] == "a", "and the label round-trips");

    // A blank line is not a row of blanks.
    const auto bl = d / "blank.csv";
    write(bl, "a,b\n1,2\n\n3,4\n");
    check(Table::load(bl.string()).rows() == 2, "blank lines are skipped");
}

// --- spec ------------------------------------------------------------------

void check_spec() {
    Spec s;
    std::string err;
    check(parse_spec("kind=scatter x=pka y=ddg colour=band where=n>3", s, err),
          "a well-formed declaration parses");
    check(s.kind == Kind::Scatter && s.x == "pka" && s.colour == "band",
          "fields land where they belong");
    check(s.y.size() == 1 && s.y[0] == "ddg", "one y");
    check(parse_spec("x=a y=b,c", s, err) && s.y.size() == 2, "several y");
    check(parse_spec("color=band", s, err) && s.colour == "band",
          "color= must alias colour=");
    // Unknown keys REFUSED, not ignored.
    check(!parse_spec("nosuch=1", s, err), "an unknown key is refused");
    check(!parse_spec("kind=pie x=a", s, err), "an unknown kind is refused");
    check(!parse_spec("x", s, err), "a bare token is refused");

    Spec rt;
    check(parse_spec("kind=line x=t y=a,b logy=1", rt, err), "round-trip parses");
    Spec rt2;
    check(parse_spec(spec_text(rt), rt2, err) && rt2.logy && rt2.y.size() == 2,
          "spec_text round-trips through parse_spec");

    auto d = tmpdir();
    const auto p = d / "f.csv";
    write(p, "pka,ddg,band\n8.1,-3.0,switch\n9.4,-7.0,persistent\n10.0,-1.0,switch\n");
    Table t = Table::load(p.string());

    Filter f;
    check(make_filter(t, "pka<9", f, err), "a filter builds");
    // THE COMPARISON THAT MATTERS. As strings "10" < "9"; a plot drawn on that
    // is wrong and looks fine.
    check(f.numeric, "a numeric column compares numerically");
    check(keep_row(t, f, 0) && !keep_row(t, f, 1) && !keep_row(t, f, 2),
          "10.0 is not less than 9");
    check(make_filter(t, "band==switch", f, err) && keep_row(t, f, 0) &&
              !keep_row(t, f, 1),
          "a string column compares as text");
    check(make_filter(t, "band~SWIT", f, err) && keep_row(t, f, 0),
          "~ is a case-insensitive substring");
    check(make_filter(t, "pka<=8.1", f, err) && keep_row(t, f, 0),
          "<= is not read as < with a stray =");
    // A column that is not there is NAMED.
    check(!make_filter(t, "absent==1", f, err) &&
              err.find("no column 'absent'") != std::string::npos,
          "a missing where column is named, not reported as an empty result");

    std::vector<Series> out;
    Spec ok;
    parse_spec("kind=scatter x=pka y=ddg", ok, err);
    check(build_series(t, ok, out, err), "series build");
    check(out.size() == 1 && out[0].xs.size() == 3, "one series, three points");
    check(out[0].label == "ddg",
          "an unnamed series takes the y column's name, never \"\"");

    Spec grouped;
    parse_spec("kind=scatter x=pka y=ddg colour=band", grouped, err);
    check(build_series(t, grouped, out, err) && out.size() == 2,
          "colour= splits into series");
    check(out[0].label == "persistent" && out[1].label == "switch",
          "series are ordered by label");

    Spec bad;
    parse_spec("x=nope y=ddg", bad, err);
    check(!build_series(t, bad, out, err) &&
              err.find("no column 'nope'") != std::string::npos,
          "a missing x column is named");

    // AN ID COLUMN IS NOT A GROUPING. colour= on one drew a legend entry per
    // row, over the whole figure, and the app reported success.
    const auto w = d / "wide.csv";
    std::string body = "x,y,id\n";
    for (int i = 0; i < 40; ++i)
        body += std::to_string(i) + "," + std::to_string(i * 2) + ",r" +
                std::to_string(i) + "\n";
    write(w, body);
    Table wt = Table::load(w.string());
    Spec idspec;
    parse_spec("x=x y=y colour=id", idspec, err);
    check(!build_series(wt, idspec, out, err), "an id column is refused");
    check(err.find("identifier") != std::string::npos &&
              err.find("40") != std::string::npos,
          "and the message names the count: " + err);

    // kind=line sorts by x; kind=scatter does not.
    const auto u = d / "unsorted.csv";
    write(u, "x,y\n3,1\n1,2\n2,3\n");
    Table ut = Table::load(u.string());
    Spec ln;
    parse_spec("kind=line x=x y=y", ln, err);
    check(build_series(ut, ln, out, err), "line series build");
    check(out[0].xs[0] == 1 && out[0].xs[2] == 3, "a line is sorted by x");
    Spec sc2;
    parse_spec("kind=scatter x=x y=y", sc2, err);
    build_series(ut, sc2, out, err);
    check(out[0].xs[0] == 3, "a scatter keeps file order");

    // A histogram counts, and every value lands in a bin.
    Spec h;
    parse_spec("kind=hist x=x bins=3", h, err);
    check(build_series(ut, h, out, err), "hist builds");
    double total = 0;
    for (double c : out[0].ys) total += c;
    near(total, 3.0, 0, "every value is counted exactly once");
}

// --- export ----------------------------------------------------------------

void check_export() {
    auto d = tmpdir();
    const auto src = d / "same.csv";
    write(src, "a,b\n1,2\n3,4\n5,6\n7,8\n");
    const std::string before = slurp(src);
    Table t = Table::load(src.string());
    Spec s;
    std::string err;
    parse_spec("kind=scatter x=a y=b", s, err);
    std::vector<Series> series;
    check(build_series(t, s, series, err), "series for export");

    // THE SIDECAR MUST NOT EAT THE INPUT. Exporting same.csv to the stem
    // "same" would put the plotted values back over the source; it destroyed a
    // 29 MB file here and nothing in the output said so.
    ExportResult r = write_gnuplot((d / "same").string(), t, s, series, nullptr);
    check(r.ok, "export runs: " + r.error);
    check(slurp(src) == before, "the export did not overwrite its own input");
    check(std::filesystem::exists(d / "same.figure.csv"),
          "the sidecar moved aside instead");
    check(std::filesystem::exists(d / "same.gp"), "the script is written");
    check(std::filesystem::exists(d / "same.caption.md"), "the caption is written");

    const std::string gp = slurp(d / "same.gp");
    check(gp.find("set terminal svg") != std::string::npos, "a terminal is set");
    check(gp.find("background rgb") == std::string::npos,
          "the SVG must not bake in a panel fill");
    check(gp.find("unset key") != std::string::npos,
          "one series draws no key");
    check(gp.find(".dat'") != std::string::npos,
          "data goes to a file, not inline");
    check(gp.find("set xlabel \"a\"") != std::string::npos, "the x axis is named");
    check(gp.find("set ylabel \"b\"") != std::string::npos, "the y axis is named");

    // A fit exports as an extra series in ink, and adds a key.
    std::vector<double> xs, ys;
    t.series(0, 1, xs, ys);
    Fit f = linfit(xs, ys);
    ExportResult r2 = write_gnuplot((d / "withfit").string(), t, s, series, &f);
    check(r2.ok, "export with a fit");
    const std::string gp2 = slurp(d / "withfit.gp");
    check(gp2.find("title 'fit'") != std::string::npos, "the fit is a named series");
    check(gp2.find("unset key") == std::string::npos,
          "a fit over the points is two things to tell apart, so a key is drawn");
}

// --- the app's own operations ---------------------------------------------
//
// ui_state.cpp is deliberately free of ImGui so it can be driven here. These
// are the paths a click takes, with no window in the way.

void check_app() {
    auto d = tmpdir();
    const auto p = d / "run.csv";
    std::string body = "dose,response,batch\n";
    for (int i = 0; i < 24; ++i)
        body += std::to_string(i * 0.5) + "," +
                std::to_string(3.0 * (i * 0.5) - 1.5) + ",day" +
                std::to_string(i % 3) + "\n";
    write(p, body);

    App app;
    check(app.open(p.string()), "the app opens a file");
    Doc* doc = app.doc();
    check(doc != nullptr, "and selects it");
    // The first plot should be one click, not six.
    check(doc->x == 0 && doc->ys.size() == 1 && doc->ys[0] == 1,
          "the first two numeric columns are picked for you");
    check(!doc->series.empty(), "and a series exists without touching anything");
    check(app.spec_text_buf.find("x=dose") != std::string::npos,
          "the declaration reflects the pickers: " + app.spec_text_buf);

    // HAND EDITS WIN. The declaration is what gets drawn.
    app.spec_text_buf = "kind=line x=dose y=response colour=batch";
    check(app.apply_spec_text(), "a hand-edited declaration applies");
    check(doc->colour == 2, "and pushes back into the pickers");
    check(doc->series.size() == 3, "colour=batch made three series");
    check(doc->series[0].xs[0] <= doc->series[0].xs[1], "kind=line sorted by x");

    // A bad declaration is reported, and must not half-apply.
    app.spec_text_buf = "kind=scatter x=dose y=response nosuch=1";
    check(!app.apply_spec_text(), "an unknown key is refused");
    check(doc->series.size() == 3, "and the previous plot still stands");

    app.spec_text_buf = "kind=scatter x=dose y=response";
    check(app.apply_spec_text(), "back to a good one");

    doc->fit_model = Model::Linear;
    app.run_fit();
    check(doc->has_fit, "the fit runs");
    near(doc->fit.slope, 3.0, 1e-9, "and recovers the slope");
    check(app.fit_report.find("R squared") != std::string::npos,
          "the report names what it reports");

    // Interpolation inside the range is interpolation; outside it is not, and
    // must say so -- an extrapolation looks exactly like an interpolation.
    app.interp_buf = "5";
    app.run_interp();
    check(app.fit_report.find("EXTRAPOLATED") == std::string::npos,
          "x=5 is inside the fitted range");
    app.interp_buf = "500";
    app.run_interp();
    check(app.fit_report.find("EXTRAPOLATED") != std::string::npos,
          "x=500 is not, and is labelled");

    // Excluding a row must reach the fit AND be reported.
    doc->table.set_excluded(0, true);
    app.run_fit();
    check(app.fit_report.find("1 row(s) excluded") != std::string::npos,
          "the fit reports what was left out");
    doc->table.set_excluded(0, false);

    // Outliers are SUGGESTED, never applied.
    doc->table.set_cell(3, 1, "9999");
    std::vector<std::size_t> hits;
    app.suggest_outliers(hits);
    check(!hits.empty(), "the planted outlier is suggested");
    check(doc->table.n_excluded() == 0,
          "suggesting must not exclude anything by itself");

    // An edited cell reaches the next fit.
    app.run_fit();
    check(doc->fit.ok && std::fabs(doc->fit.slope - 3.0) > 1e-6,
          "the edit moved the fit, so the edit reached the analysis");

    doc->test_kind = 5;   // Pearson
    doc->test_a = 0;
    doc->test_b = 1;
    app.run_test();
    check(app.test_report.find("r") != std::string::npos, "a test reports");

    doc->test_kind = 4;   // ANOVA, grouped
    doc->test_by = 2;
    app.run_test();
    check(app.test_report.find("F") != std::string::npos,
          "ANOVA groups by a column: " + app.test_report.substr(0, 40));

    check(app.export_figure((d / "fig").string()), "export from the app");
    check(std::filesystem::exists(d / "fig.gp"), "and the script lands");

    app.close_current();
    check(app.doc() == nullptr, "closing the last table leaves none");
}

}  // namespace

int main() {
    std::printf("echelle core self-check\n");
    check_parsing();
    check_distributions();
    check_linfit();
    check_nlinfit();
    check_tests();
    check_table();
    check_spec();
    check_export();
    check_app();
    if (failures) {
        std::printf("\n%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf(
        "passed: numbers refuse a partial parse, t and F match published "
        "critical\nvalues, the models recover the parameters they were "
        "generated from, an\nexcluded row leaves every consumer, an id column "
        "is refused as a grouping,\nand the sidecar does not eat its input\n");
    return 0;
}
