// Regression, curve fitting and the tests, on the standard library alone.
//
// The Python this replaces used scipy when it was importable and a pure
// fallback when it was not, and cross-checked the two. There is no such split
// here: this IS the fast path. A 2,000-point exp_decay fit took 55 ms through
// the interpreter and the same simplex in C++ has no boxing, no closure call
// per residual, and no allocation inside the loop.
//
// The published critical values the Python asserted against are asserted
// against here too, so the port is checked against arithmetic rather than
// against the code it came from.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ech {

// --- distributions ---------------------------------------------------------
// I_x(a, b) is the only special function needed: the tails of t and F both
// reduce to it.
double betainc(double a, double b, double x);
double t_sf2(double t, double df);        // two-tailed Student's t
double f_sf(double f, double d1, double d2);
double z_sf2(double z);                   // two-tailed normal
double t_crit(double df, double conf = 0.95);

// --- models ----------------------------------------------------------------
// The Prism shortlist, in Prism's parameterisation so the numbers compare.
enum class Model {
    Linear, ExpDecay, ExpGrowth, OnePhaseAssoc, MichaelisMenten,
    Hill4PL, Gaussian, Poly2
};

struct ModelInfo {
    Model id;
    const char* name;
    std::vector<const char*> params;
};
const std::vector<ModelInfo>& models();
const ModelInfo* model_by_name(std::string_view name);

// y at x for a model. A plain switch on an enum, not a std::function: the
// simplex calls this once per point per evaluation -- millions of times for a
// real fit -- and an indirect call through a type-erased wrapper there is the
// difference between a fit that feels instant and one that does not.
double eval(Model m, double x, std::span<const double> p);

// --- results ---------------------------------------------------------------
struct Fit {
    bool ok = false;
    std::string error;
    std::string model;
    Model id = Model::Linear;
    std::size_t n = 0;
    int df = 0;
    double sse = 0, r2 = 0, sy_x = 0;
    std::vector<std::string> names;
    std::vector<double> values;
    std::vector<double> se;          // empty when df <= 0 or J'J is singular
    std::vector<double> ci_lo, ci_hi;
    // The x range the fit was made over, so a drawn curve cannot be
    // extrapolated past its own data by accident.
    double x_lo = 0, x_hi = 0;

    // Linear only, and the numbers Prism prints beside it.
    double slope = 0, intercept = 0, se_slope = 0, se_intercept = 0;
    double p_slope = 0;
};

Fit linfit(std::span<const double> xs, std::span<const double> ys,
           double conf = 0.95);
Fit nlinfit(std::span<const double> xs, std::span<const double> ys,
            Model m, double conf = 0.95);
double predict(const Fit& f, double x);
// n points of the fitted curve over [x0, x1], for drawing.
void curve(const Fit& f, double x0, double x1, int n,
           std::vector<double>& xs, std::vector<double>& ys);

// --- descriptive and tests -------------------------------------------------
struct Describe {
    std::size_t n = 0;
    double mean = 0, sd = 0, sem = 0, median = 0, q1 = 0, q3 = 0;
    double min = 0, max = 0, ci_lo = 0, ci_hi = 0;
};
Describe describe(std::span<const double> v);

struct TestResult {
    bool ok = false;
    std::string error;
    std::string name;
    // Whatever the test reports, in print order. Kept as pairs rather than
    // named fields because the panel only ever formats them into lines, and a
    // struct per test would be eight structs and eight formatters.
    std::vector<std::pair<std::string, double>> stats;
    std::string note;
};

TestResult ttest(std::span<const double> a, std::span<const double> b,
                 bool paired, bool welch = true);
TestResult one_sample_t(std::span<const double> a, double mu = 0.0);
TestResult anova1(const std::vector<std::vector<double>>& groups);
TestResult pearson(std::span<const double> xs, std::span<const double> ys);
TestResult spearman(std::span<const double> xs, std::span<const double> ys);
TestResult mannwhitney(std::span<const double> a, std::span<const double> b);

// Tukey fences. SUGGESTED, never applied -- a point dropped without a trace
// is how a result stops being reproducible.
std::vector<std::size_t> outliers(std::span<const double> v, double k = 1.5);

}  // namespace ech
