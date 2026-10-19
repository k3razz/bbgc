#pragma once

#include <cstdarg>

namespace Platform
{

enum class LogLevel
{
    Debug,
    Info,
    Success,
    Warning,
    Error,
};

// called right after we load into the process
void Initialize();

// core logger. don't call this directly unless you already have a va_list,
// use the Print* wrappers below
void LogV(LogLevel level, const char *format, va_list ap);

// mikkotodo could make these macros so we don't pay the va_list dance on
// every call, but they inline fine and macros would break type checking.
// probably not worth it.

inline void PrintDebug(const char *format, ...)
{
    va_list ap; va_start(ap, format);
    LogV(LogLevel::Debug, format, ap);
    va_end(ap);
}

// legacy name — everything used to call Platform::Print. keep it around
// so we don't have to touch every call site in one commit. maps to Info.
inline void Print(const char *format, ...)
{
    va_list ap; va_start(ap, format);
    LogV(LogLevel::Info, format, ap);
    va_end(ap);
}

inline void PrintInfo(const char *format, ...)
{
    va_list ap; va_start(ap, format);
    LogV(LogLevel::Info, format, ap);
    va_end(ap);
}

inline void PrintSuccess(const char *format, ...)
{
    va_list ap; va_start(ap, format);
    LogV(LogLevel::Success, format, ap);
    va_end(ap);
}

inline void PrintWarning(const char *format, ...)
{
    va_list ap; va_start(ap, format);
    LogV(LogLevel::Warning, format, ap);
    va_end(ap);
}

inline void PrintError(const char *format, ...)
{
    va_list ap; va_start(ap, format);
    LogV(LogLevel::Error, format, ap);
    va_end(ap);
}

// fatal error, show a message box if possible and exit the program
[[noreturn]] void Error(const char *format, ...);

// get steamclient's path without incrementing its refcount
// buffer is only accessed by the platform code, on
// windows we stick utf16 to it and utf8 on other platforms
bool SteamClientPath(void *buffer, size_t bufferSize);

// load steamclient from the provided path, increment its refcount
// and get a pointer to the factory function (exported symbol CreateInterface)
void *SteamClientFactory(const void *pathBuffer);

// set an envar to the specified value even if it's already set
void SetEnvVar(const char *name, const char *value);

// patch the graffiti public key in the specified module to get sprays working
// the module name is given in a platform-agnostic format
// (e.g. on linux pass server as moduleName, and it'll operate on server_client.so)
bool PatchGraffitiPublicKey(std::string_view moduleName, const void *original, const void *replacement, size_t size);

// returns true if serverbrowser was loaded and we patched it
bool PatchServerBrowserAppId(uint32_t appId);

void *ResolveModuleInterface(const char *moduleName, const char *interfaceName);

} // namespace Platform