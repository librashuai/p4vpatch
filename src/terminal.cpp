#include "terminal.h"
#include "preferences.h"
#include "qt_focus_hook.h"

#include <windows.h>

#include <QAction>
#include <QApplication>
#include <QTabBar>
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QTextStream>
#include <QMetaMethod>
#include <QEvent>
#include <QVBoxLayout>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QWebChannel>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
QUrl pageUrl() { return QUrl(QStringLiteral("qrc:/p4vpatch/terminal/index.html")); }

class PseudoConsole final {
public:
    ~PseudoConsole() { stop(); }
    bool start(const QString& directory, std::function<void(QByteArray)> onOutput, QString* error) {
        HANDLE inputRead = nullptr, outputWrite = nullptr;
        if (!CreatePipe(&inputRead, &inputWrite_, nullptr, 0) ||
            !CreatePipe(&outputRead_, &outputWrite, nullptr, 0)) {
            *error = QStringLiteral("CreatePipe failed (%1)").arg(GetLastError());
            close(inputRead); close(outputWrite); stop(); return false;
        }
        const HRESULT hr = CreatePseudoConsole(COORD{80, 24}, inputRead, outputWrite, 0, &console_);
        if (FAILED(hr)) {
            *error = QStringLiteral("CreatePseudoConsole failed (0x%1)").arg(quint32(hr), 8, 16, QLatin1Char('0'));
            close(inputRead); close(outputWrite); stop(); return false;
        }
        SIZE_T length = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &length);
        auto* attributes = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(HeapAlloc(GetProcessHeap(), 0, length));
        if (!attributes) {
            *error = QStringLiteral("Cannot allocate process attributes");
            close(inputRead); close(outputWrite); stop(); return false;
        }
        if (!InitializeProcThreadAttributeList(attributes, 1, 0, &length)) {
            *error = QStringLiteral("Cannot initialize process attributes (%1)").arg(GetLastError());
            HeapFree(GetProcessHeap(), 0, attributes);
            close(inputRead); close(outputWrite); stop(); return false;
        }
        // Microsoft ConPTY API takes HPCON's value, not &HPCON.
        const bool attributed = UpdateProcThreadAttribute(attributes, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console_, sizeof(console_), nullptr, nullptr);
        if (!attributed) *error = QStringLiteral("Cannot attach ConPTY (%1)").arg(GetLastError());

        PROCESS_INFORMATION process{};
        BOOL created = FALSE;
        DWORD creationError = 0;
        const QString shell = terminalShell();
        QString executable;
        if (shell.compare(QStringLiteral("cmd.exe"), Qt::CaseInsensitive) == 0) {
            wchar_t system[MAX_PATH]{};
            if (GetSystemDirectoryW(system, MAX_PATH))
                executable = QString::fromWCharArray(system) + QStringLiteral("\\cmd.exe");
            else creationError = GetLastError();
        } else if (shell.compare(QStringLiteral("pwsh"), Qt::CaseInsensitive) == 0) {
            executable = QStandardPaths::findExecutable(QStringLiteral("pwsh.exe"));
            if (executable.isEmpty())
                *error = QStringLiteral("pwsh.exe was not found on PATH. Select its full path in Edit > Preferences > Patch.");
        } else {
            executable = shell; // Preferences validates this as an absolute pwsh.exe path.
        }
        if (attributed && !executable.isEmpty()) {
            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            // Otherwise console clients may inherit P4V's (or its launcher's) std handles.
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.lpAttributeList = attributes;
            std::wstring command = executable.toStdWString();
            std::wstring line = L"\"" + command + L"\"";
            const std::wstring cwd = directory.toStdWString();
            created = CreateProcessW(command.c_str(), line.data(), nullptr, nullptr, FALSE,
                EXTENDED_STARTUPINFO_PRESENT, nullptr, cwd.c_str(), &startup.StartupInfo, &process);
            creationError = GetLastError();
        }
        // ConPTY borrows its input/output pipe endpoints until after CreateProcess.
        close(inputRead); close(outputWrite);
        DeleteProcThreadAttributeList(attributes);
        HeapFree(GetProcessHeap(), 0, attributes);
        if (!created) {
            if (attributed && error->isEmpty())
                *error = QStringLiteral("Cannot launch %1 (%2)").arg(shell).arg(creationError);
            stop(); return false;
        }
        process_ = process.hProcess;
        CloseHandle(process.hThread);
        reader_ = std::thread([this, callback = std::move(onOutput)] {
            char buffer[8192];
            DWORD count = 0;
            while (!stopping_ && ReadFile(outputRead_, buffer, sizeof(buffer), &count, nullptr) && count)
                callback(QByteArray(buffer, int(count)));
        });
        writer_ = std::thread([this] {
            for (;;) {
                QByteArray bytes;
                COORD size{};
                bool resizing = false;
                {
                    std::unique_lock<std::mutex> lock(inputMutex_);
                    inputReady_.wait(lock, [this] { return stopping_ || !input_.empty() || resizePending_; });
                    if (stopping_) break;
                    // Preserve input order; coalesce successive resize requests.
                    if (!input_.empty()) {
                        bytes = std::move(input_.front());
                        input_.pop_front();
                        inputBytes_ -= size_t(bytes.size());
                    } else {
                        size = size_;
                        resizePending_ = false;
                        resizing = true;
                    }
                }
                if (resizing) {
                    if (!stopping_) ResizePseudoConsole(console_, size);
                } else {
                    DWORD written = 0;
                    int offset = 0;
                    while (!stopping_ && offset < bytes.size()) {
                        if (!WriteFile(inputWrite_, bytes.constData() + offset,
                                       DWORD(bytes.size() - offset), &written, nullptr) || !written) break;
                        offset += int(written);
                    }
                }
            }
        });
        return true;
    }

    bool input(QByteArray bytes) {
        if (bytes.isEmpty() || bytes.size() > 4096 || stopping_) return false;
        {
            std::lock_guard<std::mutex> lock(inputMutex_);
            if (stopping_ || inputBytes_ + size_t(bytes.size()) > 256 * 1024) return false;
            inputBytes_ += size_t(bytes.size());
            input_.push_back(std::move(bytes));
        }
        inputReady_.notify_one();
        return true;
    }
    void resize(int columns, int rows) {
        if (columns < 1 || columns > 500 || rows < 1 || rows > 200 || stopping_) return;
        {
            std::lock_guard<std::mutex> lock(inputMutex_);
            size_ = COORD{SHORT(columns), SHORT(rows)};
            resizePending_ = true;
        }
        inputReady_.notify_one();
    }
    void stop() {
        if (stopping_.exchange(true)) return;
        inputReady_.notify_all();
        if (process_) {
            TerminateProcess(process_, 0);
            WaitForSingleObject(process_, 2000);
        }
        if (writer_.joinable()) CancelSynchronousIo(static_cast<HANDLE>(writer_.native_handle()));
        if (reader_.joinable()) CancelSynchronousIo(static_cast<HANDLE>(reader_.native_handle()));
        if (writer_.joinable()) writer_.join();
        if (reader_.joinable()) reader_.join();
        if (process_) close(process_);
        if (console_) { ClosePseudoConsole(console_); console_ = nullptr; }
        close(inputWrite_); close(outputRead_);
    }
private:
    static void close(HANDLE& handle) { if (handle) { CloseHandle(handle); handle = nullptr; } }
    HANDLE inputWrite_ = nullptr, outputRead_ = nullptr, process_ = nullptr;
    HPCON console_ = nullptr;
    std::atomic_bool stopping_{false};
    std::thread reader_, writer_;
    std::mutex inputMutex_;
    std::condition_variable inputReady_;
    std::deque<QByteArray> input_;
    size_t inputBytes_ = 0;
    COORD size_{80, 24};
    bool resizePending_ = false;
};

// The page has access to a command-execution bridge: never navigate to external content.
class TerminalPage final : public QWebEnginePage {
public:
    explicit TerminalPage(QObject* parent) : QWebEnginePage(parent) {
        settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, false);
    }
protected:
    bool acceptNavigationRequest(const QUrl& url, NavigationType, bool isMainFrame) override {
        return isMainFrame && url == pageUrl();
    }
};

class TerminalSession final : public QObject {
    Q_OBJECT
public:
    explicit TerminalSession(QObject* parent) : QObject(parent) {
        auto* timer = new QTimer(this);
        timer->setInterval(16);
        connect(timer, &QTimer::timeout, this, [this] { pump(); });
        timer->start();
    }
    ~TerminalSession() override { stop(); }
    bool start(QString* error) {
        return pty_.start(QDir::homePath(), [this](QByteArray data) {
            std::unique_lock<std::mutex> lock(outputMutex_);
            space_.wait(lock, [this, &data] {
                return stopped_ || queuedBytes_ + size_t(data.size()) <= 512 * 1024;
            });
            if (stopped_) return;
            queuedBytes_ += size_t(data.size());
            output_.push_back(std::move(data));
        }, error);
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(outputMutex_);
            stopped_ = true;
        }
        space_.notify_all();
        pty_.stop();
    }
    Q_INVOKABLE void ready() { ready_ = true; pump(); }
    Q_INVOKABLE bool send(const QString& encoded) {
        if (encoded.size() > 5500) return false;
        auto decoded = QByteArray::fromBase64Encoding(encoded.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
        return decoded && pty_.input(std::move(*decoded));
    }
    Q_INVOKABLE void resize(int columns, int rows) { pty_.resize(columns, rows); }
    Q_INVOKABLE void ack(int bytes) {
        if (bytes < 1 || bytes > inFlight_) return;
        inFlight_ -= bytes;
        pump();
    }
signals:
    void output(const QString& encoded);
private:
    void pump() {
        if (!ready_ || stopped_) return;
        while (inFlight_ < 64 * 1024) {
            QByteArray data;
            {
                std::lock_guard<std::mutex> lock(outputMutex_);
                if (output_.empty() || int(output_.front().size()) > 64 * 1024 - inFlight_) break;
                data = std::move(output_.front());
                output_.pop_front();
                queuedBytes_ -= size_t(data.size());
            }
            space_.notify_one();
            inFlight_ += data.size();
            emit output(QString::fromLatin1(data.toBase64()));
        }
    }
    PseudoConsole pty_;
    std::mutex outputMutex_;
    std::condition_variable space_;
    std::deque<QByteArray> output_;
    size_t queuedBytes_ = 0;
    bool stopped_ = false; // set on GUI thread and under outputMutex_ in stop()
    bool ready_ = false;
    int inFlight_ = 0;
};

QString plainText(QString value) {
    value.remove(QLatin1Char('&'));
    return value.trimmed();
}

// Read-only, opt-in diagnostic of Qt's registered focus receivers.
QFile* focusConnectionLog = nullptr;
void focusConnectionMessage(QtMsgType, const QMessageLogContext&, const QString& message) {
    if (focusConnectionLog) {
        focusConnectionLog->write(message.toUtf8());
        focusConnectionLog->write("\n");
    }
}

class TerminalController final : public QObject {
public:
    explicit TerminalController(QMainWindow* main) : QObject(main), main_(main) {
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this, timer] {
            if (findHost()) timer->stop();
        });
        timer->start(1000);
        if (findHost()) timer->stop();
        if (qEnvironmentVariableIsSet("P4VPATCH_TRACE_WINDOW")) {
            connect(qApp, &QApplication::lastWindowClosed, this,
                    [this] { traceWindow(QStringLiteral("lastWindowClosed")); });
            connect(qApp, &QApplication::aboutToQuit, this,
                    [this] { traceWindow(QStringLiteral("aboutToQuit")); });
        }
    }
    ~TerminalController() override {
        if (focusFilterAttached_) setTerminalFocusFiltering(false);
    }
private:
    void traceWindow(const QString& action) const {
        if (!qEnvironmentVariableIsSet("P4VPATCH_TRACE_WINDOW")) return;
        QFile file(QDir::temp().filePath(QStringLiteral("p4vpatch-window-events.txt")));
        if (!file.open(QIODevice::Append | QIODevice::Text)) return;
        QTextStream out(&file);
        out << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << ' ' << action
            << " appSignalsBlocked=" << (qApp && qApp->signalsBlocked())
            << " terminalAttached=" << (nativeProbePage_ && tabs_ && tabs_->indexOf(nativeProbePage_) >= 0)
            << " filteredFocus=" << terminalFocusSkips() << '\n';
    }
    void diagnosticStatus(const QString& status) {
        if (!qEnvironmentVariableIsSet("P4VPATCH_INSPECT_NATIVE_TABS") || status == lastStatus_) return;
        lastStatus_ = status;
        QFile file(QDir::temp().filePath(QStringLiteral("p4vpatch-native-tabs.txt")));
        if (file.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream out(&file);
            out << status << '\n';
        }
    }
    QTabWidget* logTabs() const {
        if (!main_) return nullptr;
        QTabWidget* result = nullptr;
        // P4V changes this widget's objectName as layouts change. Identify it
        // by type AND its Log tab instead; fail closed on ambiguous matches.
        for (auto* tabs : main_->findChildren<QTabWidget*>()) {
            if (std::strcmp(tabs->metaObject()->className(), "UILogTabWidget") != 0) continue;
            bool containsLog = false;
            for (int i = 0; i < tabs->count(); ++i)
                if (tabs->tabText(i) == QStringLiteral("Log")) containsLog = true;
            if (!containsLog) continue;
            if (result) return nullptr;
            result = tabs;
        }
        return result;
    }
    bool findHost() {
        if (inspected_ || menuAction_) return true;
        if (!main_) return false;
        QTabWidget* host = logTabs();
        if (!host || !main_->menuBar()) {
            diagnosticStatus(QStringLiteral("UIWorkspace2 found; waiting for unique UILogTabWidget with Log tab"));
            return false;
        }
        QMenu* viewMenu = nullptr;
        for (auto* action : main_->menuBar()->actions()) {
            if (plainText(action->text()).compare(QStringLiteral("View"), Qt::CaseInsensitive) == 0)
                viewMenu = action->menu();
        }
        if (!viewMenu) {
            diagnosticStatus(QStringLiteral("Log tab found; waiting for View menu"));
            return false;
        }
        const auto actions = viewMenu->actions();
        auto* log = static_cast<QAction*>(nullptr);
        for (auto* item : actions)
            if (plainText(item->text()).compare(QStringLiteral("Log"), Qt::CaseInsensitive) == 0) {
                log = item; break;
            }
        if (!log) {
            diagnosticStatus(QStringLiteral("View menu found; waiting for Log action"));
            return false;
        }
        if (qEnvironmentVariableIsSet("P4VPATCH_INSPECT_NATIVE_TABS")) {
            inspected_ = true;
            inspectHost(host);
            if (qEnvironmentVariableIsSet("P4VPATCH_TRACE_FOCUS_CONNECTIONS") && qApp) {
                QFile file(QDir::temp().filePath(QStringLiteral("p4vpatch-focus-connections.txt")));
                if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    focusConnectionLog = &file;
                    const auto previous = qInstallMessageHandler(focusConnectionMessage);
                    qApp->dumpObjectInfo();
                    qInstallMessageHandler(previous);
                    focusConnectionLog = nullptr;
                }
            }
            return true; // read-only diagnostic: do not create a terminal page
        }
        if (qEnvironmentVariableIsSet("P4VPATCH_DISABLE_TERMINAL")) return true;
        // P4V's focusChanged receiver is not safe with an unregistered page.
        // Filter only that Qt signal; leave all other application signals intact.
        // Do not expose the tab if the verified Qt import cannot be patched.
        if (!installTerminalFocusHook()) return true;
        // The placeholder page is opt-in for diagnostics only.
        const bool realTerminal = !qEnvironmentVariableIsSet("P4VPATCH_EXPERIMENT_NATIVE_TAB");
        tabs_ = host;
        nativeCloseGuard_ = true;
        QApplication::instance()->installEventFilter(this);
        menuAction_ = new QAction(QStringLiteral("Terminal"), viewMenu);
        menuAction_->setObjectName(QStringLiteral("p4vpatch.terminalAction"));
        menuAction_->setCheckable(true);
        const int index = actions.indexOf(log);
        if (index + 1 < actions.size()) viewMenu->insertAction(actions[index + 1], menuAction_);
        else viewMenu->addAction(menuAction_);
        connect(host, &QTabWidget::currentChanged, this, [this, host](int index) {
            if (menuAction_ && nativeProbePage_) {
                const int terminalIndex = host->indexOf(nativeProbePage_);
                const bool active = index == terminalIndex && terminalIndex >= 0 &&
                                    host->tabBar()->isTabVisible(terminalIndex);
                menuAction_->setChecked(active);
                if (active) QTimer::singleShot(0, this, [this] {
                    if (!nativeProbePage_ || !menuAction_ || !menuAction_->isChecked()) return;
                    auto* view = nativeProbePage_->findChild<QWebEngineView*>();
                    if (view) {
                        view->setFocus(Qt::OtherFocusReason);
                        view->page()->runJavaScript(QStringLiteral("window.p4vpatchFocusTerminal?.()"));
                    }
                });
            }
        });
        connect(menuAction_, &QAction::triggered, this, [this, host, realTerminal](bool checked) {
            if (!checked) { hideNative(host); return; }
            // QMenu is still handling the click; don't shift focus inside it.
            QTimer::singleShot(0, this, [this, host, realTerminal] {
                if (menuAction_ && menuAction_->isChecked()) openNative(host, realTerminal);
            });
        });
        return true;
    }
    void hideNative(QTabWidget* host) {
        if (!nativeProbePage_ || host->indexOf(nativeProbePage_) < 0) return;
        const int index = host->indexOf(nativeProbePage_);
        if (host->currentIndex() == index) {
            for (int i = 0; i < host->count(); ++i)
                if (host->tabText(i) == QStringLiteral("Log")) { host->setCurrentIndex(i); break; }
        }
        host->tabBar()->setTabVisible(index, false);
    }
    void openNative(QTabWidget* host, bool realTerminal) {
        if (detachedPage_) {
            // Close was attempted but P4V kept the window open. Do not add a
            // second session to a host whose teardown state is unknown.
            if (menuAction_) menuAction_->setChecked(false);
            return;
        }
        if (!nativeProbePage_) {
            auto* page = new QWidget(host);
            if (realTerminal) {
                page->setAttribute(Qt::WA_DontCreateNativeAncestors);
                page->setAttribute(Qt::WA_NativeWindow);
            }
            page->setObjectName(QStringLiteral("p4vpatch.nativeProbePage"));
            auto* layout = new QVBoxLayout(page);
            layout->setContentsMargins(0, 0, 0, 0);
            if (realTerminal) {
                auto* session = new TerminalSession(this);
                QString error;
                if (!session->start(&error)) {
                    delete page; delete session;
                    menuAction_->setChecked(false);
                    QMessageBox::warning(main_, QStringLiteral("Terminal unavailable"), error);
                    return;
                }
                session_ = session;
                auto* view = new QWebEngineView(page);
                view->setObjectName(QStringLiteral("p4vpatch.nativeTerminalView"));
                layout->addWidget(view);
                auto* channel = new QWebChannel(view);
                channel->registerObject(QStringLiteral("terminal"), session);
                auto* webPage = new TerminalPage(view);
                view->setPage(webPage);
                webPage->setWebChannel(channel);
                connect(view, &QWebEngineView::loadFinished, this, [this, view](bool ok) {
                    if (!ok && main_) QMessageBox::warning(main_, QStringLiteral("Terminal"),
                        QStringLiteral("The bundled terminal page could not be loaded."));
                    else if (ok && tabs_ && nativeProbePage_ && menuAction_ &&
                             menuAction_->isChecked() && tabs_->currentWidget() == nativeProbePage_)
                        view->setFocus(Qt::OtherFocusReason);
                });
                QTimer::singleShot(0, view, [view] { view->setUrl(pageUrl()); });
            } else {
                layout->addWidget(new QLabel(QStringLiteral("Native tab probe (no terminal session)"), page));
                auto* field = new QLineEdit(page);
                field->setObjectName(QStringLiteral("p4vpatch.nativeProbeInput"));
                field->setAccessibleName(QStringLiteral("Native probe input"));
                layout->addWidget(field);
            }
            nativeProbePage_ = page;
            setTerminalFocusFiltering(true);
            focusFilterAttached_ = true;
            const int index = host->addTab(page, QStringLiteral("Terminal"));
            // P4V recreates its own QToolButton close widget after selection,
            // so replacing it is ineffective. The GUI event filter intercepts
            // clicks on this tab's *current* button before P4V's handler runs.
        }
        const int index = host->indexOf(nativeProbePage_);
        if (index < 0) { menuAction_->setChecked(false); return; }
        host->tabBar()->setTabVisible(index, true);
        host->setCurrentIndex(index);
    }
    void inspectHost(QTabWidget* host) {
        QFile file(QDir::temp().filePath(QStringLiteral("p4vpatch-native-tabs.txt")));
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) return;
        QTextStream out(&file);
        out << "Qt " << qVersion() << "\n";
        auto methods = [&out](const QObject* object) {
            if (!object) return;
            const QMetaObject* meta = object->metaObject();
            out << "  class " << meta->className() << "\n";
            for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
                const auto method = meta->method(i);
                out << "    " << method.methodSignature() << "\n";
            }
        };
        out << "Log container:\n";
        methods(host);
        out << "tab bar:\n";
        methods(host->tabBar());
        for (int i = 0; i < host->count(); ++i) {
            const QString name = host->tabText(i);
            out << "tab " << i << " "
                << (name == QStringLiteral("Log") || name == QStringLiteral("Dashboard") ? name : QStringLiteral("[other]"))
                << ":\n";
            methods(host->widget(i));
        }
        out << "ancestor widget classes / tab-related methods:\n";
        for (auto* widget = host->parentWidget(); widget; widget = widget->parentWidget()) {
            const QMetaObject* meta = widget->metaObject();
            out << "  " << meta->className() << "\n";
            for (int i = meta->methodOffset(); i < meta->methodCount(); ++i) {
                const QByteArray signature = meta->method(i).methodSignature();
                if (signature.contains("Tab") || signature.contains("tab") ||
                    signature.contains("Log") || signature.contains("log") ||
                    signature.contains("Dock") || signature.contains("dock") ||
                    signature.contains("Close") || signature.contains("close"))
                    out << "    " << signature << "\n";
            }
        }
    }
    bool eventFilter(QObject* object, QEvent* event) override {
        if (object == main_ && event->type() == QEvent::Close) traceWindow(QStringLiteral("Close begin"));
        if (object == main_ && event->type() == QEvent::Close && nativeProbePage_ && tabs_) {
            // P4V does not know how to destroy a foreign page. Remove it from
            // its tab/stack before the main window starts destroying children.
            // Keep the detached widget alive until process exit: deleting it
            // inside P4V's close handler is also unsafe.
            const int index = tabs_->indexOf(nativeProbePage_);
            if (index >= 0) {
                QWidget* page = nativeProbePage_;
                tabs_->removeTab(index);
                page->hide();
                page->setParent(nullptr);
                detachedPage_ = page;
                nativeProbePage_ = nullptr;
                if (focusFilterAttached_) {
                    focusFilterAttached_ = false;
                    setTerminalFocusFiltering(false);
                }
                if (menuAction_) menuAction_->setChecked(false);
                traceWindow(QStringLiteral("Close detached"));
            }
        }
        if (object == main_ && qEnvironmentVariableIsSet("P4VPATCH_TRACE_WINDOW") &&
            (event->type() == QEvent::Hide || event->type() == QEvent::Show)) {
            QFile file(QDir::temp().filePath(QStringLiteral("p4vpatch-window-events.txt")));
            if (file.open(QIODevice::Append | QIODevice::Text)) {
                QTextStream out(&file);
                out << QDateTime::currentDateTime().toString(Qt::ISODateWithMs) << ' '
                    << (event->type() == QEvent::Hide ? "Hide" : "Show") << '\n';
            }
        }
        if (nativeCloseGuard_ && nativeProbePage_ && tabs_ && menuAction_ &&
            (event->type() == QEvent::MouseButtonPress ||
             event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::KeyPress)) {
            const int index = tabs_->indexOf(nativeProbePage_);
            if (index >= 0 && menuAction_->isChecked() &&
                object == tabs_->tabBar()->tabButton(index, QTabBar::RightSide)) {
                bool close = event->type() == QEvent::MouseButtonRelease;
                if (event->type() == QEvent::KeyPress) {
                    const int key = static_cast<QKeyEvent*>(event)->key();
                    close = key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter;
                }
                if (close) QTimer::singleShot(0, this, [this] {
                    if (menuAction_ && menuAction_->isChecked()) menuAction_->trigger();
                });
                return true; // do not enter P4V's private tabCloseRequest slot
            }
        }
        return false;
    }
    QPointer<QMainWindow> main_;
    QPointer<QAction> menuAction_;
    QPointer<QTabWidget> tabs_;
    QPointer<QWidget> nativeProbePage_;
    QPointer<QWidget> detachedPage_;
    QPointer<TerminalSession> session_;
    bool nativeCloseGuard_ = false;
    bool focusFilterAttached_ = false;
    bool inspected_ = false;
    QString lastStatus_;
};
} // namespace

void installTerminal(QMainWindow* main) {
    if (!main || std::strcmp(main->metaObject()->className(), "UIWorkspace2") != 0 ||
        main->property("p4vpatch.terminalController").toBool()) return;
    main->setProperty("p4vpatch.terminalController", true);
    new TerminalController(main);
}
#include "terminal.moc"
