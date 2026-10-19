#pragma once

// tiny sync HTTP client. used by both the LTP registry and the party
// backend (which runs every message through here).
//
// on windows this is winhttp — no external deps, TLS works out of the box.
// elsewhere everything returns an empty result, so callers fall back.

#include <string>
#include <vector>

struct HttpResult
{
    int status = 0;      // http status; 0 means transport failed
    std::string body;

    bool ok() const { return status == 200; }
};

// simple key/value pair for request headers
struct HttpHeader
{
    std::string name;
    std::string value;
};

HttpResult HttpGetJson(const std::string &url);
HttpResult HttpPostJson(const std::string &url, const std::string &jsonBody);

// raw binary post, used by the party backend to move the exact same
// buffer that used to go over steam p2p
HttpResult HttpPostBinary(const std::string &url,
                          const void *body, size_t size,
                          const std::vector<HttpHeader> &headers);