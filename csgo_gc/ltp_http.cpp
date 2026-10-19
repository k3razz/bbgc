#include "stdafx.h"
#include "ltp_http.h"

#if defined(_WIN32)

#include <windows.h>
#include <winhttp.h>

// Some Windows SDK versions gate WINHTTP_DISABLE_* behind _WIN32_WINNT >= 0x0603
// even though the runtime option itself is supported from Win8.1 up. Define the
// feature bits ourselves if the SDK header didn't, so the option call always
// compiles on any toolchain.
#ifndef WINHTTP_DISABLE_COOKIES
#define WINHTTP_DISABLE_COOKIES      0x00000001
#endif
#ifndef WINHTTP_DISABLE_REDIRECTS
#define WINHTTP_DISABLE_REDIRECTS    0x00000002
#endif
#ifndef WINHTTP_DISABLE_AUTHENTICATION
#define WINHTTP_DISABLE_AUTHENTICATION 0x00000004
#endif
#ifndef WINHTTP_DISABLE_CACHING
#define WINHTTP_DISABLE_CACHING      0x00000008
#endif

#ifndef WINHTTP_OPTION_DISABLE_FEATURE
// Also gated in some SDKs. Value is stable across every WinHTTP version.
#define WINHTTP_OPTION_DISABLE_FEATURE 97
#endif

namespace {

struct ParsedUrl
{
    std::wstring host;   // no port, no brackets
    std::wstring path;   // always starts with '/'
    uint16_t     port = 0;
    bool         https = false;
};

// very dumb parser, good enough for "https://host[:port][/path][?query]".
// hostnames are ascii in practice, so char-by-char widening is fine.
bool ParseUrl(const std::string &url, ParsedUrl &out)
{
    std::string s = url;
    if (s.rfind("https://", 0) == 0)     { out.https = true;  s.erase(0, 8); }
    else if (s.rfind("http://", 0) == 0) { out.https = false; s.erase(0, 7); }
    else return false;

    auto slash = s.find('/');
    std::string hostport = (slash == std::string::npos) ? s : s.substr(0, slash);
    std::string path     = (slash == std::string::npos) ? "/" : s.substr(slash);

    auto colon = hostport.find(':');
    if (colon != std::string::npos)
    {
        try { out.port = static_cast<uint16_t>(std::stoi(hostport.substr(colon + 1))); }
        catch (...) { return false; }
        hostport.erase(colon);
    }
    else
    {
        out.port = out.https ? 443 : 80;
    }

    if (hostport.empty()) return false;

    out.host.assign(hostport.begin(), hostport.end());
    out.path.assign(path.begin(), path.end());
    return true;
}

HttpResult DoRequest(const std::string &url, const wchar_t *method,
                     const void *body, size_t bodySize,
                     const std::vector<HttpHeader> &headers)
{
    HttpResult result;

    ParsedUrl parsed;
    if (!ParseUrl(url, parsed))
        return result;

	HINTERNET session = WinHttpOpen(
		L"csgo_gc/1.0",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return result;

    HINTERNET connect = WinHttpConnect(session, parsed.host.c_str(), parsed.port, 0);
    if (!connect) { WinHttpCloseHandle(session); return result; }

    DWORD flags = parsed.https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, method, parsed.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request)
    {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }

	// Disable WinHTTP's own response cache and cookie jar. Every call must
    // hit the worker: a cached profile would defeat "no caching anywhere".
    {
        DWORD disableFeatures = WINHTTP_DISABLE_CACHING | WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE,
                         &disableFeatures, sizeof(disableFeatures));
    }

    // don't stall forever if the worker is dead
    WinHttpSetTimeouts(request, 2000, 2000, 2000, 2000);

    if (parsed.https)
    {
        DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
        protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
        WinHttpSetOption(request, WINHTTP_OPTION_SECURE_PROTOCOLS,
                         &protocols, sizeof(protocols));
    }

    // build the header blob, if any
    std::wstring headerBlob;
    for (const auto &h : headers)
    {
        std::wstring wname(h.name.begin(), h.name.end());
        std::wstring wvalue(h.value.begin(), h.value.end());
        if (!headerBlob.empty()) headerBlob += L"\r\n";
        headerBlob += wname;
        headerBlob += L": ";
        headerBlob += wvalue;
    }

    BOOL sent = WinHttpSendRequest(request,
        headerBlob.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headerBlob.c_str(),
        headerBlob.empty() ? 0 : static_cast<DWORD>(headerBlob.size()),
        const_cast<void *>(body),
        static_cast<DWORD>(bodySize),
        static_cast<DWORD>(bodySize), 0);

    if (!sent || !WinHttpReceiveResponse(request, nullptr))
    {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    WinHttpQueryHeaders(request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
        WINHTTP_NO_HEADER_INDEX);
    result.status = static_cast<int>(status);

    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0)
    {
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read))
            break;
        result.body.append(chunk.data(), read);
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return result;
}

} // anonymous namespace

HttpResult HttpGetJson(const std::string &url)
{
    return DoRequest(url, L"GET", nullptr, 0, {});
}

HttpResult HttpPostJson(const std::string &url, const std::string &jsonBody)
{
    std::vector<HttpHeader> headers{ { "Content-Type", "application/json" } };
    return DoRequest(url, L"POST", jsonBody.data(), jsonBody.size(), headers);
}

HttpResult HttpPostBinary(const std::string &url,
                          const void *body, size_t size,
                          const std::vector<HttpHeader> &headers)
{
    return DoRequest(url, L"POST", body, size, headers);
}

#else // !_WIN32

HttpResult HttpGetJson(const std::string &) { return {}; }
HttpResult HttpPostJson(const std::string &, const std::string &) { return {}; }
HttpResult HttpPostBinary(const std::string &, const void *, size_t,
                          const std::vector<HttpHeader> &) { return {}; }

#endif