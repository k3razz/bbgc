#include <windows.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <memory>
#include <string>

#if !defined(DEDICATED)

#define DLL_EXPORT extern "C" __declspec(dllexport)

DLL_EXPORT DWORD NvOptimusEnablement = 1;
DLL_EXPORT int AmdPowerXpressRequestHighPerformance = 1;

DLL_EXPORT bool BSecureAllowed(unsigned char*, int, int) { return true; }
DLL_EXPORT int CountFilesCompletedTrustCheck() { return 0; }
DLL_EXPORT int CountFilesNeedTrustCheck() { return 0; }
DLL_EXPORT int GetTotalFilesLoaded() { return 0; }
DLL_EXPORT int RuntimeCheck(int, int) { return 0; }

#endif

#if defined(DEDICATED)
#define SYMBOL_NAME "DedicatedMain"
typedef int (*LauncherMain_t)(HINSTANCE, HINSTANCE, LPSTR, int);
#else
#define SYMBOL_NAME "LauncherMain"
typedef int (*LauncherMain_t)(bool, HINSTANCE, HINSTANCE, LPSTR, int);
#endif

typedef void (*InstallGC_t)(bool dedicated);

namespace {

// RAII wrapper for HANDLEs that are closed with CloseHandle().
struct HandleCloser {
    void operator()(HANDLE h) const {
        if (h && h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
        }
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

void ErrorMessageBox(const wchar_t* format, ...) {
    wchar_t buffer[4096];
    va_list ap;
    va_start(ap, format);
    vswprintf_s(buffer, std::size(buffer), format, ap);
    va_end(ap);

    MessageBoxW(nullptr, buffer, L"Launcher Error", MB_OK | MB_ICONERROR);
}

std::wstring LastErrorString() {
    const DWORD error = GetLastError();
    wchar_t buffer[4096];

    const DWORD result = FormatMessageW(
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS |
        FORMAT_MESSAGE_MAX_WIDTH_MASK,
        nullptr,
        error,
        0,
        buffer,
        static_cast<DWORD>(std::size(buffer)),
        nullptr);

    if (!result) {
        swprintf_s(buffer, std::size(buffer), L"Unknown error (%lu)", error);
    }

    return buffer;
}

std::wstring GetExecutableDirectory() {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer, static_cast<DWORD>(std::size(buffer)));

    if (!length || length == std::size(buffer)) {
        ErrorMessageBox(L"GetModuleFileName failed:\n%ls",
                        LastErrorString().c_str());
        return {};
    }

    std::wstring path(buffer, length);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        path.resize(slash);
    }
    return path;
}

void* LoadModuleAndFindSymbol(const std::wstring& modulePath,
                              const char* symbol) {
    HMODULE module = LoadLibraryExW(
        modulePath.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (!module) {
        ErrorMessageBox(L"Could not load '%ls':\n%ls",
                        modulePath.c_str(),
                        LastErrorString().c_str());
        return nullptr;
    }

    void* function = GetProcAddress(module, symbol);
    if (!function) {
        ErrorMessageBox(L"Could not find '%S' from '%ls':\n%ls",
                        symbol,
                        modulePath.c_str(),
                        LastErrorString().c_str());
        return nullptr;
    }

    return function;
}

bool EqualsIgnoreCase(const wchar_t* a, const wchar_t* b) {
    return _wcsicmp(a, b) == 0;
}

bool FindProcessIdByName(const wchar_t* exeName, DWORD& outPid) {
    outPid = 0;

    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) {
        return false;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot.get(), &entry)) {
        do {
            if (EqualsIgnoreCase(entry.szExeFile, exeName)) {
                outPid = entry.th32ProcessID;
                return true;
            }
        } while (Process32NextW(snapshot.get(), &entry));
    }

    return false;
}

bool InjectDllIntoProcess(DWORD processId, const std::wstring& dllPath) {
    UniqueHandle process(OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId));
    if (!process) {
        return false;
    }

    const SIZE_T pathBytes = (dllPath.size() + 1) * sizeof(wchar_t);
    LPVOID remotePath = VirtualAllocEx(
        process.get(), nullptr, pathBytes, MEM_COMMIT, PAGE_READWRITE);

    if (!remotePath) {
        return false;
    }

    bool ok = false;

    if (WriteProcessMemory(process.get(), remotePath,
                           dllPath.c_str(), pathBytes, nullptr)) {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(kernel32, "LoadLibraryW"));

        if (loadLibrary) {
            UniqueHandle remoteThread(CreateRemoteThread(
                process.get(), nullptr, 0, loadLibrary, remotePath, 0, nullptr));

            if (remoteThread) {
                WaitForSingleObject(remoteThread.get(), INFINITE);
                ok = true;
            }
        }
    }

    VirtualFreeEx(process.get(), remotePath, 0, MEM_RELEASE);
    return ok;
}

DWORD WINAPI InjectHookDllsThread(LPVOID lpParam) {
    std::unique_ptr<std::wstring> baseDir(
        static_cast<std::wstring*>(lpParam));

    Sleep(5000);

    const std::wstring targetDir = *baseDir + L"\\csgo_gc\\path";
    const std::wstring pattern = *baseDir + L"\\*.dll";

    WIN32_FIND_DATAW findData;
    HANDLE find = FindFirstFileW(pattern.c_str(), &findData);
    if (find == INVALID_HANDLE_VALUE) {
        return 0;
    }

    do {
        if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            const std::wstring dllPath =
                targetDir + L"\\" + findData.cFileName;

            DWORD processId = 0;
            if (FindProcessIdByName(L"csgo.exe", processId)) {
                InjectDllIntoProcess(processId, dllPath);
            }
        }
    } while (FindNextFileW(find, &findData));

    FindClose(find);
    return 0;
}

bool HasCommandLineSwitch(const char* cmdLine, const char* switchName) {
    if (!cmdLine) {
        return false;
    }

    const size_t switchLen = strlen(switchName);
    const char* p = cmdLine;

    while ((p = StrStrIA(p, switchName)) != nullptr) {
        const char next = p[switchLen];
        if (next == '\0' || next == ' ' || next == '\t' || next == '"') {
            return true;
        }
        p += switchLen;
    }

    return false;
}

} // namespace

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nShowCmd) {
    const bool noGui = HasCommandLineSwitch(lpCmdLine, "-nogui");

    const std::wstring baseDir = GetExecutableDirectory();
    if (baseDir.empty()) {
        return 1;
    }

    // Delete cached flash weapon icons.
    {
        const std::wstring cachedPath =
            baseDir + L"\\csgo\\resource\\flash\\econ\\weapons\\cached";

        if (PathFileExistsW(cachedPath.c_str())) {
            SetFileAttributesW(cachedPath.c_str(), FILE_ATTRIBUTE_HIDDEN);

            std::wstring fromPath = cachedPath;
            fromPath.push_back(L'\0'); // SHFileOperation wants double-null.

            SHFILEOPSTRUCTW op = {};
            op.wFunc = FO_DELETE;
            op.pFrom = fromPath.c_str();
            op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
            SHFileOperationW(&op);
        }
    }

    // Prepend "<baseDir>\bin\.\;" to PATH.
    {
        std::wstring path = baseDir + L"\\bin\\.\\;";
        if (const wchar_t* existing = _wgetenv(L"PATH")) {
            path += existing;
        }
        _wputenv_s(L"PATH", path.c_str());
    }

    std::wstring modulePath = baseDir + L"\\bin\\launcher.dll";
    auto LauncherMain = reinterpret_cast<LauncherMain_t>(
        LoadModuleAndFindSymbol(modulePath, SYMBOL_NAME));
    if (!LauncherMain) {
        return 1;
    }

    modulePath = baseDir + L"\\csgo_gc\\.\\csgo_gc.dll";
    auto InstallGC = reinterpret_cast<InstallGC_t>(
        LoadModuleAndFindSymbol(modulePath, "InstallGC"));
    if (!InstallGC) {
        return 1;
    }

    // If -nogui is passed, wait for csgo.exe and inject the no-gui hook.
    if (noGui) {
        const std::wstring hookPath =
            baseDir + L"\\csgo_gc\\path\\no_gui_hook.dll";

        if (PathFileExistsW(hookPath.c_str())) {
            DWORD processId = 0;
            for (int attempt = 0; attempt < 30; ++attempt) {
                Sleep(1000);
                if (FindProcessIdByName(L"csgo.exe", processId)) {
                    break;
                }
            }

            if (processId) {
                InjectDllIntoProcess(processId, hookPath);
            }
        }
    }

    // Background injector thread.
    {
        auto* threadParam = new std::wstring(baseDir);
        HANDLE thread = CreateThread(nullptr, 0, InjectHookDllsThread,
                                     threadParam, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        } else {
            delete threadParam;
        }
    }

#if defined(DEDICATED)
    InstallGC(true);
    return LauncherMain(hInstance, hPrevInstance, lpCmdLine, nShowCmd);
#else
    InstallGC(false);
    LauncherMain(true, hInstance, hPrevInstance, lpCmdLine, nShowCmd);
#endif
}