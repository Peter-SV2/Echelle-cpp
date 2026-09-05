// Series colours, in one place because two places would drift.
//
// The old palette was five fixed colours and a modulo. Six groups meant the
// sixth was drawn in the first one's colour, which is not a palette running
// out -- it is two different measurements rendered identically, in the one
// part of the figure whose whole job is telling them apart. A ramp sampled by
// HOW MANY series there are cannot do that: n series get n distinct points.
//
// The ramp is violet through purple to fuchsia. A single hue varying only in
// lightness looks tidier and is much harder to read at eight or ten series, so
// hue moves too -- it stays recognisably one family while giving the eye a
// second thing to separate on.
//
// Both ends are pulled in from the extremes on purpose. The dark end stops
// short of near-black so the fitted curve, which is drawn in ink, is not
// mistaken for another series; the light end stops short of pastel so the last
// series is still legible against a white plot.
#pragma once

#include <cstddef>
#include <cstdint>

namespace ech {

struct Rgb {
    std::uint8_t r, g, b;
};

// FOUR stops, not six. More stops make a smoother ramp and a smoother ramp is
// the wrong goal here: with six, twelve series landed as little as 19 apart in
// summed channel distance, which on a scatter marker is no difference at all.
// Fewer, further-apart stops mean longer segments, so consecutive samples jump
// further. The self-check pins the worst pair over every series count.
inline constexpr Rgb kRampStops[] = {
    {0x2e, 0x10, 0x65},   // deep indigo-violet
    {0x6d, 0x28, 0xd9},
    {0xa8, 0x55, 0xf7},
    {0xe8, 0x79, 0xf9},   // light fuchsia
};
inline constexpr std::size_t kRampN = sizeof kRampStops / sizeof *kRampStops;

// The ramp at 0..1.
inline Rgb ramp_at(double t) {
    if (t <= 0.0) return kRampStops[0];
    if (t >= 1.0) return kRampStops[kRampN - 1];
    const double pos = t * static_cast<double>(kRampN - 1);
    const std::size_t i = static_cast<std::size_t>(pos);
    const double k = pos - static_cast<double>(i);
    const Rgb& a = kRampStops[i];
    const Rgb& b = kRampStops[i + 1];
    auto mix = [k](std::uint8_t x, std::uint8_t y) {
        return static_cast<std::uint8_t>(x + (y - x) * k);
    };
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b)};
}

// Colour for series `i` of `n`. One series sits at a fixed point a third of
// the way along rather than at t=0: a lone series should be the confident
// mid-purple the app is built around, not whichever end of a gradient it
// happens to start at.
inline Rgb series_colour(std::size_t i, std::size_t n) {
    if (n <= 1) return ramp_at(0.34);
    return ramp_at(static_cast<double>(i) / static_cast<double>(n - 1));
}

}  // namespace ech
