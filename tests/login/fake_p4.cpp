// Offline fake CLI. It never connects to Perforce or writes a real ticket.
#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QThread>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    const QString tickets = qEnvironmentVariable("P4TICKETS");
    QSettings state(tickets + QStringLiteral(".test-state"), QSettings::IniFormat);
    const QString scenario = state.value(QStringLiteral("scenario")).toString();
    const bool check = args.contains(QStringLiteral("-s"));
    const QString countKey = check ? QStringLiteral("checks") : QStringLiteral("logins");
    const int count = state.value(countKey, 0).toInt() + 1;
    state.setValue(countKey, count);
    state.sync();
    // Ensure the monitor supplies explicit identity/path and no password argument.
    if (!args.contains(QStringLiteral("-p")) || !args.contains(QStringLiteral("test:1666")) ||
        !args.contains(QStringLiteral("-u")) || !args.contains(QStringLiteral("alice")) ||
        !args.contains(QStringLiteral("P4TICKETS=") + tickets) ||
        args.contains(QStringLiteral("synthetic-secret"))) return 90;
    if (scenario == QStringLiteral("hang")) QThread::sleep(10);
    if (scenario == QStringLiteral("network") && count > 1) {
        std::puts("Connect to server failed; check $P4PORT.");
        return 1;
    }
    if (scenario == QStringLiteral("unknown") && count > 1) {
        std::puts("Some unrecognized result.");
        return 0;
    }
    if (check) {
        if (scenario == QStringLiteral("verify-network") && count == 3) {
            std::puts("Connect to server failed; check $P4PORT.");
            return 1;
        }
        if (scenario == QStringLiteral("initial-expired") ||
            ((scenario == QStringLiteral("expired") || scenario == QStringLiteral("wrong-password")) &&
             count > 1 && !state.value(QStringLiteral("renewed"), false).toBool())) {
            std::puts("Perforce password (P4PASSWD) invalid or unset.");
            return 1;
        }
        const int expiry = scenario == QStringLiteral("short") ? 10 :
            (count == 1 ? 3600 : (state.value(QStringLiteral("renewed"), false).toBool() ? 3600 : 30));
        std::printf("... User alice\n... TicketExpiration %d\n", expiry);
        return 0;
    }
    if (scenario == QStringLiteral("login-hang")) QThread::sleep(10);
    // P4V all-host policy must be applied only to login, not multi-record status.
    if (!args.contains(QStringLiteral("-a"))) return 92;
    char input[256]{};
    if (!std::fgets(input, sizeof(input), stdin) || QByteArray(input) != "synthetic-secret\n") return 91;
    if (scenario == QStringLiteral("wrong-password")) {
        std::puts("Password invalid.");
        return 1;
    }
    state.setValue(QStringLiteral("renewed"), true);
    state.sync();
    std::puts("User alice logged in.");
    return 0;
}
