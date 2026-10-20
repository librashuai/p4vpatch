#include <windows.h>
#include <tlhelp32.h>

#include <cwchar>
#include "launcher_process.h"
#include <QDir>
#include <QFileInfo>
#include <string>
#include <vector>

namespace {
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

namespace launcher {
QProcessEnvironment p4vEnvironment(const QString& directory) {
    auto environment = QProcessEnvironment::systemEnvironment();
    // The launcher may use copied Qt plugins in a build directory. P4V must
    // instead use the plugins from its own complete installation.
    environment.insert("QT_PLUGIN_PATH", QDir::toNativeSeparators(directory + "/plugins"));
    environment.insert("QT_QPA_PLATFORM_PLUGIN_PATH", QDir::toNativeSeparators(directory + "/plugins/platforms"));
    return environment;
}

LaunchResult launchP4v(const QString& executable, const QString& library, const QStringList& arguments) {
    const auto exe = executable.toStdWString();
    const auto dll = library.toStdWString();
    std::wstring command = quote(exe.c_str());
    for (const auto& argument : arguments) { command += L" "; command += quote(argument.toStdWString().c_str()); }
    std::vector<wchar_t> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION proc{};
    const auto directory = QFileInfo(executable).absolutePath();
    const auto cwd = QDir::toNativeSeparators(directory).toStdWString();
    auto entries = p4vEnvironment(directory).toStringList();
    entries.sort(Qt::CaseInsensitive);
    std::vector<wchar_t> environment;
    for (const auto& entry : entries) {
        const auto native = entry.toStdWString();
        environment.insert(environment.end(), native.begin(), native.end());
        environment.push_back(L'\0');
    }
    environment.push_back(L'\0');
    if (!CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT,
                        environment.data(), cwd.c_str(), &startup, &proc)) {
        return {false, QStringLiteral("Unable to start P4V (Win32 error %1)").arg(GetLastError())};
    }
    CloseHandle(proc.hThread);
    bool ready = false;
    for (int i = 0; i < 300; ++i) {
        if (moduleBase(proc.dwProcessId, L"Qt6Widgets.dll")) { ready = true; break; }
        if (WaitForSingleObject(proc.hProcess, 0) == WAIT_OBJECT_0) break;
        Sleep(100);
    }
    const bool ok = ready && inject(proc.hProcess, proc.dwProcessId, dll);
    CloseHandle(proc.hProcess);
    return {ok, ok ? QStringLiteral("Patch loaded. This does not confirm patch initialization.")
                   : QStringLiteral("Patch injection failed; P4V may still be running unmodified.")};
}
} // namespace launcher
