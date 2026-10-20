#include "changelist_links.h"

#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QDesktopServices>
#include <QEvent>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextDocument>
#include <QTextFragment>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <utility>

namespace {
const QRegularExpression& jiraPattern() {
    static const QRegularExpression pattern(QStringLiteral(R"(\[Jira\](BR-[0-9]+)\b)"));
    return pattern;
}

bool eligible(QTextBrowser* browser) {
    if (!browser || browser->objectName() != QStringLiteral("markdownBrowser") || !browser->isReadOnly()) return false;
    bool description = false;
    for (auto* parent = browser->parentWidget(); parent; parent = parent->parentWidget()) {
        if (parent->inherits("Gui::ExpandableTextEdit") && parent->objectName() == QStringLiteral("expanding QTE")) {
            if (!parent->property("readOnly").toBool()) return false;
            description = true;
        }
        if (parent->inherits("UIChangeWindow"))
            return description && parent->windowTitle().startsWith(QStringLiteral("Submitted Changelist:"));
    }
    return false;
}

// Do not replace P4V's existing Markdown/HTML links, including links enclosing a Jira token.
bool hasExistingLink(const QTextBlock& block, int start, int length) {
    const int from = block.position() + start;
    for (auto it = block.begin(); !it.atEnd(); ++it) {
        const auto fragment = it.fragment();
        if (fragment.isValid() && fragment.position() < from + length &&
            fragment.position() + fragment.length() > from && fragment.charFormat().isAnchor()) return true;
    }
    return false;
}

QString jiraUrl(const QString& issue) {
    return QStringLiteral("https://jira.garena.com/browse/") + issue;
}

class JiraHighlighter final : public QSyntaxHighlighter {
public:
    JiraHighlighter(QTextDocument* document, QTextBrowser* browser)
        : QSyntaxHighlighter(document), browser_(browser) {}
protected:
    void highlightBlock(const QString& text) override {
        if (!eligible(browser_)) return;
        auto matches = jiraPattern().globalMatch(text);
        while (matches.hasNext()) {
            const auto match = matches.next();
            const int start = int(match.capturedStart(1));
            const int length = int(match.capturedLength(1));
            if (hasExistingLink(currentBlock(), start, length)) continue;
            QTextCharFormat format;
            format.setForeground(browser_->palette().brush(QPalette::Link));
            format.setFontUnderline(true);
            setFormat(start, length, format);
        }
    }
private:
    QPointer<QTextBrowser> browser_;
};

class BrowserLinks final : public QObject {
public:
    explicit BrowserLinks(QTextBrowser* browser) : QObject(browser), browser_(browser) {
        setObjectName(QStringLiteral("p4vpatch.jiraLinks"));
        browser->installEventFilter(this);
        browser->viewport()->installEventFilter(this);
        for (auto* parent = browser->parentWidget(); parent; parent = parent->parentWidget()) {
            parent->installEventFilter(this);
            if (parent->inherits("UIChangeWindow")) break;
        }
        connect(browser, &QTextEdit::textChanged, this, [this] { scheduleRefresh(); });
        refresh();
    }
    ~BrowserLinks() override { removeHighlighter(); }

protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        switch (event->type()) {
        case QEvent::Show:
        case QEvent::WindowTitleChange:
        case QEvent::ReadOnlyChange:
        case QEvent::PaletteChange:
        case QEvent::ApplicationPaletteChange:
            scheduleRefresh();
            break;
        default: break;
        }
        if (object != browser_->viewport()) return false;
        if (event->type() == QEvent::Leave) {
            clearHandCursor();
            pressedUrl_.clear();
        } else if (event->type() == QEvent::MouseMove) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->buttons() == Qt::NoButton && !urlAt(mouse->position().toPoint()).isEmpty()) {
                browser_->viewport()->setCursor(Qt::PointingHandCursor);
                handCursor_ = true;
                return true;
            }
            clearHandCursor(); // Let Qt handle selection, drag-and-drop and existing links normally.
        } else if (event->type() == QEvent::ToolTip) {
            auto* help = static_cast<QHelpEvent*>(event);
            const QString url = urlAt(help->pos());
            if (!url.isEmpty()) {
                QToolTip::showText(help->globalPos(), url, browser_->viewport());
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            pressedUrl_.clear();
            if (mouse->button() == Qt::LeftButton && mouse->modifiers() == Qt::NoModifier) {
                pressedUrl_ = urlAt(mouse->position().toPoint());
                pressedPosition_ = mouse->position().toPoint();
            }
        } else if (event->type() == QEvent::MouseButtonRelease) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() != Qt::LeftButton) return false;
            const QString pressed = std::exchange(pressedUrl_, {});
            if (!pressed.isEmpty() && mouse->modifiers() == Qt::NoModifier &&
                !browser_->textCursor().hasSelection() &&
                (mouse->position().toPoint() - pressedPosition_).manhattanLength() < QApplication::startDragDistance() &&
                pressed == urlAt(mouse->position().toPoint())) {
                // Never let QTextBrowser navigate its own document or load a Jira page in P4V.
                QDesktopServices::openUrl(QUrl(pressed));
                return true;
            }
        }
        return false;
    }

private:
    QString urlAt(const QPoint& point) const {
        if (!eligible(browser_) || !highlighter_ || highlighter_->document() != browser_->document()) return {};
        // ExactHit avoids activating the last token when clicking empty space past the end of a line.
        const QPointF position = QPointF(point) + QPointF(browser_->horizontalScrollBar()->value(), browser_->verticalScrollBar()->value());
        auto* document = browser_->document();
        const int offset = document->documentLayout()->hitTest(position, Qt::ExactHit);
        if (offset < 0) return {};
        const auto block = document->findBlock(offset);
        auto matches = jiraPattern().globalMatch(block.text());
        while (matches.hasNext()) {
            const auto match = matches.next();
            const int start = int(match.capturedStart(1));
            const int length = int(match.capturedLength(1));
            if (offset >= block.position() + start && offset < block.position() + start + length &&
                !hasExistingLink(block, start, length)) return jiraUrl(match.captured(1));
        }
        return {};
    }

    void clearHandCursor() {
        if (handCursor_) browser_->viewport()->unsetCursor();
        handCursor_ = false;
    }
    void scheduleRefresh() {
        if (refreshPending_) return;
        refreshPending_ = true;
        QTimer::singleShot(0, this, [this] { refreshPending_ = false; refresh(); });
    }
    void removeHighlighter() {
        if (!highlighter_) return;
        auto* document = highlighter_->document();
        if (!document) { delete highlighter_.data(); return; }
        const bool modified = document->isModified();
        const QSignalBlocker documentSignals(document);
        delete highlighter_.data();
        document->setModified(modified);
    }
    void refresh() {
        auto* document = browser_->document();
        const QSignalBlocker browserSignals(browser_);
        bool created = false;
        if (!highlighter_ || highlighter_->document() != document) {
            removeHighlighter();
            // Qt supports one syntax highlighter per document; never overwrite P4V/another extension's.
            if (!document->findChildren<QSyntaxHighlighter*>().isEmpty()) return;
            highlighter_ = new JiraHighlighter(document, browser_);
            created = true;
        }
        const bool active = eligible(browser_);
        const auto color = browser_->palette().color(QPalette::Link);
        if (created || active != active_ || color != color_) {
            active_ = active;
            color_ = color;
            // rehighlight() marks layout ranges dirty in Qt. Suppress those formatting-only
            // signals and restore the flag; immediate rehighlight also cancels its deferred pass.
            const bool modified = document->isModified();
            const QSignalBlocker documentSignals(document);
            highlighter_->rehighlight();
            document->setModified(modified);
        }
        if (!active) { clearHandCursor(); pressedUrl_.clear(); }
    }

    QTextBrowser* browser_;
    QPointer<JiraHighlighter> highlighter_;
    bool active_ = false;
    QColor color_;
    bool refreshPending_ = false;
    bool handCursor_ = false;
    QString pressedUrl_;
    QPoint pressedPosition_;
};

class ChangelistWatcher final : public QObject {
public:
    explicit ChangelistWatcher(QApplication* app) : QObject(app) {
        setObjectName(QStringLiteral("p4vpatch.changelistLinks"));
        app->installEventFilter(this);
        for (auto* widget : QApplication::allWidgets()) attach(qobject_cast<QTextBrowser*>(widget));
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() != QEvent::Show && event->type() != QEvent::WindowTitleChange) return false;
        auto* widget = qobject_cast<QWidget*>(object);
        if (!widget || (widget->objectName() != QStringLiteral("markdownBrowser") && !widget->inherits("UIChangeWindow"))) return false;
        QPointer<QWidget> weak(widget);
        QTimer::singleShot(0, this, [weak] {
            if (!weak) return;
            attach(qobject_cast<QTextBrowser*>(weak.data()));
            for (auto* browser : weak->findChildren<QTextBrowser*>()) attach(browser);
        });
        return false;
    }

private:
    static void attach(QTextBrowser* browser) {
        if (eligible(browser) && !browser->findChild<QObject*>(QStringLiteral("p4vpatch.jiraLinks"), Qt::FindDirectChildrenOnly))
            new BrowserLinks(browser);
    }
};
} // namespace

void installChangelistLinks(QApplication* app) {
    if (app && !app->findChild<QObject*>(QStringLiteral("p4vpatch.changelistLinks"), Qt::FindDirectChildrenOnly))
        new ChangelistWatcher(app);
}
