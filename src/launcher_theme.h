#pragma once
#include <QFont>
#include <QString>

class QWidget;
namespace launcher {
struct P4vAppearance {
    bool dark = false;
    QFont font = QFont(QStringLiteral("Microsoft YaHei UI"), 9);
};
P4vAppearance readP4vAppearance(const QString& settingsFile);

// Loads installed P4V skins/resources without copying or redistributing them.
class LauncherTheme {
public:
    LauncherTheme() = default;
    LauncherTheme(const LauncherTheme&) = delete;
    LauncherTheme& operator=(const LauncherTheme&) = delete;
    ~LauncherTheme();
    void apply(QWidget& window, const QString& executable, const QString& settingsFile = {});
    bool loadedP4vSkin() const { return loadedSkin_; }
private:
    QString resource_;
    bool loadedSkin_ = false;
};
}
