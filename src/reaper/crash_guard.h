#pragma once
// Runs a callable and, on Windows, turns a hardware fault (access violation,
// illegal instruction, ...) into an error message that names the exception
// code and the module the faulting address belongs to, instead of taking
// REAPER down. Used around every call into the machine-learning runtime so
// a bad library on the user's machine produces a console line, not a crash.
// On other platforms it just runs the callable.

#include <functional>
#include <string>

namespace srt::reaper {

// Returns true on success; on a fault returns false and fills `error`.
// C++ exceptions thrown by `fn` propagate normally.
bool runGuarded(const std::function<void()>& fn, std::string& error);

} // namespace srt::reaper
