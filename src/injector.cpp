// Standalone launcher for the Slick Window Arrangement mod.
//
// It injects SlickWindowArrangement.dll into processes that own visible
// top-level windows, installs a global CBT hook so that new GUI processes get
// the DLL as well, and provides a small tray menu for the mod's settings.

#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include "resource.h"

#ifdef SLICK_EMBEDDED
#include "embedded_dll.h"
#endif

namespace {

constexpr wchar_t kAppName[] = L"SlickWindowArrangement";
constexpr wchar_t kDllName[] = L"SlickWindowArrangement.dll";
constexpr wchar_t kWindowClass[] = L"SlickWindowArrangementTray";
constexpr wchar_t kMutexName[] = L"Local\\SlickWindowArrangement_Exe";
constexpr wchar_t kExitEventName[] = L"Local\\SlickWindowArrangement_Exit";

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT_PTR kScanTimer = 1;

constexpr int IDM_ENABLED = 100;
constexpr int IDM_SNAP_DRAGGING = 101;
constexpr int IDM_SNAP_SLIDING = 102;
constexpr int IDM_SLIDING = 103;
constexpr int IDM_AUTOSTART = 104;
constexpr int IDM_TRAY_SETTINGS = 105;
constexpr int IDM_OPEN_CONFIG = 110;
constexpr int IDM_RELOAD = 111;
constexpr int IDM_EXIT = 120;

constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"SlickWindowArrangement";
constexpr wchar_t kTaskName[] = L"SlickWindowArrangement";

HINSTANCE g_instance = nullptr;
HWND g_window = nullptr;
HHOOK g_cbtHook = nullptr;
HMODULE g_hookDll = nullptr;
HANDLE g_exitEvent = nullptr;
NOTIFYICONDATAW g_trayIcon = {};
UINT g_taskbarCreatedMessage = 0;
volatile LONG g_injecting = 0;

std::wstring g_exeDir;
std::wstring g_dllPath;
std::wstring g_dllBaseName;
std::wstring g_configPath;
std::wstring g_configDir;

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

HICON LoadAppIcon() {
    HICON icon =
        LoadIconW(g_instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    return icon ? icon : LoadIconW(nullptr, IDI_APPLICATION);
}

bool IsRunValuePresent(HKEY root) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, kRunKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS) {
        return false;
    }

    wchar_t value[2048] = {};
    DWORD type = 0;
    DWORD size = sizeof(value);
    LSTATUS status =
        RegQueryValueExW(key, kRunValue, nullptr, &type,
                         reinterpret_cast<BYTE*>(value), &size);
    RegCloseKey(key);
    return status == ERROR_SUCCESS && type == REG_SZ && value[0] != L'\0';
}

std::wstring GetStartupFolder(bool common);
bool IsScheduledTaskPresent();

bool IsAutostartEnabled() {
    if (IsRunValuePresent(HKEY_CURRENT_USER) ||
        IsRunValuePresent(HKEY_LOCAL_MACHINE)) {
        return true;
    }

    for (bool common : {false, true}) {
        std::wstring folder = GetStartupFolder(common);
        if (folder.empty()) {
            continue;
        }
        std::wstring shortcut = JoinPath(folder, L"SlickWindowArrangement.lnk");
        std::wstring command = JoinPath(folder, L"SlickWindowArrangement.cmd");
        if (GetFileAttributesW(shortcut.c_str()) != INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW(command.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return true;
        }
    }
    return IsScheduledTaskPresent();
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                  sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

LSTATUS UpdateRunValue(HKEY root, bool enable,
                       const std::wstring& command) {
    HKEY key = nullptr;
    DWORD disposition = 0;
    LSTATUS status =
        RegCreateKeyExW(root, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, &disposition);
    if (status != ERROR_SUCCESS) {
        return status;
    }

    if (enable) {
        status = RegSetValueExW(
            key, kRunValue, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kRunValue);
    }

    RegCloseKey(key);
    return status;
}

std::wstring GetStartupFolder(bool common) {
    int csidl =
        (common ? CSIDL_COMMON_STARTUP : CSIDL_STARTUP) | CSIDL_FLAG_CREATE;
    wchar_t folder[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT,
                                folder))) {
        return std::wstring();
    }
    return folder;
}

std::wstring GetStartupShortcutPath(bool common) {
    std::wstring folder = GetStartupFolder(common);
    return folder.empty() ? std::wstring()
                          : JoinPath(folder, L"SlickWindowArrangement.lnk");
}

std::wstring GetStartupCommandPath(bool common) {
    std::wstring folder = GetStartupFolder(common);
    return folder.empty() ? std::wstring()
                          : JoinPath(folder, L"SlickWindowArrangement.cmd");
}

bool CreateStartupShortcut(bool common) {
    std::wstring shortcutPath = GetStartupShortcutPath(common);
    if (shortcutPath.empty()) {
        return false;
    }

    HRESULT initResult =
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninitialize = SUCCEEDED(initResult);

    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                  reinterpret_cast<void**>(&link));
    if (FAILED(hr) || !link) {
        if (uninitialize) {
            CoUninitialize();
        }
        return false;
    }

    std::wstring exePath = GetModulePath(nullptr);
    std::wstring exeDir = DirectoryOf(exePath);
    link->SetPath(exePath.c_str());
    link->SetWorkingDirectory(exeDir.c_str());
    link->SetDescription(L"Slick Window Arrangement");
    link->SetIconLocation(exePath.c_str(), 0);

    IPersistFile* file = nullptr;
    hr = link->QueryInterface(IID_IPersistFile,
                              reinterpret_cast<void**>(&file));
    if (SUCCEEDED(hr) && file) {
        hr = file->Save(shortcutPath.c_str(), TRUE);
        file->Release();
    }
    link->Release();

    if (uninitialize) {
        CoUninitialize();
    }
    return SUCCEEDED(hr);
}

void DeleteStartupShortcut(bool common) {
    std::wstring shortcut = GetStartupShortcutPath(common);
    if (!shortcut.empty()) {
        DeleteFileW(shortcut.c_str());
    }
    std::wstring command = GetStartupCommandPath(common);
    if (!command.empty()) {
        DeleteFileW(command.c_str());
    }
}

bool CreateStartupCommand(bool common) {
    std::wstring path = GetStartupCommandPath(common);
    if (path.empty()) {
        return false;
    }

    std::wstring exePath = GetModulePath(nullptr);
    std::wstring content = L"@echo off\r\nstart \"\" \"" + exePath +
                           L"\"\r\nexit\r\n";

    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD bytes = static_cast<DWORD>(content.size() * sizeof(wchar_t));
    DWORD written = 0;
    BOOL ok = WriteFile(file, content.c_str(), bytes, &written, nullptr);
    CloseHandle(file);
    if (!ok || written != bytes) {
        DeleteFileW(path.c_str());
        return false;
    }
    return true;
}

bool RunHidden(const std::wstring& commandLine, DWORD timeoutMs,
               DWORD* exitCode) {
    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process)) {
        return false;
    }

    DWORD result = WaitForSingleObject(process.hProcess, timeoutMs);
    DWORD code = 1;
    if (result == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &code);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode) {
        *exitCode = code;
    }
    return result == WAIT_OBJECT_0;
}

std::wstring SchtasksPath() {
    wchar_t systemDir[MAX_PATH] = {};
    if (!GetSystemDirectoryW(systemDir, ARRAYSIZE(systemDir))) {
        return L"schtasks.exe";
    }
    return JoinPath(systemDir, L"schtasks.exe");
}

bool IsScheduledTaskPresent() {
    DWORD code = 1;
    std::wstring commandLine = L"\"" + SchtasksPath() +
                               L"\" /Query /TN \"" +
                               std::wstring(kTaskName) + L"\"";
    return RunHidden(commandLine, 10000, &code) && code == 0;
}

bool CreateScheduledTask(bool elevated) {
    std::wstring exePath = GetModulePath(nullptr);
    wchar_t shortPath[MAX_PATH * 4] = {};
    if (GetShortPathNameW(exePath.c_str(), shortPath, ARRAYSIZE(shortPath))) {
        exePath = shortPath;
    }

    std::wstring commandLine =
        L"\"" + SchtasksPath() + L"\" /Create /TN \"" +
        std::wstring(kTaskName) + L"\" /TR \"" + exePath + L"\" /SC ONLOGON /RL " +
        (elevated ? L"HIGHEST" : L"LIMITED") + L" /IT /F";

    DWORD code = 1;
    return RunHidden(commandLine, 15000, &code) && code == 0;
}

void DeleteScheduledTask() {
    std::wstring commandLine = L"\"" + SchtasksPath() +
                               L"\" /Delete /TN \"" +
                               std::wstring(kTaskName) + L"\" /F";
    DWORD code = 1;
    RunHidden(commandLine, 10000, &code);
}

void SetAutostart(bool enable) {
    std::wstring command = L"\"" + GetModulePath(nullptr) + L"\"";
    bool elevated = IsElevated();

    LSTATUS userStatus = UpdateRunValue(HKEY_CURRENT_USER, enable, command);
    LSTATUS machineStatus = ERROR_ACCESS_DENIED;
    if (elevated) {
        machineStatus = UpdateRunValue(HKEY_LOCAL_MACHINE, enable, command);
    }

    auto succeeded = [](LSTATUS status) {
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    };

    bool registryOk = succeeded(userStatus) ||
                      (elevated && succeeded(machineStatus));

    if (enable) {
        if (registryOk) {
            // Remove shortcuts left over from a previous fallback attempt.
            DeleteStartupShortcut(false);
            DeleteStartupShortcut(true);
            return;
        }

        bool shortcutOk = false;
        if (elevated) {
            // Use the all-users startup folder when elevated, so that UAC
            // elevation with a different administrator account still starts
            // the program for the logged-on user.
            shortcutOk = CreateStartupShortcut(true);
        }
        if (!shortcutOk) {
            shortcutOk = CreateStartupShortcut(false);
        }
        if (!shortcutOk) {
            shortcutOk = CreateStartupCommand(elevated);
        }
        if (!shortcutOk && elevated) {
            shortcutOk = CreateStartupCommand(false);
        }
        bool taskOk = false;
        if (!shortcutOk) {
            taskOk = CreateScheduledTask(elevated);
        }
        if (!shortcutOk && !taskOk) {
            wchar_t message[512];
            swprintf_s(
                message,
                L"无法设置开机自启。\n\n"
                L"注册表 HKCU 错误码：%ld\n"
                L"注册表 HKLM 错误码：%ld\n"
                L"管理员权限：%s\n\n"
                L"可能被安全软件拦截。",
                static_cast<long>(userStatus),
                static_cast<long>(machineStatus),
                elevated ? L"是" : L"否");
            MessageBoxW(nullptr, message, L"窗口滑动排列", MB_ICONWARNING);
        }
        return;
    }

    DeleteStartupShortcut(false);
    DeleteStartupShortcut(true);
    DeleteScheduledTask();

    if (!registryOk) {
        wchar_t message[512];
        swprintf_s(
            message,
            L"无法完全删除开机自启项。\n\n"
            L"注册表 HKCU 错误码：%ld\n"
            L"注册表 HKLM 错误码：%ld\n"
            L"如果 HKLM 项存在，请以管理员身份运行后再取消。",
            static_cast<long>(userStatus), static_cast<long>(machineStatus));
        MessageBoxW(nullptr, message, L"窗口滑动排列", MB_ICONWARNING);
    }
}

std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

std::wstring ResolveDllPath() {
    std::wstring external = JoinPath(g_exeDir, kDllName);
    if (GetFileAttributesW(external.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return external;
    }

#ifdef SLICK_EMBEDDED
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < kEmbeddedDllSize; ++i) {
        hash ^= kEmbeddedDll[i];
        hash *= 1099511628211ULL;
    }

    wchar_t fileName[128];
    swprintf(fileName, ARRAYSIZE(fileName),
             L"SlickWindowArrangement-%016llx.dll",
             static_cast<unsigned long long>(hash));
    std::wstring embedded = JoinPath(g_configDir, fileName);

    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (GetFileAttributesExW(embedded.c_str(), GetFileExInfoStandard,
                             &attributes)) {
        ULARGE_INTEGER size;
        size.HighPart = attributes.nFileSizeHigh;
        size.LowPart = attributes.nFileSizeLow;
        if (size.QuadPart == kEmbeddedDllSize) {
            return embedded;
        }
    }

    HANDLE file = CreateFileW(embedded.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        BOOL ok = WriteFile(file, kEmbeddedDll,
                            static_cast<DWORD>(kEmbeddedDllSize), &written,
                            nullptr);
        CloseHandle(file);
        if (ok && written == kEmbeddedDllSize) {
            return embedded;
        }
        DeleteFileW(embedded.c_str());
    }
#endif

    return external;
}

bool IsDirectoryWritable(const std::wstring& directory) {
    std::wstring testPath = JoinPath(directory, L".write-test.tmp");
    HANDLE file = CreateFileW(testPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(file);
    DeleteFileW(testPath.c_str());
    return true;
}

void InitPaths() {
    g_exeDir = DirectoryOf(GetModulePath(nullptr));

    g_configDir = g_exeDir;
    if (!IsDirectoryWritable(g_configDir)) {
        wchar_t localAppData[MAX_PATH * 4] = {};
        DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData,
                                            ARRAYSIZE(localAppData));
        if (len && len < ARRAYSIZE(localAppData)) {
            g_configDir = JoinPath(std::wstring(localAppData, len), kAppName);
            CreateDirectoryW(g_configDir.c_str(), nullptr);
        }

        if (!IsDirectoryWritable(g_configDir)) {
            wchar_t tempPath[MAX_PATH * 4] = {};
            DWORD tempLen = GetTempPathW(ARRAYSIZE(tempPath), tempPath);
            if (tempLen && tempLen < ARRAYSIZE(tempPath)) {
                g_configDir =
                    JoinPath(std::wstring(tempPath, tempLen), kAppName);
                CreateDirectoryW(g_configDir.c_str(), nullptr);
            }
        }
    }

    g_configPath = JoinPath(g_configDir, L"config.ini");
    g_dllPath = ResolveDllPath();

    size_t slash = g_dllPath.find_last_of(L"\\/");
    g_dllBaseName =
        slash == std::wstring::npos ? g_dllPath : g_dllPath.substr(slash + 1);
}

int GetSetting(const wchar_t* name, int defaultValue) {
    return static_cast<int>(GetPrivateProfileIntW(
        L"Settings", name, defaultValue, g_configPath.c_str()));
}

void SetSetting(const wchar_t* name, int value) {
    wchar_t text[32];
    wsprintfW(text, L"%d", value);
    WritePrivateProfileStringW(L"Settings", name, text, g_configPath.c_str());
}

void WriteDefaultConfig() {
    if (GetFileAttributesW(g_configPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return;
    }

    SetSetting(L"Enabled", 1);
    SetSetting(L"SnapWindowsWhenDragging", 1);
    SetSetting(L"SnapWindowsDistance", 25);
    SetSetting(L"KeysToDisableSnapping.Ctrl", 0);
    SetSetting(L"KeysToDisableSnapping.Alt", 1);
    SetSetting(L"KeysToDisableSnapping.Shift", 0);
    SetSetting(L"SlidingAnimation", 1);
    SetSetting(L"SnapWindowsWhenSliding", 1);
    SetSetting(L"SlidingAnimationSlowdown", 15);
}

void BroadcastReload();

void SignalReload() {
    BroadcastReload();
}

bool IsCriticalProcessName(const std::wstring& name) {
    static const wchar_t* kNames[] = {
        L"system",          L"registry",       L"smss.exe",
        L"csrss.exe",       L"wininit.exe",    L"services.exe",
        L"lsass.exe",       L"winlogon.exe",   L"fontdrvhost.exe",
        L"dwm.exe",         L"svchost.exe",    L"audiodg.exe",
        L"msmpeng.exe",     L"securityhealthservice.exe",
        L"easyanticheat.exe", L"easyanticheat_eos.exe",
        L"beservice.exe",   L"vgc.exe",        L"vgtray.exe",
        L"gameguard.des",   L"nprotect.exe",   L"tenprotect.exe",
        L"anticheatexpert.exe", L"ace-game.exe",
        L"slickwindowarrangement.exe",
    };

    for (const wchar_t* critical : kNames) {
        if (name == critical) {
            return true;
        }
    }
    return false;
}

bool IsSameSession(DWORD processId) {
    DWORD sessionId = 0;
    DWORD ownSessionId = 0;
    if (!ProcessIdToSessionId(processId, &sessionId) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &ownSessionId)) {
        return false;
    }
    return sessionId == ownSessionId;
}

bool Is64BitProcess(HANDLE process) {
    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    static auto isWow64Process2 =
        reinterpret_cast<IsWow64Process2Fn>(GetProcAddress(
            GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));

    if (isWow64Process2) {
        USHORT processMachine = 0;
        USHORT nativeMachine = 0;
        if (isWow64Process2(process, &processMachine, &nativeMachine)) {
            return processMachine == IMAGE_FILE_MACHINE_UNKNOWN;
        }
    }

    BOOL wow64 = FALSE;
    if (IsWow64Process(process, &wow64)) {
        return !wow64;
    }
    return false;
}

bool ProcessHasModule(DWORD processId, const wchar_t* moduleName) {
    HANDLE snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                 processId);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szModule, moduleName) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

bool ProcessHasUser32(DWORD processId) {
    return ProcessHasModule(processId, L"user32.dll");
}

bool ProcessHasSlickModule(DWORD processId) {
    HANDLE snapshot =
        CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                                 processId);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }

    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    bool found = false;
    if (Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsnicmp(entry.szModule, L"SlickWindowArrangement", 22) == 0) {
                found = true;
                break;
            }
        } while (Module32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return found;
}

void BroadcastReload() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            DWORD processId = entry.th32ProcessID;
            if (!processId || processId == 4 || !IsSameSession(processId)) {
                continue;
            }
            if (!ProcessHasSlickModule(processId)) {
                continue;
            }

            wchar_t eventName[128];
            swprintf(eventName, ARRAYSIZE(eventName),
                     L"Local\\SlickWindowArrangement_Reload_%u", processId);
            HANDLE event =
                OpenEventW(EVENT_MODIFY_STATE, FALSE, eventName);
            if (event) {
                SetEvent(event);
                CloseHandle(event);
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
}

bool InjectDll(DWORD processId, const std::wstring& dllPath) {
    if (processId == GetCurrentProcessId() || !IsSameSession(processId)) {
        return false;
    }

    if (ProcessHasModule(processId, g_dllBaseName.c_str()) ||
        ProcessHasSlickModule(processId)) {
        return true;
    }

    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                     PROCESS_VM_READ,
                                 FALSE, processId);
    if (!process) {
        return false;
    }

    bool ok = false;
    if (Is64BitProcess(process)) {
        const size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
        void* remoteMemory =
            VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                           PAGE_READWRITE);
        if (remoteMemory &&
            WriteProcessMemory(process, remoteMemory, dllPath.c_str(), bytes,
                               nullptr)) {
            HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
            auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
                GetProcAddress(kernel32, "LoadLibraryW"));
            if (loadLibrary) {
                HANDLE thread =
                    CreateRemoteThread(process, nullptr, 0, loadLibrary,
                                       remoteMemory, 0, nullptr);
                if (thread) {
                    WaitForSingleObject(thread, 8000);
                    CloseHandle(thread);
                }
            }
        }
        if (remoteMemory) {
            VirtualFreeEx(process, remoteMemory, 0, MEM_RELEASE);
        }

        // LoadLibrary's return value is truncated by GetExitCodeThread, so
        // verify by looking for the module instead.
        for (int i = 0; i < 10 && !ok; ++i) {
            ok = ProcessHasModule(processId, g_dllBaseName.c_str());
            if (!ok) {
                Sleep(100);
            }
        }
    }

    CloseHandle(process);
    return ok;
}

std::vector<DWORD> GetWindowProcessIds() {
    struct EnumData {
        std::vector<DWORD> pids;
    } data;

    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* pids = reinterpret_cast<EnumData*>(param);
            if (!IsWindowVisible(hwnd)) {
                return TRUE;
            }
            if (GetWindow(hwnd, GW_OWNER) != nullptr) {
                return TRUE;
            }

            DWORD processId = 0;
            GetWindowThreadProcessId(hwnd, &processId);
            if (processId) {
                pids->pids.push_back(processId);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&data));

    HWND shellWindow = GetShellWindow();
    if (shellWindow) {
        DWORD processId = 0;
        GetWindowThreadProcessId(shellWindow, &processId);
        if (processId) {
            data.pids.push_back(processId);
        }
    }

    std::sort(data.pids.begin(), data.pids.end());
    data.pids.erase(std::unique(data.pids.begin(), data.pids.end()),
                    data.pids.end());
    return data.pids;
}

DWORD WINAPI InjectExistingThread(LPVOID) {
    if (InterlockedCompareExchange(&g_injecting, 1, 0) != 0) {
        return 0;
    }

    std::vector<DWORD> pids = GetWindowProcessIds();
    for (DWORD processId : pids) {
        if (processId == GetCurrentProcessId()) {
            continue;
        }

        HANDLE process =
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
        if (!process) {
            continue;
        }

        wchar_t imageName[MAX_PATH] = {};
        DWORD size = ARRAYSIZE(imageName);
        bool haveName =
            QueryFullProcessImageNameW(process, 0, imageName, &size) != FALSE;
        CloseHandle(process);

        if (haveName) {
            std::wstring fullName(imageName);
            size_t slash = fullName.find_last_of(L"\\/");
            std::wstring baseName =
                slash == std::wstring::npos ? fullName : fullName.substr(slash + 1);
            if (IsCriticalProcessName(Lowercase(baseName))) {
                continue;
            }
        }

        if (!ProcessHasUser32(processId)) {
            continue;
        }

        InjectDll(processId, g_dllPath);
        Sleep(15);
    }

    InterlockedExchange(&g_injecting, 0);
    return 0;
}

void StartInjection() {
    HANDLE thread =
        CreateThread(nullptr, 0, InjectExistingThread, nullptr, 0, nullptr);
    if (thread) {
        CloseHandle(thread);
    }
}

bool InstallCbtHook() {
    if (g_cbtHook) {
        return true;
    }

    g_hookDll = LoadLibraryW(g_dllPath.c_str());
    if (!g_hookDll) {
        return false;
    }

    auto hookProc =
        reinterpret_cast<HOOKPROC>(GetProcAddress(g_hookDll, "SlickCbtProc"));
    if (!hookProc) {
        return false;
    }

    g_cbtHook = SetWindowsHookExW(WH_CBT, hookProc, g_hookDll, 0);
    return g_cbtHook != nullptr;
}

void AddTrayIcon() {
    g_trayIcon.cbSize = sizeof(g_trayIcon);
    g_trayIcon.hWnd = g_window;
    g_trayIcon.uID = 1;
    g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_trayIcon.uCallbackMessage = WM_TRAYICON;
    g_trayIcon.hIcon = LoadAppIcon();
    lstrcpynW(g_trayIcon.szTip, L"Slick Window Arrangement",
              ARRAYSIZE(g_trayIcon.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_trayIcon);

    if (GetSetting(L"TrayTipShown", 0) == 0) {
        g_trayIcon.uFlags |= NIF_INFO;
        g_trayIcon.dwInfoFlags = NIIF_INFO;
        lstrcpynW(g_trayIcon.szInfoTitle, L"窗口滑动排列",
                  ARRAYSIZE(g_trayIcon.szInfoTitle));
        lstrcpynW(
            g_trayIcon.szInfo,
            L"如果托盘图标不见了，请点击任务栏的 ^ 隐藏图标区，"
            L"把本程序图标拖到任务栏上固定显示。",
            ARRAYSIZE(g_trayIcon.szInfo));
        Shell_NotifyIconW(NIM_MODIFY, &g_trayIcon);
        g_trayIcon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        SetSetting(L"TrayTipShown", 1);
    }
}

void RemoveTrayIcon() {
    Shell_NotifyIconW(NIM_DELETE, &g_trayIcon);
}

HMENU BuildTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return nullptr;
    }

    UINT enabledFlags = MF_STRING;
    if (GetSetting(L"Enabled", 1)) {
        enabledFlags |= MF_CHECKED;
    }
    AppendMenuW(menu, enabledFlags, IDM_ENABLED, L"启用");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    UINT snapDragFlags = MF_STRING;
    if (GetSetting(L"SnapWindowsWhenDragging", 1)) {
        snapDragFlags |= MF_CHECKED;
    }
    AppendMenuW(menu, snapDragFlags, IDM_SNAP_DRAGGING, L"拖动时吸附窗口");

    UINT snapSlideFlags = MF_STRING;
    if (GetSetting(L"SnapWindowsWhenSliding", 1)) {
        snapSlideFlags |= MF_CHECKED;
    }
    AppendMenuW(menu, snapSlideFlags, IDM_SNAP_SLIDING, L"滑动时吸附窗口");

    UINT slidingFlags = MF_STRING;
    if (GetSetting(L"SlidingAnimation", 1)) {
        slidingFlags |= MF_CHECKED;
    }
    AppendMenuW(menu, slidingFlags, IDM_SLIDING, L"滑动动画");

    UINT autostartFlags = MF_STRING;
    if (IsAutostartEnabled()) {
        autostartFlags |= MF_CHECKED;
    }
    AppendMenuW(menu, autostartFlags, IDM_AUTOSTART, L"开机自启");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_SETTINGS, L"托盘图标设置...");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_OPEN_CONFIG, L"打开设置文件...");
    AppendMenuW(menu, MF_STRING, IDM_RELOAD, L"重新加载设置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    SetMenuDefaultItem(menu, IDM_OPEN_CONFIG, FALSE);
    return menu;
}

void ToggleSetting(const wchar_t* name, int defaultValue) {
    SetSetting(name, GetSetting(name, defaultValue) ? 0 : 1);
    SignalReload();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam,
                            LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            // g_window is assigned only after CreateWindowExW returns, but
            // Shell_NotifyIcon needs a valid window handle now.
            g_window = hwnd;
            AddTrayIcon();
            SetTimer(hwnd, kScanTimer, 5000, nullptr);
            StartInjection();
            return 0;

        case WM_TIMER:
            if (wParam == kScanTimer) {
                StartInjection();
            }
            return 0;

        case WM_TRAYICON:
            if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
                SetForegroundWindow(hwnd);
                POINT cursor;
                GetCursorPos(&cursor);
                HMENU menu = BuildTrayMenu();
                if (menu) {
                    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0,
                                   hwnd, nullptr);
                    DestroyMenu(menu);
                }
                PostMessage(hwnd, WM_NULL, 0, 0);
            } else if (lParam == WM_LBUTTONDBLCLK) {
                ShellExecuteW(nullptr, L"open", g_configPath.c_str(), nullptr,
                              nullptr, SW_SHOWNORMAL);
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDM_ENABLED:
                    ToggleSetting(L"Enabled", 1);
                    break;
                case IDM_SNAP_DRAGGING:
                    ToggleSetting(L"SnapWindowsWhenDragging", 1);
                    break;
                case IDM_SNAP_SLIDING:
                    ToggleSetting(L"SnapWindowsWhenSliding", 1);
                    break;
                case IDM_SLIDING:
                    ToggleSetting(L"SlidingAnimation", 1);
                    break;
                case IDM_AUTOSTART:
                    SetAutostart(!IsAutostartEnabled());
                    break;
                case IDM_TRAY_SETTINGS:
                    ShellExecuteW(nullptr, L"open", L"ms-settings:taskbar",
                                  nullptr, nullptr, SW_SHOWNORMAL);
                    break;
                case IDM_OPEN_CONFIG:
                    ShellExecuteW(nullptr, L"open", g_configPath.c_str(), nullptr,
                                  nullptr, SW_SHOWNORMAL);
                    break;
                case IDM_RELOAD:
                    SignalReload();
                    break;
                case IDM_EXIT:
                    DestroyWindow(hwnd);
                    break;
                default:
                    break;
            }
            return 0;

        case WM_DESTROY:
            KillTimer(hwnd, kScanTimer);
            if (g_exitEvent) {
                SetEvent(g_exitEvent);
            }
            if (g_cbtHook) {
                UnhookWindowsHookEx(g_cbtHook);
                g_cbtHook = nullptr;
            }
            RemoveTrayIcon();
            PostQuitMessage(0);
            return 0;

        default:
            if (message == g_taskbarCreatedMessage && g_taskbarCreatedMessage) {
                AddTrayIcon();
                return 0;
            }
            break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    g_instance = instance;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr,
                    L"窗口滑动排列已经在运行。\n"
                    L"请使用托盘图标修改设置或退出。",
                    L"窗口滑动排列", MB_ICONINFORMATION);
        CloseHandle(mutex);
        return 0;
    }

    InitPaths();
    WriteDefaultConfig();

    if (GetFileAttributesW(g_dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr,
                    L"找不到 SlickWindowArrangement.dll。\n"
                    L"请将程序和运行库放在同一个文件夹。",
                    L"窗口滑动排列", MB_ICONERROR);
        return 1;
    }

    g_exitEvent = CreateEventW(nullptr, TRUE, FALSE, kExitEventName);
    if (!g_exitEvent) {
        MessageBoxW(nullptr, L"无法创建运行事件。",
                    L"窗口滑动排列", MB_ICONERROR);
        return 1;
    }

    // An earlier instance may have left the manual-reset exit event signaled.
    ResetEvent(g_exitEvent);

    g_taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIcon = LoadAppIcon();
    windowClass.hIconSm = LoadAppIcon();
    if (!RegisterClassExW(&windowClass)) {
        return 1;
    }

    g_window = CreateWindowExW(0, kWindowClass, L"Slick Window Arrangement", 0,
                               0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        return 1;
    }

    if (!InstallCbtHook()) {
        // Injection of already running processes still works; new processes
        // are picked up by the polling timer.
        OutputDebugStringW(L"Slick: failed to install CBT hook\n");
    }

    // Re-enable DLLs that were injected by an earlier instance of this
    // launcher and are still resident in running applications.
    BroadcastReload();

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (mutex) {
        CloseHandle(mutex);
    }
    return 0;
}
