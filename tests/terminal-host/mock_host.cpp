// Fake P4V host exercising the production terminal without injecting P4V.
#include "../../src/terminal.h"
#include "../../src/qt_focus_hook.h"
#include <windows.h>
#include <tlhelp32.h>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QSplitter>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <cstdio>

class UIWorkspace2 : public QMainWindow {
    Q_OBJECT
};
class UILogTabWidget : public QTabWidget {
    Q_OBJECT
};
class WindowEvents : public QObject {
public:
    int hides = 0, shows = 0;
protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Hide) ++hides;
        if (event->type() == QEvent::Show) ++shows;
        return false;
    }
};
static int childCmdCount() {
    auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return -1;
    PROCESSENTRY32W entry{sizeof(entry)};
    int count = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (entry.th32ParentProcessID == GetCurrentProcessId() &&
                _wcsicmp(entry.szExeFile, L"cmd.exe") == 0) ++count;
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return count;
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    UIWorkspace2 main;
    main.resize(900, 520);
    auto* menu = main.menuBar()->addMenu(QStringLiteral("&View"));
    menu->addAction(QStringLiteral("Lo&g"));
    auto* splitter = new QSplitter(Qt::Vertical, &main);
    splitter->addWidget(new QWidget(splitter));
    auto* tabs = new UILogTabWidget;
    tabs->addTab(new QWidget(tabs), QStringLiteral("Log"));
    tabs->addTab(new QWidget(tabs), QStringLiteral("Dashboard"));
    tabs->setTabsClosable(true);
    int hostCloseRequests = 0;
    QObject::connect(tabs, &QTabWidget::tabCloseRequested, &main,
                     [&](int) { ++hostCloseRequests; });
    splitter->addWidget(tabs);
    main.setCentralWidget(splitter);
    main.show();
    WindowEvents events;
    main.installEventFilter(&events);
    installTerminal(&main);
    if (qEnvironmentVariableIsSet("P4VPATCH_INSPECT_NATIVE_TABS")) {
        if (main.findChild<QAction*>(QStringLiteral("p4vpatch.terminalAction")) ||
            !QFile::exists(QDir::temp().filePath(QStringLiteral("p4vpatch-native-tabs.txt")))) return 2;
        std::puts("PASS: read-only P4V tab inspection mode");
        return 0;
    }
    auto* action = main.findChild<QAction*>(QStringLiteral("p4vpatch.terminalAction"));
    const bool probe = qEnvironmentVariableIsSet("P4VPATCH_EXPERIMENT_NATIVE_TAB");
    if (qEnvironmentVariableIsSet("P4VPATCH_DISABLE_TERMINAL")) {
        if (action || tabs->count() != 2 || childCmdCount()) return 3;
        std::puts("PASS: terminal explicitly disabled");
        return 0;
    }
    if (!action) { std::fprintf(stderr, "Native Terminal failed closed in mock host\n"); return 3; }
    action->trigger();
    action->trigger(); // cancel opening before the menu returns
    QTimer::singleShot(0, &app, [&] {
        if (tabs->count() != 2 || childCmdCount()) { app.exit(4); return; }
        action->trigger();
        QTimer::singleShot(0, &app, [&] {
            const int index = tabs->count() - 1;
            if (tabs->count() != 3 || tabs->tabText(index) != QStringLiteral("Terminal") ||
                tabs->currentIndex() != index || !tabs->findChild<QWidget*>(QStringLiteral("p4vpatch.nativeProbePage")) ||
                !tabs->tabBar()->tabButton(index, QTabBar::RightSide) || hostCloseRequests) {
                std::fprintf(stderr, "Native Terminal page not created\n"); app.exit(5); return;
            }
            auto finish = [&, index] {
                auto* button = tabs->tabBar()->tabButton(index, QTabBar::RightSide);
                const QPoint click(button->width() / 2, button->height() / 2);
                QMouseEvent press(QEvent::MouseButtonPress, click, Qt::LeftButton,
                                  Qt::LeftButton, Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, click, Qt::LeftButton,
                                    Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(button, &press);
                QApplication::sendEvent(button, &release);
                QTimer::singleShot(0, &app, [&, index] {
                    if (tabs->count() != 3 || tabs->tabBar()->isTabVisible(index) || hostCloseRequests ||
                        action->isChecked() || (!probe && childCmdCount() != 1)) {
                        std::fprintf(stderr, "Native close must hide without P4V close request\n"); app.exit(6); return;
                    }
                    action->trigger();
                    QTimer::singleShot(0, &app, [&, index] {
                        if (tabs->count() != 3 || !tabs->tabBar()->isTabVisible(index) ||
                            tabs->currentIndex() != index || hostCloseRequests ||
                            (!probe && childCmdCount() != 1)) { app.exit(7); return; }
                        tabs->setCurrentIndex(0);
                        if (action->isChecked()) { app.exit(8); return; }
                        tabs->setCurrentIndex(index);
                        if (!action->isChecked()) { app.exit(9); return; }
                        action->trigger();
                        if (QApplication::instance()->signalsBlocked() || (!probe && terminalFocusSkips() < 1)) {
                            std::fprintf(stderr, "Focus signal interception not active\n"); app.exit(16); return;
                        }
                        if (events.hides || events.shows) {
                            std::fprintf(stderr, "Main window flashed: %d hides, %d shows\n",
                                         events.hides, events.shows); app.exit(10); return;
                        }
                        main.close();
                        if (tabs->count() != 2 || hostCloseRequests) {
                            std::fprintf(stderr, "Main close must detach foreign page\n"); app.exit(15); return;
                        }
                        std::puts("PASS: native tab, rendered shell output, focus hook, close/reopen and teardown");
                        app.exit(0);
                    });
                });
            };
            if (probe) { finish(); return; }
            auto* view = tabs->findChild<QWebEngineView*>(QStringLiteral("p4vpatch.nativeTerminalView"));
            if (!view || childCmdCount() != 1) { app.exit(11); return; }
            QObject::connect(view, &QWebEngineView::loadFinished, &app, [&, view, finish](bool ok) {
                if (!ok) { app.exit(12); return; }
                QTimer::singleShot(1000, &app, [&, view, finish] {
                    view->page()->runJavaScript(QStringLiteral("({ready:typeof window.p4vpatchFocusTerminal === 'function',prompt:document.body.innerText.includes('>')})"),
                                                [&, finish](const QVariant& result) {
                        const auto state = result.toMap();
                        if (!state.value(QStringLiteral("ready")).toBool() ||
                            !state.value(QStringLiteral("prompt")).toBool()) {
                            std::fprintf(stderr, "xterm not ready or shell output not parsed\n");
                            app.exit(13); return;
                        }
                        finish();
                    });
                });
            });
        });
    });
    QTimer::singleShot(20000, &app, [&] { std::fprintf(stderr, "mock host timeout\n"); app.exit(14); });
    return app.exec();
}
#include "mock_host.moc"
