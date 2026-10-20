// Exercise the real welcome/auto-start state machine with an in-memory GitHub transport.
#define P4VPATCH_LAUNCHER_TESTING
#include "../../src/launcher.cpp"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QThread>
#include <atomic>
#include <iostream>

namespace {
int failures = 0;
void check(bool ok, const char* name) {
    std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
    if (!ok) ++failures;
}
QPushButton* button(QWidget& window, const QString& text) {
    for (auto* b : window.findChildren<QPushButton*>()) if (b->text() == text) return b;
    return nullptr;
}
QString labels(QWidget& window) {
    QString result;
    for (auto* label : window.findChildren<QLabel*>()) result += label->text() + '\n';
    return result;
}
void events() { for (int i = 0; i < 10; ++i) QApplication::processEvents(); }
bool errorColors(QWidget& window, bool expected) {
    const QColor red(window.property("p4vDarkTheme").toBool() ? "#FF7B72" : "#C62828");
    for (const auto* name : {"statusTitle", "statusSummary", "statusDiagnostic"}) {
        auto* label = window.findChild<QLabel*>(QString::fromLatin1(name));
        if (!label || label->property("error").toBool() != expected ||
            (label->palette().color(QPalette::WindowText) == red) != expected) return false;
    }
    return true;
}
bool waitFor(const std::function<bool()>& condition) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 3000) { events(); QThread::msleep(1); }
    return condition();
}
QList<QRect> geometries(std::initializer_list<QWidget*> widgets) {
    QList<QRect> result;
    for (auto* widget : widgets) result.append(QRect(widget->mapToGlobal(QPoint()), widget->size()));
    return result;
}
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/launcher/icon.ico")));
    app.setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName("p4vpatch-tests");
    QCoreApplication::setApplicationName("welcome-tests");
    QStandardPaths::setTestModeEnabled(true);
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/launcher.ini", QSettings::IniFormat);
    settings.clear();
    settings.setValue("skipWelcome", true);
    settings.sync();
    QTemporaryDir temp;
    const QString exe = temp.filePath("p4v.exe"); // Never launched; no real P4V or DLL is required.
    const QString base = "https://github.com/" + repository() + "/releases/";
    QJsonObject release{{"tag_name", "v9.0.0"}, {"draft", false}, {"prerelease", false},
        {"html_url", base + "tag/v9.0.0"}, {"body", "<h1>untrusted notes</h1>"},
        {"assets", QJsonArray{QJsonObject{{"name", "p4vpatch-update.json"}, {"browser_download_url", base + "download/v9.0.0/p4vpatch-update.json"}}}}};
    int downloads = 0;
    bool failRequest = false;
    auto transport = [&](const QUrl& url, qint64, WelcomeWindow::FetchCallback callback) {
        if (failRequest) { callback({}, "offline"); return; }
        if (url == latestReleaseUrl()) callback(QJsonDocument(release).toJson(), {});
        else if (url.path().endsWith("p4vpatch-update.json")) {
            QJsonObject manifest{{"schema", 1}, {"version", "9.0.0"}, {"minimum_launcher_version", "9.0.0"},
                {"p4v_version", "2026.2"}, {"qt_version", "6.8.6"},
                {"dll", QJsonObject{{"url", base + "download/v9.0.0/p4vpatch.dll"}, {"size", 100}, {"sha256", QString(64, 'a')}}}};
            callback(QJsonDocument(manifest).toJson(), {});
        } else { ++downloads; callback({}, "Unexpected binary download"); }
    };
    {
        WelcomeWindow window(exe, {}, false, transport);
        check(!window.isVisible(), "skip-welcome starts hidden while checking");
        events();
        check(window.isVisible(), "new release overrides skip-welcome");
        check(labels(window).contains(QStringLiteral("需要 Launcher")), "launcher upgrade shown");
        check(button(window, QStringLiteral("打开 Release"))->isEnabled(), "manual release download available");
        check(!button(window, QStringLiteral("更新补丁"))->isEnabled(), "incompatible launcher cannot install");
        check(window.findChild<QTextEdit*>()->toPlainText() == "<h1>untrusted notes</h1>", "notes displayed as plain text");
        check(downloads == 0, "no binary download without user consent");
        failRequest = true;
        button(window, QStringLiteral("检查更新"))->click();
        events();
        check(window.isVisible() && button(window, QStringLiteral("启动 P4V"))->isEnabled(), "network failure allows manual launch");
        check(downloads == 0, "retry does not install");
    }
    {
        WelcomeWindow window(exe, {}, false, transport); // transport is still offline
        events();
        check(window.isVisible() && labels(window).contains("9.0.0"), "cached new release still interrupts auto-start when offline");
    }
    failRequest = false;
    if (app.arguments().size() != 3 || !QFile::copy(app.arguments()[1], exe)) return 2;
    for (const auto* name : {L"Qt6Core.dll", L"Qt6Gui.dll", L"Qt6Widgets.dll"}) {
        wchar_t path[32768]{};
        if (!GetModuleFileNameW(GetModuleHandleW(name), path, 32768) ||
            !QFile::copy(QString::fromWCharArray(path), temp.filePath(QString::fromWCharArray(name)))) return 2;
    }
    release["body"] = QStringLiteral("更顺畅的启动体验\n\n• 欢迎页跟随 P4V 主题\n• 优化补丁更新与启动流程");
    // Cross-directory targets must never change which patch is shown or updated.
    QTemporaryDir patchHome;
    const auto patch = patchHome.filePath("p4vpatch.dll");
    if (!QFile::copy(app.arguments()[1], patch)) return 2;
    const auto environment = p4vEnvironment(temp.path());
    check(environment.value("QT_PLUGIN_PATH") == QDir::toNativeSeparators(temp.path() + "/plugins") &&
          environment.value("QT_QPA_PLATFORM_PLUGIN_PATH") == QDir::toNativeSeparators(temp.path() + "/plugins/platforms"),
          "child P4V uses target installation plugins, not launcher plugins");
    {
        WelcomeWindow window(exe, {}, false, [&](const QUrl& url, qint64, WelcomeWindow::FetchCallback callback) {
            if (url == latestReleaseUrl()) callback(QJsonDocument(release).toJson(), {});
            else {
                const auto parts = fileVersion(exe, true).split('.');
                QJsonObject manifest{{"schema", 1}, {"version", "9.0.0"}, {"minimum_launcher_version", QStringLiteral(P4VPATCH_VERSION)},
                    {"p4v_version", parts[0] + '.' + parts[1]}, {"qt_version", QString::fromLatin1(qVersion())},
                    {"dll", QJsonObject{{"url", base + "download/v9.0.0/p4vpatch.dll"}, {"size", 100}, {"sha256", QString(64, 'a')}}}};
                callback(QJsonDocument(manifest).toJson(), {});
            }
        });
        events();
        check(window.isVisible() && button(window, QStringLiteral("更新补丁"))->isEnabled(), "compatible update offered despite skip-welcome");
        check(labels(window).contains(QDir::toNativeSeparators(QCoreApplication::applicationDirPath() + "/p4vpatch.dll")), "default patch remains beside launcher for external P4V");
        check(downloads == 0 && !QFile::exists(temp.filePath("p4vpatch.dll")), "compatible update is never automatically installed");
        check(window.findChild<QTextEdit*>()->isVisible(), "new release automatically reveals release notes");
        const auto snapshots = qEnvironmentVariable("P4VPATCH_UI_SNAPSHOT_DIR");
        if (!snapshots.isEmpty()) {
            QDir().mkpath(snapshots);
            window.grab().save(QDir(snapshots).filePath("launcher-update.png"));
        }
    }
    release["assets"] = QJsonArray();
    {
        WelcomeWindow window(exe, {}, false, transport);
        events();
        check(window.isVisible() && labels(window).contains(QStringLiteral("无更新清单")), "release without manifest still interrupts auto-start");
    }
    release["assets"] = QJsonArray{QJsonObject{{"name", "p4vpatch-update.json"}, {"browser_download_url", base + "download/v9.0.0/p4vpatch-update.json"}}};
    {
        WelcomeWindow window(exe, {}, false, [&](const QUrl& url, qint64, WelcomeWindow::FetchCallback callback) {
            if (url == latestReleaseUrl()) callback(QJsonDocument(release).toJson(), {});
            else callback("not json", {});
        });
        events();
        check(window.isVisible() && labels(window).contains(QStringLiteral("无法读取更新清单")), "bad manifest still announces new release");
    }
    {
        WelcomeWindow window(exe, {}, true, [](const QUrl&, qint64, WelcomeWindow::FetchCallback callback) { callback({}, "offline"); });
        events();
        check(window.isVisible() && labels(window).contains("offline"), "forced welcome remains visible offline");
    }
    {
        WelcomeWindow window(exe, {}, true, [](const QUrl&, qint64, WelcomeWindow::FetchCallback callback) { callback({}, "offline"); }, patch);
        events();
        check(labels(window).contains(QDir::toNativeSeparators(patch)) && labels(window).contains(QStringLiteral("补丁：" P4VPATCH_VERSION)),
              "external P4V does not redirect local patch version/path");
    }
    {
        WelcomeWindow window(temp.filePath("missing/p4v.exe"), {}, true,
            [](const QUrl&, qint64, WelcomeWindow::FetchCallback callback) { callback({}, "offline"); });
        events();
        check(window.isVisible() && button(window, QStringLiteral("选择 P4V…"))->isEnabled(), "missing P4V still allows welcome UI and target selection");
    }
    settings.clear();
    {
        WelcomeWindow::FetchCallback pending;
        WelcomeWindow window(exe, {}, true, [&](const QUrl&, qint64, WelcomeWindow::FetchCallback callback) {
            pending = std::move(callback);
        }, patch);
        window.move(100, 80);
        events();
        auto* details = window.findChild<QWidget*>("launcherDetails");
        auto* toggle = window.findChild<QToolButton*>("detailsToggle");
        auto* activity = window.findChild<QProgressBar*>("activity");
        auto topGeometry = [&] {
            return geometries({window.findChild<QLabel*>("appTitle"), window.findChild<QLabel*>("versionBadge"),
                window.findChild<QFrame*>("statusCard"), window.findChild<QCheckBox*>(), toggle});
        };
        auto pageGeometry = [&] {
            auto result = topGeometry();
            result.append(geometries({window.findChild<QLabel*>("statusTitle"), window.findChild<QLabel*>("statusSummary"),
                window.findChild<QLabel*>("targetCaption"), button(window, QStringLiteral("选择 P4V…")),
                button(window, QStringLiteral("检查更新")), button(window, QStringLiteral("启动 P4V"))}));
            result.append(window.geometry());
            return result;
        };
        check(activity->isVisible() && bool(pending), "progress visible during deferred update check");
        const auto checkingGeometry = pageGeometry();
        pending({}, "https://api.github.com/very/long/request failed (HTTP 403)");
        events();
        check(activity->isHidden() && pageGeometry() == checkingGeometry, "hiding progress preserves window and surrounding layout");
        check(errorColors(window, true), "network error title, summary and diagnostic are red");
        const auto idleGeometry = pageGeometry();
        button(window, QStringLiteral("检查更新"))->click();
        events();
        check(activity->isVisible() && pageGeometry() == idleGeometry, "showing progress preserves window and surrounding layout");
        check(errorColors(window, false), "retry clears previous error coloring");
        pending({}, "https://api.github.com/very/long/request failed (HTTP 403)");
        events();
        check(details->isHidden() && !window.findChild<QTextEdit*>()->isVisible(), "default page collapses diagnostics and empty release notes");
        check(!window.findChild<QLabel*>("statusSummary")->text().contains("https://"), "network diagnostics do not clutter the main page");
        check(button(window, QStringLiteral("更新补丁"))->isHidden() && button(window, QStringLiteral("打开 Release"))->isHidden(),
              "irrelevant release actions hidden without an update");
        for (const bool enlarged : {false, true}) {
            if (enlarged) {
                window.resize(window.width() + 80, window.height() + 120);
                events();
            }
            const auto collapsedTop = topGeometry();
            const auto collapsedPosition = window.pos();
            const auto collapsedWidth = window.width();
            const auto collapsedHeight = window.height();
            toggle->click();
            events();
            check(details->isVisible() && labels(window).contains("HTTP 403"), "diagnostics remain available on demand");
            check(topGeometry() == collapsedTop && window.pos() == collapsedPosition && window.width() == collapsedWidth,
                  "expanding details anchors upper layout, including after manual resize");
            if (!enlarged) check(window.height() > collapsedHeight, "details expand window downwards");
            toggle->click();
            events();
            check(details->isHidden() && topGeometry() == collapsedTop && window.pos() == collapsedPosition,
                  "collapsing details preserves upper layout");
        }
        const auto snapshots = qEnvironmentVariable("P4VPATCH_UI_SNAPSHOT_DIR");
        if (!snapshots.isEmpty()) {
            QDir().mkpath(snapshots);
            window.grab().save(QDir(snapshots).filePath("launcher-light.png"));
        }

        const auto config = temp.filePath("appearance.xml");
        QFile appearance(config);
        appearance.open(QIODevice::WriteOnly);
        appearance.write("<Settings><Bool varName=\"DarkTheme\">true</Bool><Font varName=\"AppFont\"><family>Segoe UI</family><pointSize>10</pointSize></Font></Settings>");
        appearance.close();
        const auto preferences = readP4vAppearance(config);
        check(preferences.dark && preferences.font.family() == "Segoe UI" && preferences.font.pointSize() == 10, "read P4V theme and UI font without changing settings");
        LauncherTheme preview;
        preview.apply(window, exe, config);
        events();
        check(window.property("p4vDarkTheme").toBool() && window.palette().color(QPalette::Window).name() == "#1e1f22", "dark fallback matches P4V palette");
        check(errorColors(window, true), "dark theme uses readable red error text");
        if (!snapshots.isEmpty()) window.grab().save(QDir(snapshots).filePath("launcher-dark.png"));
        appearance.open(QIODevice::WriteOnly | QIODevice::Truncate);
        appearance.write("<Settings><Bool varName=\"DarkTheme\">false</Bool></Settings>");
        appearance.close();
        const auto skinDir = temp.filePath("P4VResources/skins");
        QDir().mkpath(skinDir);
        QFile skin(skinDir + "/Default.qss");
        skin.open(QIODevice::WriteOnly);
        skin.write("QPushButton { border-radius: 17px; }");
        skin.close();
        preview.apply(window, exe, config);
        check(preview.loadedP4vSkin() && window.styleSheet().contains("border-radius: 17px") &&
              !window.property("p4vDarkTheme").toBool(), "reuse the selected P4V skin at runtime");
        check(errorColors(window, true), "light P4V skin preserves red error text");
        appearance.open(QIODevice::WriteOnly | QIODevice::Truncate);
        appearance.write("<Settings><Bool varName=\"DarkTheme\">true</Bool>");
        appearance.close();
        check(!readP4vAppearance(config).dark, "malformed settings safely fall back to light theme");
        const auto installedP4v = qEnvironmentVariable("P4VPATCH_TEST_P4V_PATH");
        if (!installedP4v.isEmpty()) {
            for (const bool dark : {false, true}) {
                appearance.open(QIODevice::WriteOnly | QIODevice::Truncate);
                appearance.write(dark ? "<Settings><Bool varName=\"DarkTheme\">true</Bool></Settings>"
                                      : "<Settings><Bool varName=\"DarkTheme\">false</Bool></Settings>");
                appearance.close();
                preview.apply(window, installedP4v, config);
                events();
                check(preview.loadedP4vSkin(), "installed P4V skin loaded without copying theme assets");
                check(errorColors(window, true), "installed P4V skins preserve red error text");
                if (!snapshots.isEmpty()) window.grab().save(QDir(snapshots).filePath(dark ? "launcher-p4v-dark.png" : "launcher-p4v-light.png"));
            }
        }
    }
    settings.clear();
    {
        WelcomeWindow::FetchCallback pending;
        WelcomeWindow window(exe, {}, true, [&](const QUrl&, qint64, WelcomeWindow::FetchCallback callback) { pending = std::move(callback); }, patch);
        events();
        pending({}, "synthetic network failure");
        events();
        check(errorColors(window, true), "failed check enters error state");
        button(window, QStringLiteral("检查更新"))->click();
        events();
        QJsonObject current{{"tag_name", QStringLiteral("v" P4VPATCH_VERSION)}, {"draft", false}, {"prerelease", false},
            {"html_url", base + QStringLiteral("tag/v" P4VPATCH_VERSION)}, {"assets", QJsonArray()}};
        pending(QJsonDocument(current).toJson(), {});
        events();
        check(errorColors(window, false), "successful check restores normal text colors");
        button(window, QStringLiteral("启动 P4V"))->click(); // Incompatible inert fixture, never launched.
        events();
        check(errorColors(window, true) && window.findChild<QWidget*>("launcherDetails")->isVisible(),
              "launch preflight error is red and exposes details");
    }
    if (QString::fromLatin1(qVersion()) == QStringLiteral(P4VPATCH_QT_VERSION)) {
        if (!QFile::remove(exe) || !QFile::copy(app.arguments()[2], exe)) return 2;
        QProcess existing;
        existing.start(app.arguments()[2], QStringList()); // Only this sleeping mock process is created/ended.
        if (!existing.waitForStarted()) return 2;
        QString runningError;
        check(p4vIsRunning(runningError), "windowless mock P4V is running during multi-instance launch test");
        std::atomic<int> launches{0};
        auto launch = [&](const QString&, const QString&, const QStringList&) {
            ++launches;
            return LaunchResult{false, "synthetic launch failure"}; // No process launch, injection or app quit.
        };
        auto offline = [](const QUrl&, qint64, WelcomeWindow::FetchCallback callback) { callback({}, "offline"); };
        WelcomeWindow first(exe, {}, true, offline, patch, launch);
        events();
        button(first, QStringLiteral("启动 P4V"))->click();
        check(waitFor([&] { return first.findChild<QLabel*>("statusTitle")->text() == QStringLiteral("启动未完成"); }) && launches == 1,
              "existing P4V process does not prevent launch handler execution");
        check(errorColors(first, true), "asynchronous launch failure is red");
        WelcomeWindow second(exe, {}, true, offline, patch, launch);
        events();
        button(second, QStringLiteral("启动 P4V"))->click();
        check(waitFor([&] { return second.findChild<QLabel*>("statusTitle")->text() == QStringLiteral("启动未完成"); }) && launches == 2,
              "another welcome window can request another P4V launch while existing process stays alive");
        WelcomeWindow updateWindow(exe, {}, true, [&](const QUrl& url, qint64, WelcomeWindow::FetchCallback callback) {
            if (url == latestReleaseUrl()) callback(QJsonDocument(release).toJson(), {});
            else {
                QJsonObject manifest{{"schema", 1}, {"version", "9.0.0"}, {"minimum_launcher_version", QStringLiteral(P4VPATCH_VERSION)},
                    {"p4v_version", QStringLiteral(P4VPATCH_P4V_VERSION)}, {"qt_version", QStringLiteral(P4VPATCH_QT_VERSION)},
                    {"dll", QJsonObject{{"url", base + "download/v9.0.0/p4vpatch.dll"}, {"size", 100}, {"sha256", QString(64, 'a')}}}};
                callback(QJsonDocument(manifest).toJson(), {});
            }
        }, patch, launch);
        events();
        check(button(updateWindow, QStringLiteral("更新补丁"))->isEnabled(), "compatible fixture offers manual update");
        bool dialogChecked = false;
        QTimer::singleShot(0, &updateWindow, [&] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto* message = dialog->findChild<QLabel*>(QStringLiteral("qt_msgbox_label"));
            const QColor red(updateWindow.property("p4vDarkTheme").toBool() ? "#FF7B72" : "#C62828");
            dialogChecked = message && dialog->property("error").toBool() && message->palette().color(QPalette::WindowText) == red;
            dialog->accept();
        });
        button(updateWindow, QStringLiteral("更新补丁"))->click();
        check(dialogChecked && errorColors(updateWindow, true), "running P4V still blocks DLL update with red status and dialog text");
        existing.kill(); // This handle belongs to our inert fixture, never a real P4V.
        existing.waitForFinished();
    } else std::cout << "SKIP multi-instance preflight requires P4V Qt " P4VPATCH_QT_VERSION " runtime\n";
    {
        bool dialogChecked = false;
        QTimer::singleShot(0, &app, [&] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto* message = dialog->findChild<QLabel*>(QStringLiteral("qt_msgbox_label"));
            const QColor red(QApplication::palette().color(QPalette::Window).lightness() < 128 ? "#FF7B72" : "#C62828");
            dialogChecked = message && message->palette().color(QPalette::WindowText) == red;
            dialog->accept();
        });
        showLauncherError(nullptr, QStringLiteral("synthetic startup failure"), QStringLiteral("synthetic diagnostic"));
        check(dialogChecked, "early startup error dialog without a welcome window is also red");
    }
    settings.clear();
    std::cout << failures << " failure(s)\n";
    return failures ? 1 : 0;
}
