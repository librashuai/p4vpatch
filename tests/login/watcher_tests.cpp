// Compile the real watcher, with harmless terminal/preferences stubs and an
// injected options resolver so the user's real home/config/tickets are untouched.
#include "../../src/background_login.h"
static BackgroundLoginOptions fixtureOptions;
static std::optional<BackgroundLoginOptions> testBackgroundLoginOptions() { return fixtureOptions; }
#define backgroundLoginOptions testBackgroundLoginOptions
#include "../../src/patch.cpp"
#undef backgroundLoginOptions
#include <QFileInfo>
#include <QEventLoop>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QVBoxLayout>
#include <cstdio>
#include <functional>

void installTerminal(QMainWindow*) {}
void installPatchPreferences(QDialog*) {}
void installSubmitAiDescription(QApplication*) {}
void inspectSubmitDialog(QDialog*) {}

class UIWorkspace2 final : public QMainWindow {
    Q_OBJECT
};

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
bool waitFor(const std::function<bool()>& condition, int ms = 4000) {
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return condition();
}
void spin(int ms) {
    QElapsedTimer clock;
    clock.start();
    waitFor([&] { return clock.elapsed() >= ms; }, ms + 100);
}
int count(const QString& tickets, const char* key) {
    QSettings settings(tickets + QStringLiteral(".test-state"), QSettings::IniFormat);
    settings.sync();
    return settings.value(QString::fromLatin1(key), 0).toInt();
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    QApplication app(argc, argv);
    QTemporaryDir home;
    const QString tickets = home.filePath(QStringLiteral("synthetic-tickets"));
    fixtureOptions.program = QString::fromLocal8Bit(argv[1]);
    fixtureOptions.ticketsFile = tickets;
    fixtureOptions.hostLocked = false;
    fixtureOptions.pollMs = 35;
    fixtureOptions.timeoutMs = 5000;
    {
        QFile file(tickets);
        file.open(QIODevice::WriteOnly);
        file.write("before");
        QSettings state(tickets + QStringLiteral(".test-state"), QSettings::IniFormat);
        state.setValue(QStringLiteral("scenario"), QStringLiteral("renew"));
    }
    UIWorkspace2 main;
    main.setWindowTitle(QStringLiteral("alice@test:1666"));
    main.showMinimized();
    PasswordWatcher watcher(&main);
    QDialog dialog(&main);
    dialog.setWindowTitle(QStringLiteral("Perforce Password Required"));
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral("A password is required for user 'alice' on server 'test:1666'."), &dialog));
    auto* field = new QLineEdit(&dialog);
    field->setEchoMode(QLineEdit::Password);
    layout->addWidget(field);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    int guiSubmissions = 0;
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        ++guiSubmissions;
        QFile file(tickets);
        file.open(QIODevice::WriteOnly);
        file.write("after-manual-p4v-login");
        file.close();
        dialog.accept();
    });
    dialog.show();
    watcher.inspectVisible(&dialog);
    QPushButton* save = nullptr;
    for (auto* button : dialog.findChildren<QPushButton*>())
        if (button->text() == QStringLiteral("Save")) save = button;
    expect(save != nullptr, "watcher adds Save");
    if (!save) return 6;
    field->setText(QStringLiteral("synthetic-secret"));
    save->click();
    expect(guiSubmissions == 1 && !dialog.isVisible(), "Save uses original OK handler");
    QObject* monitor = watcher.findChild<QObject*>(QStringLiteral("p4vpatch.backgroundLogin"));
    expect(monitor != nullptr, "manual Save creates monitor with parsed identity");
    if (!monitor) return 7;
    expect(waitFor([&] { return monitor->property("p4vpatch.backgroundStatus").toString() ==
        QStringLiteral("renewal-confirmed"); }), "minimized main window gets background renewal");
    expect(count(tickets, "logins") == 1, "watcher provided password through stdin");
    main.setWindowTitle(QStringLiteral("different connection"));
    expect(waitFor([&] { return monitor->property("p4vpatch.backgroundStatus").toString() ==
        QStringLiteral("paused-connection-or-dialog"); }), "connection title change pauses monitor");
    const int checks = count(tickets, "checks");
    spin(150);
    expect(count(tickets, "checks") == checks, "no background commands for changed connection");
    main.setWindowTitle(QStringLiteral("alice@test:1666"));
    field->setText(QStringLiteral("manual-edit"));
    dialog.show();
    watcher.inspectVisible(&dialog);
    spin(150);
    expect(guiSubmissions == 1 && count(tickets, "logins") == 1, "manual password edit is respected");
    field->clear();
    watcher.inspectVisible(&dialog);
    expect(guiSubmissions == 2 && field->text() == QStringLiteral("synthetic-secret"), "GUI fallback still works");
    main.hide();
    expect(waitFor([&] { return monitor->property("p4vpatch.backgroundStatus").toString() ==
        QStringLiteral("paused-connection-or-dialog"); }), "hidden/closed workspace pauses monitor");
    std::printf("Password watcher tests: %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}

#include "watcher_tests.moc"
