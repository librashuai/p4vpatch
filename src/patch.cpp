#include "terminal.h"

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
constexpr int kMaxAttempts = 3;

struct Credential {
    QString password;
    int submissions = 0;
};

class PasswordWatcher final : public QObject {
public:
    explicit PasswordWatcher(QObject* parent) : QObject(parent) {}

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
                save->setToolTip(QStringLiteral("Remember this password only until P4V exits"));
                QPointer<QLineEdit> weakField(field);
                QPointer<QAbstractButton> weakSubmit(submitButton(dialog));
                connect(save, &QPushButton::clicked, dialog, [this, key, weakField, weakSubmit] {
                    if (!weakField || weakField->text().isEmpty() ||
                        !weakSubmit || !weakSubmit->isEnabled()) return;
                    auto& entry = saved_[key];
                    if (!entry.password.isEmpty()) entry.password.fill(QChar('\0'));
                    entry.password = weakField->text();
                    entry.submissions = 0; // explicit user action resets the automatic limit
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
        if (it == saved_.end() || it->password.isEmpty() || it->submissions >= kMaxAttempts)
            return;
        QAbstractButton* submit = submitButton(dialog);
        if (!submit || !submit->isEnabled()) return;
        // Increment before clicking: click() can synchronously open a new dialog.
        ++it->submissions;
        field->setText(it->password);
        submit->click();

        // Some P4V builds leave the *same* dialog open on a rejected password
        // rather than showing it again. Retry only after an explicit error.
        QPointer<QDialog> weakDialog(dialog);
        QPointer<QLineEdit> weakField(field);
        QTimer::singleShot(1500, dialog, [this, weakDialog, weakField, key] {
            if (!weakDialog || !weakDialog->isVisible() || !weakField ||
                !invalidPassword(weakDialog)) return;
            auto it = saved_.find(key);
            if (it == saved_.end() || it->submissions >= kMaxAttempts) return;
            // Respect manual edits made after the rejected submission.
            if (!weakField->text().isEmpty() && weakField->text() != it->password) return;
            tryAutomatic(weakDialog, key, weakField);
        });
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
