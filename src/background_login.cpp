#include "background_login.h"

#include <windows.h>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QTextStream>
#include <QTimer>
#include <QXmlStreamReader>

namespace {
bool asciiOnly(const QString& value) {
    for (QChar c : value)
        if (c.unicode() < 32 || c.unicode() > 126) return false;
    return !value.isEmpty();
}

QString registryTickets(QSettings::Scope scope) {
    QSettings settings(scope == QSettings::UserScope
        ? QStringLiteral("HKEY_CURRENT_USER\\Software\\Perforce\\Environment")
        : QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Perforce\\Environment"),
        QSettings::NativeFormat);
    return settings.value(QStringLiteral("P4TICKETS")).toString();
}
}

std::optional<LoginConnection> parseLoginPrompt(const QString& prompt) {
    static const QRegularExpression pattern(QStringLiteral(
        "^A password is required for user '([^'\\r\\n]+)' on server '([^'\\r\\n]+)'\\.$"));
    const auto match = pattern.match(prompt.trimmed());
    if (!match.hasMatch()) return std::nullopt;
    LoginConnection result{match.captured(1), match.captured(2)};
    // Non-ASCII credentials need P4V's per-connection charset, not a guessed CLI charset.
    if (!asciiOnly(result.user) || !asciiOnly(result.server) ||
        result.user.startsWith('-') || result.server.startsWith('-')) return std::nullopt;
    return result;
}

std::optional<BackgroundLoginOptions> backgroundLoginOptions() {
    auto disabled = [](const char* reason) -> std::optional<BackgroundLoginOptions> {
        noteBackgroundLogin(reason);
        return std::nullopt;
    };
    if (qEnvironmentVariableIsSet("P4VPATCH_DISABLE_BACKGROUND_LOGIN"))
        return disabled("disabled-by-environment");
    BackgroundLoginOptions result;
    result.program = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("p4.exe"));
    if (!QFileInfo(result.program).isFile()) return disabled("disabled-no-sibling-p4-exe");

    result.ticketsFile = qEnvironmentVariable("P4TICKETS");
    if (result.ticketsFile.isEmpty()) result.ticketsFile = registryTickets(QSettings::UserScope);
    if (result.ticketsFile.isEmpty()) result.ticketsFile = registryTickets(QSettings::SystemScope);
    if (result.ticketsFile.isEmpty())
        result.ticketsFile = QDir::home().filePath(QStringLiteral("p4tickets.txt"));
    if (!QFileInfo(result.ticketsFile).isAbsolute()) return disabled("disabled-relative-ticket-path");
    result.ticketsFile = QDir::cleanPath(result.ticketsFile);

    // Match P4V's host-specific/all-host ticket policy. Unknown settings fail closed.
    QFile settings(QDir::home().filePath(QStringLiteral(".p4qt/ApplicationSettings.xml")));
    if (!settings.open(QIODevice::ReadOnly)) return disabled("disabled-no-p4v-settings");
    QXmlStreamReader xml(&settings);
    bool found = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QStringLiteral("Bool") &&
            xml.attributes().value(QStringLiteral("varName")) == QStringLiteral("UseIpSpecificTickets")) {
            const QString value = xml.readElementText();
            if (found || (value != QStringLiteral("true") && value != QStringLiteral("false")))
                return disabled("disabled-ambiguous-ticket-policy");
            result.hostLocked = value == QStringLiteral("true");
            found = true;
        }
    }
    if (xml.hasError() || !found) return disabled("disabled-unknown-ticket-policy");
    return result;
}

TicketStatus parseTicketStatus(const QByteArray& output, int exitCode, const QString& user) {
    const QString text = QString::fromUtf8(output).trimmed();
    if (exitCode == 0) {
        // Some server versions provide tagged output, others provide a plain message.
        static const QRegularExpression taggedUser(QStringLiteral("(?:^|\\n)\\.\\.\\. User ([^\\r\\n]+)(?:\\r?\\n|$)"));
        static const QRegularExpression expiration(QStringLiteral("(?:^|\\n)\\.\\.\\. TicketExpiration ([0-9]+)(?:\\r?\\n|$)"));
        const auto u = taggedUser.match(text);
        const auto t = expiration.match(text);
        if (u.hasMatch() && u.captured(1) == user && t.hasMatch()) {
            bool ok = false;
            const qint64 seconds = t.captured(1).toLongLong(&ok);
            if (ok) return {seconds > 0 ? TicketStatus::Valid : TicketStatus::Expired, seconds};
        }
        const QRegularExpression plain(QStringLiteral("^User %1 ticket expires in ([0-9]+) hours ([0-9]+) minutes\\.$")
            .arg(QRegularExpression::escape(user)));
        const auto p = plain.match(text);
        if (p.hasMatch()) {
            bool hOk = false, mOk = false;
            const qint64 hours = p.captured(1).toLongLong(&hOk);
            const qint64 minutes = p.captured(2).toLongLong(&mOk);
            if (hOk && mOk && hours <= 1000000000 && minutes < 60)
                return {TicketStatus::Valid, hours * 3600 + minutes * 60};
        }
        return {}; // No expiry, password-authenticated or unknown format: do not guess.
    }
    // Only explicit authentication errors trigger a password submission. Not SSL/network errors.
    if (text.contains(QStringLiteral("Perforce password (P4PASSWD) invalid or unset.")) ||
        text.contains(QStringLiteral("Your session has expired, please login again.")) ||
        text == QStringLiteral("Ticket expired.")) return {TicketStatus::Expired, 0};
    return {};
}

BackgroundLogin::BackgroundLogin(const BackgroundLoginOptions& options,
        const LoginConnection& connection, std::function<bool()> canCheck,
        std::function<bool(QString&)> acquirePassword, std::function<void()> confirmed,
        QObject* parent)
    : QObject(parent), options_(options), connection_(connection), canCheck_(std::move(canCheck)),
      acquirePassword_(std::move(acquirePassword)), confirmed_(std::move(confirmed)) {
    setObjectName(QStringLiteral("p4vpatch.backgroundLogin"));
    const QFileInfo before(options_.ticketsFile);
    if (before.isFile()) {
        originalModified_ = before.lastModified().toMSecsSinceEpoch();
        originalSize_ = before.size();
    }
    clock_.start();
    process_ = new QProcess(this);
    process_->setProcessChannelMode(QProcess::MergedChannels);
    process_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
    timer_ = new QTimer(this);
    timer_->setInterval(options_.pollMs);
    timeout_ = new QTimer(this);
    timeout_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, [this] { poll(); });
    connect(timeout_, &QTimer::timeout, this, [this] {
        timedOut_ = true;
        process_->kill();
    });
    connect(process_, &QProcess::started, this, [this] {
        if (command_ == Command::Login && !cancelled_) {
            process_->write(input_);
            process_->closeWriteChannel();
        } else {
            process_->closeWriteChannel();
        }
        input_.fill('\0');
        input_.clear();
    });
    connect(process_, &QProcess::readyRead, this, [this] {
        QByteArray data = process_->readAll();
        if (output_.size() + data.size() > 65536) {
            outputOverflow_ = true;
            process_->kill();
        } else {
            output_ += data;
        }
        data.fill('\0');
    });
    connect(process_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        complete(code, status == QProcess::NormalExit);
    });
    connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) complete(-1, false);
    });
    setState("waiting-for-manual-login");
}

BackgroundLogin::~BackgroundLogin() {
    disconnect(process_, nullptr, this, nullptr);
    process_->kill();
    output_.fill('\0');
    input_.fill('\0');
}

void BackgroundLogin::setState(const char* state) {
    setProperty("p4vpatch.backgroundStatus", QString::fromLatin1(state));
    noteBackgroundLogin(state);
}

void noteBackgroundLogin(const char* state) {
    if (!qEnvironmentVariableIsSet("P4VPATCH_LOGIN_DIAGNOSTICS")) return;
    // Fixed state strings only; never raw command output, credentials or tickets.
    QFile log(QDir::temp().filePath(QStringLiteral("p4vpatch-login.txt")));
    if (log.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&log);
        stream << QDateTime::currentDateTimeUtc().toString(Qt::ISODate) << ' ' << state << '\n';
    }
}

bool BackgroundLogin::ticketFileChanged() const {
    const QFileInfo after(options_.ticketsFile);
    return after.isFile() && (after.size() != originalSize_ ||
        after.lastModified().toMSecsSinceEpoch() != originalModified_);
}

void BackgroundLogin::arm() {
    if (enabled_) return;
    // Evidence that this path was written by the just-completed manual P4V login.
    // Do not read ticket contents or create a ticket file to make this check pass.
    if (!ticketFileChanged()) {
        setState("disabled-ticket-path-unconfirmed");
        return;
    }
    enabled_ = true;
    timer_->start();
    poll();
}

bool BackgroundLogin::isAuthenticating() const {
    return process_->state() != QProcess::NotRunning && command_ != Command::Check;
}

void BackgroundLogin::cancelCommand() {
    if (process_->state() == QProcess::NotRunning) return;
    cancelled_ = true;
    verificationPending_ = false;
    timeout_->stop();
    process_->kill();
}

void BackgroundLogin::defer() {
    transientFailures_ = qMin(transientFailures_ + 1, 5);
    const qint64 delay = qMin<qint64>(300000, qint64(options_.pollMs) << (transientFailures_ - 1));
    nextCheck_ = clock_.elapsed() + delay;
}

void BackgroundLogin::poll() {
    if (!enabled_ || process_->state() != QProcess::NotRunning || clock_.elapsed() < nextCheck_)
        return;
    if (!canCheck_()) {
        setState("paused-connection-or-dialog");
        return;
    }
    if (!QFileInfo(options_.ticketsFile).isFile()) {
        enabled_ = false;
        timer_->stop();
        setState("disabled-ticket-file-removed");
        return;
    }
    run(verificationPending_ ? Command::Verify : Command::Check);
}

void BackgroundLogin::run(Command command, const QString& password) {
    command_ = command;
    if (command == Command::Login) verificationPending_ = true;
    cancelled_ = timedOut_ = outputOverflow_ = false;
    output_.fill('\0');
    output_.clear();
    QStringList args{QStringLiteral("--no-script"), QStringLiteral("-ztag"),
        QStringLiteral("-p"), connection_.server, QStringLiteral("-u"), connection_.user,
        QStringLiteral("-E"), QStringLiteral("P4TICKETS=") + options_.ticketsFile,
        QStringLiteral("-E"), QStringLiteral("P4PASSWD="),
        QStringLiteral("-E"), QStringLiteral("P4LOGINSSO="),
        QStringLiteral("-L"), QStringLiteral("en"), QStringLiteral("login")};
    if (command != Command::Login) args << QStringLiteral("-s");
    // For status, omit -a: the all-host ticket is also valid on this host, and -s -a
    // can return multiple expirations. For login, match P4V's saved policy exactly.
    if (command == Command::Login && !options_.hostLocked) args << QStringLiteral("-a");
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("P4TICKETS"), options_.ticketsFile);
    environment.remove(QStringLiteral("P4PASSWD"));
    environment.remove(QStringLiteral("P4LOGINSSO"));
    process_->setProcessEnvironment(environment);
    process_->setWorkingDirectory(QDir::homePath());
    process_->setProgram(options_.program);
    process_->setArguments(args);
    // No secrets in argv or files. The started handler sends then wipes this copy.
    input_.fill('\0');
    input_.clear();
    if (command == Command::Login) {
        input_ = password.toLatin1();
        input_ += '\n';
    }
    setState(command == Command::Login ? "logging-in" :
        command == Command::Verify ? "verifying-renewal" : "checking-ticket");
    timeout_->start(options_.timeoutMs);
    process_->start();
}

void BackgroundLogin::complete(int exitCode, bool normalExit) {
    timeout_->stop();
    input_.fill('\0');
    input_.clear();
    if (cancelled_) {
        output_.fill('\0');
        output_.clear();
        nextCheck_ = clock_.elapsed() + options_.pollMs;
        setState("cancelled-for-gui");
        return;
    }
    if (!normalExit || timedOut_ || outputOverflow_ || !canCheck_()) {
        output_.fill('\0');
        output_.clear();
        defer();
        setState("deferred-command-or-connection");
        return;
    }
    if (command_ == Command::Login) {
        output_.fill('\0');
        output_.clear();
        if (exitCode == 0) {
            run(Command::Verify);
        } else {
            verificationPending_ = false;
            defer();
            setState("login-failed");
        }
        return;
    }
    const TicketStatus status = parseTicketStatus(output_, exitCode, connection_.user);
    output_.fill('\0');
    output_.clear();
    if (status.kind == TicketStatus::Unknown) {
        defer();
        setState("deferred-unrecognized-status");
        return;
    }
    if (!sharedTicketVerified_) {
        if (status.kind != TicketStatus::Valid) {
            enabled_ = false;
            timer_->stop();
            setState("disabled-initial-ticket-verification");
            return;
        }
        sharedTicketVerified_ = true;
    }
    if (command_ == Command::Verify) {
        verificationPending_ = false;
        if (status.kind == TicketStatus::Valid) {
            transientFailures_ = 0;
            preemptiveDone_ = true;
            nextCheck_ = clock_.elapsed() + options_.pollMs;
            setState("renewal-confirmed");
            confirmed_();
        } else {
            defer();
            setState("renewal-not-confirmed");
        }
        return;
    }
    transientFailures_ = 0;
    if (status.kind == TicketStatus::Valid && status.seconds > options_.renewBeforeSeconds) {
        preemptiveDone_ = false;
        setState("ticket-valid");
        return;
    }
    if (status.kind == TicketStatus::Valid && preemptiveDone_) return;
    QString password;
    if (!acquirePassword_(password)) {
        password.fill(QChar('\0'));
        setState("paused-budget-or-user-edit");
        return;
    }
    if (!asciiOnly(password)) {
        password.fill(QChar('\0'));
        enabled_ = false;
        timer_->stop();
        setState("disabled-password-charset");
        return;
    }
    run(Command::Login, password);
    password.fill(QChar('\0'));
}
