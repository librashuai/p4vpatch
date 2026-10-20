#include "launcher_process.h"
#include "launcher_update.h"
#include "launcher_theme.h"
#include "version.h"
#include <windows.h>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFrame>
#include <QProgressBar>
#include <QStyleFactory>
#include <QToolButton>
#include <QLabel>
#include <QLockFile>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTextEdit>
#include <QTextDocument>
#include <QResizeEvent>
#include <QtMath>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QCryptographicHash>
#include <functional>
#include <memory>

namespace {
using namespace launcher;

class WelcomeWindow : public QWidget {
public:
    using FetchCallback = std::function<void(QByteArray, QString)>;
    using FetchHandler = std::function<void(const QUrl&, qint64, FetchCallback)>;
    WelcomeWindow(QString exe, QStringList arguments, bool forceWelcome, FetchHandler fetchHandler = {}, QString patchPath = {})
        : exe_(std::move(exe)), arguments_(std::move(arguments)),
          dll_(patchPath.isEmpty() ? QCoreApplication::applicationDirPath() + "/p4vpatch.dll" : std::move(patchPath)),
          settings_(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/launcher.ini", QSettings::IniFormat),
          fetchHandler_(std::move(fetchHandler)) {
        if (exe_.isEmpty()) {
            const QString adjacent = QCoreApplication::applicationDirPath() + "/p4v.exe";
            const QString saved = settings_.value("p4vExecutable").toString();
            exe_ = QFileInfo(adjacent).isFile() ? adjacent : saved;
        }
        setObjectName(QStringLiteral("LauncherWindow"));
        setWindowTitle(QStringLiteral("P4V Patch"));
        setMinimumWidth(540);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(24, 24, 24, 22);
        layout->setSpacing(18);
        // Keep the upper sections anchored when details change the window height.
        layout->setAlignment(Qt::AlignTop);
        layout->setSizeConstraint(QLayout::SetMinimumSize);
        auto* header = new QHBoxLayout;
        auto* icon = new QLabel(this);
        icon->setPixmap(QApplication::windowIcon().pixmap(40, 40));
        icon->setFixedSize(40, 40);
        header->addWidget(icon);
        header->addSpacing(8);
        auto* title = new QLabel(QStringLiteral("P4V Patch"), this);
        title->setObjectName(QStringLiteral("appTitle"));
        header->addWidget(title);
        header->addStretch();
        versionBadge_ = new QLabel(this);
        versionBadge_->setObjectName(QStringLiteral("versionBadge"));
        header->addWidget(versionBadge_);
        layout->addLayout(header);

        auto* card = new QFrame(this);
        card->setObjectName(QStringLiteral("statusCard"));
        auto* cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(18, 18, 18, 14);
        cardLayout->setSpacing(9);
        statusTitle_ = new QLabel(card);
        statusTitle_->setObjectName(QStringLiteral("statusTitle"));
        statusTitle_->setTextFormat(Qt::PlainText);
        statusTitle_->setWordWrap(true);
        cardLayout->addWidget(statusTitle_);
        status_ = new QLabel(card);
        status_->setObjectName(QStringLiteral("statusSummary"));
        status_->setTextFormat(Qt::PlainText);
        status_->setWordWrap(true);
        cardLayout->addWidget(status_);
        auto* targetRow = new QHBoxLayout;
        target_ = new QLabel(card);
        target_->setObjectName(QStringLiteral("targetCaption"));
        target_->setTextFormat(Qt::PlainText);
        targetRow->addWidget(target_, 1);
        browse_ = new QPushButton(QStringLiteral("选择 P4V…"), card);
        browse_->setProperty("secondary", true);
        targetRow->addWidget(browse_);
        cardLayout->addLayout(targetRow);
        activity_ = new QProgressBar(card);
        activity_->setObjectName(QStringLiteral("activity"));
        activity_->setRange(0, 0);
        activity_->setTextVisible(false);
        activity_->setFixedHeight(2);
        // Hide only the indicator, not its space or the preceding layout spacing.
        auto activityPolicy = activity_->sizePolicy();
        activityPolicy.setRetainSizeWhenHidden(true);
        activity_->setSizePolicy(activityPolicy);
        cardLayout->addWidget(activity_);
        layout->addWidget(card);

        notesSection_ = new QWidget(this);
        auto* notesLayout = new QVBoxLayout(notesSection_);
        notesLayout->setContentsMargins(0, 0, 0, 0);
        notesLayout->setSpacing(8);
        notesLayout->addWidget(new QLabel(QStringLiteral("更新内容"), notesSection_));
        notes_ = new QTextEdit(notesSection_);
        notes_->setObjectName(QStringLiteral("releaseNotes"));
        notes_->setReadOnly(true);
        notes_->setMinimumHeight(112);
        notes_->setMaximumHeight(208);
        notesLayout->addWidget(notes_);
        notesSection_->hide();
        layout->addWidget(notesSection_);

        auto* preferences = new QHBoxLayout;
        skip_ = new QCheckBox(QStringLiteral("无更新时直接启动"), this);
        skip_->setChecked(settings_.value("skipWelcome", false).toBool());
        preferences->addWidget(skip_);
        preferences->addStretch();
        detailsToggle_ = new QToolButton(this);
        detailsToggle_->setObjectName(QStringLiteral("detailsToggle"));
        detailsToggle_->setText(QStringLiteral("详情"));
        detailsToggle_->setCheckable(true);
        detailsToggle_->setArrowType(Qt::RightArrow);
        detailsToggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        preferences->addWidget(detailsToggle_);
        layout->addLayout(preferences);
        details_ = new QWidget(this);
        details_->setObjectName(QStringLiteral("launcherDetails"));
        auto* detailsLayout = new QVBoxLayout(details_);
        detailsLayout->setContentsMargins(12, 12, 12, 12);
        detailsLayout->setSpacing(8);
        auto detailLabel = [this, detailsLayout](const QString& text = {}) {
            auto* label = new QLabel(text, details_);
            label->setTextFormat(Qt::PlainText);
            label->setWordWrap(true);
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            detailsLayout->addWidget(label);
            return label;
        };
        versions_ = detailLabel();
        targetDetails_ = detailLabel();
        detailLabel(QStringLiteral("补丁路径：%1").arg(QDir::toNativeSeparators(dll_)));
        detailLabel(QStringLiteral("支持 P4V " P4VPATCH_P4V_VERSION " / Qt " P4VPATCH_QT_VERSION " · Windows x64"));
        diagnostic_ = detailLabel();
        details_->hide();
        layout->addWidget(details_);
        connect(detailsToggle_, &QToolButton::toggled, this, [this](bool expanded) {
            details_->setVisible(expanded);
            detailsToggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
            resizeForContent();
        });
        refreshVersion();
        refreshTarget();
        auto* buttons = new QHBoxLayout;
        buttons->setSpacing(8);
        check_ = new QPushButton(QStringLiteral("检查更新"), this);
        check_->setProperty("secondary", true);
        page_ = new QPushButton(QStringLiteral("打开 Release"), this);
        page_->setProperty("secondary", true);
        update_ = new QPushButton(QStringLiteral("更新补丁"), this);
        update_->setObjectName(QStringLiteral("updateAction"));
        start_ = new QPushButton(QStringLiteral("启动 P4V"), this);
        start_->setObjectName(QStringLiteral("primaryAction"));
        start_->setDefault(true);
        buttons->addWidget(check_);
        buttons->addWidget(page_);
        buttons->addStretch();
        buttons->addWidget(update_);
        buttons->addWidget(start_);
        layout->addLayout(buttons);
        theme_.apply(*this, exe_);
        resize(580, sizeHint().height());
        connect(skip_, &QCheckBox::toggled, this, [this](bool checked) {
            settings_.setValue("skipWelcome", checked);
            settings_.sync();
        });
        connect(browse_, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("选择已安装的 p4v.exe"), exe_, QStringLiteral("P4V (p4v.exe)"));
            if (path.isEmpty()) return;
            exe_ = QFileInfo(path).absoluteFilePath();
            settings_.setValue("p4vExecutable", exe_);
            settings_.sync();
            refreshTarget();
            theme_.apply(*this, exe_);
            // Revalidate a detected release against the newly selected installation.
            checkUpdates(false);
        });
        connect(check_, &QPushButton::clicked, this, [this] { checkUpdates(false); });
        connect(page_, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(release_.page); });
        connect(update_, &QPushButton::clicked, this, [this] { updatePatch(); });
        connect(start_, &QPushButton::clicked, this, [this] { startP4v(); });
        QString cacheError;
        if (parseRelease(settings_.value("latestReleaseJson").toByteArray(), release_, cacheError)) {
            hasNewRelease_ = isNewRelease(release_);
            if (hasNewRelease_) notes_->setPlainText(release_.notes);
        }
        autoStart_ = !forceWelcome && skip_->isChecked();
        if (!autoStart_ || hasNewRelease_) show();
        setBusy(true);
        QTimer::singleShot(0, this, [this] { checkUpdates(true); });
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (notes_ && notesSection_ && !notesSection_->isHidden()) sizeNotes();
    }
    void closeEvent(QCloseEvent* event) override {
        if (workerActive_) {
            showStatus(QStringLiteral("请稍候"), QStringLiteral("正在启动或安装补丁，请等待操作结束。"));
            event->ignore();
        } else QWidget::closeEvent(event);
    }

private:
    void fetch(const QUrl& url, qint64 limit, FetchCallback callback) {
        if (fetchHandler_) { fetchHandler_(url, limit, std::move(callback)); return; }
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("p4vpatch-launcher/" P4VPATCH_VERSION));
        request.setRawHeader("Accept", "application/vnd.github+json");
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setTransferTimeout(8000);
        auto* reply = network_.get(request);
        auto bytes = std::make_shared<QByteArray>();
        auto failure = std::make_shared<QString>();
        auto* deadline = new QTimer(reply);
        deadline->setSingleShot(true);
        connect(deadline, &QTimer::timeout, reply, [reply, failure] {
            *failure = QStringLiteral("请求超时。可以继续使用当前补丁。");
            reply->abort();
        });
        deadline->start(limit > 1024 * 1024 ? 120000 : 15000);
        connect(reply, &QIODevice::readyRead, this, [reply, bytes, failure, limit] {
            if (!failure->isEmpty()) return;
            if (bytes->size() + reply->bytesAvailable() > limit) {
                *failure = QStringLiteral("服务器响应超出大小限制。");
                reply->abort();
                return;
            }
            bytes->append(reply->readAll());
        });
        connect(reply, &QNetworkReply::finished, this, [reply, bytes, failure, limit, callback = std::move(callback)] {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (failure->isEmpty() && (reply->error() != QNetworkReply::NoError || status != 200))
                *failure = QStringLiteral("GitHub 请求失败（HTTP %1）：%2").arg(status).arg(reply->errorString());
            if (failure->isEmpty()) {
                if (bytes->size() + reply->bytesAvailable() > limit) *failure = QStringLiteral("响应过大。");
                else bytes->append(reply->readAll());
            }
            reply->deleteLater();
            callback(*bytes, *failure);
        });
    }

    bool isNewRelease(const Release& release) const {
        return newerVersion(release.version, installed_.isEmpty() ? "0.0.0" : installed_) ||
               newerVersion(release.version, QStringLiteral(P4VPATCH_VERSION));
    }
    QString checkFailure(const QString& error) const {
        return hasNewRelease_
            ? QStringLiteral("已知新版 %1；本次检查失败：%2\n请重试，或打开 Release 手动下载。").arg(release_.version, error)
            : error;
    }
    void sizeNotes() {
        notes_->document()->setTextWidth(qMax(240, width() - 72));
        notes_->setFixedHeight(qBound(112, qCeil(notes_->document()->size().height()) + 28, 208));
    }
    void resizeForContent() {
        layout()->activate();
        resize(width(), sizeHint().height());
    }
    void showStatus(const QString& title, const QString& summary, const QString& detail = {}) {
        statusTitle_->setText(title);
        status_->setText(summary);
        status_->setVisible(!summary.isEmpty());
        diagnostic_->setText(detail);
        diagnostic_->setVisible(!detail.isEmpty());
    }
    void refreshVersion() {
        installed_ = fileVersion(dll_);
        versionBadge_->setText(installed_.isEmpty()
            ? (QFileInfo(dll_).isFile() ? QStringLiteral("版本未知") : QStringLiteral("未安装补丁")) : "v" + installed_);
        versions_->setText(QStringLiteral("Launcher：%1    补丁：%2    Qt：%3")
            .arg(QStringLiteral(P4VPATCH_VERSION), installed_.isEmpty() ? QStringLiteral("未知 / 未安装") : installed_, QString::fromLatin1(qVersion())));
    }
    void refreshTarget() {
        const bool found = QFileInfo(exe_).isFile();
        const auto version = fileVersion(exe_, true);
        target_->setText(found ? (version.isEmpty() ? QStringLiteral("已选择 P4V") : "P4V " + version.section('.', 0, 1))
                               : QStringLiteral("尚未选择 P4V"));
        target_->setToolTip(QDir::toNativeSeparators(exe_));
        targetDetails_->setText(found ? QStringLiteral("P4V：%1").arg(QDir::toNativeSeparators(exe_)) : QStringLiteral("P4V：未选择"));
    }
    void setBusy(bool busy) {
        busy_ = busy;
        activity_->setVisible(busy);
        browse_->setEnabled(!busy);
        check_->setEnabled(!busy);
        start_->setEnabled(!busy);
        skip_->setEnabled(!busy);
        page_->setEnabled(!busy && !release_.page.isEmpty());
        page_->setVisible(hasNewRelease_);
        update_->setEnabled(!busy && canUpdate_);
        update_->setVisible(canUpdate_);
        const bool showNotes = hasNewRelease_ && !notes_->toPlainText().isEmpty();
        if (showNotes) sizeNotes();
        if (notesSection_->isHidden() == showNotes) {
            notesSection_->setVisible(showNotes);
            resizeForContent();
        }
    }
    void finishCheck(bool startup, const QString& message, const QString& summary = {}) {
        const bool found = QFileInfo(exe_).isFile();
        showStatus(hasNewRelease_ ? QStringLiteral("发现新版本 %1").arg(release_.version)
                                 : (found ? QStringLiteral("准备就绪") : QStringLiteral("选择 P4V")),
                   found || hasNewRelease_ ? (summary.isEmpty() ? message : summary)
                                          : QStringLiteral("选择已安装的 P4V，即可开始。"), message);
        setBusy(false);
        // New releases always interrupt auto-start, including manifest failures and launcher-only updates.
        if (startup && autoStart_ && !hasNewRelease_) startP4v();
        else show();
    }
    void checkUpdates(bool startup) {
        setBusy(true);
        canUpdate_ = false;
        // Do not forget a previously detected release on a retry/network failure.
        showStatus(QStringLiteral("正在检查更新"), QStringLiteral("正在连接 GitHub…"));
        fetch(latestReleaseUrl(), 1024 * 1024, [this, startup](QByteArray bytes, QString error) {
            if (!error.isEmpty()) { finishCheck(startup, checkFailure(error), QStringLiteral("暂时无法检查更新，可稍后重试。")); return; }
            Release candidate;
            if (!parseRelease(bytes, candidate, error)) { finishCheck(startup, checkFailure(error), QStringLiteral("暂时无法检查更新，可稍后重试。")); return; }
            release_ = candidate;
            settings_.setValue("latestReleaseJson", bytes);
            settings_.sync();
            notes_->setPlainText(release_.notes);
            hasNewRelease_ = isNewRelease(release_);
            if (!hasNewRelease_) { finishCheck(startup, QStringLiteral("当前已是最新版本。")); return; }
            showStatus(QStringLiteral("发现新版本 %1").arg(release_.version), QStringLiteral("正在读取更新信息…"));
            setBusy(true);
            show();
            raise();
            activateWindow();
            if (release_.manifest.isEmpty()) {
                finishCheck(startup, QStringLiteral("发现新版 %1。此 Release 无更新清单，请打开 Release 手动安装。").arg(release_.version),
                            QStringLiteral("请打开 Release 手动安装。"));
                return;
            }
            fetch(release_.manifest, 64 * 1024, [this, startup](QByteArray data, QString failure) {
                if (!failure.isEmpty() || !parseManifest(data, release_, manifest_, failure)) {
                    finishCheck(startup, QStringLiteral("发现新版 %1，但无法读取更新清单：%2\n可打开 Release 手动下载。").arg(release_.version, failure),
                                QStringLiteral("更新信息暂不可用，可打开 Release 手动下载。"));
                    return;
                }
                if (newerVersion(manifest_.minimumLauncher, QStringLiteral(P4VPATCH_VERSION))) {
                    finishCheck(startup, QStringLiteral("发现新版 %1，需要 Launcher %2 或以上。请打开 Release 下载完整 ZIP，退出后手动替换两个文件。")
                        .arg(release_.version, manifest_.minimumLauncher),
                        QStringLiteral("需要 Launcher %1 或以上，请下载完整安装包。").arg(manifest_.minimumLauncher));
                    return;
                }
                QString compatibilityError;
                if (manifest_.qtVersion != QString::fromLatin1(qVersion()) ||
                    !compatibleP4v(exe_, manifest_.p4vVersion, manifest_.qtVersion, compatibilityError)) {
                    finishCheck(startup, QStringLiteral("发现新版 %1，但它要求 P4V %2 / Qt %3，与所选安装不兼容；禁止安装。\n%4")
                        .arg(release_.version, manifest_.p4vVersion, manifest_.qtVersion, compatibilityError),
                        QStringLiteral("更新需要 P4V %1 / Qt %2，请检查所选安装。").arg(manifest_.p4vVersion, manifest_.qtVersion));
                    return;
                }
                canUpdate_ = newerVersion(manifest_.version, installed_.isEmpty() ? "0.0.0" : installed_);
                finishCheck(startup, canUpdate_
                    ? QStringLiteral("可手动更新，也可以继续使用当前版本。")
                    : QStringLiteral("补丁已更新；Launcher 新版请到 Release 下载。"));
            });
        });
    }

    void runWorker(std::function<LaunchResult()> work, std::function<void(LaunchResult)> done) {
        workerActive_ = true;
        auto result = std::make_shared<LaunchResult>();
        auto* thread = QThread::create([work = std::move(work), result] { *result = work(); });
        connect(thread, &QThread::finished, this, [this, result, done = std::move(done)] {
            workerActive_ = false;
            done(*result);
        });
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        thread->start();
    }
    void updatePatch() {
        if (!canUpdate_ || busy_) return;
        QString error;
        if (p4vIsRunning(error)) { QMessageBox::warning(this, QStringLiteral("无法更新"), error); return; }
        if (QMessageBox::question(this, QStringLiteral("确认更新"),
            QStringLiteral("下载并安装补丁 %1？\n旧版保留为 p4vpatch.dll.bak。不会自动升级 Launcher，也不会自动启动 P4V。")
                .arg(manifest_.version)) != QMessageBox::Yes) return;
        setBusy(true);
        showStatus(QStringLiteral("正在下载更新"), QStringLiteral("完成后会保留旧版备份。"));
        fetch(manifest_.dllUrl, manifest_.size, [this](QByteArray bytes, QString error) {
            if (!error.isEmpty()) {
                showStatus(QStringLiteral("下载未完成"), QStringLiteral("当前补丁未更改，可稍后重试。"), error);
                setBusy(false);
                return;
            }
            showStatus(QStringLiteral("正在安装更新"), QStringLiteral("正在校验补丁，请稍候。"));
            const auto target = dll_;
            const auto manifest = manifest_;
            runWorker([target, bytes = std::move(bytes), manifest] {
                QString failure;
                const bool ok = installPatch(target, bytes, manifest, failure);
                return LaunchResult{ok, failure};
            }, [this](LaunchResult result) {
                refreshVersion();
                if (result.ok) {
                    canUpdate_ = false;
                    showStatus(QStringLiteral("更新完成"), QStringLiteral("启动 P4V，即可使用新版补丁。"));
                } else {
                    showStatus(QStringLiteral("更新未完成"), QStringLiteral("请展开详情查看原因。"), result.message);
                    QMessageBox::warning(this, QStringLiteral("更新失败"), result.message);
                }
                setBusy(false);
            });
        });
    }
    void startP4v() {
        if (busy_) return;
        QString error;
        if (!QFileInfo(dll_).isFile()) error = QStringLiteral("找不到 Launcher 旁的 p4vpatch.dll，请先安装或更新补丁。");
        else if (QString::fromLatin1(qVersion()) != QStringLiteral(P4VPATCH_QT_VERSION)) error = QStringLiteral("不支持当前 Qt 版本，仅支持 Qt " P4VPATCH_QT_VERSION "。");
        else if (compatibleP4v(exe_, QStringLiteral(P4VPATCH_P4V_VERSION), QStringLiteral(P4VPATCH_QT_VERSION), error)) p4vIsRunning(error);
        if (!error.isEmpty()) {
            show();
            showStatus(QStringLiteral("暂时无法启动"), QStringLiteral("请检查 P4V 选择及兼容性。"), error);
            detailsToggle_->setChecked(true);
            return;
        }
        settings_.setValue("skipWelcome", skip_->isChecked());
        settings_.setValue("p4vExecutable", exe_);
        settings_.sync();
        setBusy(true);
        showStatus(QStringLiteral("正在启动 P4V"), QStringLiteral("正在加载补丁…"));
        const auto exe = exe_;
        const auto dll = dll_;
        const auto arguments = arguments_;
        runWorker([exe, dll, arguments] { return launchP4v(exe, dll, arguments); }, [this](LaunchResult result) {
            if (result.ok) QCoreApplication::quit();
            else {
                show();
                showStatus(QStringLiteral("启动未完成"), QStringLiteral("请展开详情查看原因。"), result.message);
                detailsToggle_->setChecked(true);
                setBusy(false);
            }
        });
    }

    QString exe_;
    QStringList arguments_;
    QString dll_;
    QString installed_;
    QSettings settings_;
    QNetworkAccessManager network_;
    FetchHandler fetchHandler_;
    LauncherTheme theme_;
    Release release_;
    Manifest manifest_;
    QLabel* versions_ = nullptr;
    QLabel* versionBadge_ = nullptr;
    QLabel* target_ = nullptr;
    QLabel* targetDetails_ = nullptr;
    QPushButton* browse_ = nullptr;
    QLabel* statusTitle_ = nullptr;
    QLabel* status_ = nullptr;
    QLabel* diagnostic_ = nullptr;
    QToolButton* detailsToggle_ = nullptr;
    QWidget* details_ = nullptr;
    QProgressBar* activity_ = nullptr;
    QWidget* notesSection_ = nullptr;
    QTextEdit* notes_ = nullptr;
    QCheckBox* skip_ = nullptr;
    QPushButton* check_ = nullptr;
    QPushButton* page_ = nullptr;
    QPushButton* update_ = nullptr;
    QPushButton* start_ = nullptr;
    bool autoStart_ = false;
    bool hasNewRelease_ = false;
    bool canUpdate_ = false;
    bool busy_ = false;
    bool workerActive_ = false;
};
} // namespace

#ifndef P4VPATCH_LAUNCHER_TESTING
int main(int argc, char* argv[]) {
    // Restrict platform/plugin discovery before QApplication loads qwindows.dll.
    std::wstring modulePath(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
    if (!length || length >= modulePath.size()) return 2;
    modulePath.resize(length);
    const auto plugins = QFileInfo(QString::fromStdWString(modulePath)).absolutePath() + "/plugins";
    QCoreApplication::setLibraryPaths({plugins});
    qputenv("QT_PLUGIN_PATH", plugins.toUtf8());
    qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", (plugins + "/platforms").toUtf8());
    // GUI-subsystem executable: Qt's entry point supplies Unicode arguments on Windows.
    QApplication app(argc, argv);
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QCoreApplication::setOrganizationName(QStringLiteral("p4vpatch"));
    QCoreApplication::setApplicationName(QStringLiteral("p4vpatch-launcher"));
    QCoreApplication::setApplicationVersion(QStringLiteral(P4VPATCH_VERSION));
    // Do not mix SDK plugins or plugins from a second Qt installation with P4V's DLLs.
    QCoreApplication::setLibraryPaths({QCoreApplication::applicationDirPath() + "/plugins"});
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/launcher/icon.ico")));
    auto args = QCoreApplication::arguments().mid(1);
    bool forceWelcome = false;
    if (!args.isEmpty() && args.first() == "--welcome") { forceWelcome = true; args.removeFirst(); }
    QString exe; // Missing P4V must not prevent opening the welcome/test UI.
    if (!args.isEmpty() && args.first() != "--") exe = QFileInfo(args.takeFirst()).absoluteFilePath();
    if (!args.isEmpty() && args.first() == "--") args.removeFirst();
    const QString lockDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!QDir().mkpath(lockDir)) {
        QMessageBox::critical(nullptr, QStringLiteral("P4V Patch"), QStringLiteral("无法创建 Launcher 配置目录。"));
        return 2;
    }
    const auto key = QCryptographicHash::hash(QDir(QCoreApplication::applicationDirPath()).canonicalPath().toLower().toUtf8(), QCryptographicHash::Sha256).toHex();
    QLockFile lock(lockDir + '/' + QString::fromLatin1(key) + ".lock");
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        QMessageBox::warning(nullptr, QStringLiteral("P4V Patch"), QStringLiteral("此安装目录已有 Launcher 运行，请先关闭原窗口。"));
        return 2;
    }
    WelcomeWindow window(exe, args, forceWelcome);
    return app.exec();
}
#endif
