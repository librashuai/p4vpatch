#include <windows.h>
#include <tlhelp32.h>

#include <cwchar>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::wstring fullPath(const std::wstring& path) {
    DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!size) return {};
    std::wstring result(size, L'\0');
    DWORD written = GetFullPathNameW(path.c_str(), size, result.data(), nullptr);
    if (!written || written >= size) return {};
    result.resize(written);
    return result;
}

std::wstring directory(const std::wstring& path) {
    auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : path.substr(0, pos);
}

std::wstring ownPath() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!size) return {};
        if (size < path.size()) { path.resize(size); return path; }
        path.resize(path.size() * 2);
    }
}

// Toolhelp can return ERROR_BAD_LENGTH while the target loader is working.
uintptr_t moduleBase(DWORD pid, const wchar_t* name) {
    for (int i = 0; i < 4; ++i) {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        if (snapshot == INVALID_HANDLE_VALUE) { Sleep(30); continue; }
        MODULEENTRY32W entry{sizeof(entry)};
        uintptr_t base = 0;
        if (Module32FirstW(snapshot, &entry)) {
            do {
                if (_wcsicmp(entry.szModule, name) == 0) {
                    base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
                    break;
                }
            } while (Module32NextW(snapshot, &entry));
        }
        CloseHandle(snapshot);
        return base;
    }
    return 0;
}

// LoadLibraryW is sometimes forwarded from kernel32 to KernelBase. Resolve
// the actual local module containing the function, then calculate its RVA in
// the target process rather than assuming identical ASLR base addresses.
LPTHREAD_START_ROUTINE remoteLoadLibrary(DWORD pid) {
    auto fn = reinterpret_cast<const void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    if (!fn) return nullptr;
    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(fn), &owner)) return nullptr;
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(owner, path, MAX_PATH)) return nullptr;
    const wchar_t* filename = wcsrchr(path, L'\\');
    auto base = moduleBase(pid, filename ? filename + 1 : path);
    if (!base) return nullptr;
    return reinterpret_cast<LPTHREAD_START_ROUTINE>(
        base + reinterpret_cast<uintptr_t>(fn) - reinterpret_cast<uintptr_t>(owner));
}

std::wstring quote(const wchar_t* input) {
    std::wstring result = L"\"";
    unsigned slashes = 0;
    for (const wchar_t* p = input; ; ++p) {
        if (*p == L'\\') { ++slashes; continue; }
        if (*p == L'\"' || *p == L'\0') result.append(slashes * 2, L'\\');
        else result.append(slashes, L'\\');
        slashes = 0;
        if (!*p) break;
        if (*p == L'\"') result += L'\\';
        result += *p;
    }
    return result + L'\"';
}

bool inject(HANDLE process, DWORD pid, const std::wstring& dll) {
    auto load = remoteLoadLibrary(pid);
    if (!load) return false;
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remote) return false;
    SIZE_T written = 0;
    bool ok = WriteProcessMemory(process, remote, dll.c_str(), bytes, &written) && written == bytes;
    HANDLE thread = ok ? CreateRemoteThread(process, nullptr, 0, load, remote, 0, nullptr) : nullptr;
    if (!thread) {
        VirtualFreeEx(process, remote, 0, MEM_RELEASE);
        return false;
    }
    DWORD waited = WaitForSingleObject(thread, 30000);
    DWORD result = 0;
    ok = waited == WAIT_OBJECT_0 && GetExitCodeThread(thread, &result) && result != 0;
    // Never free the argument while a timed-out remote thread may still read it.
    if (waited == WAIT_OBJECT_0) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(thread);
    return ok;
}
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    // No arguments: launch the p4v.exe next to this launcher (double-click case).
    // An explicit executable path may be followed by P4V arguments.
    const std::wstring launcherDir = directory(ownPath());
    const std::wstring exe = fullPath(argc > 1 ? argv[1] : launcherDir + L"\\p4v.exe");
    const std::wstring dll = launcherDir + L"\\p4vpatch.dll";
    auto reportError = [argc](const std::wstring& message) {
        std::wcerr << message << L'\n';
        // A double-clicked console disappears immediately; keep errors visible.
        if (argc == 1) MessageBoxW(nullptr, message.c_str(), L"P4V Patch Launcher", MB_OK | MB_ICONERROR);
    };
    if (exe.empty() || GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        reportError(L"P4V executable or patch DLL not found: " + exe + L" / " + dll);
        return 2;
    }

    std::wstring command = quote(exe.c_str());
    for (int i = 2; i < argc; ++i) { command += L" "; command += quote(argv[i]); }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION proc{};
    const auto cwd = directory(exe);
    if (!CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, cwd.c_str(), &startup, &proc)) {
        reportError(L"Unable to start P4V (Win32 error " +
                    std::to_wstring(GetLastError()) + L")");
        return 1;
    }
    CloseHandle(proc.hThread);
    bool ready = false;
    for (int i = 0; i < 300; ++i) {
        if (moduleBase(proc.dwProcessId, L"Qt6Widgets.dll")) { ready = true; break; }
        if (WaitForSingleObject(proc.hProcess, 0) == WAIT_OBJECT_0) break;
        Sleep(100);
    }
    if (!ready || !inject(proc.hProcess, proc.dwProcessId, dll)) {
        reportError(L"Patch injection failed; P4V is running unmodified.");
        CloseHandle(proc.hProcess);
        return 1;
    }
    std::wcout << L"Patch injected into P4V (PID " << proc.dwProcessId << L").\n";
    CloseHandle(proc.hProcess);
    return 0;
}
