#pragma once
#include <QString>
#include <QStringList>
#include <QProcessEnvironment>

namespace launcher {
struct LaunchResult {
    bool ok = false;
    QString message;
};
QProcessEnvironment p4vEnvironment(const QString& directory);
LaunchResult launchP4v(const QString& exe, const QString& dll, const QStringList& arguments);
}
