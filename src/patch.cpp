#include "terminal.h"
#include "preferences.h"
#include "background_login.h"

#include <windows.h>

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QEvent>
#include <QElapsedTimer>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QLayout>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QTimer>

#include <cstring>
#include <cwchar>

namespace {
// Deliberately fail closed if a different P4V/Qt build is installed.
constexpr const char* kQtVersion = "6.8.6";
constexpr int kMaxRetries = 3; // one initial automatic submission, then three retries
constexpr int kRetryDelayMs = 10000;

struct Credential {
    QString password;
    int submissions = 0; // since the last confirmed dialog acceptance or manual Save
    QElapsedTimer lastSubmission;
    QPointer<QDialog> pendingDialog;
    bool pending = false;
    bool accepted = false;
    bool retryScheduled = false;
    quint64 serial = 0; // invalidate callbacks from an earlier attempt/Save
    quint64 backgroundSerial = 0;
    QPointer<BackgroundLogin> background;
};

class PasswordWatcher final : public QObject {
public:
    explicit PasswordWatcher(QObject* parent) : QObject(parent) {}

    ~PasswordWatcher() override {
        for (auto& entry : saved_) entry.password.fill(QChar('\0'));
    }

    void inspectVisible(QDialog* dialog) {
        if (dialog->isVisible() &&
            dialog->windowTitle() == QStringLiteral("Perforce Password Required"))
            inspect(dialog);
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() != QEvent::Show) return false;
        if (auto* main = qobject_cast<QMainWindow*>(object)) {
            QPointer<QMainWindow> weak(main);
            QTimer::singleShot(0, main, [weak] { if (weak) installTerminal(weak); });
        }
        auto* dialog = qobject_cast<QDialog*>(object);
        if (dialog && std::strcmp(dialog->metaObject()->className(), "P4VPreferencesDialog") == 0) {
            QPointer<QDialog> weak(dialog);
            QTimer::singleShot(0, dialog, [weak] { if (weak) installPatchPreferences(weak); });
        }
        if (!dialog || dialog->windowTitle() != QStringLiteral("Perforce Password Required"))
            return false;
        // Run after the application's Show event processing has finished.
        QPointer<QDialog> weak(dialog);
        QTimer::singleShot(0, dialog, [this, weak] {
            if (weak && weak->isVisible()) inspect(weak);
        });
        return false;
    }

private:
    QHash<QString, Credential> saved_;

    static QLineEdit* passwordField(QDialog* dialog) {
        QLineEdit* result = nullptr;
        for (auto* field : dialog->findChildren<QLineEdit*>()) {
            if (field->echoMode() != QLineEdit::Password &&
                field->echoMode() != QLineEdit::PasswordEchoOnEdit) continue;
            if (result) return nullptr; // ambiguous: do not guess a secret field
            result = field;
        }
        return result;
    }

    static QString connectionKey(QDialog* dialog) {
        for (auto* label : dialog->findChildren<QLabel*>()) {
            const QString text = label->text().trimmed();
            if (text.startsWith(QStringLiteral("A password is required for user '")) &&
                text.contains(QStringLiteral(" on "))) {
                // The full prompt distinguishes both username and server.
                return text;
            }
        }
        return {};
    }

    static bool invalidPassword(QDialog* dialog) {
        for (auto* label : dialog->findChildren<QLabel*>()) {
            if (label->text().contains(QStringLiteral("Password not valid"),
                                       Qt::CaseInsensitive)) return true;
        }
        return false;
    }

    static QAbstractButton* submitButton(QDialog* dialog) {
        for (auto* box : dialog->findChildren<QDialogButtonBox*>()) {
            if (auto* ok = box->button(QDialogButtonBox::Ok)) return ok;
            for (auto* button : box->buttons()) {
                if (box->buttonRole(button) == QDialogButtonBox::AcceptRole) return button;
            }
        }
        // P4V may use a custom layout without a QDialogButtonBox.
        for (auto* button : dialog->findChildren<QPushButton*>()) {
            if (button->isDefault() && button->text() != QStringLiteral("Save")) return button;
        }
        return nullptr; // do not call accept(): that could bypass P4V validation
    }

    void prepareBackgroundLogin(QDialog* dialog, const QString& key) {
        auto& entry = saved_[key];
        if (entry.background) {
            entry.background->cancelCommand();
            entry.background->deleteLater();
            entry.background = nullptr;
        }
        const quint64 generation = ++entry.backgroundSerial;
        const auto connection = parseLoginPrompt(key);
        const auto options = backgroundLoginOptions();
        QPointer<QMainWindow> owner;
        for (QWidget* widget = dialog->parentWidget(); widget; widget = widget->parentWidget()) {
            if (auto* main = qobject_cast<QMainWindow*>(widget)) {
                if (std::strcmp(main->metaObject()->className(), "UIWorkspace2") == 0) {
                    owner = main;
                    break;
                }
            }
        }
        if (!connection || !owner || owner->windowTitle().isEmpty()) {
            noteBackgroundLogin("disabled-unverified-prompt-or-window");
            return;
        }
        if (!options) return;
        const QString title = owner->windowTitle();
        auto* background = new BackgroundLogin(*options, *connection,
            [this, key, generation, owner, title] {
                auto current = saved_.find(key);
                if (current == saved_.end() || current->backgroundSerial != generation ||
                    current->pending || !owner || !owner->isVisible() ||
                    owner->windowTitle() != title) return false;
                // Respect manual edits, including challenges that appeared while p4.exe ran.
                for (auto* widget : QApplication::topLevelWidgets()) {
                    auto* challenge = qobject_cast<QDialog*>(widget);
                    if (challenge && challenge->isVisible() &&
                        challenge->windowTitle() == QStringLiteral("Perforce Password Required") &&
                        connectionKey(challenge) == key) return false;
                }
                return true;
            },
            [this, key, generation](QString& password) {
                auto current = saved_.find(key);
                if (current == saved_.end() || current->backgroundSerial != generation ||
                    current->pending || current->retryScheduled || current->password.isEmpty() ||
                    current->submissions >= kMaxRetries + 1 ||
                    (current->lastSubmission.isValid() &&
                     current->lastSubmission.elapsed() < kRetryDelayMs)) return false;
                for (QChar c : current->password)
                    if (c.unicode() < 32 || c.unicode() > 126) return false;
                ++current->submissions; // GUI and background share a single retry budget.
                current->lastSubmission.start();
                ++current->serial;
                password = current->password;
                return true;
            },
            [this, key, generation] {
                auto current = saved_.find(key);
                if (current == saved_.end() || current->backgroundSerial != generation || current->pending)
                    return;
                current->submissions = 0;
                current->lastSubmission.invalidate();
            }, this);
        entry.background = background;
        QPointer<BackgroundLogin> weak(background);
        connect(dialog, &QDialog::accepted, background, [weak] {
            // Allow P4V to finish its ticket write. Rejected/hidden dialogs do not arm it.
            if (weak) QTimer::singleShot(1000, weak, [weak] { if (weak) weak->arm(); });
        });
    }

    void inspect(QDialog* dialog) {
        QString key = connectionKey(dialog);
        QLineEdit* field = passwordField(dialog);
        if (key.isEmpty() || !field || !submitButton(dialog)) return;

        if (!dialog->property("p4vpatch.saveAdded").toBool()) {
            auto* box = dialog->findChild<QDialogButtonBox*>();
            QPushButton* save = nullptr;
            if (box) {
                save = box->addButton(QStringLiteral("Save"), QDialogButtonBox::ActionRole);
            } else if (dialog->layout()) {
                // Fallback only if Qt owns the existing layout; otherwise do
                // not overlay an unknown layout with a native Win32 window.
                save = new QPushButton(QStringLiteral("Save"), dialog);
                dialog->layout()->addWidget(save);
            }
            if (save) {
                save->setAutoDefault(false);
                save->setToolTip(QStringLiteral("Remember this password only until P4V exits.\n"
                    "Background ticket renewal requires a verified shared ticket file."));
                QPointer<QLineEdit> weakField(field);
                QPointer<QAbstractButton> weakSubmit(submitButton(dialog));
                connect(save, &QPushButton::clicked, dialog, [this, key, weakField, weakSubmit, dialog] {
                    if (!weakField || weakField->text().isEmpty() ||
                        !weakSubmit || !weakSubmit->isEnabled()) return;
                    auto& entry = saved_[key];
                    if (!entry.password.isEmpty()) entry.password.fill(QChar('\0'));
                    entry.password = weakField->text();
                    entry.submissions = 0;
                    entry.pending = false;
                    entry.accepted = false;
                    entry.retryScheduled = false;
                    entry.lastSubmission.invalidate();
                    ++entry.serial;
                    prepareBackgroundLogin(dialog, key);
                    // Use P4V's existing OK handler, never QDialog::accept().
                    weakSubmit->click();
                });
                dialog->setProperty("p4vpatch.saveAdded", true);
            }
        }

        tryAutomatic(dialog, key, field);
    }

    void tryAutomatic(QDialog* dialog, const QString& key, QLineEdit* field) {
        auto it = saved_.find(key);
        if (it == saved_.end() || it->password.isEmpty()) return;
        if (it->background) {
            // Do not submit the same password concurrently via CLI and the GUI.
            const bool authenticating = it->background->isAuthenticating();
            it->background->cancelCommand();
            if (authenticating) {
                QPointer<QDialog> weak(dialog);
                QTimer::singleShot(100, this, [this, weak] {
                    if (weak && weak->isVisible()) inspect(weak);
                });
                return;
            }
        }
        if (it->retryScheduled) {
            // A new Show may have replaced the dialog targeted by the timer.
            it->retryScheduled = false;
            ++it->serial;
        }
        if (it->pending) {
            if (it->pendingDialog == dialog) {
                // A duplicate Show is not a failed submission. A visible error
                // on the same dialog is; the retry still observes the cooldown.
                if (!invalidPassword(dialog)) return;
            }
            // P4V can replace the rejected dialog with a new instance.
            it->pending = false;
            it->accepted = false;
            ++it->serial;
        }
        if (it->submissions >= kMaxRetries + 1) return;
        // Do not overwrite a password the user has started editing.
        if (!field->text().isEmpty() && field->text() != it->password) return;
        QAbstractButton* submit = submitButton(dialog);
        if (!submit || !submit->isEnabled()) return;

        if (it->lastSubmission.isValid() && it->lastSubmission.elapsed() < kRetryDelayMs) {
            if (it->retryScheduled) return;
            it->retryScheduled = true;
            const quint64 serial = it->serial;
            QPointer<QDialog> weakDialog(dialog);
            QPointer<QLineEdit> weakField(field);
            QTimer::singleShot(kRetryDelayMs - it->lastSubmission.elapsed(), this,
                               [this, weakDialog, weakField, key, serial] {
                auto current = saved_.find(key);
                if (current == saved_.end() || current->serial != serial) return;
                current->retryScheduled = false;
                if (weakDialog && weakDialog->isVisible() && weakField)
                    tryAutomatic(weakDialog, key, weakField);
            });
            return;
        }

        // Count before clicking: P4V may synchronously show another dialog.
        ++it->submissions;
        it->lastSubmission.start();
        it->pending = true;
        it->accepted = false;
        it->pendingDialog = dialog;
        it->retryScheduled = false;
        const quint64 serial = ++it->serial;
        QPointer<QDialog> weakDialog(dialog);
        QPointer<QLineEdit> weakField(field);
        connect(dialog, &QDialog::accepted, dialog, [this, key, serial] {
            auto current = saved_.find(key);
            if (current != saved_.end() && current->serial == serial)
                current->accepted = true;
        });
        field->setText(it->password);
        submit->click();

        // Only a completed, accepted dialog without a subsequent challenge
        // resets the budget. A hidden/rejected dialog alone is not success.
        QTimer::singleShot(kRetryDelayMs, this, [this, weakDialog, weakField, key, serial] {
            checkAttempt(key, serial, weakDialog, weakField);
        });
    }

    void checkAttempt(const QString& key, quint64 serial, QPointer<QDialog> dialog,
                      QPointer<QLineEdit> field) {
        auto current = saved_.find(key);
        if (current == saved_.end() || current->serial != serial || !current->pending)
            return;
        if (current->accepted && (!dialog || !dialog->isVisible())) {
            current->submissions = 0;
            current->pending = false;
            current->lastSubmission.invalidate();
            return;
        }
        if (dialog && dialog->isVisible()) {
            if (field && invalidPassword(dialog)) {
                current->pending = false;
                tryAutomatic(dialog, key, field);
                return;
            }
            // The server may respond after 10 seconds. Wait for an explicit
            // error instead of treating a slow request as a failed login.
            QTimer::singleShot(2000, this, [this, key, serial, dialog, field] {
                checkAttempt(key, serial, dialog, field);
            });
        }
    }
};

DWORD WINAPI bootstrap(void*) {
    // Qt is loaded by P4V. Avoid doing Qt work inside DllMain/loader lock.
    wchar_t exe[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return 0;
    const wchar_t* name = wcsrchr(exe, L'\\');
    if (_wcsicmp(name ? name + 1 : exe, L"p4v.exe") != 0) return 0;
    const bool inspect = qEnvironmentVariableIsSet("P4VPATCH_INSPECT_NATIVE_TABS");
    auto note = [inspect](const QString& text) {
        if (!inspect) return;
        QFile file(QDir::temp().filePath(QStringLiteral("p4vpatch-native-tabs.txt")));
        if (file.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream out(&file);
            out << text << '\n';
        }
    };
    note(QStringLiteral("DLL bootstrapped; Qt %1 (requires %2)")
         .arg(QString::fromLatin1(qVersion()), QString::fromLatin1(kQtVersion)));
    if (std::strcmp(qVersion(), kQtVersion) != 0) return 0;
    for (int i = 0; i < 300; ++i) {
        auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
        if (app) {
            note(QStringLiteral("QApplication found; waiting for UIWorkspace2 / View / Log"));
            QTimer::singleShot(0, app, [app] {
                // Keep the watcher on the GUI thread for the app's lifetime.
                auto* watcher = new PasswordWatcher(app);
                app->installEventFilter(watcher);
                // Cover a dialog that appeared between startup and injection.
                for (auto* widget : QApplication::topLevelWidgets()) {
                    if (auto* main = qobject_cast<QMainWindow*>(widget))
                        installTerminal(main);
                    auto* dialog = qobject_cast<QDialog*>(widget);
                    if (dialog && dialog->isVisible()) {
                        installPatchPreferences(dialog);
                        watcher->inspectVisible(dialog);
                    }
                }
            });
            return 0;
        }
        Sleep(100);
    }
    return 0;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, bootstrap, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
