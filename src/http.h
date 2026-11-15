#ifndef SRC_HTTP_H_
#define SRC_HTTP_H_

#include <curl/curl.h>

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace khanbot {

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
    static std::expected<std::unique_ptr<Http>, std::string>
    Create(std::string_view kaas = {});

    ~Http();

    Http(const Http &) = delete;
    Http &operator=(const Http &) = delete;

    std::expected<Response, std::string> Get(const std::string &url);
    std::expected<Response, std::string> Post(const std::string &url,
                                              const std::string &body);

  private:
    explicit Http(CURL *handle) : handle_(handle) {}

    std::expected<Response, std::string> Perform(const std::string &url);

    CURL *handle_;
};

} // namespace khanbot

#endif
