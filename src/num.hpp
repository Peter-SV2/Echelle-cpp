// Number parsing, kept in one place because it is the hottest thing in the
// program and the easy spelling of it is the slow one.
//
// `std::stod` allocates a std::string per field, consults the C locale, and
// throws on failure -- three costs a CSV column of 50,000 numbers pays 50,000
// times, and the throw is the worst of them because a column of blanks is
// normal input, not an error. `std::from_chars` allocates nothing, ignores the
// locale (so a machine set to a comma-decimal locale reads the same file the
// same way, which strtod does NOT guarantee) and reports failure in its return
// value.
#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string_view>

namespace ech {

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

inline bool is_num(double v) { return !std::isnan(v); }

// Trim ASCII space and the stray '\r' a CRLF file leaves on every last field.
inline std::string_view trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// A field as a double, or NaN when it is not one. NaN is the "not a number
// here" marker throughout: it propagates through arithmetic instead of needing
// a parallel validity array, and every consumer already has to skip it because
// real data has holes.
inline double to_num(std::string_view s) {
    s = trim(s);
    if (s.empty()) return kNaN;
    double v{};
    const char* first = s.data();
    const char* last = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, v);
    // The WHOLE field must parse. Accepting a prefix would read "12abc" as 12
    // and "1,5" as 1, which is exactly how a comma-decimal file turns into a
    // plot that is wrong and looks fine.
    if (ec != std::errc{} || ptr != last) return kNaN;
    return v;
}

}  // namespace ech
