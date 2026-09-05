// Writing files so a crash cannot destroy what was already there, and the
// session file that uses it.
//
// TWO different failures are called "corruption" and they need different
// answers:
//
//   1. The write is interrupted -- power loss, a full disk, the process
//      killed. The old file must survive intact. Answer: never write over the
//      real file. Write a temporary beside it, force it to the platter, then
//      replace the target in ONE atomic step. At every instant the path holds
//      either the whole old file or the whole new one, and never a prefix.
//
//   2. The bytes on disk are damaged after the fact -- a bad sector, a failed
//      copy, a sync client truncating. Nothing can prevent that, so the answer
//      is to NOTICE. The file carries its own length and checksum and a load
//      that does not match is refused outright rather than half-applied.
//
// The second matters more than it sounds. A session that loads three of five
// tables and says nothing is worse than one that refuses: the analysis looks
// complete and is not, and the fit that comes out of it is wrong in a way no
// one can see.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ech {

struct App;

// FNV-1a, 64-bit. A CHECKSUM, not a signature: it catches truncation, flipped
// bits and a half-written file, which is what actually happens to a file on a
// disk. It is not a defence against someone deliberately editing a session to
// match -- that would need a keyed hash and there is nothing here worth
// forging.
std::uint64_t checksum(std::string_view bytes);

// Write `body` to `path` so that `path` is never left partially written.
// Returns false and fills `err` on any failure, leaving any existing file at
// `path` exactly as it was.
bool write_atomic(const std::string& path, std::string_view body,
                  std::string& err);

// --- the session -----------------------------------------------------------
// Every open table, its data AS EDITED, its exclusions, its selection and
// every picker, in one file.
//
// The tables' contents are embedded rather than referenced by path. A session
// that stored paths would reopen into whatever those files say TODAY -- a
// re-exported CSV, a corrected typo, a row appended -- while the exclusions
// and cell edits saved with it still refer to row numbers from the old file.
// That silently mismatches, which is the failure this format exists to avoid.
bool save_session(const App& app, const std::string& path, std::string& err);

// Replaces the app's tables with the session's, but only if the whole file
// reads: a session that fails validation leaves the app untouched rather than
// half-loaded.
bool load_session(App& app, const std::string& path, std::string& err);

}  // namespace ech
