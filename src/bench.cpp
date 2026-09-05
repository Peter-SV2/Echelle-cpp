// The same stages the Python was profiled on, so the two columns compare.
//
// Generates its own file rather than taking a path: a benchmark that depends
// on a file somebody has to produce first is a benchmark that stops being run.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "fitstat.hpp"
#include "gpexport.hpp"
#include "spec.hpp"
#include "table.hpp"

using namespace ech;
using Clock = std::chrono::steady_clock;

namespace {

template <class F>
double ms(F&& f, int reps = 1) {
    const auto t0 = Clock::now();
    for (int i = 0; i < reps; ++i) f();
    const auto t1 = Clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count() / reps;
}

void row(const char* label, double v) {
    std::printf("%-34s %8.2f ms\n", label, v);
}

}  // namespace

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "echelle_bench";
    std::filesystem::create_directories(dir);
    const auto src = dir / "big.csv";

    constexpr int N = 50000;
    {
        std::mt19937 rng(1);
        std::normal_distribution<double> n0(0.0, 2.0), n1(50.0, 5.0);
        std::string body = "time,signal,ref,batch\n";
        body.reserve(N * 40);
        char buf[128];
        for (int i = 0; i < N; ++i) {
            const double t = i * 0.001;
            std::snprintf(buf, sizeof buf, "%.4f,%.5f,%.5f,day%d\n", t,
                          100.0 * (1.0 - std::exp(-t / 6.0)) + n0(rng), n1(rng),
                          i % 4);
            body += buf;
        }
        std::ofstream(src, std::ios::binary) << body;
    }
    std::printf("rows %d, file %.1f MB\n\n", N,
                static_cast<double>(std::filesystem::file_size(src)) / 1e6);

    row("Table::load (columnar)", ms([&] { Table::load(src.string()); }, 5));
    Table t = Table::load(src.string());
    row("numeric column sniff", ms([&] { (void)t.numeric_names(); }, 100));

    Spec s;
    std::string err;
    parse_spec("kind=scatter x=time y=signal colour=batch", s, err);
    std::vector<Series> series;
    row("build_series + group",
        ms([&] { build_series(t, s, series, err); }, 5));
    build_series(t, s, series, err);

    std::vector<double> xs, ys;
    row("series() numeric pairs", ms([&] { t.series(0, 1, xs, ys); }, 20));
    t.series(0, 1, xs, ys);
    std::printf("\n");

    row("linfit (50k pts)", ms([&] { linfit(xs, ys); }, 20));
    std::vector<double> sx(xs.begin(), xs.begin() + 2000),
        sy(ys.begin(), ys.begin() + 2000);
    row("nlinfit exp_decay (2k pts)",
        ms([&] { nlinfit(sx, sy, Model::ExpDecay); }, 5));
    std::printf("\n");

    Fit f = linfit(xs, ys);
    row("write_gnuplot export (on Save)", ms([&] {
            write_gnuplot((dir / "out").string(), t, s, series, &f);
        }));

    // What a redraw actually costs now: nothing. The series are already
    // contiguous doubles and ImPlot is handed the pointer, so the per-frame
    // work is a bounds scan, not a rebuild. This is the number the gnuplot
    // round-trip and the 50,052 canvas objects used to pay.
    double acc = 0;
    row("redraw: hand ImPlot the pointers", ms([&] {
            for (const auto& ser : series) {
                const double* px = ser.xs.data();
                const double* py = ser.ys.data();
                acc += px[0] + py[ser.ys.size() - 1];
            }
        }, 1000));
    if (acc == 12345.6789) std::printf("");  // keep the loop
    return 0;
}
