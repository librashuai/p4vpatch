#pragma once
#include <QByteArray>
#include <QString>
#include <QUrl>

namespace launcher {
constexpr qint64 MaxPatchBytes = 64 * 1024 * 1024;
struct Release {
    QString version;
    QString notes;
    QUrl page;
    QUrl manifest;
};
struct Manifest {
    QString version;
    QString minimumLauncher;
    QString qtVersion;
    QString p4vVersion;
    QUrl dllUrl;
    QByteArray sha256;
    qint64 size = 0;
};
// Stable releases only: exactly three numeric components, no prerelease/build suffix.
bool validVersion(const QString& version);
bool newerVersion(const QString& candidate, const QString& installed);
QString fileVersion(const QString& path, bool product = false);
bool compatibleP4v(const QString& executable, const QString& p4vVersion, const QString& qtVersion, QString& error);
QString repository();
QUrl latestReleaseUrl();
bool parseRelease(const QByteArray& json, Release& release, QString& error);
bool parseManifest(const QByteArray& json, const Release& release, Manifest& manifest, QString& error);
bool p4vIsRunning(QString& error); // Also returns true if enumeration fails (fail closed).
bool validatePatch(const QByteArray& bytes, const Manifest& manifest, QString& error);
bool installPatch(const QString& target, const QByteArray& bytes, const Manifest& manifest, QString& error);
}
