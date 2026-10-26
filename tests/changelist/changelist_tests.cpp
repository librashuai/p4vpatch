#include "../../src/changelist_links.h"
#include "../../src/preferences.h"
#include "../../src/submit_ai.h"
#include <QApplication>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QSyntaxHighlighter>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocumentFragment>
#include <QTextLayout>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <cstdio>
#include <functional>

class UIChangeWindow final : public QDialog {
    Q_OBJECT
};
class UISubmitWindow final : public QDialog {
    Q_OBJECT
};
class P4VPreferencesDialog final : public QDialog {
    Q_OBJECT
};
class ConnectionsPage final : public QWidget {
    Q_OBJECT
};
namespace Gui {
class ExpandableTextEdit final : public QTextEdit {
    Q_OBJECT
public:
    using QTextEdit::QTextEdit;
};
}
class UrlRecorder final : public QObject {
    Q_OBJECT
public:
    QList<QUrl> urls;
public slots:
    void open(const QUrl& url) { urls.append(url); }
};

namespace {
int failures = 0;
void check(bool ok, const char* message) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", message);
    if (!ok) ++failures;
}
void events() { for (int i = 0; i < 12; ++i) QApplication::processEvents(); }
bool waitFor(const std::function<bool()>& condition, int ms = 6000) {
    QElapsedTimer clock;
    clock.start();
    while (!condition() && clock.elapsed() < ms) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return condition();
}
struct SubmitFixture {
    UISubmitWindow window;
    QGridLayout* grid = nullptr;
    QLabel* descLabel = nullptr;
    Gui::ExpandableTextEdit* desc = nullptr;
    QTreeView* filesTree = nullptr;
    QStandardItemModel* filesModel = nullptr;
    SubmitFixture(const QString& title = QStringLiteral("Submit Changelist: 4660363 (pengshuai.yin_02,  p4.hon.jingle.cn:1666,  pengshuai.yin)")) {
        window.setWindowTitle(title);
        window.resize(700, 500);
        auto* outer = new QVBoxLayout(&window);
        grid = new QGridLayout();
        outer->addLayout(grid);
        auto* descButton = new QPushButton(&window);
        descButton->setObjectName(QStringLiteral("descButton"));
        descButton->hide();
        descLabel = new QLabel(QStringLiteral("Write a changelist description"), &window);
        descLabel->setObjectName(QStringLiteral("descLabel"));
        auto* descContainer = new QWidget(&window);
        auto* descLayout = new QVBoxLayout(descContainer);
        desc = new Gui::ExpandableTextEdit(descContainer);
        desc->setObjectName(QStringLiteral("desc"));
        desc->setReadOnly(false);
        descLayout->addWidget(desc);
        grid->addWidget(descButton, 0, 0);
        grid->addWidget(descLabel, 0, 1);
        grid->addWidget(descContainer, 1, 1);

        auto* fileContainer = new QWidget(&window);
        fileContainer->setObjectName(QStringLiteral("fileContainer"));
        auto* fileLayout = new QVBoxLayout(fileContainer);
        filesTree = new QTreeView(fileContainer);
        filesTree->setObjectName(QStringLiteral("filesTree"));
        filesModel = new QStandardItemModel(0, 3, filesTree);
        filesTree->setModel(filesModel);
        fileLayout->addWidget(filesTree);
        grid->addWidget(fileContainer, 3, 1);
    }
    void addFileRow(const QString& file, const QString& folder, bool checked) {
        auto* c0 = new QStandardItem();
        c0->setCheckable(true);
        c0->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        auto* c1 = new QStandardItem(file);
        auto* c2 = new QStandardItem(folder);
        filesModel->appendRow({c0, c1, c2});
    }
    void show() { window.show(); events(); }
};
struct PreferencesFixture {
    P4VPreferencesDialog dialog;
    QSplitter* splitter = nullptr;
    QTreeWidget* tree = nullptr;
    QStackedWidget* stack = nullptr;
    QDialogButtonBox* buttons = nullptr;
    PreferencesFixture() {
        auto* layout = new QVBoxLayout(&dialog);
        splitter = new QSplitter(&dialog);
        splitter->setObjectName(QStringLiteral("preferenceSplitter"));
        tree = new QTreeWidget(splitter);
        auto* connItem = new QTreeWidgetItem(tree);
        connItem->setText(0, QStringLiteral("Connections"));
        stack = new QStackedWidget(splitter);
        stack->addWidget(new ConnectionsPage());
        layout->addWidget(splitter);
        buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, &dialog);
        layout->addWidget(buttons);
    }
};
struct Fixture {
    UIChangeWindow window;
    Gui::ExpandableTextEdit* source = new Gui::ExpandableTextEdit(&window);
    QTextBrowser* browser = new QTextBrowser(source);
    Fixture(const QString& title = QStringLiteral("Submitted Changelist: 123 (test)")) {
        window.setWindowTitle(title);
        window.resize(700, 400);
        auto* layout = new QVBoxLayout(&window);
        layout->addWidget(source);
        source->setObjectName(QStringLiteral("expanding QTE"));
        source->setReadOnly(true);
        browser->setObjectName(QStringLiteral("markdownBrowser"));
        auto* nested = new QVBoxLayout(source);
        nested->setContentsMargins(0, 0, 0, 0);
        nested->addWidget(browser);
        browser->setOpenLinks(false); // Existing link clicks are observable without navigation/network.
    }
    void text(const QString& value) {
        source->setPlainText(value);
        browser->setPlainText(value);
    }
    void show() { window.show(); events(); }
};
bool highlighted(QTextBrowser* browser, int position) {
    const auto block = browser->document()->findBlock(position);
    for (const auto& range : block.layout()->formats())
        if (position >= block.position() + range.start && position < block.position() + range.start + range.length &&
            range.format.fontUnderline()) return true;
    return false;
}
QPoint pointAt(QTextBrowser* browser, int position) {
    QTextCursor cursor(browser->document());
    cursor.setPosition(position);
    return browser->cursorRect(cursor).center() + QPoint(2, 0);
}
void click(QTextBrowser* browser, int position, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QTest::mouseClick(browser->viewport(), Qt::LeftButton, modifiers, pointAt(browser, position));
    events();
}
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    UrlRecorder recorder;
    QDesktopServices::setUrlHandler(QStringLiteral("https"), &recorder, "open");
    {
        Fixture fixture;
        const QString original = QStringLiteral("中文😀 [Jira]BR-339863\n[Jira]BR-1 [Jira]BR-123456789\n[Jira]BR-12x BR-123 [jira]BR-1 <img src=x>");
        fixture.text(original);
        fixture.browser->document()->setModified(false);
        fixture.show(); // Exercise installation into an already open window.
        const auto sourceHtml = fixture.source->toHtml();
        const auto browserHtml = fixture.browser->toHtml();
        const auto geometry = fixture.browser->geometry();
        int sourceChanges = 0, displayChanges = 0;
        QObject::connect(fixture.source, &QTextEdit::textChanged, &app, [&] { ++sourceChanges; });
        QObject::connect(fixture.browser, &QTextEdit::textChanged, &app, [&] { ++displayChanges; });
        installChangelistLinks(&app);
        installChangelistLinks(&app);
        events();
        const int first = original.indexOf(QStringLiteral("BR-339863"));
        check(highlighted(fixture.browser, first), "existing submitted description gets a highlighted Jira token");
        check(!highlighted(fixture.browser, original.indexOf(QStringLiteral("[Jira]"))), "Jira prefix remains unchanged");
        check(highlighted(fixture.browser, original.indexOf(QStringLiteral("BR-1 "))) &&
              highlighted(fixture.browser, original.indexOf(QStringLiteral("BR-123456789"))), "multiple tokens and variable digit lengths supported");
        check(!highlighted(fixture.browser, original.indexOf(QStringLiteral("BR-12x"))) &&
              !highlighted(fixture.browser, original.indexOf(QStringLiteral("BR-123 "))) &&
              !highlighted(fixture.browser, original.indexOf(QStringLiteral("[jira]")) + 6), "malformed, untagged and wrong-case tokens ignored");
        check(fixture.source->toHtml() == sourceHtml && sourceChanges == 0, "source content and signals untouched");
        check(fixture.browser->toHtml() == browserHtml && fixture.browser->toPlainText() == original && displayChanges == 0,
              "highlighting leaves display document content, markup and textChanged signals untouched");
        check(!fixture.browser->document()->isModified(), "highlighting leaves modified flag untouched");
        check(!fixture.browser->document()->isUndoAvailable() && fixture.browser->geometry() == geometry,
              "no undo entries or layout replacement");
        check(fixture.browser->findChildren<QObject*>(QStringLiteral("p4vpatch.jiraLinks")).size() == 1,
              "repeated installation is idempotent");
        QTest::mouseMove(fixture.browser->viewport(), pointAt(fixture.browser, first));
        check(fixture.browser->viewport()->cursor().shape() == Qt::PointingHandCursor, "hover shows link hand cursor");
        click(fixture.browser, first);
        check(recorder.urls.size() == 1 && recorder.urls.last() == QUrl(QStringLiteral("https://jira.garena.com/browse/BR-339863")),
              "Jira click opens exactly the constructed HTTPS URL");
        check(fixture.browser->source().isEmpty() && fixture.browser->toPlainText() == original && !fixture.browser->openLinks(),
              "click does not navigate browser or change its existing link settings");
        click(fixture.browser, original.indexOf(QStringLiteral("[Jira]")));
        click(fixture.browser, first, Qt::ShiftModifier);
        check(recorder.urls.size() == 1, "prefix click and shift-selection do not open Jira");
        auto cursor = fixture.browser->textCursor();
        cursor.clearSelection();
        fixture.browser->setTextCursor(cursor);
        const auto from = pointAt(fixture.browser, first);
        const auto to = pointAt(fixture.browser, first + 7);
        QTest::mousePress(fixture.browser->viewport(), Qt::LeftButton, Qt::NoModifier, from);
        QMouseEvent move(QEvent::MouseMove, QPointF(to), QPointF(fixture.browser->viewport()->mapToGlobal(to)),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(fixture.browser->viewport(), &move);
        QTest::mouseRelease(fixture.browser->viewport(), Qt::LeftButton, Qt::NoModifier, to);
        check(recorder.urls.size() == 1 && fixture.browser->textCursor().hasSelection(), "dragging across Jira selects text without opening URL");
        cursor.select(QTextCursor::Document);
        check(QTextDocumentFragment(cursor).toPlainText() == original, "selected/copied plain text stays identical (no clipboard modified)");
        cursor.clearSelection();
        fixture.browser->setTextCursor(cursor);
        QTest::mouseClick(fixture.browser->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(600, from.y()));
        check(recorder.urls.size() == 1, "empty space after Jira line is not clickable");

        fixture.text(QStringLiteral("[Jira]BR-42"));
        events();
        check(highlighted(fixture.browser, 6), "content refresh rebuilds highlighting");
        click(fixture.browser, 6);
        check(recorder.urls.last().path() == QStringLiteral("/browse/BR-42"), "refreshed text has no stale click target");
        fixture.window.setWindowTitle(QStringLiteral("Pending Changelist: 123"));
        events();
        const auto count = recorder.urls.size();
        click(fixture.browser, 6);
        check(!highlighted(fixture.browser, 6) && recorder.urls.size() == count, "changing to pending disables highlight and click handling");
        fixture.window.setWindowTitle(QStringLiteral("Submitted Changelist: 456"));
        events();
        check(highlighted(fixture.browser, 6), "returning to submitted re-enables links");
        fixture.source->setReadOnly(false);
        events();
        click(fixture.browser, 6);
        check(!highlighted(fixture.browser, 6) && recorder.urls.size() == count, "editable outer description is never enhanced");
        fixture.source->setReadOnly(true);
        fixture.browser->setReadOnly(false);
        events();
        click(fixture.browser, 6);
        check(!highlighted(fixture.browser, 6) && recorder.urls.size() == count, "editable inner browser is never enhanced");
    }
    {
        QTextDocument replacement;
        Fixture fixture;
        fixture.text(QStringLiteral("[Jira]BR-1"));
        fixture.show();
        check(highlighted(fixture.browser, 6), "new windows discovered after watcher installation");
        replacement.setPlainText(QStringLiteral("[Jira]BR-987"));
        fixture.browser->setDocument(&replacement);
        events();
        check(highlighted(fixture.browser, 6), "replacement document is enhanced");
        click(fixture.browser, 6);
        check(recorder.urls.last().path() == QStringLiteral("/browse/BR-987"), "replacement document click uses new content");
        auto palette = fixture.browser->palette();
        palette.setColor(QPalette::Link, QColor("#123456"));
        fixture.browser->setPalette(palette);
        events();
        const auto formats = replacement.firstBlock().layout()->formats();
        check(!formats.isEmpty() && formats.first().format.foreground().color() == QColor("#123456"), "link color tracks theme palette");
        QTextEdit::ExtraSelection search;
        search.cursor = QTextCursor(&replacement);
        search.cursor.setPosition(0);
        search.cursor.setPosition(3, QTextCursor::KeepAnchor);
        search.format.setBackground(Qt::yellow);
        fixture.browser->setExtraSelections({search});
        fixture.browser->viewport()->repaint();
        events();
        const auto merged = fixture.browser->extraSelections();
        check(merged.size() == 1 && merged.first().format.background().color() == QColor(Qt::yellow) && highlighted(fixture.browser, 6),
              "host search selections and Jira highlighting coexist without interference");
    }
    {
        Fixture fixture;
        fixture.browser->setHtml(QStringLiteral("<b>Bold</b> <a href=\"https://example.invalid/existing\">[Jira]BR-99</a> [Jira]BR-88"));
        const QString html = fixture.browser->toHtml();
        fixture.show();
        const QString plain = fixture.browser->toPlainText();
        const int existing = plain.indexOf(QStringLiteral("BR-99"));
        const int added = plain.indexOf(QStringLiteral("BR-88"));
        check(!highlighted(fixture.browser, existing) && highlighted(fixture.browser, added) && fixture.browser->toHtml() == html,
              "existing Markdown/HTML links and bold formatting preserved");
        QUrl existingUrl;
        QObject::connect(fixture.browser, &QTextBrowser::anchorClicked, &app, [&](const QUrl& url) { existingUrl = url; });
        const auto count = recorder.urls.size();
        click(fixture.browser, existing);
        check(existingUrl == QUrl(QStringLiteral("https://example.invalid/existing")) && recorder.urls.size() == count,
              "existing links retain native click handling");
    }
    {
        Fixture fixture;
        fixture.text(QString(80, '\n') + QStringLiteral("[Jira]BR-555"));
        fixture.show();
        fixture.browser->verticalScrollBar()->setValue(fixture.browser->verticalScrollBar()->maximum());
        events();
        const auto count = recorder.urls.size();
        click(fixture.browser, fixture.browser->toPlainText().indexOf(QStringLiteral("BR-555")));
        check(recorder.urls.size() == count + 1 && recorder.urls.last().path() == QStringLiteral("/browse/BR-555"),
              "hit testing accounts for vertical scrolling");
        fixture.browser->setLineWrapMode(QTextEdit::NoWrap);
        fixture.text(QString(180, 'x') + QStringLiteral(" [Jira]BR-556"));
        events();
        fixture.browser->horizontalScrollBar()->setValue(fixture.browser->horizontalScrollBar()->maximum());
        events();
        click(fixture.browser, fixture.browser->toPlainText().indexOf(QStringLiteral("BR-556")));
        check(recorder.urls.last().path() == QStringLiteral("/browse/BR-556"), "hit testing accounts for horizontal scrolling");
    }
    {
        class ExistingHighlighter final : public QSyntaxHighlighter {
        public:
            using QSyntaxHighlighter::QSyntaxHighlighter;
            void highlightBlock(const QString&) override {}
        };
        Fixture fixture;
        fixture.text(QStringLiteral("[Jira]BR-1"));
        new ExistingHighlighter(fixture.browser->document());
        fixture.show();
        const auto count = recorder.urls.size();
        click(fixture.browser, 6);
        check(!highlighted(fixture.browser, 6) && recorder.urls.size() == count,
              "preexisting syntax highlighter is not overwritten");
    }
    for (const auto& title : {QStringLiteral("Pending Changelist: 123"), QStringLiteral("Submit Changelist"), QStringLiteral("Other Window")}) {
        Fixture fixture(title);
        fixture.text(QStringLiteral("[Jira]BR-1"));
        fixture.show();
        check(!highlighted(fixture.browser, 6), "non-submitted windows are untouched");
    }
    {
        Fixture fixture;
        fixture.browser->setObjectName(QStringLiteral("someOtherBrowser"));
        fixture.text(QStringLiteral("[Jira]BR-1"));
        fixture.show();
        check(!highlighted(fixture.browser, 6), "unrecognized browser is untouched");
    }
    if (argc >= 2) {
        const QString fakeCli = QString::fromLocal8Bit(argv[1]);
        QTemporaryDir tempDir;
        const QString settingsPath = tempDir.filePath(QStringLiteral("p4vpatch.ini"));
        const QString statePath = tempDir.filePath(QStringLiteral("test-state.ini"));
        qputenv("P4VPATCH_SETTINGS_PATH", settingsPath.toUtf8());
        qputenv("P4VPATCH_CODEX_EXECUTABLE", fakeCli.toUtf8());
        qputenv("P4VPATCH_P4_EXECUTABLE", fakeCli.toUtf8());
        qputenv("P4VPATCH_TEST_STATE", statePath.toUtf8());
        qputenv("P4VPATCH_SUPPRESS_AI_MESSAGEBOX", "1");

        // 1. Parser and description cleaner tests
        const auto parsedNumbered = parseSubmitWindowTitle(
            QStringLiteral("Submit Changelist: 4660363 (pengshuai.yin_02,  p4.hon.jingle.cn:1666,  pengshuai.yin)"));
        check(parsedNumbered && parsedNumbered->changelist == QStringLiteral("4660363") &&
              parsedNumbered->client == QStringLiteral("pengshuai.yin_02") &&
              parsedNumbered->server == QStringLiteral("p4.hon.jingle.cn:1666") &&
              parsedNumbered->user == QStringLiteral("pengshuai.yin"),
              "parse numbered Submit Changelist window title");
        const auto parsedDefault = parseSubmitWindowTitle(
            QStringLiteral("Submit Changelist: default (ws_client, ssl:p4:1666, alice)"));
        check(parsedDefault && parsedDefault->changelist == QStringLiteral("default") &&
              parsedDefault->client == QStringLiteral("ws_client") &&
              parsedDefault->server == QStringLiteral("ssl:p4:1666") &&
              parsedDefault->user == QStringLiteral("alice"),
              "parse default Submit Changelist window title");
        check(!parseSubmitWindowTitle(QStringLiteral("Submitted Changelist: 123")) &&
              !parseSubmitWindowTitle(QStringLiteral("Submit Changelist: 123 (-bad, p4:1666, alice)")),
              "reject non-submit or option-injected window titles");
        check(cleanGeneratedDescription(QStringLiteral("```text\n修复配置加载问题\n```")) == QStringLiteral("修复配置加载问题"),
              "strip markdown code fences from generated description");

        // 2. Preferences -> Patch -> Codex model fetching and saving
        {
            PreferencesFixture prefs;
            installPatchPreferences(&prefs.dialog);
            auto* modelCombo = prefs.dialog.findChild<QComboBox*>(QStringLiteral("p4vpatch.codexModel"));
            auto* refreshBtn = prefs.dialog.findChild<QPushButton*>(QStringLiteral("p4vpatch.codexRefreshModels"));
            auto* statusLabel = prefs.dialog.findChild<QLabel*>(QStringLiteral("p4vpatch.codexModelStatus"));
            check(modelCombo && refreshBtn && statusLabel, "Patch preferences includes Codex model selector and refresh button");
            check(waitFor([&] { return modelCombo->count() == 3 && refreshBtn->isEnabled(); }),
                  "Codex model list fetched asynchronously via app-server model/list API (hidden models excluded)");
            check(modelCombo->itemText(0) == QStringLiteral("gpt-6-astra") &&
                  modelCombo->itemText(1) == QStringLiteral("gpt-5.6-sol") &&
                  modelCombo->itemText(2) == QStringLiteral("gpt-5.6-luna") &&
                  modelCombo->currentText() == QStringLiteral("gpt-5.6-luna"),
                  "Visible Codex models populated and luna model selected by default");

            prefs.buttons->button(QDialogButtonBox::Apply)->click();
            events();
            check(codexModel() == QStringLiteral("gpt-5.6-luna"),
                  "Apply saves default selected luna model to p4vpatch.ini");
        }

        // 3. Submit Changelist AI button, waiting animation, diff passing, and description overwrite
        {
            installSubmitAiDescription(&app);
            SubmitFixture submit;
            submit.desc->setPlainText(QStringLiteral("FFI审计出包shelve"));
            submit.addFileRow(QStringLiteral("GameConfig.cs"),
                              QStringLiteral("//DTS/Branch/FF_PRELive/Client/Assets/Scripts/COWLauncher"), true);
            submit.addFileRow(QStringLiteral("IgnoredFile.cs"),
                              QStringLiteral("//DTS/Branch/FF_PRELive/Client/Assets/Scripts/COWLauncher"), false);
            submit.show();

            auto* aiBtn = submit.window.findChild<QPushButton*>(QStringLiteral("p4vpatch.aiGenerateDescription"));
            check(aiBtn != nullptr, "Submit Changelist window gets AI Generate button");
            inspectSubmitDialog(&submit.window);
            check(submit.window.findChildren<QPushButton*>(QStringLiteral("p4vpatch.aiGenerateDescription")).size() == 1,
                  "Submit Changelist AI button installation is idempotent");
            if (aiBtn) {
                check(aiBtn->geometry().left() > submit.descLabel->geometry().right(),
                      "AI Generate button is positioned to the right of Write a changelist description label");
                int descChangedCount = 0;
                QObject::connect(submit.desc, &QTextEdit::textChanged, &app, [&] { ++descChangedCount; });

                aiBtn->click();
                check(aiBtn->property("p4vpatch.aiLoading").toBool() &&
                      !aiBtn->isEnabled() &&
                      aiBtn->text() == QStringLiteral("Generating..."),
                      "Clicking AI Generate immediately starts waiting state and disables button");
                check(waitFor([&] { return aiBtn->property("p4vpatch.aiSpinnerStep").toInt() > 0; }, 2000),
                      "Waiting animation spinner advances frames while waiting for AI response");
                check(waitFor([&] { return !aiBtn->property("p4vpatch.aiLoading").toBool(); }),
                      "AI generation completes asynchronously");
                check(aiBtn->isEnabled() && aiBtn->text() == QStringLiteral("AI Generate"),
                      "Button state and label restored after generation completes");
                check(submit.desc->toPlainText() == QStringLiteral("更新 GameConfig 印度区正式服与调试开关配置") &&
                      descChangedCount > 0,
                      "Generated description overwrites the description text edit and emits textChanged");

                QSettings state(statePath, QSettings::IniFormat);
                state.sync();
                check(state.value(QStringLiteral("lastCodexModel")).toString() == QStringLiteral("gpt-5.6-luna"),
                      "Codex CLI invoked with the model configured in Patch preferences");
                const QString prompt = state.value(QStringLiteral("lastCodexPrompt")).toString();
                check(prompt.contains(QStringLiteral("GameConfig.cs")) &&
                      prompt.contains(QStringLiteral("ffi-version.freefireind.in")) &&
                      !prompt.contains(QStringLiteral("IgnoredFile.cs")),
                      "Codex CLI receives checked changelist diff content and excludes unchecked files");

                // 4. Error handling preserves existing description text
                state.setValue(QStringLiteral("scenario"), QStringLiteral("codex-error"));
                state.sync();
                aiBtn->click();
                check(waitFor([&] { return !aiBtn->property("p4vpatch.aiLoading").toBool(); }),
                      "Failed AI generation finishes and stops waiting animation");
                check(submit.desc->toPlainText() == QStringLiteral("更新 GameConfig 印度区正式服与调试开关配置") &&
                      submit.window.property("p4vpatch.lastAiError").toString().contains(QStringLiteral("quota exceeded")),
                      "Failed AI generation does not overwrite existing description text");
            }
        }
    }
    events();
    QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
    std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
#include "changelist_tests.moc"
