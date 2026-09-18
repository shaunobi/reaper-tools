#pragma once
// Non-modal window listing running jobs with a progress bar each. Opens when
// a job starts and closes when the last one finishes. Main thread only.

#include "srt/jobs.h"

namespace srt::reaper {

void progressWindowRefresh(srt::JobRunner& runner);
void progressWindowDestroy();

} // namespace srt::reaper
