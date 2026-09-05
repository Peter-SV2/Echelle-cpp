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

## It is actually standalone now

The Python shipped as an exe that drew by spawning gnuplot, and on the machine
it was built on gnuplot only existed inside WSL. So a "standalone" build was
quietly driving a virtual machine to render a scatter plot -- portable right up
until it met a laptop without WSL. `gp.backend()` said so on the rail, which
was honest, but it was still the shape of the thing.

This draws its own figures. It spawns nothing, at any point, for any reason.
gnuplot is not required to run it and is not bundled with it; the `.gp` script
written on Save is a record for a reader to rerun if they want to, not
something the app depends on. There is no 167 MB folder to copy beside the exe.

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

It builds and runs on Windows. `Echelle.exe` is 5.1 MB, statically linked, and
imports only system DLLs -- nothing to copy beside it.

The self-check is 151 assertions over the numerics, the table, the
declaration, the export, the palette and the app's own operations.
`ui_state.cpp` is free of ImGui precisely so a test can drive a fit, an edit,
an exclusion and an export with no window in the way; the check links no
graphics library and needs no GPU, so it runs in CI and under WSL as well.

    winget install MSYS2.MSYS2 Kitware.CMake     # or the VS 2022 build tools
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build --target check           # the assertions
    cmake --build build                          # Echelle.exe

If a build fails on `collect2: ld returned 1`, the exe is still running and
holding its own file. Close it.

---

The Python original, which this was measured against, is at
<https://github.com/Peter-SV2/Echelle>. It is not a dependency of this one.
