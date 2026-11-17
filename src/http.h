#ifndef SRC_HTTP_H_
#define SRC_HTTP_H_

#include <curl/curl.h>

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "src/result.h"

namespace khanbot {
inline constexpr int kHttpOk = 200;
inline constexpr int kHttpBadRequest = 400;
inline constexpr int kHttpUnauthorized = 401;
inline constexpr int kHttpForbidden = 403;
inline constexpr int kHttpNotFound = 404;

struct Response {
    int status = 0;
    std::string body;
    std::string headers;
    std::string effective_url;
};

std::string DescribeResponse(const Response &response);

void WriteResponseDump(const std::filesystem::path &path,
                       const std::string &url, const Response &response);

class CurlRuntime {
  public:
    CurlRuntime();
    ~CurlRuntime();

    CurlRuntime(const CurlRuntime &) = delete;
    CurlRuntime &operator=(const CurlRuntime &) = delete;
};

class Http {
  public:
    static Result<std::unique_ptr<Http>> Create(std::string_view cookie = {});

    ~Http();

    Http(const Http &) = delete;
    Http &operator=(const Http &) = delete;

    Result<Response> Get(const std::string &url);

    Result<Response> Post(const std::string &url, const std::string &body,
                          std::span<const char *const> headers = {});

  private:
    explicit Http(CURL *handle);

    Result<Response> Perform(const std::string &url);

    CURL *handle_;
    char error_[CURL_ERROR_SIZE];
};

} // namespace khanbot

#endif
