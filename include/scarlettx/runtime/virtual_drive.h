#pragma once

#include <string>

namespace scarlettx {
namespace vfs {

// Maps `letter`: to `target` for the lifetime of the process. The mapping is
// removed on normal exit, on an unhandled exception and on console close. A
// marker file records it, so that a later run can remove a mapping left behind
// by a process that was killed. Returns false if the letter is in use.
bool MapPackageDrive(char letter, const std::string& target);

// Removes the mapping made by MapPackageDrive. Safe to call more than once.
void UnmapPackageDrive();

// Removes a mapping recorded by a ScarlettX process that no longer exists.
// Returns true if a stale mapping was removed.
bool RecoverStaleDrive(char letter);

}  // namespace vfs
}  // namespace scarlettx
