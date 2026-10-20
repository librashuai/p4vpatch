#pragma once

#include <QObject>
#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <functional>
#include <optional>

class QProcess;
class QTimer;

struct LoginConnection {
    QString user;
    QString server;
};

std::optional<LoginConnection> parseLoginPrompt(const QString& prompt);

struct BackgroundLoginOptions {
    QString program;
    QString ticketsFile;
    bool hostLocked = true;
    int pollMs = 30000;
    int timeoutMs = 15000;
    int renewBeforeSeconds = 60;
};

// Resolve standard P4V/CLI ticket settings. Never search PATH for an executable.
std::optional<BackgroundLoginOptions> backgroundLoginOptions();
// Optional diagnostic log accepts fixed state labels only, never process output.
void noteBackgroundLogin(const char* state);

struct TicketStatus {
    enum Kind { Valid, Expired, Unknown } kind = Unknown;
    qint64 seconds = 0;
};
TicketStatus parseTicketStatus(const QByteArray& output, int exitCode,
                              const QString& user);

// One monitor per manually saved connection. Password/budget remain in the watcher.
// No Q_OBJECT needed: all callbacks run on the object's Qt thread.
class BackgroundLogin final : public QObject {
public:
    BackgroundLogin(const BackgroundLoginOptions& options, const LoginConnection& connection,
                    std::function<bool()> canCheck,
                    std::function<bool(QString&)> acquirePassword,
                    std::function<void()> confirmed, QObject* parent);
    ~BackgroundLogin() override;
    void arm(); // Only after P4V accepts the manual Save login.
    void cancelCommand(); // A GUI password challenge takes precedence.
    bool isAuthenticating() const;

private:
    enum class Command { Check, Login, Verify };
    void poll();
    void run(Command command, const QString& password = {});
    void complete(int exitCode, bool normalExit);
    void defer();
    void setState(const char* state);
    bool ticketFileChanged() const;

    BackgroundLoginOptions options_;
    LoginConnection connection_;
    std::function<bool()> canCheck_;
    std::function<bool(QString&)> acquirePassword_;
    std::function<void()> confirmed_;
    QProcess* process_ = nullptr;
    QTimer* timer_ = nullptr;
    QTimer* timeout_ = nullptr;
    QByteArray output_;
    QByteArray input_;
    QElapsedTimer clock_;
    qint64 nextCheck_ = 0;
    qint64 originalModified_ = -1;
    qint64 originalSize_ = -1;
    int transientFailures_ = 0;
    Command command_ = Command::Check;
    bool enabled_ = false;
    bool sharedTicketVerified_ = false;
    bool preemptiveDone_ = false;
    bool verificationPending_ = false;
    bool cancelled_ = false;
    bool timedOut_ = false;
    bool outputOverflow_ = false;
};
