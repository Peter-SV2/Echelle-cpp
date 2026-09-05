# Echelle in C++

A port of the Python Echelle, for speed. The measured reason it exists, on a
50,000-row file:

| stage | Python | C++ | |
|---|---|---|---|
| load a 1.6 MB CSV | 57.3 ms | ~12.7 ms | 4.5x |
| filter + group into series | 86.3 ms | ~1.2 ms | 70x |
| `linfit`, 50k points | 16.2 ms | ~0.85 ms | 19x |
| `nlinfit` exp_decay, 2k points | 132 ms (scipy)<br>8841 ms (pure) | ~10 ms | 13x<br>880x |
| **draw the figure** | **846 ms** gnuplot spawn<br>+ **845 ms** building 50,052<br>Tk canvas objects | **~0 ms** | — |

The last row is the one that mattered. Python was never the bottleneck for
drawing: a subprocess round-trip through a 5.2 MB SVG and a retained-mode
canvas holding one object per point accounted for 1.7 s of a 1.76 s redraw.
ImPlot is handed a pointer to the `std::vector<double>` that already exists and
draws from it in immediate mode, so a redraw has no per-point cost at all.

## What it is

- `src/num.hpp` — field parsing. `from_chars`, not `stod`: no allocation, no
  locale, and a partial parse is refused rather than silently truncating
  `1,5` to `1`.
- `src/table.*` — columnar storage. A numeric column IS a
  `std::vector<double>`; cell text is `{offset, length}` into one buffer, not a
  `std::string` per cell.
- `src/spec.*` — the `kind=… x=… y=…` declaration, the one-comparison filter,
  and series building.
- `src/fitstat.*` — distributions, linear regression, Levenberg-Marquardt with
  a Nelder-Mead fallback, and the tests.
- `src/gpexport.*` — the gnuplot script and CSV sidecar, written on Save.
- `src/selfcheck.cpp` — the Python's assertions, carried over.

## Building

The core is headless and builds anywhere:

    make check      # compile and run the self-check
    make bench      # the table above

## Status

Everything is written. 151 assertions pass, covering the numerics, the table,
the declaration, the export and the app's own operations -- `ui_state.cpp` is
free of ImGui precisely so a test can drive a fit, an edit, an exclusion and
an export with no window in the way.

`src/ui.cpp` compiles clean against ImGui 1.91.5 and ImPlot 0.16, so the
drawing API usage is checked too.

`src/main.cpp` -- the Win32 + D3D11 host loop -- is the one file that has NOT
been compiled. It needs a Windows C++ toolchain, and this machine has none:
no MSVC, no MinGW, no CMake. Everything else was built and run under WSL.

To finish it you need one of:

    winget install Microsoft.VisualStudio.2022.BuildTools Kitware.CMake
    winget install MSYS2.MSYS2 Kitware.CMake

then:

    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --config Release --target check   # the 151 assertions
    cmake --build build --config Release                  # Echelle.exe

Expect main.cpp to need a small fix or two on that first compile. Nothing else
should.
