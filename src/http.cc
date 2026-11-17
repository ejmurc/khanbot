#include "src/http.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

#include "src/text.h"

namespace khanbot {
namespace {
constexpr char kImpersonateEnv[] = "KA_IMPERSONATE";
constexpr char kDefaultImpersonateTarget[] = "chrome152";
constexpr long kTimeoutSeconds = 30;
constexpr long kConnectTimeoutSeconds = 10;
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

constexpr char ToLower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool StartsWithNoCase(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() &&
           std::ranges::equal(
               text.substr(0, prefix.size()), prefix,
               [](char a, char b) { return ToLower(a) == ToLower(b); });
}

std::string RedactCookies(std::string_view headers) {
    std::string out;
    std::size_t start = 0;
    while (start < headers.size()) {
        const std::size_t newline = headers.find('\n', start);
        const std::size_t end = newline == kNpos ? headers.size() : newline;
        std::string_view line = headers.substr(start, end - start);
        const bool carriage_return = line.ends_with('\r');
        if (carriage_return)
            line.remove_suffix(1);
        const std::size_t equals = line.find('=');
        if (StartsWithNoCase(line, "set-cookie:") && equals != kNpos) {
            out.append(line.substr(0, equals + 1)).append("<redacted>");
        } else {
            out.append(line);
        }
        if (carriage_return)
            out += '\r';
        if (newline != kNpos)
            out += '\n';
        start = end + 1;
    }
    return out;
}

using HeaderList = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

} // namespace

std::string DescribeResponse(const Response &response) {
    return "status " + std::to_string(response.status) +
           ", starts: " + Preview(response.body, kPreviewLength);
}

void WriteResponseDump(const std::filesystem::path &path,
                       const std::string &url, const Response &response) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "requested " << url << "\nstatus " << response.status << "\nfinal "
        << response.effective_url << "\n\n"
        << RedactCookies(response.headers) << "\n"
        << response.body;
}

CurlRuntime::CurlRuntime() { curl_global_init(CURL_GLOBAL_DEFAULT); }

CurlRuntime::~CurlRuntime() { curl_global_cleanup(); }

Result<std::unique_ptr<Http>> Http::Create(std::string_view cookie) {
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
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);

    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(handle, CURLOPT_COOKIEFILE, "");
    if (!cookie.empty()) {
        const std::string value(cookie);
        curl_easy_setopt(handle, CURLOPT_COOKIE, value.c_str());
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

Http::Http(CURL *handle) : handle_(handle) {
    error_[0] = '\0';
    curl_easy_setopt(handle_, CURLOPT_ERRORBUFFER, error_);
}

Http::~Http() { curl_easy_cleanup(handle_); }

Result<Response> Http::Get(const std::string &url) {
    curl_easy_setopt(handle_, CURLOPT_HTTPGET, 1L);
    return Perform(url);
}

Result<Response> Http::Post(const std::string &url, const std::string &body,
                            std::span<const char *const> headers) {
    curl_slist *raw = nullptr;
    for (const char *header : headers) {
        curl_slist *next = curl_slist_append(raw, header);
        if (next == nullptr) {
            curl_slist_free_all(raw);
            return std::unexpected(std::string("out of memory"));
        }
        raw = next;
    }
    const HeaderList list(raw, curl_slist_free_all);
    curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, list.get());
    curl_easy_setopt(handle_, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(handle_, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(body.size()));
    Result<Response> response = Perform(url);
    curl_easy_setopt(handle_, CURLOPT_HTTPHEADER, nullptr);
    return response;
}

Result<Response> Http::Perform(const std::string &url) {
    Response response;
    curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(handle_, CURLOPT_HEADERDATA, &response.headers);
    error_[0] = '\0';
    const CURLcode code = curl_easy_perform(handle_);
    if (code != CURLE_OK) {
        return std::unexpected(error_[0] != '\0'
                                   ? std::string(error_)
                                   : std::string(curl_easy_strerror(code)));
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
