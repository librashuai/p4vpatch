#include "submit_ai.h"
#include "preferences.h"
#include "version.h"

#include <windows.h>

#include <QAbstractItemModel>
#include <QApplication>
#include <QBoxLayout>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QListView>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextEdit>
#include <QTimer>
#include <QTreeView>
#include <QtMath>

#include <utility>

namespace {
constexpr int kP4TimeoutMs = 30000;
constexpr int kCodexModelsTimeoutMs = 15000;
constexpr int kCodexExecTimeoutMs = 120000;
constexpr int kMaxDiffBytes = 128 * 1024;
constexpr int kSpinnerSteps = 12;
constexpr int kSpinnerIntervalMs = 50;

QString cachedDefaultCodexModel;

QString firstNonEmpty(std::initializer_list<QString> values) {
    for (const QString& value : values) {
        const QString trimmed = value.trimmed();
        if (!trimmed.isEmpty()) return trimmed;
    }
    return {};
}

QString resolveNpmVendorCodex(const QString& npmDirPath) {
    if (npmDirPath.isEmpty()) return {};
    const QDir npmDir(npmDirPath);
    const QStringList candidates{
        npmDir.filePath(QStringLiteral("node_modules/@openai/codex/node_modules/@openai/codex-win32-x64/vendor/x86_64-pc-windows-msvc/bin/codex.exe")),
        npmDir.filePath(QStringLiteral("node_modules/@openai/codex-win32-x64/vendor/x86_64-pc-windows-msvc/bin/codex.exe")),
        npmDir.filePath(QStringLiteral("node_modules/@openai/codex/vendor/x86_64-pc-windows-msvc/bin/codex.exe")),
    };
    for (const QString& candidate : candidates) {
        if (QFileInfo(candidate).isFile()) return QDir::cleanPath(candidate);
    }
    return {};
}

void configureNoWindow(QProcess* process) {
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) {
        args->flags |= CREATE_NO_WINDOW;
    });
}

void startCliProcess(QProcess* process, const QString& executable, const QStringList& arguments) {
    if (executable.endsWith(QStringLiteral(".cmd"), Qt::CaseInsensitive) ||
        executable.endsWith(QStringLiteral(".bat"), Qt::CaseInsensitive)) {
        QString comspec = qEnvironmentVariable("COMSPEC").trimmed();
        if (comspec.isEmpty()) comspec = QStringLiteral("cmd.exe");
        QStringList cmdArgs{QStringLiteral("/d"), QStringLiteral("/s"), QStringLiteral("/c"), executable};
        cmdArgs.append(arguments);
        process->setProgram(comspec);
        process->setArguments(cmdArgs);
    } else {
        process->setProgram(executable);
        process->setArguments(arguments);
    }
    process->start();
}

QList<CodexModelInfo> parseModelArray(const QJsonArray& array) {
    QList<CodexModelInfo> models;
    for (const QJsonValue& itemVal : array) {
        if (!itemVal.isObject()) continue;
        const QJsonObject item = itemVal.toObject();
        if (item.value(QStringLiteral("hidden")).toBool(false)) continue;
        const QString visibility = item.value(QStringLiteral("visibility")).toString().trimmed();
        if (!visibility.isEmpty() && visibility != QStringLiteral("list")) continue;

        const QString id = firstNonEmpty({
            item.value(QStringLiteral("id")).toString(),
            item.value(QStringLiteral("model")).toString(),
            item.value(QStringLiteral("slug")).toString(),
        });
        if (id.isEmpty()) continue;

        bool duplicate = false;
        for (const auto& existing : models) {
            if (existing.id == id) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        CodexModelInfo info;
        info.id = id;
        info.displayName = firstNonEmpty({
            item.value(QStringLiteral("displayName")).toString(),
            item.value(QStringLiteral("display_name")).toString(),
            id,
        });
        info.description = item.value(QStringLiteral("description")).toString().trimmed();
        info.isDefault = item.value(QStringLiteral("isDefault")).toBool(false) ||
                         item.value(QStringLiteral("is_default")).toBool(false);
        models.append(std::move(info));
    }
    const QString defaultModel = selectDefaultCodexModel(models);
    if (!defaultModel.isEmpty()) cachedDefaultCodexModel = defaultModel;
    return models;
}

class CodexModelsFetcher final : public QObject {
public:
    CodexModelsFetcher(const QString& executable,
                       std::function<void(QList<CodexModelInfo>, QString)> callback,
                       QObject* parent)
        : QObject(parent), executable_(executable), callback_(std::move(callback)) {
        process_ = new QProcess(this);
        configureNoWindow(process_);
        process_->setWorkingDirectory(QDir::tempPath());
        timeout_ = new QTimer(this);
        timeout_->setSingleShot(true);

        connect(timeout_, &QTimer::timeout, this, [this] {
            timedOut_ = true;
            process_->kill();
            finish({}, QStringLiteral("Timed out while fetching models from Codex CLI."));
        });
        connect(process_, &QProcess::started, this, [this] {
            const QByteArray payload =
                R"({"id":1,"method":"initialize","params":{"clientInfo":{"name":"p4vpatch","title":"P4V Patch","version":")"
                P4VPATCH_VERSION
                R"("},"capabilities":{"experimentalApi":true}}})" "\n"
                R"({"method":"initialized","params":{}})" "\n"
                R"({"id":2,"method":"model/list","params":{"limit":100,"includeHidden":false}})" "\n";
            process_->write(payload);
        });
        connect(process_, &QProcess::readyReadStandardOutput, this, [this] {
            stdoutBuffer_ += process_->readAllStandardOutput();
            processLines();
        });
        connect(process_, &QProcess::readyReadStandardError, this, [this] {
            if (stderrBuffer_.size() < 32768)
                stderrBuffer_ += process_->readAllStandardError();
            else
                process_->readAllStandardError();
        });
        connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                finish({}, QStringLiteral("Failed to start Codex CLI (%1).").arg(executable_));
        });
        connect(process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus) {
            if (finished_) return;
            processLines();
            if (finished_) return;
            // Fallback if stdout contains a full JSON catalog object (e.g. debug models).
            const QJsonDocument doc = QJsonDocument::fromJson(stdoutBuffer_.trimmed());
            if (doc.isObject()) {
                const QJsonObject root = doc.object();
                if (root.value(QStringLiteral("models")).isArray()) {
                    auto models = parseModelArray(root.value(QStringLiteral("models")).toArray());
                    if (!models.isEmpty()) {
                        finish(std::move(models), {});
                        return;
                    }
                }
            }
            const QString errText = QString::fromUtf8(stderrBuffer_).trimmed();
            finish({}, errText.isEmpty()
                ? QStringLiteral("Codex CLI exited (%1) without returning a model list.").arg(exitCode)
                : errText);
        });
    }

    ~CodexModelsFetcher() override {
        if (process_ && process_->state() != QProcess::NotRunning) {
            disconnect(process_, nullptr, this, nullptr);
            process_->kill();
        }
    }

    void start() {
        timeout_->start(kCodexModelsTimeoutMs);
        startCliProcess(process_, executable_, {QStringLiteral("app-server")});
    }

private:
    void processLines() {
        while (!finished_) {
            const int newline = stdoutBuffer_.indexOf('\n');
            if (newline < 0) break;
            const QByteArray line = stdoutBuffer_.left(newline).trimmed();
            stdoutBuffer_.remove(0, newline + 1);
            if (line.isEmpty()) continue;

            const QJsonDocument doc = QJsonDocument::fromJson(line);
            if (!doc.isObject()) continue;
            const QJsonObject obj = doc.object();
            if (obj.value(QStringLiteral("id")).toInt(-1) != 2) continue;

            if (obj.value(QStringLiteral("error")).isObject()) {
                const QString message = obj.value(QStringLiteral("error")).toObject()
                    .value(QStringLiteral("message")).toString().trimmed();
                process_->closeWriteChannel();
                process_->kill();
                finish({}, message.isEmpty()
                    ? QStringLiteral("Codex model/list returned an error.")
                    : message);
                return;
            }
            const QJsonObject result = obj.value(QStringLiteral("result")).toObject();
            const QJsonArray data = result.value(QStringLiteral("data")).toArray();
            auto models = parseModelArray(data);
            process_->closeWriteChannel();
            process_->kill();
            if (models.isEmpty()) {
                finish({}, QStringLiteral("Codex CLI returned an empty model list."));
            } else {
                finish(std::move(models), {});
            }
            return;
        }
    }

    void finish(QList<CodexModelInfo> models, const QString& error) {
        if (finished_) return;
        finished_ = true;
        timeout_->stop();
        auto cb = std::move(callback_);
        deleteLater();
        if (cb) cb(std::move(models), error);
    }

    QString executable_;
    std::function<void(QList<CodexModelInfo>, QString)> callback_;
    QProcess* process_ = nullptr;
    QTimer* timeout_ = nullptr;
    QByteArray stdoutBuffer_;
    QByteArray stderrBuffer_;
    bool finished_ = false;
    bool timedOut_ = false;
};

struct OpenedFileEntry {
    QString depotFile;
    QString action;
    QString type;
    QString rev;
};

QList<OpenedFileEntry> parseOpenedZtag(const QByteArray& raw) {
    QList<OpenedFileEntry> entries;
    OpenedFileEntry current;
    const QString text = QString::fromUtf8(raw);
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r?\n")));
    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty()) {
            if (!current.depotFile.isEmpty()) {
                entries.append(current);
                current = {};
            }
            continue;
        }
        if (!line.startsWith(QStringLiteral("... "))) continue;
        const int space = line.indexOf(QLatin1Char(' '), 4);
        if (space < 0) continue;
        const QString key = line.mid(4, space - 4).trimmed();
        const QString value = line.mid(space + 1).trimmed();
        if (key == QStringLiteral("depotFile")) {
            if (!current.depotFile.isEmpty()) {
                entries.append(current);
                current = {};
            }
            current.depotFile = value;
        } else if (key == QStringLiteral("action")) {
            current.action = value;
        } else if (key == QStringLiteral("type")) {
            current.type = value;
        } else if (key == QStringLiteral("rev")) {
            current.rev = value;
        }
    }
    if (!current.depotFile.isEmpty()) entries.append(current);
    return entries;
}

QStringList parseFstatClientFiles(const QByteArray& raw) {
    QStringList paths;
    const QString text = QString::fromUtf8(raw);
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r?\n")));
    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.startsWith(QStringLiteral("... clientFile "))) {
            const QString path = line.mid(15).trimmed();
            if (!path.isEmpty()) paths.append(path);
        }
    }
    return paths;
}

bool isBinaryType(const QString& type) {
    return type.startsWith(QStringLiteral("binary"), Qt::CaseInsensitive) ||
           type.startsWith(QStringLiteral("apple"), Qt::CaseInsensitive) ||
           type.startsWith(QStringLiteral("resource"), Qt::CaseInsensitive);
}

bool isDiffableAction(const QString& action) {
    return action == QStringLiteral("edit") ||
           action == QStringLiteral("integrate") ||
           action.isEmpty();
}

bool isAddAction(const QString& action) {
    return action == QStringLiteral("add") ||
           action == QStringLiteral("move/add") ||
           action == QStringLiteral("branch") ||
           action == QStringLiteral("import");
}

bool matchesSelection(const QString& depotFile, const QStringList& selectedFiles) {
    if (selectedFiles.isEmpty()) return true;
    const QString fileName = depotFile.section(QLatin1Char('/'), -1);
    for (const QString& selected : selectedFiles) {
        const QString clean = selected.trimmed();
        if (clean.isEmpty()) continue;
        if (depotFile.compare(clean, Qt::CaseInsensitive) == 0 ||
            fileName.compare(clean, Qt::CaseInsensitive) == 0 ||
            depotFile.endsWith(QLatin1Char('/') + clean, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

QString extractCodexError(const QByteArray& stderrBytes, int exitCode) {
    const QString text = QString::fromUtf8(stderrBytes).trimmed();
    if (!text.isEmpty()) {
        const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r?\n")), Qt::SkipEmptyParts);
        for (int i = int(lines.size()) - 1; i >= 0; --i) {
            const QString line = lines[i].trimmed();
            if (!line.startsWith(QStringLiteral("warning:"), Qt::CaseInsensitive) &&
                line != QStringLiteral("--------")) {
                return line;
            }
        }
        return lines.last().trimmed();
    }
    return QStringLiteral("Codex CLI failed with exit code %1.").arg(exitCode);
}

class DescriptionGenerator final : public QObject {
public:
    DescriptionGenerator(const QString& p4Exe,
                         const QString& codexExe,
                         const SubmitChangelistTarget& target,
                         const QStringList& selectedFiles,
                         const QString& model,
                         std::function<void(QString, QString)> callback,
                         QObject* parent)
        : QObject(parent), p4Exe_(p4Exe), codexExe_(codexExe), target_(target),
          selectedFiles_(selectedFiles), model_(model.trimmed()), callback_(std::move(callback)) {
        process_ = new QProcess(this);
        configureNoWindow(process_);
        timeout_ = new QTimer(this);
        timeout_->setSingleShot(true);

        connect(timeout_, &QTimer::timeout, this, [this] {
            timedOut_ = true;
            process_->kill();
            finish({}, stage_ == Stage::CodexExec
                ? QStringLiteral("Timed out while waiting for Codex CLI to generate description.")
                : QStringLiteral("Timed out while querying Perforce changelist diff."));
        });
        connect(process_, &QProcess::started, this, [this] {
            if (!stdinPayload_.isEmpty()) {
                process_->write(stdinPayload_);
                stdinPayload_.clear();
            }
            process_->closeWriteChannel();
        });
        connect(process_, &QProcess::readyReadStandardOutput, this, [this] {
            const QByteArray chunk = process_->readAllStandardOutput();
            if (stdoutBuffer_.size() < kMaxDiffBytes) {
                const int remaining = kMaxDiffBytes - int(stdoutBuffer_.size());
                stdoutBuffer_ += chunk.left(remaining);
                if (chunk.size() > remaining) truncated_ = true;
            } else {
                truncated_ = true;
            }
        });
        connect(process_, &QProcess::readyReadStandardError, this, [this] {
            const QByteArray chunk = process_->readAllStandardError();
            if (stderrBuffer_.size() < 32768)
                stderrBuffer_ += chunk.left(32768 - int(stderrBuffer_.size()));
        });
        connect(process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                finish({}, stage_ == Stage::CodexExec
                    ? QStringLiteral("Failed to start Codex CLI (%1).").arg(codexExe_)
                    : QStringLiteral("Failed to start p4.exe (%1).").arg(p4Exe_));
            }
        });
        connect(process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
            onStepFinished(exitCode, status == QProcess::NormalExit);
        });
    }

    ~DescriptionGenerator() override {
        if (process_ && process_->state() != QProcess::NotRunning) {
            disconnect(process_, nullptr, this, nullptr);
            process_->kill();
        }
    }

    void start() {
        if (model_.isEmpty() && cachedDefaultCodexModel.isEmpty()) {
            fetchCodexModels(this, [this](const QList<CodexModelInfo>& models, const QString&) {
                if (model_.isEmpty()) {
                    model_ = selectDefaultCodexModel(models);
                    if (model_.isEmpty()) model_ = QStringLiteral("gpt-5.6-luna");
                }
                startP4Opened();
            });
            return;
        }
        if (model_.isEmpty()) model_ = cachedDefaultCodexModel;
        startP4Opened();
    }

private:
    enum class Stage { P4Opened, P4Diff, P4FstatAdded, P4DescribeShelved, CodexExec };

    QStringList baseP4Args(bool ztag = false, bool clearDiffEnv = false) const {
        QStringList args{QStringLiteral("--no-script")};
        if (ztag) args << QStringLiteral("-ztag");
        args << QStringLiteral("-p") << target_.server
             << QStringLiteral("-u") << target_.user
             << QStringLiteral("-c") << target_.client;
        if (clearDiffEnv) {
            args << QStringLiteral("-E") << QStringLiteral("P4DIFF=")
                 << QStringLiteral("-E") << QStringLiteral("P4DIFFUNICODE=");
        }
        return args;
    }

    void runP4(Stage stage, const QStringList& subArgs, bool ztag = false, bool clearDiffEnv = false) {
        stage_ = stage;
        stdoutBuffer_.clear();
        stderrBuffer_.clear();
        stdinPayload_.clear();
        truncated_ = false;
        timedOut_ = false;

        auto env = QProcessEnvironment::systemEnvironment();
        if (clearDiffEnv) {
            env.remove(QStringLiteral("P4DIFF"));
            env.remove(QStringLiteral("P4DIFFUNICODE"));
        }
        process_->setProcessEnvironment(env);
        process_->setWorkingDirectory(QDir::tempPath());
        QStringList args = baseP4Args(ztag, clearDiffEnv);
        args.append(subArgs);
        timeout_->start(kP4TimeoutMs);
        startCliProcess(process_, p4Exe_, args);
    }

    void startP4Opened() {
        runP4(Stage::P4Opened,
              {QStringLiteral("opened"), QStringLiteral("-c"), target_.changelist},
              /*ztag=*/true);
    }

    void onStepFinished(int exitCode, bool normalExit) {
        if (finished_ || timedOut_ || !normalExit) return;
        timeout_->stop();

        if (stage_ == Stage::P4Opened) {
            if (exitCode != 0 && stdoutBuffer_.isEmpty()) {
                const QString err = QString::fromUtf8(stderrBuffer_).trimmed();
                finish({}, err.isEmpty()
                    ? QStringLiteral("Failed to list opened files for changelist %1.").arg(target_.changelist)
                    : err);
                return;
            }
            auto entries = parseOpenedZtag(stdoutBuffer_);
            if (!selectedFiles_.isEmpty()) {
                QList<OpenedFileEntry> filtered;
                for (const auto& entry : entries) {
                    if (matchesSelection(entry.depotFile, selectedFiles_))
                        filtered.append(entry);
                }
                if (!filtered.isEmpty()) entries = std::move(filtered);
            }
            openedEntries_ = std::move(entries);
            if (openedEntries_.isEmpty()) {
                if (target_.changelist != QStringLiteral("default")) {
                    runP4(Stage::P4DescribeShelved,
                          {QStringLiteral("describe"), QStringLiteral("-S"),
                           QStringLiteral("-du"), QStringLiteral("-a"), target_.changelist},
                          /*ztag=*/false, /*clearDiffEnv=*/true);
                    return;
                }
                finish({}, QStringLiteral("No opened files found in changelist %1.").arg(target_.changelist));
                return;
            }

            QStringList summaryLines;
            summaryLines << QStringLiteral("Changelist: %1").arg(target_.changelist);
            summaryLines << QStringLiteral("Files:");
            for (const auto& entry : openedEntries_) {
                const QString action = entry.action.isEmpty() ? QStringLiteral("edit") : entry.action;
                const QString type = entry.type.isEmpty() ? QStringLiteral("text") : entry.type;
                summaryLines << QStringLiteral("- [%1] %2 (%3)").arg(action, entry.depotFile, type);
                if (!isBinaryType(type)) {
                    if (isDiffableAction(action) && diffFiles_.size() < 50)
                        diffFiles_.append(entry.depotFile);
                    else if (isAddAction(action) && addedFiles_.size() < 20)
                        addedFiles_.append(entry.depotFile);
                }
            }
            changelistContext_ = summaryLines.join(QLatin1Char('\n')) + QStringLiteral("\n");

            if (!diffFiles_.isEmpty()) {
                QStringList diffArgs{QStringLiteral("diff"), QStringLiteral("-du")};
                diffArgs.append(diffFiles_);
                runP4(Stage::P4Diff, diffArgs, /*ztag=*/false, /*clearDiffEnv=*/true);
                return;
            }
            if (!addedFiles_.isEmpty()) {
                QStringList fstatArgs{QStringLiteral("fstat")};
                fstatArgs.append(addedFiles_);
                runP4(Stage::P4FstatAdded, fstatArgs, /*ztag=*/true);
                return;
            }
            startCodexExec();
            return;
        }

        if (stage_ == Stage::P4Diff) {
            const QString diffText = QString::fromUtf8(stdoutBuffer_).trimmed();
            if (!diffText.isEmpty()) {
                changelistContext_ += QStringLiteral("\nDiff:\n") + diffText;
                if (truncated_) changelistContext_ += QStringLiteral("\n... (diff truncated)");
            }
            if (!addedFiles_.isEmpty() && changelistContext_.toUtf8().size() < kMaxDiffBytes / 2) {
                QStringList fstatArgs{QStringLiteral("fstat")};
                fstatArgs.append(addedFiles_);
                runP4(Stage::P4FstatAdded, fstatArgs, /*ztag=*/true);
                return;
            }
            startCodexExec();
            return;
        }

        if (stage_ == Stage::P4FstatAdded) {
            const QStringList localPaths = parseFstatClientFiles(stdoutBuffer_);
            QString addedSnippets;
            for (const QString& localPath : localPaths) {
                QFile file(localPath);
                if (!file.open(QIODevice::ReadOnly)) continue;
                const QByteArray content = file.read(8192);
                if (content.contains('\0')) continue;
                addedSnippets += QStringLiteral("\n--- Added file: %1 ---\n").arg(QDir::fromNativeSeparators(localPath));
                addedSnippets += QString::fromUtf8(content);
                if (file.size() > content.size())
                    addedSnippets += QStringLiteral("\n... (file truncated)");
                addedSnippets += QLatin1Char('\n');
                if (changelistContext_.toUtf8().size() + addedSnippets.toUtf8().size() >= kMaxDiffBytes)
                    break;
            }
            if (!addedSnippets.isEmpty())
                changelistContext_ += QStringLiteral("\nAdded file contents:") + addedSnippets;
            startCodexExec();
            return;
        }

        if (stage_ == Stage::P4DescribeShelved) {
            const QString shelvedText = QString::fromUtf8(stdoutBuffer_).trimmed();
            if (exitCode != 0 || shelvedText.isEmpty()) {
                finish({}, QStringLiteral("No opened or shelved diff found for changelist %1.").arg(target_.changelist));
                return;
            }
            changelistContext_ = shelvedText;
            if (truncated_) changelistContext_ += QStringLiteral("\n... (diff truncated)");
            startCodexExec();
            return;
        }

        if (stage_ == Stage::CodexExec) {
            if (exitCode != 0) {
                finish({}, extractCodexError(stderrBuffer_, exitCode));
                return;
            }
            const QString description = cleanGeneratedDescription(QString::fromUtf8(stdoutBuffer_));
            if (description.isEmpty()) {
                finish({}, QStringLiteral("Codex CLI returned an empty description."));
                return;
            }
            finish(description, {});
        }
    }

    void startCodexExec() {
        stage_ = Stage::CodexExec;
        stdoutBuffer_.clear();
        stderrBuffer_.clear();
        truncated_ = false;
        timedOut_ = false;

        const QString prompt = QStringLiteral(
            "请根据以下 Perforce changelist 的文件变更与 diff 内容，使用简体中文编写简洁、准确的提交描述（changelist description）。\n"
            "要求：\n"
            "1. 只输出最终可直接填入提交描述文本框的正文，不要包含任何解释、前后缀、引导语、标题或 Markdown 代码块标记（不要输出 ```）。\n"
            "2. 概括核心改动内容与目的，语言简明扼要。\n\n"
            "Changelist 变更内容：\n") + changelistContext_;

        stdinPayload_ = prompt.toUtf8();
        process_->setProcessEnvironment(QProcessEnvironment::systemEnvironment());
        process_->setWorkingDirectory(QDir::tempPath());

        QStringList args{
            QStringLiteral("exec"),
            QStringLiteral("--skip-git-repo-check"),
            QStringLiteral("--ephemeral"),
            QStringLiteral("-s"), QStringLiteral("read-only"),
            QStringLiteral("-C"), QDir::tempPath(),
            QStringLiteral("-c"), QStringLiteral("model_reasoning_effort=\"low\""),
            QStringLiteral("-c"), QStringLiteral("mcp_servers={}"),
        };
        if (!model_.isEmpty()) {
            args << QStringLiteral("-m") << model_;
        }
        args << QStringLiteral("-");

        timeout_->start(kCodexExecTimeoutMs);
        startCliProcess(process_, codexExe_, args);
    }

    void finish(const QString& description, const QString& error) {
        if (finished_) return;
        finished_ = true;
        timeout_->stop();
        auto cb = std::move(callback_);
        deleteLater();
        if (cb) cb(description, error);
    }

    QString p4Exe_;
    QString codexExe_;
    SubmitChangelistTarget target_;
    QStringList selectedFiles_;
    QString model_;
    std::function<void(QString, QString)> callback_;
    QProcess* process_ = nullptr;
    QTimer* timeout_ = nullptr;
    Stage stage_ = Stage::P4Opened;
    QByteArray stdoutBuffer_;
    QByteArray stderrBuffer_;
    QByteArray stdinPayload_;
    QList<OpenedFileEntry> openedEntries_;
    QStringList diffFiles_;
    QStringList addedFiles_;
    QString changelistContext_;
    bool truncated_ = false;
    bool timedOut_ = false;
    bool finished_ = false;
};

QIcon createSparkleIcon(const QColor& color, qreal dpr) {
    const int px = qMax(14, qRound(14.0 * qMax(1.0, dpr)));
    QPixmap pixmap(px, px);
    pixmap.setDevicePixelRatio(qMax(1.0, dpr));
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);

    auto drawStar = [&painter](qreal cx, qreal cy, qreal outerR, qreal innerR) {
        QPainterPath path;
        path.moveTo(cx, cy - outerR);
        path.quadTo(cx, cy, cx + outerR, cy);
        path.quadTo(cx, cy, cx, cy + outerR);
        path.quadTo(cx, cy, cx - outerR, cy);
        path.quadTo(cx, cy, cx, cy - outerR);
        painter.drawPath(path);
        Q_UNUSED(innerR);
    };
    drawStar(6.0, 7.5, 4.8, 1.5);
    drawStar(11.2, 3.6, 2.4, 0.8);
    painter.end();

    QIcon icon;
    icon.addPixmap(pixmap, QIcon::Normal);
    icon.addPixmap(pixmap, QIcon::Disabled);
    return icon;
}

QIcon createSpinnerIcon(int step, const QColor& color, qreal dpr) {
    const int px = qMax(14, qRound(14.0 * qMax(1.0, dpr)));
    QPixmap pixmap(px, px);
    pixmap.setDevicePixelRatio(qMax(1.0, dpr));
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(7.0, 7.0);

    for (int i = 0; i < kSpinnerSteps; ++i) {
        const int age = (step - i + kSpinnerSteps) % kSpinnerSteps;
        const int alpha = qMax(45, 255 - age * 18);
        QColor spokeColor = color;
        spokeColor.setAlpha(alpha);
        QPen pen(spokeColor, 1.5, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(pen);
        painter.save();
        painter.rotate(i * (360.0 / kSpinnerSteps));
        painter.drawLine(QPointF(0.0, -3.2), QPointF(0.0, -5.8));
        painter.restore();
    }
    painter.end();

    QIcon icon;
    icon.addPixmap(pixmap, QIcon::Normal);
    icon.addPixmap(pixmap, QIcon::Disabled);
    return icon;
}

QLayout* findContainingLayout(QLayout* layout, QWidget* target) {
    if (!layout || !target) return nullptr;
    if (layout->indexOf(target) >= 0) return layout;
    for (int i = 0; i < layout->count(); ++i) {
        if (QLayout* child = layout->itemAt(i)->layout()) {
            if (QLayout* found = findContainingLayout(child, target))
                return found;
        }
    }
    return nullptr;
}

QStringList collectCheckedSubmitFiles(QDialog* dialog) {
    if (!dialog) return {};
    if (auto* tree = dialog->findChild<QTreeView*>(QStringLiteral("filesTree"))) {
        if (auto* model = tree->model()) {
            const int rows = model->rowCount();
            bool hasUnchecked = false;
            QStringList checked;
            for (int r = 0; r < rows; ++r) {
                const QVariant checkData = model->index(r, 0).data(Qt::CheckStateRole);
                if (checkData.isValid() && checkData.toInt() == Qt::Unchecked) {
                    hasUnchecked = true;
                    continue;
                }
                const QString file = model->index(r, 1).data(Qt::DisplayRole).toString().trimmed();
                const QString folder = model->columnCount() > 2
                    ? model->index(r, 2).data(Qt::DisplayRole).toString().trimmed()
                    : QString();
                if (!folder.isEmpty() && folder.startsWith(QStringLiteral("//")) && !file.isEmpty()) {
                    checked.append(folder.endsWith(QLatin1Char('/')) ? (folder + file) : (folder + QLatin1Char('/') + file));
                } else if (!file.isEmpty()) {
                    checked.append(file);
                }
            }
            if (hasUnchecked && !checked.isEmpty()) return checked;
        }
    }
    return {};
}

bool eligibleSubmitDialog(QDialog* dialog, QLabel** outLabel, QTextEdit** outDesc, QLayout** outLayout) {
    if (!dialog || !dialog->inherits("UISubmitWindow") ||
        !dialog->windowTitle().startsWith(QStringLiteral("Submit Changelist:"))) {
        return false;
    }
    QLabel* descLabel = nullptr;
    for (auto* label : dialog->findChildren<QLabel*>(QStringLiteral("descLabel"))) {
        if (!label->text().trimmed().startsWith(QStringLiteral("Write a changelist description")))
            continue;
        if (descLabel) return false; // ambiguous
        descLabel = label;
    }
    if (!descLabel || !descLabel->parentWidget()) return false;

    QTextEdit* descEdit = nullptr;
    for (auto* edit : dialog->findChildren<QTextEdit*>(QStringLiteral("desc"))) {
        if (edit->isReadOnly()) continue;
        if (descEdit) return false; // ambiguous
        descEdit = edit;
    }
    if (!descEdit) return false;

    QLayout* containingLayout = findContainingLayout(descLabel->parentWidget()->layout(), descLabel);
    if (!containingLayout ||
        (!qobject_cast<QGridLayout*>(containingLayout) && !qobject_cast<QBoxLayout*>(containingLayout))) {
        return false;
    }
    if (outLabel) *outLabel = descLabel;
    if (outDesc) *outDesc = descEdit;
    if (outLayout) *outLayout = containingLayout;
    return true;
}

class SubmitAiWatcher final : public QObject {
public:
    explicit SubmitAiWatcher(QApplication* app) : QObject(app) {
        setObjectName(QStringLiteral("p4vpatch.submitAiWatcher"));
        app->installEventFilter(this);
        for (auto* widget : QApplication::topLevelWidgets())
            inspectSubmitDialog(qobject_cast<QDialog*>(widget));
    }

protected:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() != QEvent::Show && event->type() != QEvent::WindowTitleChange)
            return false;
        auto* dialog = qobject_cast<QDialog*>(object);
        if (!dialog || !dialog->inherits("UISubmitWindow")) return false;
        QPointer<QDialog> weak(dialog);
        QTimer::singleShot(0, this, [weak] {
            if (weak) inspectSubmitDialog(weak);
        });
        return false;
    }
};
} // namespace

std::optional<SubmitChangelistTarget> parseSubmitWindowTitle(const QString& title) {
    static const QRegularExpression pattern(QStringLiteral(
        R"(^Submit Changelist:\s+(default|[1-9][0-9]*)\s+\(([^,()\r\n]+),\s*([^,()\r\n]+),\s*([^,()\r\n]+)\)$)"));
    const auto match = pattern.match(title.trimmed());
    if (!match.hasMatch()) return std::nullopt;
    SubmitChangelistTarget target{
        match.captured(1).trimmed(),
        match.captured(2).trimmed(),
        match.captured(3).trimmed(),
        match.captured(4).trimmed(),
    };
    if (target.changelist.isEmpty() || target.client.isEmpty() ||
        target.server.isEmpty() || target.user.isEmpty() ||
        target.changelist.startsWith(QLatin1Char('-')) ||
        target.client.startsWith(QLatin1Char('-')) ||
        target.server.startsWith(QLatin1Char('-')) ||
        target.user.startsWith(QLatin1Char('-'))) {
        return std::nullopt;
    }
    return target;
}

QString cleanGeneratedDescription(const QString& raw) {
    QString text = raw.trimmed();
    if (text.startsWith(QStringLiteral("```"))) {
        const int firstNewline = text.indexOf(QLatin1Char('\n'));
        if (firstNewline >= 0) {
            text = text.mid(firstNewline + 1);
            if (text.endsWith(QStringLiteral("```"))) {
                text.chop(3);
            }
            text = text.trimmed();
        }
    }
    return text;
}

QString selectDefaultCodexModel(const QList<CodexModelInfo>& models) {
    for (const auto& model : models) {
        if (model.id.contains(QStringLiteral("luna"), Qt::CaseInsensitive) ||
            model.displayName.contains(QStringLiteral("luna"), Qt::CaseInsensitive)) {
            return model.id;
        }
    }
    for (const auto& model : models) {
        if (model.isDefault) return model.id;
    }
    return models.isEmpty() ? QString() : models.first().id;
}

QString resolveCodexExecutable() {
    const QString overrideExe = qEnvironmentVariable("P4VPATCH_CODEX_EXECUTABLE").trimmed();
    if (!overrideExe.isEmpty() && QFileInfo(overrideExe).isFile())
        return QDir::cleanPath(overrideExe);

    QString found = QStandardPaths::findExecutable(QStringLiteral("codex.exe"));
    if (found.isEmpty()) found = QStandardPaths::findExecutable(QStringLiteral("codex"));
    if (!found.isEmpty()) {
        if (found.endsWith(QStringLiteral(".cmd"), Qt::CaseInsensitive) ||
            found.endsWith(QStringLiteral(".bat"), Qt::CaseInsensitive)) {
            const QString native = resolveNpmVendorCodex(QFileInfo(found).absolutePath());
            if (!native.isEmpty()) return native;
        }
        return QDir::cleanPath(found);
    }

    const QString roamingNpm = QDir::home().filePath(QStringLiteral("AppData/Roaming/npm"));
    const QString native = resolveNpmVendorCodex(roamingNpm);
    if (!native.isEmpty()) return native;
    const QString npmCmd = QDir(roamingNpm).filePath(QStringLiteral("codex.cmd"));
    if (QFileInfo(npmCmd).isFile()) return QDir::cleanPath(npmCmd);
    return {};
}

QString resolveP4Executable() {
    const QString overrideExe = qEnvironmentVariable("P4VPATCH_P4_EXECUTABLE").trimmed();
    if (!overrideExe.isEmpty() && QFileInfo(overrideExe).isFile())
        return QDir::cleanPath(overrideExe);

    const QString sibling = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("p4.exe"));
    if (QFileInfo(sibling).isFile()) return QDir::cleanPath(sibling);

    const QString pathExe = QStandardPaths::findExecutable(QStringLiteral("p4.exe"));
    return pathExe.isEmpty() ? QString() : QDir::cleanPath(pathExe);
}

void fetchCodexModels(QObject* context,
                      std::function<void(QList<CodexModelInfo> models, QString error)> callback) {
    const QString executable = resolveCodexExecutable();
    if (executable.isEmpty()) {
        if (callback) callback({}, QStringLiteral("Codex CLI was not found on PATH."));
        return;
    }
    auto* fetcher = new CodexModelsFetcher(executable, std::move(callback), context);
    fetcher->start();
}

void generateChangelistDescription(QObject* context,
                                   const SubmitChangelistTarget& target,
                                   const QStringList& selectedFiles,
                                   const QString& model,
                                   std::function<void(QString description, QString error)> callback) {
    const QString p4Exe = resolveP4Executable();
    if (p4Exe.isEmpty()) {
        if (callback) callback({}, QStringLiteral("p4.exe was not found next to P4V or on PATH."));
        return;
    }
    const QString codexExe = resolveCodexExecutable();
    if (codexExe.isEmpty()) {
        if (callback) callback({}, QStringLiteral("Codex CLI was not found on PATH."));
        return;
    }
    auto* generator = new DescriptionGenerator(
        p4Exe, codexExe, target, selectedFiles, model, std::move(callback), context);
    generator->start();
}

void inspectSubmitDialog(QDialog* dialog) {
    QLabel* descLabel = nullptr;
    QTextEdit* descEdit = nullptr;
    QLayout* containingLayout = nullptr;
    if (!eligibleSubmitDialog(dialog, &descLabel, &descEdit, &containingLayout)) return;
    if (dialog->findChild<QPushButton*>(QStringLiteral("p4vpatch.aiGenerateDescription"))) return;

    QWidget* containerParent = descLabel->parentWidget();
    auto* button = new QPushButton(QStringLiteral("AI Generate"), containerParent);
    button->setObjectName(QStringLiteral("p4vpatch.aiGenerateDescription"));
    button->setAutoDefault(false);
    button->setDefault(false);
    button->setCursor(Qt::PointingHandCursor);
    button->setIconSize(QSize(14, 14));
    button->setIcon(createSparkleIcon(button->palette().color(QPalette::ButtonText), button->devicePixelRatioF()));
    button->setToolTip(QStringLiteral("Generate changelist description from diff using Codex CLI"));
    button->setProperty("p4vpatch.aiLoading", false);

    auto* rowLayout = new QHBoxLayout();
    rowLayout->setObjectName(QStringLiteral("p4vpatch.descHeaderLayout"));
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(8);

    if (auto* grid = qobject_cast<QGridLayout*>(containingLayout)) {
        const int index = grid->indexOf(descLabel);
        int row = 0, column = 0, rowSpan = 1, columnSpan = 1;
        grid->getItemPosition(index, &row, &column, &rowSpan, &columnSpan);
        delete grid->takeAt(index);
        rowLayout->addWidget(descLabel, 0, Qt::AlignVCenter);
        rowLayout->addStretch(1);
        rowLayout->addWidget(button, 0, Qt::AlignVCenter);
        grid->addLayout(rowLayout, row, column, rowSpan, columnSpan);
    } else if (auto* box = qobject_cast<QBoxLayout*>(containingLayout)) {
        const int index = box->indexOf(descLabel);
        delete box->takeAt(index);
        rowLayout->addWidget(descLabel, 0, Qt::AlignVCenter);
        rowLayout->addStretch(1);
        rowLayout->addWidget(button, 0, Qt::AlignVCenter);
        box->insertLayout(index, rowLayout);
    }

    auto* spinnerTimer = new QTimer(button);
    spinnerTimer->setObjectName(QStringLiteral("p4vpatch.aiSpinnerTimer"));
    spinnerTimer->setInterval(kSpinnerIntervalMs);
    QObject::connect(spinnerTimer, &QTimer::timeout, button, [button] {
        const int nextStep = (button->property("p4vpatch.aiSpinnerStep").toInt() + 1) % kSpinnerSteps;
        button->setProperty("p4vpatch.aiSpinnerStep", nextStep);
        button->setIcon(createSpinnerIcon(
            nextStep, button->palette().color(QPalette::ButtonText), button->devicePixelRatioF()));
    });

    QPointer<QDialog> weakDialog(dialog);
    QPointer<QTextEdit> weakDesc(descEdit);
    QPointer<QPushButton> weakButton(button);
    QPointer<QTimer> weakTimer(spinnerTimer);

    QObject::connect(button, &QPushButton::clicked, dialog, [weakDialog, weakDesc, weakButton, weakTimer] {
        if (!weakDialog || !weakDesc || !weakButton || !weakTimer) return;
        if (weakButton->property("p4vpatch.aiLoading").toBool()) return;

        const auto target = parseSubmitWindowTitle(weakDialog->windowTitle());
        if (!target) {
            if (!qEnvironmentVariableIsSet("P4VPATCH_SUPPRESS_AI_MESSAGEBOX")) {
                QMessageBox::warning(weakDialog, QStringLiteral("AI Generate Description"),
                    QStringLiteral("Could not parse changelist connection details from window title."));
            }
            weakDialog->setProperty("p4vpatch.lastAiError",
                QStringLiteral("Could not parse changelist connection details from window title."));
            return;
        }

        weakDialog->setProperty("p4vpatch.lastAiError", QString());
        weakButton->setEnabled(false);
        weakButton->setProperty("p4vpatch.aiLoading", true);
        weakButton->setProperty("p4vpatch.aiSpinnerStep", 0);
        weakButton->setText(QStringLiteral("Generating..."));
        weakButton->setIcon(createSpinnerIcon(
            0, weakButton->palette().color(QPalette::ButtonText), weakButton->devicePixelRatioF()));
        weakTimer->start();

        const QStringList selectedFiles = collectCheckedSubmitFiles(weakDialog);
        generateChangelistDescription(weakDialog, *target, selectedFiles, codexModel(),
            [weakDialog, weakDesc, weakButton, weakTimer](const QString& description, const QString& error) {
                if (weakTimer) weakTimer->stop();
                if (weakButton) {
                    weakButton->setProperty("p4vpatch.aiLoading", false);
                    weakButton->setText(QStringLiteral("AI Generate"));
                    weakButton->setIcon(createSparkleIcon(
                        weakButton->palette().color(QPalette::ButtonText), weakButton->devicePixelRatioF()));
                    weakButton->setEnabled(true);
                }
                if (!weakDialog || !weakDesc) return;
                if (!error.isEmpty()) {
                    weakDialog->setProperty("p4vpatch.lastAiError", error);
                    if (!qEnvironmentVariableIsSet("P4VPATCH_SUPPRESS_AI_MESSAGEBOX")) {
                        QMessageBox::warning(weakDialog, QStringLiteral("AI Generate Description"), error);
                    }
                    return;
                }
                weakDesc->setPlainText(description);
                weakDesc->setFocus();
            });
    });
}

void installSubmitAiDescription(QApplication* app) {
    if (app && !app->findChild<QObject*>(QStringLiteral("p4vpatch.submitAiWatcher"), Qt::FindDirectChildrenOnly))
        new SubmitAiWatcher(app);
}
