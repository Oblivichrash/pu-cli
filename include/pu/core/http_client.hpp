// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <functional>
#include <utility>
#include <string>
#include <vector>

#include "pu/core/base.hpp"

namespace pu::http {

using WriteCallback = std::function<size_t(char*, size_t)>;

// The one way an API key becomes a header. Empty stays empty: a gateway told nothing is
// asked in the clear rather than with a bearer of nothing.
inline std::string BearerHeader(const std::string& api_key) {
  return api_key.empty() ? std::string{} : "Authorization: Bearer " + api_key;
}

struct Response {
  unsigned status = 0;
  std::string body;
};

// What a streamed request is asked with. A feed is read with GET and carries no body; a chat
// turn is posted with one. The method is the caller's, since only the caller knows which
// endpoint answers a stream.
enum class HttpMethod { kGet, kPost };

class HttpClient {
 public:
  virtual ~HttpClient() = default;

  virtual void StreamRequest(HttpMethod method, const std::string& url, const std::string& body,
                             const std::vector<std::string>& headers, WriteCallback write_cb,
                             CancelToken cancel_token = nullptr) = 0;

  // The one shape most callers want. Kept as a name rather than spelled at each call site so
  // a test that fakes the interface can still be driven by what the caller asked for.
  void PostStream(const std::string& url, const std::string& body,
                  const std::vector<std::string>& headers, WriteCallback write_cb,
                  CancelToken cancel_token = nullptr) {
    StreamRequest(HttpMethod::kPost, url, body, headers, std::move(write_cb), cancel_token);
  }

  // For a read that is answered in one piece rather than streamed. Throws HttpError
  // on a non-2xx status, as PostStream does.
  virtual Response Get(const std::string& url, const std::vector<std::string>& headers) = 0;
};

}  // namespace pu::http
