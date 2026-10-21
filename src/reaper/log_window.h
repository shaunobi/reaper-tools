#pragma once
// The SRT log window: a non-modal, resizable text window showing every line
// the extension has reported. It never opens by itself except on an error
// line; the "SRT: Show/hide log window" action toggles it. Closing it only
// hides it, so nothing is lost. The lines themselves live in host.cpp (in
// memory for the session, and in a log file for the next one). Main thread only.

#include <string>

namespace srt::reaper {

// A new line was logged (already timestamped). Appends it if the window exists.
void logWindowAppend(const std::string& line);

// Show (and raise) the window, hide it if it is showing.
void logWindowToggle();

// Show and raise it (used for errors).
void logWindowShow();

void logWindowDestroy();

} // namespace srt::reaper
