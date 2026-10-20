#include "../../src/background_login.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <functional>
#include <memory>

namespace {
int failures = 0;
void expect(bool ok, const char* message) {
    if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}

bool waitFor(const std::function<bool()>& condition, int milliseconds = 8000) {
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return condition();
}

void spin(int milliseconds) {
    QElapsedTimer clock;
    clock.start();
    waitFor([&] { return clock.elapsed() >= milliseconds; }, milliseconds + 100);
}

int count(const QString& file, const char* key) {
    QSettings settings(file + QStringLiteral(".test-state"), QSettings::IniFormat);
    settings.sync();
    return settings.value(QString::fromLatin1(key), 0).toInt();
}

void scenario(const QString& program, const QString& name) {
    QTemporaryDir directory;
    const QString tickets = directory.filePath(QStringLiteral("synthetic-tickets"));
    {
        QFile file(tickets);
        file.open(QIODevice::WriteOnly);
        file.write("before");
        QSettings state(tickets + QStringLiteral(".test-state"), QSettings::IniFormat);
        state.setValue(QStringLiteral("scenario"), name);
    }
    BackgroundLoginOptions options;
    options.program = program;
    options.ticketsFile = tickets;
    options.hostLocked = false;
    options.pollMs = 35;
    options.timeoutMs = 5000;
    if (name == QStringLiteral("hang")) options.timeoutMs = 700;
    bool allowed = true;
    int attempts = 0;
    int confirmed = 0;
    QTimer heartbeat;
    heartbeat.setInterval(5);
    int beats = 0;
    QObject::connect(&heartbeat, &QTimer::timeout, [&] { ++beats; });
    heartbeat.start();
    auto monitor = std::make_unique<BackgroundLogin>(options, LoginConnection{"alice", "test:1666"},
        [&] { return allowed; }, [&] (QString& password) {
            if (attempts >= 4) return false;
            ++attempts;
            password = QStringLiteral("synthetic-secret");
            return true;
        }, [&] { ++confirmed; }, nullptr);
    if (name != QStringLiteral("no-write")) {
        QFile file(tickets);
        file.open(QIODevice::WriteOnly);
        file.write("after-manual-login"); // changed metadata, standing in for P4V's write
    }
    if (name == QStringLiteral("paused")) allowed = false;
    monitor->arm();
    if (name == QStringLiteral("no-write")) {
        spin(150);
        expect(count(tickets, "checks") == 0, "unconfirmed ticket path must not execute CLI");
    } else if (name == QStringLiteral("paused")) {
        spin(150);
        expect(count(tickets, "checks") == 0, "inactive scope must not run");
        allowed = true;
        expect(waitFor([&] { return confirmed == 1; }), "scope can resume when safe");
    } else if (name == QStringLiteral("wrong-password")) {
        expect(waitFor([&] { return attempts == 4 && count(tickets, "logins") == 4; }), "retry budget reaches four");
        spin(250);
        expect(attempts == 4 && count(tickets, "logins") == 4 && confirmed == 0,
               "wrong password must not loop or reset budget");
    } else if (name == QStringLiteral("network") || name == QStringLiteral("unknown")) {
        expect(waitFor([&] { return count(tickets, "checks") >= 3; }), "status failures are rechecked");
        expect(attempts == 0 && count(tickets, "logins") == 0, "unknown/network errors never send password");
    } else if (name == QStringLiteral("initial-expired")) {
        expect(waitFor([&] { return monitor->property("p4vpatch.backgroundStatus").toString() ==
            QStringLiteral("disabled-initial-ticket-verification"); }), "initial valid ticket required");
        expect(attempts == 0, "initial expired status cannot establish sharing");
    } else if (name == QStringLiteral("login-hang")) {
        expect(waitFor([&] { return count(tickets, "logins") == 1; }), "login command starts");
        allowed = false; // The UI opened a challenge or changed connection.
        monitor->cancelCommand();
        expect(waitFor([&] { return monitor->property("p4vpatch.backgroundStatus").toString() ==
            QStringLiteral("cancelled-for-gui"); }), "GUI can cancel in-flight login");
        spin(150);
        expect(attempts == 1 && confirmed == 0, "cancelled login cannot reset budget or resubmit");
    } else if (name == QStringLiteral("hang")) {
        expect(waitFor([&] { return count(tickets, "checks") >= 2; }), "timeout frees command and allows retry");
        expect(beats > 10 && attempts == 0, "slow CLI cannot block Qt event loop");
        monitor->cancelCommand();
        spin(50);
    } else {
        expect(waitFor([&] { return confirmed == 1; }), "renewal must be verified");
        if (name == QStringLiteral("short")) {
            spin(250);
            expect(count(tickets, "logins") == 1, "short ticket must not be renewed every poll");
        }
        expect(attempts == 1 && count(tickets, "logins") == 1, "one password submission per renewal");
        expect(beats > 0, "asynchronous command leaves event loop live");
    }
    monitor.reset();
    spin(30);
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    const auto connection = parseLoginPrompt(QStringLiteral("A password is required for user 'alice' on server 'ssl:test:1666'."));
    expect(connection && connection->user == "alice" && connection->server == "ssl:test:1666", "parse prompt identity");
    expect(!parseLoginPrompt(QStringLiteral("A password is required for user 'alice' on test:1666")), "reject changed prompt");
    expect(!parseLoginPrompt(QStringLiteral("A password is required for user '-alice' on server 'test:1666'.")), "reject option-shaped identity");
    expect(parseTicketStatus("... User alice\n... TicketExpiration 120\n", 0, "alice").seconds == 120, "tagged expiry");
    expect(parseTicketStatus("... User bob\n... TicketExpiration 120\n", 0, "alice").kind == TicketStatus::Unknown, "reject wrong user");
    expect(parseTicketStatus("User alice ticket expires in 2 hours 3 minutes.", 0, "alice").seconds == 7380, "plain expiry");
    expect(parseTicketStatus("Connect to server failed", 1, "alice").kind == TicketStatus::Unknown, "network is not expiry");
    expect(parseTicketStatus("Your session has expired, please login again.", 1, "alice").kind == TicketStatus::Expired, "explicit expiry");
    expect(parseTicketStatus("User alice was authenticated by password not ticket.", 0, "alice").kind == TicketStatus::Unknown, "no ticket authentication");
    for (const QString& name : {"renew", "expired", "short", "wrong-password", "network", "unknown",
                                "initial-expired", "hang", "login-hang", "verify-network", "paused", "no-write"})
        scenario(QString::fromLocal8Bit(argv[1]), name);
    std::printf("Background login tests: %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
