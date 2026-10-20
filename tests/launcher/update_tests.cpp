#include "launcher_update.h"
#include "version.h"
#include <windows.h>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <iostream>

using namespace launcher;
namespace {
int failures = 0;
void check(bool ok, const char* name) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    if (!ok) ++failures;
}
QByteArray read(const QString& path) { QFile file(path); file.open(QIODevice::ReadOnly); return file.readAll(); }
void write(const QString& path, const QByteArray& bytes) { QFile file(path); file.open(QIODevice::WriteOnly); file.write(bytes); }
}
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().size() != 3) return 2;
    check(validVersion("0.1.0") && validVersion("65535.65535.65535"), "stable versions");
    for (const auto* invalid : {"v1.2.3", "1.2", "01.2.3", "1.2.3-rc1", "1.2.3+build", "65536.0.0", "-1.0.0", "1.2.3.4"})
        check(!validVersion(invalid), invalid);
    check(newerVersion("0.10.0", "0.9.9") && !newerVersion("1.0.0", "1.0.0") && !newerVersion("0.9.0", "1.0.0"), "numeric comparison / no downgrade");
    const QString version = QStringLiteral(P4VPATCH_VERSION);
    const QString base = "https://github.com/" + repository() + "/releases/";
    QJsonObject releaseJson{{"tag_name", "v" + version}, {"draft", false}, {"prerelease", false},
        {"html_url", base + "tag/v" + version}, {"body", "<b>plain text release notes</b>"},
        {"assets", QJsonArray{QJsonObject{{"name", "p4vpatch-update.json"}, {"browser_download_url", base + "download/v" + version + "/p4vpatch-update.json"}}}}};
    Release release;
    QString error;
    check(parseRelease(QJsonDocument(releaseJson).toJson(), release, error) && !release.manifest.isEmpty(), "GitHub release parse");
    auto badRelease = releaseJson;
    badRelease["prerelease"] = true;
    check(!parseRelease(QJsonDocument(badRelease).toJson(), release, error), "reject prerelease");
    badRelease = releaseJson;
    badRelease["html_url"] = "https://example.org/evil";
    check(!parseRelease(QJsonDocument(badRelease).toJson(), release, error), "reject foreign release URL");
    badRelease = releaseJson;
    badRelease["assets"] = QJsonArray();
    check(parseRelease(QJsonDocument(badRelease).toJson(), release, error) && release.manifest.isEmpty(), "announce release without manifest");
    parseRelease(QJsonDocument(releaseJson).toJson(), release, error);

    const QByteArray bytes = read(app.arguments()[1]);
    check(!bytes.isEmpty() && fileVersion(app.arguments()[1]) == version, "read version without LoadLibrary");
    QJsonObject manifestJson{{"schema", 1}, {"version", version}, {"minimum_launcher_version", version},
        {"qt_version", "6.8.6"}, {"p4v_version", "2026.2"},
        {"dll", QJsonObject{{"url", base + "download/v" + version + "/p4vpatch.dll"},
          {"size", bytes.size()}, {"sha256", QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())}}}};
    Manifest manifest;
    check(parseManifest(QJsonDocument(manifestJson).toJson(), release, manifest, error), "manifest parse");
    for (const auto* key : {"version", "minimum_launcher_version", "qt_version"}) {
        auto bad = manifestJson;
        bad[key] = "bad";
        check(!parseManifest(QJsonDocument(bad).toJson(), release, manifest, error), key);
    }
    auto bad = manifestJson;
    auto dll = bad["dll"].toObject();
    dll["url"] = "https://example.org/p4vpatch.dll";
    bad["dll"] = dll;
    check(!parseManifest(QJsonDocument(bad).toJson(), release, manifest, error), "reject arbitrary download URL");
    dll = manifestJson["dll"].toObject();
    dll["size"] = MaxPatchBytes + 1;
    bad["dll"] = dll;
    check(!parseManifest(QJsonDocument(bad).toJson(), release, manifest, error), "reject oversized DLL");
    dll["size"] = 1.5;
    bad["dll"] = dll;
    check(!parseManifest(QJsonDocument(bad).toJson(), release, manifest, error), "reject fractional size");
    parseManifest(QJsonDocument(manifestJson).toJson(), release, manifest, error);
    check(validatePatch(bytes, manifest, error), "validate x64 fixture");
    auto corrupt = bytes;
    corrupt[0] ^= 1;
    check(!validatePatch(corrupt, manifest, error), "reject checksum mismatch");
    auto textManifest = manifest;
    textManifest.size = 4;
    textManifest.sha256 = QCryptographicHash::hash("oops", QCryptographicHash::Sha256).toHex();
    check(!validatePatch("oops", textManifest, error), "reject non-PE even with matching hash");

    error.clear();
    if (p4vIsRunning(error)) {
        std::cerr << "Close P4V before running installer tests: " << error.toStdString() << '\n';
        return 2;
    }
    QTemporaryDir temp;
    check(temp.isValid(), "temporary install directory");
    const QString host = temp.filePath("external-p4v");
    QDir().mkpath(host);
    const auto series = version.section('.', 0, 1);
    check(!compatibleP4v(host + "/p4v.exe", series, version, error), "missing target P4V rejected");
    QFile::copy(app.arguments()[1], host + "/p4v.exe");
    check(!compatibleP4v(host + "/p4v.exe", series, version, error), "missing target Qt rejected");
    for (const auto* name : {"Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll"}) QFile::copy(app.arguments()[1], host + '/' + name);
    check(compatibleP4v(host + "/p4v.exe", series, version, error), "external target with matching resources accepted");
    check(!compatibleP4v(host + "/p4v.exe", series, "99.0.0", error), "mismatched target Qt rejected");
    check(!compatibleP4v(host + "/p4v.exe", "9999.0", version, error), "mismatched target P4V rejected");
    const QString target = temp.filePath("p4vpatch.dll");
    const QByteArray previous("old patch sentinel");
    write(target, previous);
    check(!installPatch(target, corrupt, manifest, error) && read(target) == previous, "corrupt update preserves original");
    auto wrongVersion = manifest;
    wrongVersion.version = "99.0.0";
    check(!installPatch(target, bytes, wrongVersion, error) && read(target) == previous, "resource version mismatch preserves original");
    const auto native = QDir::toNativeSeparators(target).toStdWString();
    HANDLE locked = CreateFileW(native.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "lock destination");
    check(!installPatch(target, bytes, manifest, error), "locked destination rejected");
    CloseHandle(locked);
    check(read(target) == previous, "locked update preserves original");
    const bool replaced = installPatch(target, bytes, manifest, error);
    if (!replaced) std::cerr << error.toStdString() << '\n';
    check(replaced, "replace patch");
    check(read(target) == bytes && read(target + ".bak") == previous, "new patch and previous backup");
    QFile::remove(target);
    const bool installed = installPatch(target, bytes, manifest, error);
    if (!installed) std::cerr << error.toStdString() << '\n';
    check(installed && read(target) == bytes, "initial installation");
    check(!installPatch(temp.filePath("missing/patch.dll"), bytes, manifest, error), "unwritable/missing staging directory rejected");
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    const auto fakeExe = QDir::toNativeSeparators(app.arguments()[2]).toStdWString();
    const bool started = CreateProcessW(fakeExe.c_str(), nullptr, nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                                        nullptr, nullptr, &startup, &process);
    check(started, "start test-owned dummy p4v process");
    if (started) {
        check(p4vIsRunning(error), "detect running P4V by process name");
        check(!installPatch(target, bytes, manifest, error) && read(target) == bytes, "running P4V blocks replacement");
        // Only terminate the mock process whose handle we just created, never a real P4V.
        TerminateProcess(process.hProcess, 0);
        WaitForSingleObject(process.hProcess, 5000);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    std::cout << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
