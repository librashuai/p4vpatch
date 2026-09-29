#include "qt_focus_hook.h"

#include <windows.h>
#include <QApplication>
#include <QTabWidget>
#include <QWidget>
#include <atomic>
#include <cstring>

namespace {
using Activate = void (*)(QObject*, const QMetaObject*, int, void**);
Activate originalActivate = nullptr;
bool installed = false;
int focusSignal = -1;
std::atomic<int> skipped{0};
std::atomic<int> attachedPages{0};

bool isTerminalWidget(QWidget* widget) {
    while (widget) {
        if (widget->objectName() == QStringLiteral("p4vpatch.nativeProbePage")) return true;
        widget = widget->parentWidget();
    }
    return false;
}

void filteredActivate(QObject* sender, const QMetaObject* meta, int signal, void** args) {
    if (sender == qApp && meta == &QApplication::staticMetaObject &&
        signal == focusSignal && args && args[1] && args[2] &&
        (attachedPages.load() > 0 ||
         isTerminalWidget(*static_cast<QWidget**>(args[1])) ||
         isTerminalWidget(*static_cast<QWidget**>(args[2])))) {
        ++skipped;
        return;
    }
    originalActivate(sender, meta, signal, args);
}
}

int terminalFocusSkips() { return skipped.load(); }
void setTerminalFocusFiltering(bool attached) {
    if (attached) ++attachedPages;
    else --attachedPages;
}

bool installTerminalFocusHook() {
    if (installed) return true;
    if (!qApp || QString::fromLatin1(qVersion()) != QStringLiteral("6.8.6")) return false;
    focusSignal = QApplication::staticMetaObject.indexOfSignal("focusChanged(QWidget*,QWidget*)") -
                  QApplication::staticMetaObject.methodOffset();
    if (focusSignal < 0) return false;

    auto* module = reinterpret_cast<unsigned char*>(GetModuleHandleW(L"Qt6Widgets.dll"));
    auto* core = GetModuleHandleW(L"Qt6Core.dll");
    if (!module || !core) return false;
    constexpr char symbol[] = "?activate@QMetaObject@@SAXPEAVQObject@@PEBU1@HPEAPEAX@Z";
    auto* expected = reinterpret_cast<void*>(GetProcAddress(core, symbol));
    if (!expected) return false;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096) return false;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;
    const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size || dir.VirtualAddress >= nt->OptionalHeader.SizeOfImage) return false;
    const auto imageSize = nt->OptionalHeader.SizeOfImage;
    auto* imports = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(module + dir.VirtualAddress);
    for (unsigned n = 0; (n + 1) * sizeof(IMAGE_IMPORT_DESCRIPTOR) <= dir.Size && imports[n].Name; ++n) {
        if (imports[n].Name >= imageSize || _stricmp(reinterpret_cast<const char*>(module + imports[n].Name),
                                                     "Qt6Core.dll") != 0) continue;
        if (!imports[n].OriginalFirstThunk || !imports[n].FirstThunk ||
            imports[n].OriginalFirstThunk >= imageSize || imports[n].FirstThunk >= imageSize) return false;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(module + imports[n].OriginalFirstThunk);
        auto* addresses = reinterpret_cast<IMAGE_THUNK_DATA64*>(module + imports[n].FirstThunk);
        const auto maxEntries = (imageSize - imports[n].FirstThunk) / sizeof(IMAGE_THUNK_DATA64);
        for (unsigned i = 0; i < maxEntries && names[i].u1.AddressOfData; ++i) {
            if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal) ||
                names[i].u1.AddressOfData >= imageSize) continue;
            const auto* name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(module + names[i].u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(name->Name), symbol) != 0) continue;
            auto* slot = reinterpret_cast<void* volatile*>(&addresses[i].u1.Function);
            if (*slot != expected) return false;
            DWORD protection;
            if (!VirtualProtect(const_cast<void**>(slot), sizeof(void*), PAGE_READWRITE, &protection)) return false;
            originalActivate = reinterpret_cast<Activate>(*slot);
            InterlockedExchangePointer(slot, reinterpret_cast<void*>(&filteredActivate));
            DWORD ignored;
            VirtualProtect(const_cast<void**>(slot), sizeof(void*), protection, &ignored);
            installed = true;
            return true;
        }
    }
    return false;
}
