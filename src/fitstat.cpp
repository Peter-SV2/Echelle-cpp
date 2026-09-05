#include "fitstat.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "num.hpp"

namespace ech {
namespace {

double mean_of(std::span<const double> v) {
    if (v.empty()) return kNaN;
    // Pairwise-free but compensated: a plain running sum over 50,000 values
    // loses low bits, and the variance below is computed from this mean.
    double sum = 0.0, c = 0.0;
    for (double x : v) {
        const double y = x - c;
        const double t = sum + y;
        c = (t - sum) - y;
        sum = t;
    }
    return sum / static_cast<double>(v.size());
}

double sum_sq_dev(std::span<const double> v, double m) {
    double s = 0.0;
    for (double x : v) {
        const double d = x - m;
        s += d * d;
    }
    return s;
}

double quantile_sorted(const std::vector<double>& s, double q) {
    if (s.empty()) return kNaN;
    const double h = (static_cast<double>(s.size()) - 1) * q;
    const std::size_t lo = static_cast<std::size_t>(std::floor(h));
    const std::size_t hi = std::min(lo + 1, s.size() - 1);
    return s[lo] + (h - static_cast<double>(lo)) * (s[hi] - s[lo]);
}

// Continued fraction for the incomplete beta, by Lentz's method.
double betacf(double a, double b, double x) {
    constexpr double TINY = 1e-30, EPS = 3e-16;
    constexpr int ITMAX = 300;
    const double qab = a + b, qap = a + 1.0, qam = a - 1.0;
    double c = 1.0, d = 1.0 - qab * x / qap;
    if (std::fabs(d) < TINY) d = TINY;
    d = 1.0 / d;
    double h = d;
    for (int m = 1; m < ITMAX; ++m) {
        const int m2 = 2 * m;
        double aa = m * (b - m) * x / ((qam + m2) * (a + m2));
        d = 1.0 + aa * d;
        c = 1.0 + aa / c;
        if (std::fabs(d) < TINY) d = TINY;
        if (std::fabs(c) < TINY) c = TINY;
        d = 1.0 / d;
        h *= d * c;
        aa = -(a + m) * (qab + m) * x / ((a + m2) * (qap + m2));
        d = 1.0 + aa * d;
        c = 1.0 + aa / c;
        if (std::fabs(d) < TINY) d = TINY;
        if (std::fabs(c) < TINY) c = TINY;
        d = 1.0 / d;
        const double de = d * c;
        h *= de;
        if (std::fabs(de - 1.0) < EPS) break;
    }
    return h;
}

// Ranks with ties averaged, which is what Spearman and Mann-Whitney both need.
std::vector<double> ranks_of(std::span<const double> v) {
    const std::size_t n = v.size();
    std::vector<std::size_t> idx(n);
    std::iota(idx.begin(), idx.end(), std::size_t{0});
    std::sort(idx.begin(), idx.end(),
              [&](std::size_t a, std::size_t b) { return v[a] < v[b]; });
    std::vector<double> r(n);
    std::size_t i = 0;
    while (i < n) {
        std::size_t j = i;
        while (j + 1 < n && v[idx[j + 1]] == v[idx[i]]) ++j;
        const double avg = (static_cast<double>(i) + static_cast<double>(j)) / 2.0 + 1.0;
        for (std::size_t k = i; k <= j; ++k) r[idx[k]] = avg;
        i = j + 1;
    }
    return r;
}

}  // namespace

// --- distributions ---------------------------------------------------------

double betainc(double a, double b, double x) {
    if (x <= 0.0) return 0.0;
    if (x >= 1.0) return 1.0;
    const double lbeta = std::lgamma(a + b) - std::lgamma(a) - std::lgamma(b) +
                         a * std::log(x) + b * std::log1p(-x);
    if (x < (a + 1.0) / (a + b + 2.0))
        return std::exp(lbeta) * betacf(a, b, x) / a;
    return 1.0 - std::exp(lbeta) * betacf(b, a, 1.0 - x) / b;
}

double t_sf2(double t, double df) {
    if (df <= 0) return kNaN;
    if (t == 0) return 1.0;
    return betainc(df / 2.0, 0.5, df / (df + t * t));
}

double f_sf(double f, double d1, double d2) {
    if (f <= 0 || d1 <= 0 || d2 <= 0) return 1.0;
    return betainc(d2 / 2.0, d1 / 2.0, d2 / (d2 + d1 * f));
}

double z_sf2(double z) { return std::erfc(std::fabs(z) / std::sqrt(2.0)); }

double t_crit(double df, double conf) {
    // Bisection on t_sf2 -- no table, and correct for any df including
    // fractional ones out of Welch.
    const double target = 1.0 - conf;
    double lo = 0.0, hi = 1000.0;
    for (int i = 0; i < 200; ++i) {
        const double mid = (lo + hi) / 2.0;
        if (t_sf2(mid, df) > target) lo = mid; else hi = mid;
    }
    return (lo + hi) / 2.0;
}

// --- models ----------------------------------------------------------------

const std::vector<ModelInfo>& models() {
    static const std::vector<ModelInfo> M = {
        {Model::Linear, "linear", {"slope", "intercept"}},
        {Model::ExpDecay, "exp_decay", {"span", "k", "plateau"}},
        {Model::ExpGrowth, "exp_growth", {"span", "k", "offset"}},
        {Model::OnePhaseAssoc, "one_phase_assoc", {"span", "k", "y0"}},
        {Model::MichaelisMenten, "michaelis_menten", {"Vmax", "Km"}},
        {Model::Hill4PL, "hill_4pl", {"bottom", "top", "logEC50", "hillslope"}},
        {Model::Gaussian, "gaussian", {"amplitude", "mean", "sd"}},
        {Model::Poly2, "poly2", {"a", "b", "c"}},
    };
    return M;
}

const ModelInfo* model_by_name(std::string_view name) {
    for (const auto& m : models())
        if (name == m.name) return &m;
    return nullptr;
}

double eval(Model m, double x, std::span<const double> p) {
    switch (m) {
        case Model::Linear:          return p[0] * x + p[1];
        case Model::ExpDecay:        return p[0] * std::exp(-p[1] * x) + p[2];
        case Model::ExpGrowth:       return p[0] * std::exp(p[1] * x) + p[2];
        case Model::OnePhaseAssoc:   return p[2] + p[0] * (1.0 - std::exp(-p[1] * x));
        case Model::MichaelisMenten: {
            const double d = p[1] + x;
            return d == 0.0 ? 0.0 : p[0] * x / d;
        }
        case Model::Hill4PL:
            return p[0] + (p[1] - p[0]) /
                              (1.0 + std::pow(10.0, (p[2] - x) * p[3]));
        case Model::Gaussian: {
            const double s = p[2];
            return p[0] * std::exp(-((x - p[1]) * (x - p[1])) / (2.0 * s * s));
        }
        case Model::Poly2:           return p[0] * x * x + p[1] * x + p[2];
    }
    return kNaN;
}

namespace {

std::vector<double> start_guess(Model m, std::span<const double> xs,
                                std::span<const double> ys) {
    // Derived from the data, never constants: a simplex started far from the
    // answer on a decay curve walks off and reports a confident fit to nothing.
    const auto [xmn, xmx] = std::minmax_element(xs.begin(), xs.end());
    const auto [ymn, ymx] = std::minmax_element(ys.begin(), ys.end());
    const double my = mean_of(ys), mx = mean_of(xs);
    switch (m) {
        case Model::Linear:   return {1.0, my};
        case Model::ExpDecay:
        case Model::OnePhaseAssoc:
            return {*ymx - *ymn,
                    1.0 / std::max(1e-9, (*xmx - *xmn) / 3.0), *ymn};
        case Model::ExpGrowth: return {std::max(1e-9, std::fabs(my)), 0.1, 0.0};
        case Model::MichaelisMenten: {
            std::vector<double> s(xs.begin(), xs.end());
            std::sort(s.begin(), s.end());
            const double med = quantile_sorted(s, 0.5);
            return {*ymx, med != 0.0 ? med : 1.0};
        }
        case Model::Hill4PL:  return {*ymn, *ymx, mx, 1.0};
        case Model::Gaussian: {
            const double sd = std::sqrt(sum_sq_dev(xs, mx) /
                                        static_cast<double>(xs.size()));
            return {*ymx, mx, std::max(1e-9, sd != 0.0 ? sd : 1.0)};
        }
        case Model::Poly2:    return {0.0, 1.0, my};
    }
    return {};
}

double sse_of(Model m, std::span<const double> xs, std::span<const double> ys,
              std::span<const double> p) {
    double s = 0.0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        const double r = ys[i] - eval(m, xs[i], p);
        if (!std::isfinite(r)) return std::numeric_limits<double>::infinity();
        s += r * r;
    }
    return s;
}

// Nelder-Mead. Derivative-free on purpose: several of these models are not
// differentiable everywhere (Michaelis-Menten at x = -Km), so a gradient
// method needs guards this does not.
//
// NOTHING is allocated inside the iteration. The obvious transcription sorts
// the simplex every step, and sorting a vector-of-vectors means building a
// permuted copy -- k+1 heap blocks per iteration, up to 4,000 iterations.
// The algorithm never needs the full order anyway: only the best, the worst
// and the second worst, which is one O(k) scan.
double nelder_mead(Model m, std::span<const double> xs,
                   std::span<const double> ys, std::vector<double>& p,
                   int itmax = 4000, double tol = 1e-10) {
    const std::size_t n = p.size();

    // One flat buffer for the whole simplex: (n+1) rows of n, contiguous, so
    // a row is a span and swapping two vertices is swapping two indices.
    std::vector<double> sim((n + 1) * n);
    std::vector<double> val(n + 1);
    std::vector<double> cen(n), trial(n), trial2(n);
    auto row = [&](std::size_t i) { return std::span<double>(&sim[i * n], n); };
    auto crow = [&](std::size_t i) {
        return std::span<const double>(&sim[i * n], n);
    };

    for (std::size_t j = 0; j < n; ++j) sim[j] = p[j];
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) sim[(i + 1) * n + j] = p[j];
        sim[(i + 1) * n + i] += 0.1 * std::fabs(p[i]) + 0.05;
    }
    for (std::size_t i = 0; i <= n; ++i) val[i] = sse_of(m, xs, ys, crow(i));

    for (int it = 0; it < itmax; ++it) {
        // best, worst, second worst -- one pass, no sort, no allocation.
        std::size_t lo = 0, hi = 0, nh = 0;
        for (std::size_t i = 1; i <= n; ++i)
            if (val[i] < val[lo]) lo = i;
        hi = (lo == 0) ? 1 : 0;
        for (std::size_t i = 0; i <= n; ++i)
            if (i != lo && val[i] > val[hi]) hi = i;
        nh = (hi == 0) ? 1 : 0;
        for (std::size_t i = 0; i <= n; ++i)
            if (i != hi && i != lo && val[i] > val[nh]) nh = i;
        if (n == 1) nh = lo;

        if (std::fabs(val[hi] - val[lo]) <= tol * (std::fabs(val[lo]) + tol)) break;

        // Centroid of everything except the worst.
        for (std::size_t j = 0; j < n; ++j) cen[j] = 0.0;
        for (std::size_t i = 0; i <= n; ++i) {
            if (i == hi) continue;
            for (std::size_t j = 0; j < n; ++j) cen[j] += sim[i * n + j];
        }
        for (std::size_t j = 0; j < n; ++j) cen[j] /= static_cast<double>(n);

        for (std::size_t j = 0; j < n; ++j)
            trial[j] = cen[j] + (cen[j] - sim[hi * n + j]);
        const double fr = sse_of(m, xs, ys, trial);

        if (fr < val[lo]) {
            for (std::size_t j = 0; j < n; ++j)
                trial2[j] = cen[j] + 2.0 * (cen[j] - sim[hi * n + j]);
            const double fe = sse_of(m, xs, ys, trial2);
            const std::vector<double>& take = (fe < fr) ? trial2 : trial;
            std::copy(take.begin(), take.end(), row(hi).begin());
            val[hi] = (fe < fr) ? fe : fr;
        } else if (fr < val[nh]) {
            std::copy(trial.begin(), trial.end(), row(hi).begin());
            val[hi] = fr;
        } else {
            for (std::size_t j = 0; j < n; ++j)
                trial2[j] = cen[j] + 0.5 * (sim[hi * n + j] - cen[j]);
            const double fc = sse_of(m, xs, ys, trial2);
            if (fc < val[hi]) {
                std::copy(trial2.begin(), trial2.end(), row(hi).begin());
                val[hi] = fc;
            } else {
                for (std::size_t i = 0; i <= n; ++i) {
                    if (i == lo) continue;
                    for (std::size_t j = 0; j < n; ++j)
                        sim[i * n + j] = (sim[i * n + j] + sim[lo * n + j]) / 2.0;
                    val[i] = sse_of(m, xs, ys, crow(i));
                }
            }
        }
    }
    std::size_t best = 0;
    for (std::size_t i = 1; i <= n; ++i)
        if (val[i] < val[best]) best = i;
    p.assign(sim.begin() + static_cast<std::ptrdiff_t>(best * n),
             sim.begin() + static_cast<std::ptrdiff_t>((best + 1) * n));
    return val[best];
}

// Levenberg-Marquardt, and the reason it is here rather than the simplex
// alone.
//
// The Python reached for scipy's LM when scipy was importable and fell back to
// Nelder-Mead when it was not, and the fallback was 65x slower -- 8.8 s against
// 132 ms on a 2,000 point decay. Porting only the simplex would have made the
// SLOW path the only path: still 25x faster than the Python simplex, and still
// losing to the scipy it was meant to replace, because the gap there was never
// the language. A simplex needs thousands of function evaluations to crawl
// downhill without derivatives; LM uses the local slope and needs tens.
//
// J'J and J'r are accumulated point by point. For 50,000 points and four
// parameters, forming the full Jacobian first would build a 200,000-element
// matrix only to immediately reduce it to a 4x4 one.
bool invert(std::vector<double>& A, std::size_t n);   // defined below

bool levenberg(Model m, std::span<const double> xs, std::span<const double> ys,
               std::vector<double>& p, double& out_sse, int itmax = 200) {
    const std::size_t k = p.size();
    std::vector<double> JtJ(k * k), Jtr(k), damped(k * k), delta(k), row(k),
        trial(k), a(p), b(p);
    double lambda = 1e-3;
    double sse = sse_of(m, xs, ys, p);
    if (!std::isfinite(sse)) return false;

    for (int it = 0; it < itmax; ++it) {
        std::fill(JtJ.begin(), JtJ.end(), 0.0);
        std::fill(Jtr.begin(), Jtr.end(), 0.0);
        for (std::size_t i = 0; i < xs.size(); ++i) {
            const double x = xs[i];
            const double r = ys[i] - eval(m, x, p);
            if (!std::isfinite(r)) return false;
            for (std::size_t j = 0; j < k; ++j) {
                // Central differences, step scaled per parameter: a fixed step
                // is either noise on a parameter of order 1e-6 or a different
                // model on one of order 1e6.
                const double h = 1e-7 * std::max(1.0, std::fabs(p[j]));
                a[j] = p[j] + h;
                b[j] = p[j] - h;
                const double d = (eval(m, x, a) - eval(m, x, b)) / (2.0 * h);
                row[j] = std::isfinite(d) ? d : 0.0;
                a[j] = p[j];
                b[j] = p[j];
            }
            for (std::size_t r1 = 0; r1 < k; ++r1) {
                Jtr[r1] += row[r1] * r;
                for (std::size_t c1 = 0; c1 < k; ++c1)
                    JtJ[r1 * k + c1] += row[r1] * row[c1];
            }
        }

        bool stepped = false;
        for (int attempt = 0; attempt < 12; ++attempt) {
            damped = JtJ;
            // Marquardt's scaling: damp each parameter in proportion to its
            // own curvature, so a parameter of order 1e6 and one of order 1e-6
            // are damped comparably rather than the small one being frozen.
            for (std::size_t j = 0; j < k; ++j) {
                const double d = JtJ[j * k + j];
                damped[j * k + j] = d + lambda * (d != 0.0 ? d : 1.0);
            }
            if (!invert(damped, k)) { lambda *= 10.0; continue; }
            for (std::size_t r1 = 0; r1 < k; ++r1) {
                double acc = 0.0;
                for (std::size_t c1 = 0; c1 < k; ++c1)
                    acc += damped[r1 * k + c1] * Jtr[c1];
                delta[r1] = acc;
            }
            for (std::size_t j = 0; j < k; ++j) trial[j] = p[j] + delta[j];
            const double cand = sse_of(m, xs, ys, trial);
            if (std::isfinite(cand) && cand < sse) {
                const double gain = sse - cand;
                p = trial;
                a = p;
                b = p;
                const bool converged =
                    gain <= 1e-12 * (std::fabs(sse) + 1e-12);
                sse = cand;
                lambda = std::max(1e-12, lambda * 0.1);
                stepped = true;
                if (converged) { out_sse = sse; return true; }
                break;
            }
            lambda *= 10.0;
            if (lambda > 1e12) break;
        }
        if (!stepped) break;   // no downhill step at any damping: done or stuck
    }
    out_sse = sse;
    return std::isfinite(sse);
}

// Gauss-Jordan with partial pivoting. false when singular -- a singular J'J
// means the parameters are not separately identifiable from this data, and
// reporting a standard error for that would be a lie.
bool invert(std::vector<double>& A, std::size_t n) {
    std::vector<double> aug(n * 2 * n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) aug[i * 2 * n + j] = A[i * n + j];
        aug[i * 2 * n + n + i] = 1.0;
    }
    for (std::size_t c = 0; c < n; ++c) {
        std::size_t piv = c;
        for (std::size_t r = c + 1; r < n; ++r)
            if (std::fabs(aug[r * 2 * n + c]) > std::fabs(aug[piv * 2 * n + c]))
                piv = r;
        if (std::fabs(aug[piv * 2 * n + c]) < 1e-14) return false;
        if (piv != c)
            for (std::size_t j = 0; j < 2 * n; ++j)
                std::swap(aug[c * 2 * n + j], aug[piv * 2 * n + j]);
        const double d = aug[c * 2 * n + c];
        for (std::size_t j = 0; j < 2 * n; ++j) aug[c * 2 * n + j] /= d;
        for (std::size_t r = 0; r < n; ++r) {
            if (r == c) continue;
            const double f = aug[r * 2 * n + c];
            if (f == 0.0) continue;
            for (std::size_t j = 0; j < 2 * n; ++j)
                aug[r * 2 * n + j] -= f * aug[c * 2 * n + j];
        }
    }
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = 0; j < n; ++j) A[i * n + j] = aug[i * 2 * n + n + j];
    return true;
}

// se = sqrt(diag((J'J)^-1) s^2), J by central differences.
//
// J'J is accumulated directly instead of forming the n-by-m Jacobian: for
// 50,000 points and four parameters that is a 200,000-element matrix built
// only to be immediately reduced to a 4x4 one.
std::vector<double> param_se(Model m, std::span<const double> xs,
                             std::span<const double> p, double sse, int df) {
    const std::size_t k = p.size();
    const double s2 = sse / static_cast<double>(df);
    std::vector<double> JTJ(k * k, 0.0), row(k), a(p.begin(), p.end()),
        b(p.begin(), p.end());
    for (double x : xs) {
        for (std::size_t j = 0; j < k; ++j) {
            // A step scaled to each parameter: a fixed step is either noise on
            // a parameter of order 1e-6 or a different model on one of 1e6.
            const double h = 1e-6 * std::max(1.0, std::fabs(p[j]));
            a[j] = p[j] + h;
            b[j] = p[j] - h;
            const double d = (eval(m, x, a) - eval(m, x, b)) / (2.0 * h);
            row[j] = std::isfinite(d) ? d : 0.0;
            a[j] = p[j];
            b[j] = p[j];
        }
        for (std::size_t i = 0; i < k; ++i)
            for (std::size_t j = 0; j < k; ++j) JTJ[i * k + j] += row[i] * row[j];
    }
    if (!invert(JTJ, k)) return {};
    std::vector<double> se(k);
    for (std::size_t i = 0; i < k; ++i)
        se[i] = std::sqrt(std::fabs(JTJ[i * k + i] * s2));
    return se;
}

}  // namespace

// --- regression ------------------------------------------------------------

Fit linfit(std::span<const double> xs, std::span<const double> ys, double conf) {
    Fit f;
    f.model = "linear";
    f.id = Model::Linear;
    f.n = xs.size();
    if (xs.size() < 2) { f.error = "need at least 2 points"; return f; }
    const double mx = mean_of(xs), my = mean_of(ys);
    const double sxx = sum_sq_dev(xs, mx);
    if (sxx == 0.0) { f.error = "all x are identical"; return f; }
    double sxy = 0.0;
    for (std::size_t i = 0; i < xs.size(); ++i) sxy += (xs[i] - mx) * (ys[i] - my);
    const double syy = sum_sq_dev(ys, my);
    const double a = sxy / sxx, b = my - a * mx;
    double ss_res = 0.0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        const double r = ys[i] - (a * xs[i] + b);
        ss_res += r * r;
    }
    f.ok = true;
    f.slope = a;
    f.intercept = b;
    f.sse = ss_res;
    f.r2 = syy != 0.0 ? 1.0 - ss_res / syy : 1.0;
    f.df = static_cast<int>(xs.size()) - 2;
    f.names = {"slope", "intercept"};
    f.values = {a, b};
    const auto [lo, hi] = std::minmax_element(xs.begin(), xs.end());
    f.x_lo = *lo;
    f.x_hi = *hi;
    if (f.df > 0) {
        const double s = std::sqrt(ss_res / f.df);
        const double se_a = s / std::sqrt(sxx);
        const double se_b =
            s * std::sqrt(1.0 / static_cast<double>(xs.size()) + mx * mx / sxx);
        const double tc = t_crit(f.df, conf);
        f.sy_x = s;
        f.se_slope = se_a;
        f.se_intercept = se_b;
        f.se = {se_a, se_b};
        f.ci_lo = {a - tc * se_a, b - tc * se_b};
        f.ci_hi = {a + tc * se_a, b + tc * se_b};
        f.p_slope = se_a != 0.0 ? t_sf2(a / se_a, f.df) : 0.0;
    }
    return f;
}

Fit nlinfit(std::span<const double> xs, std::span<const double> ys, Model m,
            double conf) {
    Fit f;
    const ModelInfo* mi = nullptr;
    for (const auto& q : models())
        if (q.id == m) mi = &q;
    f.model = mi ? mi->name : "?";
    f.id = m;
    f.n = xs.size();
    const std::size_t k = mi ? mi->params.size() : 0;
    if (xs.size() <= k) {
        f.error = "need more than " + std::to_string(k) + " points for " + f.model;
        return f;
    }
    std::vector<double> p = start_guess(m, xs, ys);

    // LM first, simplex second -- the order the Python used, for the reason it
    // used it. LM is far faster but needs a decent start and can walk into a
    // region where the Jacobian says nothing; the simplex is slow and almost
    // unkillable. Trying the fast one and keeping the sturdy one as a fallback
    // costs nothing when LM works and saves the fit when it does not.
    double sse = std::numeric_limits<double>::infinity();
    std::vector<double> lm = p;
    double lm_sse = 0.0;
    const bool lm_ok = levenberg(m, xs, ys, lm, lm_sse);
    if (lm_ok && std::isfinite(lm_sse)) {
        p = lm;
        sse = lm_sse;
    }
    // The simplex runs when LM declined, and also when LM's answer looks poor
    // against the data's own spread -- a converged LM sitting in a local
    // minimum reports success just as confidently as a good fit does.
    const double sst_check = sum_sq_dev(ys, mean_of(ys));
    if (!lm_ok || !std::isfinite(sse) ||
        (sst_check > 0 && sse > 0.5 * sst_check)) {
        std::vector<double> nm = start_guess(m, xs, ys);
        double nm_sse = nelder_mead(m, xs, ys, nm);
        // A second run from the answer: the simplex collapses onto a local
        // shape and restarting it is the cheapest guard against stopping early.
        nm_sse = nelder_mead(m, xs, ys, nm);
        if (std::isfinite(nm_sse) && nm_sse < sse) {
            p = nm;
            sse = nm_sse;
        }
    }
    if (!std::isfinite(sse)) { f.error = "fit did not converge"; return f; }

    const double my = mean_of(ys);
    const double sst = sum_sq_dev(ys, my);
    f.ok = true;
    f.sse = sse;
    f.df = static_cast<int>(xs.size()) - static_cast<int>(k);
    f.r2 = sst != 0.0 ? 1.0 - sse / sst : 1.0;
    f.sy_x = f.df > 0 ? std::sqrt(sse / f.df) : kNaN;
    f.values = p;
    for (const char* nm : mi->params) f.names.emplace_back(nm);
    const auto [lo, hi] = std::minmax_element(xs.begin(), xs.end());
    f.x_lo = *lo;
    f.x_hi = *hi;
    if (f.df > 0) {
        f.se = param_se(m, xs, p, sse, f.df);
        if (!f.se.empty()) {
            const double tc = t_crit(f.df, conf);
            f.ci_lo.resize(k);
            f.ci_hi.resize(k);
            for (std::size_t i = 0; i < k; ++i) {
                f.ci_lo[i] = p[i] - tc * f.se[i];
                f.ci_hi[i] = p[i] + tc * f.se[i];
            }
        }
    }
    return f;
}

double predict(const Fit& f, double x) { return eval(f.id, x, f.values); }

void curve(const Fit& f, double x0, double x1, int n, std::vector<double>& xs,
           std::vector<double>& ys) {
    xs.clear();
    ys.clear();
    if (n < 2 || !f.ok) return;
    xs.reserve(static_cast<std::size_t>(n));
    ys.reserve(static_cast<std::size_t>(n));
    const double step = (x1 - x0) / (n - 1);
    for (int i = 0; i < n; ++i) {
        const double x = x0 + step * i;
        xs.push_back(x);
        ys.push_back(eval(f.id, x, f.values));
    }
}

// --- descriptive and tests -------------------------------------------------

Describe describe(std::span<const double> v) {
    Describe d;
    d.n = v.size();
    if (v.empty()) return d;
    std::vector<double> s(v.begin(), v.end());
    std::sort(s.begin(), s.end());
    d.mean = mean_of(v);
    d.min = s.front();
    d.max = s.back();
    d.median = quantile_sorted(s, 0.5);
    d.q1 = quantile_sorted(s, 0.25);
    d.q3 = quantile_sorted(s, 0.75);
    if (v.size() > 1) {
        d.sd = std::sqrt(sum_sq_dev(v, d.mean) / static_cast<double>(v.size() - 1));
        d.sem = d.sd / std::sqrt(static_cast<double>(v.size()));
        const double tc = t_crit(static_cast<double>(v.size() - 1));
        d.ci_lo = d.mean - tc * d.sem;
        d.ci_hi = d.mean + tc * d.sem;
    }
    return d;
}

TestResult ttest(std::span<const double> a, std::span<const double> b,
                 bool paired, bool welch) {
    TestResult r;
    r.name = paired ? "t test (paired)" : "t test (unpaired)";
    if (paired) {
        if (a.size() != b.size() || a.empty()) {
            r.error = "paired t needs two columns of the same length";
            return r;
        }
        std::vector<double> d(a.size());
        for (std::size_t i = 0; i < a.size(); ++i) d[i] = a[i] - b[i];
        const double md = mean_of(d);
        const double n = static_cast<double>(d.size());
        if (d.size() < 2) { r.error = "need at least 2 pairs"; return r; }
        const double sd = std::sqrt(sum_sq_dev(d, md) / (n - 1));
        const double se = sd / std::sqrt(n);
        const double t = se != 0.0 ? md / se : kNaN;
        const double df = n - 1;
        r.ok = true;
        r.stats = {{"mean difference", md}, {"SD of differences", sd},
                   {"t", t}, {"df", df}, {"p (two-tailed)", t_sf2(t, df)},
                   {"n pairs", n}};
        return r;
    }
    if (a.size() < 2 || b.size() < 2) {
        r.error = "need at least 2 values in each group";
        return r;
    }
    const double na = static_cast<double>(a.size()), nb = static_cast<double>(b.size());
    const double ma = mean_of(a), mb = mean_of(b);
    const double va = sum_sq_dev(a, ma) / (na - 1);
    const double vb = sum_sq_dev(b, mb) / (nb - 1);
    double t, df;
    if (welch) {
        const double se = std::sqrt(va / na + vb / nb);
        t = se != 0.0 ? (ma - mb) / se : kNaN;
        const double num = (va / na + vb / nb) * (va / na + vb / nb);
        const double den = (va * va) / (na * na * (na - 1)) +
                           (vb * vb) / (nb * nb * (nb - 1));
        df = den != 0.0 ? num / den : na + nb - 2;
    } else {
        const double sp = ((na - 1) * va + (nb - 1) * vb) / (na + nb - 2);
        const double se = std::sqrt(sp * (1.0 / na + 1.0 / nb));
        t = se != 0.0 ? (ma - mb) / se : kNaN;
        df = na + nb - 2;
    }
    r.ok = true;
    r.note = welch ? "Welch -- equal variances not assumed" : "pooled variance";
    r.stats = {{"mean A", ma}, {"mean B", mb}, {"difference", ma - mb},
               {"t", t}, {"df", df}, {"p (two-tailed)", t_sf2(t, df)},
               {"n A", na}, {"n B", nb}};
    return r;
}

TestResult one_sample_t(std::span<const double> a, double mu) {
    TestResult r;
    r.name = "one-sample t";
    if (a.size() < 2) { r.error = "need at least 2 values"; return r; }
    const double n = static_cast<double>(a.size());
    const double m = mean_of(a);
    const double sd = std::sqrt(sum_sq_dev(a, m) / (n - 1));
    const double se = sd / std::sqrt(n);
    const double t = se != 0.0 ? (m - mu) / se : kNaN;
    r.ok = true;
    r.stats = {{"mean", m}, {"hypothesised", mu}, {"SD", sd}, {"t", t},
               {"df", n - 1}, {"p (two-tailed)", t_sf2(t, n - 1)}, {"n", n}};
    return r;
}

TestResult anova1(const std::vector<std::vector<double>>& groups) {
    TestResult r;
    r.name = "one-way ANOVA";
    std::size_t k = 0, total = 0;
    for (const auto& g : groups)
        if (g.size() >= 1) { ++k; total += g.size(); }
    if (k < 2) { r.error = "need at least 2 groups"; return r; }
    double grand = 0.0;
    for (const auto& g : groups) for (double v : g) grand += v;
    grand /= static_cast<double>(total);
    double ssb = 0.0, ssw = 0.0;
    for (const auto& g : groups) {
        if (g.empty()) continue;
        const double m = mean_of(g);
        ssb += static_cast<double>(g.size()) * (m - grand) * (m - grand);
        ssw += sum_sq_dev(g, m);
    }
    const double df1 = static_cast<double>(k) - 1.0;
    const double df2 = static_cast<double>(total) - static_cast<double>(k);
    if (df2 <= 0) { r.error = "no residual degrees of freedom"; return r; }
    const double msb = ssb / df1, msw = ssw / df2;
    const double f = msw != 0.0 ? msb / msw : kNaN;
    r.ok = true;
    r.stats = {{"groups", static_cast<double>(k)},
               {"SS between", ssb}, {"SS within", ssw},
               {"df between", df1}, {"df within", df2},
               {"F", f}, {"p", f_sf(f, df1, df2)},
               {"n total", static_cast<double>(total)}};
    return r;
}

TestResult pearson(std::span<const double> xs, std::span<const double> ys) {
    TestResult r;
    r.name = "Pearson r";
    if (xs.size() != ys.size() || xs.size() < 3) {
        r.error = "need at least 3 paired values";
        return r;
    }
    const double mx = mean_of(xs), my = mean_of(ys);
    double sxy = 0.0;
    for (std::size_t i = 0; i < xs.size(); ++i) sxy += (xs[i] - mx) * (ys[i] - my);
    const double sxx = sum_sq_dev(xs, mx), syy = sum_sq_dev(ys, my);
    if (sxx == 0.0 || syy == 0.0) { r.error = "a column has no variation"; return r; }
    const double rr = sxy / std::sqrt(sxx * syy);
    const double n = static_cast<double>(xs.size());
    const double df = n - 2;
    const double t = rr * std::sqrt(df / std::max(1e-300, 1.0 - rr * rr));
    r.ok = true;
    r.stats = {{"r", rr}, {"r squared", rr * rr}, {"df", df},
               {"p (two-tailed)", t_sf2(t, df)}, {"n", n}};
    return r;
}

TestResult spearman(std::span<const double> xs, std::span<const double> ys) {
    TestResult r;
    r.name = "Spearman rho";
    if (xs.size() != ys.size() || xs.size() < 3) {
        r.error = "need at least 3 paired values";
        return r;
    }
    // Pearson ON THE RANKS, not the 1 - 6*sum(d^2) shortcut: the shortcut is
    // only correct without ties, and real measurements tie.
    const auto rx = ranks_of(xs), ry = ranks_of(ys);
    TestResult p = pearson(rx, ry);
    if (!p.ok) { r.error = p.error; return r; }
    r.ok = true;
    r.note = "Pearson on ranks; ties averaged";
    r.stats = {{"rho", p.stats[0].second}, {"df", p.stats[2].second},
               {"p (two-tailed)", p.stats[3].second},
               {"n", static_cast<double>(xs.size())}};
    return r;
}

TestResult mannwhitney(std::span<const double> a, std::span<const double> b) {
    TestResult r;
    r.name = "Mann-Whitney U";
    if (a.empty() || b.empty()) { r.error = "both groups must have values"; return r; }
    std::vector<double> all;
    all.reserve(a.size() + b.size());
    all.insert(all.end(), a.begin(), a.end());
    all.insert(all.end(), b.begin(), b.end());
    const auto rk = ranks_of(all);
    double ra = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) ra += rk[i];
    const double na = static_cast<double>(a.size()), nb = static_cast<double>(b.size());
    const double ua = ra - na * (na + 1.0) / 2.0;
    const double ub = na * nb - ua;
    const double u = std::min(ua, ub);
    const double mu = na * nb / 2.0;
    const double sd = std::sqrt(na * nb * (na + nb + 1.0) / 12.0);
    const double z = sd != 0.0 ? (u - mu) / sd : kNaN;
    r.ok = true;
    r.note = "normal approximation, no continuity correction";
    r.stats = {{"U", u}, {"U (A)", ua}, {"U (B)", ub}, {"z", z},
               {"p (two-tailed)", z_sf2(z)}, {"n A", na}, {"n B", nb}};
    return r;
}

std::vector<std::size_t> outliers(std::span<const double> v, double k) {
    std::vector<std::size_t> hits;
    if (v.size() < 4) return hits;
    std::vector<double> s(v.begin(), v.end());
    std::sort(s.begin(), s.end());
    const double q1 = quantile_sorted(s, 0.25), q3 = quantile_sorted(s, 0.75);
    const double iqr = q3 - q1;
    const double lo = q1 - k * iqr, hi = q3 + k * iqr;
    for (std::size_t i = 0; i < v.size(); ++i)
        if (v[i] < lo || v[i] > hi) hits.push_back(i);
    return hits;
}

}  // namespace ech
