#include "srt/download.h"

#include <fstream>
#include <functional>
#include <stdexcept>

namespace srt {

bool downloadSupported() { return false; }

void downloadToStream(const std::string&, std::ofstream&, const std::function<void(uint64_t, uint64_t)>&,
                      const std::function<bool()>&) {
    throw std::runtime_error("downloads are not supported in this build");
}

} // namespace srt
