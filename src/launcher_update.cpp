#include "launcher_update.h"
#include "version.h"
#include <windows.h>
#include <tlhelp32.h>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QVersionNumber>
#include <cstring>

namespace launcher {
QString repository() { return QStringLiteral(P4VPATCH_REPOSITORY); }
QUrl latestReleaseUrl() { return QUrl("https://api.github.com/repos/" + repository() + "/releases/latest"); }

bool validVersion(const QString& version) {
    static const QRegularExpression pattern(QStringLiteral("^(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})\\.(0|[1-9][0-9]{0,4})$"));
    if (!pattern.match(version).hasMatch()) return false;
    for (const auto& part : version.split('.')) if (part.toInt() > 65535) return false;
    return true;
}
bool newerVersion(const QString& candidate, const QString& installed) {
    return validVersion(candidate) && validVersion(installed) &&
           QVersionNumber::compare(QVersionNumber::fromString(candidate), QVersionNumber::fromString(installed)) > 0;
}

QString fileVersion(const QString& path, bool product) {
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    DWORD unused = 0;
    const DWORD size = GetFileVersionInfoSizeW(native.c_str(), &unused);
    if (!size || size > 1024 * 1024) return {};
    QByteArray data(size, '\0');
    if (!GetFileVersionInfoW(native.c_str(), 0, size, data.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) ||
        length < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != 0xfeef04bd) return {};
    const DWORD ms = product ? info->dwProductVersionMS : info->dwFileVersionMS;
    const DWORD ls = product ? info->dwProductVersionLS : info->dwFileVersionLS;
    return QStringLiteral("%1.%2.%3").arg(HIWORD(ms)).arg(LOWORD(ms)).arg(HIWORD(ls));
}

bool compatibleP4v(const QString& executable, const QString& p4vVersion, const QString& qtVersion, QString& error) {
    const QFileInfo exe(executable);
    if (!exe.isFile() || exe.fileName().compare("p4v.exe", Qt::CaseInsensitive) != 0) {
        error = QStringLiteral("Please select an existing p4v.exe in the welcome page.");
        return false;
    }
    if (!fileVersion(executable, true).startsWith(p4vVersion + '.')) {
        error = QStringLiteral("Selected P4V is not compatible (requires P4V %1).").arg(p4vVersion);
        return false;
    }
    for (const auto* name : {"Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll"}) {
        if (fileVersion(exe.absolutePath() + '/' + name) != qtVersion) {
            error = QStringLiteral("Selected P4V must contain %1 version %2. Do not mix Qt installations.").arg(QString::fromLatin1(name), qtVersion);
            return false;
        }
    }
    return true;
}

static QUrl assetUrl(const QString& version, const QString& name) {
    return QUrl("https://github.com/" + repository() + "/releases/download/v" + version + "/" + name);
}

bool parseRelease(const QByteArray& json, Release& release, QString& error) {
    const auto doc = QJsonDocument::fromJson(json);
    const auto obj = doc.object();
    const QString tag = obj.value("tag_name").toString();
    const QString version = tag.startsWith('v') ? tag.mid(1) : QString();
    if (!doc.isObject() || !validVersion(version) ||
        !obj.contains("draft") || !obj.contains("prerelease") ||
        obj.value("draft").toBool(true) || obj.value("prerelease").toBool(true)) {
        error = QStringLiteral("GitHub did not return a supported stable release (vMAJOR.MINOR.PATCH).");
        return false;
    }
    const QUrl expectedPage("https://github.com/" + repository() + "/releases/tag/" + tag);
    if (QUrl(obj.value("html_url").toString()) != expectedPage) {
        error = QStringLiteral("Release URL does not belong to the configured repository.");
        return false;
    }
    const auto expected = assetUrl(version, "p4vpatch-update.json");
    bool found = false;
    for (const auto& entry : obj.value("assets").toArray()) {
        const auto asset = entry.toObject();
        if (asset.value("name").toString() == "p4vpatch-update.json" &&
            QUrl(asset.value("browser_download_url").toString()) == expected) found = true;
    }
    release = {version, obj.value("body").toString().left(24000), expectedPage, found ? expected : QUrl()};
    // A release without a manifest must still be announced; offer the release page instead.
    return true;
}

bool parseManifest(const QByteArray& json, const Release& release, Manifest& manifest, QString& error) {
    const auto doc = QJsonDocument::fromJson(json);
    const auto obj = doc.object();
    Manifest parsed;
    parsed.version = obj.value("version").toString();
    parsed.minimumLauncher = obj.value("minimum_launcher_version").toString();
    parsed.qtVersion = obj.value("qt_version").toString();
    parsed.p4vVersion = obj.value("p4v_version").toString();
    const auto dll = obj.value("dll").toObject();
    parsed.dllUrl = QUrl(dll.value("url").toString());
    parsed.sha256 = dll.value("sha256").toString().toLatin1();
    const double size = dll.value("size").toDouble(-1);
    static const QRegularExpression hash(QStringLiteral("^[0-9a-f]{64}$"));
    if (!doc.isObject() || obj.value("schema").toInt() != 1 ||
        !validVersion(parsed.version) || parsed.version != release.version ||
        !validVersion(parsed.minimumLauncher) || !validVersion(parsed.qtVersion) ||
        parsed.p4vVersion.isEmpty() || parsed.dllUrl != assetUrl(parsed.version, "p4vpatch.dll") ||
        !hash.match(QString::fromLatin1(parsed.sha256)).hasMatch() ||
        size < 1 || size > MaxPatchBytes || size != static_cast<qint64>(size)) {
        error = QStringLiteral("Invalid or unsupported update manifest; no files were changed.");
        return false;
    }
    parsed.size = static_cast<qint64>(size);
    manifest = parsed;
    return true;
}

bool p4vIsRunning(QString& error) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        error = QStringLiteral("Cannot enumerate processes; update cancelled for safety.");
        return true;
    }
    PROCESSENTRY32W entry{sizeof(entry)};
    bool busy = false;
    if (!Process32FirstW(snapshot, &entry)) {
        error = QStringLiteral("Cannot read process list; operation cancelled for safety.");
        busy = true;
    } else {
        do {
            if (_wcsicmp(entry.szExeFile, L"p4v.exe") == 0) {
                error = QStringLiteral("检测到 P4V 进程（PID %1）。请退出所有 P4V 窗口及 P4VC 后台服务后再更新补丁；关闭窗口后后台服务仍可能运行。补丁不会自动终止进程。")
                    .arg(entry.th32ProcessID);
                busy = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry));
        if (!busy && GetLastError() != ERROR_NO_MORE_FILES) {
            error = QStringLiteral("Process enumeration was incomplete; operation cancelled for safety.");
            busy = true;
        }
    }
    CloseHandle(snapshot);
    return busy;
}

bool validatePatch(const QByteArray& bytes, const Manifest& manifest, QString& error) {
    if (bytes.size() != manifest.size || bytes.size() > MaxPatchBytes ||
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != manifest.sha256) {
        error = QStringLiteral("Patch size/SHA-256 verification failed. The existing patch was not changed.");
        return false;
    }
    IMAGE_DOS_HEADER dos{};
    if (bytes.size() >= sizeof(dos)) std::memcpy(&dos, bytes.constData(), sizeof(dos));
    IMAGE_NT_HEADERS64 nt{};
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < sizeof(dos) ||
        qint64(dos.e_lfanew) + sizeof(nt) > bytes.size()) {
        error = QStringLiteral("Patch is not a valid PE file.");
        return false;
    }
    std::memcpy(&nt, bytes.constData() + dos.e_lfanew, sizeof(nt));
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        !(nt.FileHeader.Characteristics & IMAGE_FILE_DLL) || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        error = QStringLiteral("Patch must be a Windows x64 DLL.");
        return false;
    }
    return true;
}

bool installPatch(const QString& target, const QByteArray& bytes, const Manifest& manifest, QString& error) {
    if (!validatePatch(bytes, manifest, error) || p4vIsRunning(error)) return false;
    // QTemporaryFile can retain its native handle after close() on Windows,
    // preventing the version-resource reader from opening it. Use a real QFile.
    QTemporaryDir staging(QFileInfo(target).absolutePath() + "/p4vpatch-XXXXXX");
    QFile staged(staging.path() + "/p4vpatch.dll");
    if (!staging.isValid() || !staged.open(QIODevice::WriteOnly | QIODevice::NewOnly) ||
        staged.write(bytes) != bytes.size() || !staged.flush()) {
        error = QStringLiteral("Cannot stage update in the P4V directory. Check write permissions; the launcher does not auto-elevate.");
        return false;
    }
    staged.close();
    if (fileVersion(staged.fileName()) != manifest.version) {
        error = QStringLiteral("Downloaded DLL version does not match the release manifest.");
        return false;
    }
    // Recheck immediately before replacing; the exclusive handle also rejects a locked DLL.
    if (p4vIsRunning(error)) return false;
    const auto native = QDir::toNativeSeparators(target).toStdWString();
    const auto source = QDir::toNativeSeparators(staged.fileName()).toStdWString();
    if (QFile::exists(target)) {
        HANDLE exclusive = CreateFileW(native.c_str(), GENERIC_READ | DELETE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (exclusive == INVALID_HANDLE_VALUE) {
            error = QStringLiteral("Patch is in use or not writable (Win32 error %1). Close P4V and check permissions.").arg(GetLastError());
            return false;
        }
        CloseHandle(exclusive);
        const auto backup = native + L".bak";
        if (!CopyFileW(native.c_str(), backup.c_str(), FALSE)) {
            error = QStringLiteral("Cannot save the previous patch as p4vpatch.dll.bak (Win32 error %1).").arg(GetLastError());
            return false;
        }
        if (!ReplaceFileW(native.c_str(), source.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
            const DWORD code = GetLastError();
            // ReplaceFile can fail after removing a destination on some filesystems.
            if (!QFile::exists(target)) CopyFileW(backup.c_str(), native.c_str(), TRUE);
            error = QStringLiteral("Cannot replace patch (Win32 error %1). Previous version is in p4vpatch.dll.bak; restore it manually if needed.").arg(code);
            return false;
        }
    } else if (!MoveFileExW(source.c_str(), native.c_str(), MOVEFILE_WRITE_THROUGH)) {
        error = QStringLiteral("Cannot install patch (Win32 error %1).").arg(GetLastError());
        return false;
    }
    return true;
}
} // namespace launcher
