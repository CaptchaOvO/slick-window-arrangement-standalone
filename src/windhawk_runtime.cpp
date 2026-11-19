// Standalone replacement for the parts of Windhawk's runtime used by the
// Slick Window Arrangement mod: function hooking, settings, logging and the
// mod lifecycle.
//
// The DLL is loaded into every process that owns a GUI window. Hooks are
// installed on the process' UI threads from a worker thread, never from
// DllMain.

#include "windhawk.h"

#include <shlobj.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "MinHook.h"

extern "C" void SlickPrepareEnable();
extern "C" void SlickDisableFeatures();

namespace {

constexpr wchar_t kAppName[] = L"SlickWindowArrangement";
constexpr wchar_t kDllName[] = L"SlickWindowArrangement.dll";
constexpr wchar_t kExitEventName[] = L"Local\\SlickWindowArrangement_Exit";

HINSTANCE g_hinst = nullptr;
std::wstring g_configPath;
std::wstring g_logPath;
std::wstring g_fallbackLogPath;
std::mutex g_logMutex;

HANDLE g_reloadEvent = nullptr;
HANDLE g_exitEvent = nullptr;

std::atomic<bool> g_ready{false};
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_modInitialized{false};

struct HookRecord {
    void* target;
    void* detour;
    void** original;
};

std::mutex g_hookMutex;
std::vector<HookRecord> g_hooks;

std::wstring GetModulePath(HMODULE module) {
    wchar_t path[MAX_PATH * 4];
    DWORD len = GetModuleFileNameW(module, path, ARRAYSIZE(path));
    if (!len || len >= ARRAYSIZE(path)) {
        return std::wstring();
    }
    return std::wstring(path, len);
}

std::wstring DirectoryOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

std::wstring JoinPath(const std::wstring& dir, const wchar_t* name) {
    std::wstring result = dir;
    if (!result.empty() && result.back() != L'\\' && result.back() != L'/') {
        result += L'\\';
    }
    result += name;
    return result;
}

void EnsureDirectory(const std::wstring& path) {
    if (!path.empty()) {
        CreateDirectoryW(path.c_str(), nullptr);
    }
}

void InitPaths() {
    std::wstring dllPath = GetModulePath(g_hinst);
    std::wstring dllDir = DirectoryOf(dllPath);

    // Prefer a config file next to the DLL (portable layout). If there is
    // none, use the per-user application data directory.
    std::wstring portableConfig = JoinPath(dllDir, L"config.ini");
    if (GetFileAttributesW(portableConfig.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_configPath = portableConfig;
        g_logPath = JoinPath(dllDir, L"SlickWindowArrangement.log");
    } else {
        wchar_t localAppData[MAX_PATH * 4] = {};
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData,
                                            ARRAYSIZE(localAppData));
        std::wstring baseDir;
        if (len && len < ARRAYSIZE(localAppData)) {
            baseDir = JoinPath(std::wstring(localAppData, len), kAppName);
        } else {
            baseDir = dllDir;
        }
        EnsureDirectory(baseDir);
        g_configPath = JoinPath(baseDir, L"config.ini");
        g_logPath = JoinPath(baseDir, L"SlickWindowArrangement.log");
    }

    wchar_t tempPath[MAX_PATH * 4] = {};
    DWORD tempLen = GetTempPathW(ARRAYSIZE(tempPath), tempPath);
    if (tempLen && tempLen < ARRAYSIZE(tempPath)) {
        g_fallbackLogPath =
            JoinPath(std::wstring(tempPath, tempLen), L"SlickWindowArrangement.log");
    }
}

int DefaultForSetting(const wchar_t* name) {
    if (wcscmp(name, L"Enabled") == 0)
        return 1;
    if (wcscmp(name, L"SnapWindowsWhenDragging") == 0)
        return 1;
    if (wcscmp(name, L"SnapWindowsDistance") == 0)
        return 25;
    if (wcscmp(name, L"KeysToDisableSnapping.Ctrl") == 0)
        return 0;
    if (wcscmp(name, L"KeysToDisableSnapping.Alt") == 0)
        return 1;
    if (wcscmp(name, L"KeysToDisableSnapping.Shift") == 0)
        return 0;
    if (wcscmp(name, L"SlidingAnimation") == 0)
        return 1;
    if (wcscmp(name, L"SnapWindowsWhenSliding") == 0)
        return 1;
    if (wcscmp(name, L"SlidingAnimationSlowdown") == 0)
        return 15;
    return 0;
}

void AppendLog(const wchar_t* text) {
    if (!text || !*text) {
        return;
    }

    OutputDebugStringW(text);

    if (g_logPath.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_logMutex);

    HANDLE file = CreateFileW(g_logPath.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE && !g_fallbackLogPath.empty()) {
        file = CreateFileW(g_fallbackLogPath.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }

    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                      nullptr, nullptr);
    if (utf8Len > 1) {
        std::string utf8(static_cast<size_t>(utf8Len - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), utf8Len,
                            nullptr, nullptr);
        utf8.push_back('\n');
        DWORD written = 0;
        WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written,
                  nullptr);
    }

    CloseHandle(file);
}

bool CreateRuntimeEvents() {
    wchar_t reloadName[128];
    swprintf_s(reloadName, L"Local\\SlickWindowArrangement_Reload_%u",
               GetCurrentProcessId());
    g_reloadEvent = CreateEventW(nullptr, FALSE, FALSE, reloadName);
    g_exitEvent = CreateEventW(nullptr, TRUE, FALSE, kExitEventName);
    return g_reloadEvent && g_exitEvent;
}

void DisableMod() {
    if (!g_enabled.exchange(false)) {
        return;
    }

    Wh_Log(L"Disabling mod");
    SlickDisableFeatures();
}

void EnableMod() {
    if (g_enabled.load()) {
        return;
    }

    Wh_Log(L"Enabling mod");

    SlickPrepareEnable();

    if (!g_modInitialized.load()) {
        if (!Wh_ModInit()) {
            Wh_Log(L"Wh_ModInit failed");
            return;
        }
        g_modInitialized = true;
    }

    g_enabled = true;
}

DWORD WINAPI MonitorThread(LPVOID) {
    HANDLE handles[2] = {g_reloadEvent, g_exitEvent};

    for (;;) {
        DWORD result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) {
            bool wanted = Wh_GetIntSetting(L"Enabled") != 0;
            if (wanted && !g_enabled.load()) {
                EnableMod();
            } else if (!wanted && g_enabled.load()) {
                DisableMod();
            } else if (wanted) {
                Wh_ModSettingsChanged();
            }
            continue;
        }

        if (result == WAIT_OBJECT_0 + 1) {
            // The launcher is exiting. Keep this DLL resident and its monitor
            // alive so that starting the launcher again can re-enable the
            // already-injected processes.
            DisableMod();
            ResetEvent(g_exitEvent);
            continue;
        }

        break;
    }

    DisableMod();
    return 0;
}

DWORD WINAPI InitThread(LPVOID) {
    InitPaths();

    if (!CreateRuntimeEvents()) {
        Wh_Log(L"Failed to create runtime events: %u", GetLastError());
    }

    if (MH_Initialize() != MH_OK) {
        Wh_Log(L"MH_Initialize failed");
        g_ready = true;
        return 0;
    }

    if (Wh_GetIntSetting(L"Enabled") != 0) {
        if (Wh_ModInit()) {
            g_enabled = true;
            g_modInitialized = true;
        } else {
            Wh_Log(L"Initial Wh_ModInit failed");
        }
    }

    g_ready = true;

    if (g_reloadEvent && g_exitEvent) {
        MonitorThread(nullptr);
    }

    return 0;
}

}  // namespace

extern "C" void SlickOnProcessAttach(HINSTANCE hinstDLL) {
    static LONG started = 0;
    if (InterlockedExchange(&started, 1) != 0) {
        return;
    }

    g_hinst = hinstDLL;
    DisableThreadLibraryCalls(hinstDLL);

    // Keep this DLL mapped for the lifetime of the process. Global hooks may
    // be removed when the launcher exits; without pinning, Windows could
    // unload the DLL while its worker/monitor threads are still running,
    // crashing the host application.
    HMODULE pinnedModule = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_PIN,
                       reinterpret_cast<LPCWSTR>(&SlickOnProcessAttach),
                       &pinnedModule);

    HANDLE thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    }
}

extern "C" BOOL Wh_SetFunctionHook(void* targetFunction, void* hookFunction,
                                   void** originalFunction) {
    if (!targetFunction || !hookFunction || !originalFunction) {
        return FALSE;
    }

    std::lock_guard<std::mutex> lock(g_hookMutex);

    for (const auto& record : g_hooks) {
        if (record.target == targetFunction) {
            *originalFunction = *record.original;
            if (MH_EnableHook(targetFunction) != MH_OK) {
                return FALSE;
            }
            return TRUE;
        }
    }

    void* trampoline = nullptr;
    MH_STATUS status =
        MH_CreateHook(targetFunction, hookFunction, &trampoline);
    if (status != MH_OK) {
        Wh_Log(L"MH_CreateHook(%p) failed: %d", targetFunction, status);
        return FALSE;
    }

    status = MH_EnableHook(targetFunction);
    if (status != MH_OK) {
        Wh_Log(L"MH_EnableHook(%p) failed: %d", targetFunction, status);
        MH_RemoveHook(targetFunction);
        return FALSE;
    }

    *originalFunction = trampoline;
    g_hooks.push_back({targetFunction, hookFunction, originalFunction});
    return TRUE;
}

extern "C" int Wh_GetIntSetting(const wchar_t* settingName) {
    if (!settingName) {
        return 0;
    }

    int defaultValue = DefaultForSetting(settingName);
    if (g_configPath.empty()) {
        return defaultValue;
    }

    return static_cast<int>(GetPrivateProfileIntW(
        L"Settings", settingName, defaultValue, g_configPath.c_str()));
}

extern "C" void Wh_Log(const wchar_t* format, ...) {
    wchar_t buffer[2048];

    va_list args;
    va_start(args, format);
    int len = _vsnwprintf_s(buffer, _TRUNCATE, format, args);
    va_end(args);

    if (len <= 0) {
        return;
    }

    wchar_t finalBuffer[2200];
    swprintf_s(finalBuffer, L"[Slick/%u] %s", GetCurrentProcessId(), buffer);
    AppendLog(finalBuffer);
}

// Exported for the injector (and for debugging with a tool such as rundll32).
extern "C" __declspec(dllexport) LRESULT CALLBACK SlickCbtProc(int nCode,
                                                               WPARAM wParam,
                                                               LPARAM lParam) {
    (void)nCode;
    (void)wParam;
    (void)lParam;
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}
