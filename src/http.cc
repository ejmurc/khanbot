#include "src/http.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

namespace khanbot {
namespace {

constexpr char kImpersonateEnv[] = "KA_IMPERSONATE";
constexpr char kDefaultImpersonateTarget[] = "chrome152";
constexpr char kJsonHeader[] = "content-type: application/json";
constexpr char kFkeyHeader[] = "x-ka-fkey: 1";
constexpr int kTimeoutSeconds = 30;
constexpr std::size_t kPreviewLength = 100;

const char *ImpersonateTarget() {
    const char *target = std::getenv(kImpersonateEnv);
    return target != nullptr ? target : kDefaultImpersonateTarget;
}

std::size_t AppendToString(char *data, std::size_t size, std::size_t count,
                           void *out) {
    const std::size_t bytes = size * count;
    static_cast<std::string *>(out)->append(data, bytes);
    return bytes;
}

} // namespace

std::string DescribeResponse(const Response &response) {
    std::string head = response.body.substr(0, kPreviewLength);
    std::replace_if(
        head.begin(), head.end(),
        [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
    return "status " + std::to_string(response.status) + ", starts: " + head;
}

void WriteResponseDump(const std::filesystem::path &path,
                       const std::string &url, const Response &response) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "requested " << url << "\nstatus " << response.status << "\nfinal "
        << response.effective_url << "\n\n"
        << response.headers << "\n"
        << response.body;
}

CurlRuntime::CurlRuntime() { curl_global_init(CURL_GLOBAL_DEFAULT); }

CurlRuntime::~CurlRuntime() { curl_global_cleanup(); }

std::expected<std::unique_ptr<Http>, std::string>
Http::Create(std::string_view kaas) {
    CURL *handle = curl_easy_init();
    if (handle == nullptr) {
        return std::unexpected(std::string("curl_easy_init failed"));
    }
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, AppendToString);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, AppendToString);
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, kTimeoutSeconds);
    curl_easy_setopt(handle, CURLOPT_COOKIEFILE, "");
    if (!kaas.empty()) {
        const std::string cookie = "KAAS=" + std::string(kaas);
        curl_easy_setopt(handle, CURLOPT_COOKIE, cookie.c_str());
    }
    const char *target = ImpersonateTarget();
    const CURLcode code = curl_easy_impersonate(handle, target, 1);
    if (code != CURLE_OK) {
        curl_easy_cleanup(handle);
        return std::unexpected(std::string("impersonate target '") + target +
                               "' rejected: " + curl_easy_strerror(code));
    }
    return std::unique_ptr<Http>(new Http(handle));
}

Http::~Http() { curl_easy_cleanup(handle_); }

std::expected<Response, std::string> Http::Get(const std::string &url) {
    curl_easy_setopt(handle_, CURLOPT_HTTPGET, 1L);
    return Perform(url);
}

std::expected<Response, std::string> Http::Post(const std::string &url,
                                                const std::string &body) {
    curl_slist *headers = curl_slist_append(nullptr, kJsonHeader);
    headers = curl_slist_append(headers, kFkeyHeader);
    curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE,
                     static_cast<long>(body.size()));
    std::expected<Response, std::string> response = Perform(url);
    curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, nullptr);
    curl_slist_free_all(headers);
    return response;
}

std::expected<Response, std::string> Http::Perform(const std::string &url) {
    Response response;
    curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(handle_, CURLOPT_HEADERDATA, &response.headers);
    const CURLcode code = curl_easy_perform(handle_);
    if (code != CURLE_OK) {
        return std::unexpected(std::string(curl_easy_strerror(code)));
    }
    auto status = 0L;
    curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &status);
    response.status = static_cast<int>(status);
    char *effective_url = nullptr;
    if (curl_easy_getinfo(handle_, CURLINFO_EFFECTIVE_URL, &effective_url) ==
            CURLE_OK &&
        effective_url != nullptr) {
        response.effective_url = effective_url;
    }
    return response;
}

} // namespace khanbot
