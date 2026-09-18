#include "srt/download.h"

#include <curl/curl.h>

#include <fstream>
#include <functional>
#include <stdexcept>

namespace srt {

bool downloadSupported() { return true; }

namespace {

struct Ctx {
    std::ofstream* out;
    const std::function<void(uint64_t, uint64_t)>* onBytes;
    const std::function<bool()>* cancelled;
    uint64_t received = 0;
};

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* user) {
    Ctx* c = static_cast<Ctx*>(user);
    c->out->write(ptr, static_cast<std::streamsize>(size * nmemb));
    c->received += size * nmemb;
    return c->out->good() ? size * nmemb : 0;
}

int progressCb(void* user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    Ctx* c = static_cast<Ctx*>(user);
    if ((*c->cancelled)()) return 1;
    (*c->onBytes)(static_cast<uint64_t>(dlnow), static_cast<uint64_t>(dltotal > 0 ? dltotal : 0));
    return 0;
}

} // namespace

void downloadToStream(const std::string& url, std::ofstream& out,
                      const std::function<void(uint64_t, uint64_t)>& onBytes, const std::function<bool()>& cancelled) {
    static const bool initialised = (curl_global_init(CURL_GLOBAL_DEFAULT), true);
    (void)initialised;
    CURL* curl = curl_easy_init();
    if (!curl) throw std::runtime_error("curl init failed");
    Ctx ctx{&out, &onBytes, &cancelled};
    char err[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "reaper-srt/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progressCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, err);
    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (rc == CURLE_ABORTED_BY_CALLBACK) throw JobCancelled{};
    if (rc != CURLE_OK) throw std::runtime_error(std::string("download failed: ") + (err[0] ? err : curl_easy_strerror(rc)));
    out.flush();
    if (!out) throw std::runtime_error("write failed while downloading");
}

} // namespace srt
