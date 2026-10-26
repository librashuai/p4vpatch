#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

class QApplication;
class QDialog;
class QObject;

struct CodexModelInfo {
    QString id;
    QString displayName;
    QString description;
    bool isDefault = false;
};

struct SubmitChangelistTarget {
    QString changelist;
    QString client;
    QString server;
    QString user;
};

std::optional<SubmitChangelistTarget> parseSubmitWindowTitle(const QString& title);
QString cleanGeneratedDescription(const QString& raw);
QString selectDefaultCodexModel(const QList<CodexModelInfo>& models);
QString resolveCodexExecutable();
QString resolveP4Executable();

// Asynchronously fetch available models from Codex CLI's app-server model/list API.
void fetchCodexModels(QObject* context,
                      std::function<void(QList<CodexModelInfo> models, QString error)> callback);

// Asynchronously collect changelist diff via p4.exe and generate description via Codex CLI.
void generateChangelistDescription(QObject* context,
                                   const SubmitChangelistTarget& target,
                                   const QStringList& selectedFiles,
                                   const QString& model,
                                   std::function<void(QString description, QString error)> callback);

// Enhance only the verified Submit Changelist dialog (UISubmitWindow).
// Must be installed on the QApplication GUI thread.
void installSubmitAiDescription(QApplication* app);
void inspectSubmitDialog(QDialog* dialog);
