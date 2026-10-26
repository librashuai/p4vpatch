#include "preferences.h"
#include "submit_ai.h"

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QAbstractButton>
#include <QSplitter>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cstring>

QString patchSettingsPath() {
    const QString overridePath = qEnvironmentVariable("P4VPATCH_SETTINGS_PATH").trimmed();
    if (!overridePath.isEmpty()) return overridePath;
    return QDir::home().filePath(QStringLiteral(".p4qt/p4vpatch.ini"));
}

namespace {
bool validShell(const QString& value) {
    if (value.compare(QStringLiteral("cmd.exe"), Qt::CaseInsensitive) == 0 ||
        value.compare(QStringLiteral("pwsh"), Qt::CaseInsensitive) == 0) return true;
    const QFileInfo file(value);
    return file.isAbsolute() && file.isFile() &&
           file.fileName().compare(QStringLiteral("pwsh.exe"), Qt::CaseInsensitive) == 0;
}

bool validCodexModel(const QString& value) {
    if (value.isEmpty()) return true;
    if (value.startsWith(QLatin1Char('-')) || value.size() > 128) return false;
    for (QChar c : value) {
        if (c.unicode() <= 32 || c.unicode() > 126) return false;
    }
    return true;
}
} // namespace

QString terminalShell() {
    QSettings settings(patchSettingsPath(), QSettings::IniFormat);
    const QString value = settings.value(QStringLiteral("Terminal/Shell"),
                                          QStringLiteral("cmd.exe")).toString().trimmed();
    return validShell(value) ? value : QStringLiteral("cmd.exe");
}

QString codexModel() {
    QSettings settings(patchSettingsPath(), QSettings::IniFormat);
    const QString value = settings.value(QStringLiteral("Codex/Model")).toString().trimmed();
    return validCodexModel(value) ? value : QString();
}

void installPatchPreferences(QDialog* dialog) {
    if (!dialog || std::strcmp(dialog->metaObject()->className(), "P4VPreferencesDialog") != 0 ||
        dialog->property("p4vpatch.preferencesInstalled").toBool()) return;

    // Only extend the verified P4V preferences layout; never modify an unrelated dialog.
    auto* splitter = dialog->findChild<QSplitter*>(QStringLiteral("preferenceSplitter"));
    if (!splitter || splitter->count() != 2) return;
    auto* tree = splitter->findChild<QTreeWidget*>(QString(), Qt::FindDirectChildrenOnly);
    auto* stack = splitter->findChild<QStackedWidget*>(QString(), Qt::FindDirectChildrenOnly);
    auto* buttons = dialog->findChild<QDialogButtonBox*>(QString(), Qt::FindDirectChildrenOnly);
    if (!tree || !stack || !buttons || !buttons->button(QDialogButtonBox::Ok) ||
        !buttons->button(QDialogButtonBox::Apply) ||
        tree->topLevelItemCount() < 1 ||
        tree->topLevelItem(0)->text(0) != QStringLiteral("Connections") ||
        stack->count() < 1 ||
        std::strcmp(stack->widget(0)->metaObject()->className(), "ConnectionsPage") != 0) return;

    auto* page = new QWidget(stack);
    page->setObjectName(QStringLiteral("p4vpatch.preferencesPage"));
    auto* layout = new QVBoxLayout(page);
    auto* group = new QGroupBox(QStringLiteral("Terminal"), page);
    auto* groupLayout = new QVBoxLayout(group);
    groupLayout->addWidget(new QLabel(QStringLiteral("Terminal Shell:"), group));
    auto* row = new QHBoxLayout;
    auto* shell = new QComboBox(group);
    shell->setObjectName(QStringLiteral("p4vpatch.terminalShell"));
    shell->setEditable(true);
    shell->addItems({QStringLiteral("cmd.exe"), QStringLiteral("pwsh")});
    shell->setCurrentText(terminalShell());
    shell->setToolTip(QStringLiteral("cmd.exe, pwsh, or an absolute path to pwsh.exe"));
    row->addWidget(shell, 1);
    auto* browse = new QPushButton(QStringLiteral("Browse..."), group);
    row->addWidget(browse);
    groupLayout->addLayout(row);
    groupLayout->addWidget(new QLabel(QStringLiteral("Changes take effect for new terminal sessions.\n"
        "If Terminal is already open, restart P4V to use the new shell."), group));
    layout->addWidget(group);

    auto* codexGroup = new QGroupBox(QStringLiteral("Codex"), page);
    codexGroup->setObjectName(QStringLiteral("p4vpatch.codexGroup"));
    auto* codexLayout = new QVBoxLayout(codexGroup);
    codexLayout->addWidget(new QLabel(QStringLiteral("Model:"), codexGroup));
    auto* modelRow = new QHBoxLayout;
    auto* modelCombo = new QComboBox(codexGroup);
    modelCombo->setObjectName(QStringLiteral("p4vpatch.codexModel"));
    modelCombo->setToolTip(QStringLiteral("Model used by Codex CLI when generating changelist descriptions"));
    const QString initialModel = codexModel();
    if (!initialModel.isEmpty()) {
        modelCombo->addItem(initialModel, initialModel);
        modelCombo->setCurrentIndex(0);
    }
    auto* refreshModels = new QPushButton(QStringLiteral("Refresh"), codexGroup);
    refreshModels->setObjectName(QStringLiteral("p4vpatch.codexRefreshModels"));
    modelRow->addWidget(modelCombo, 1);
    modelRow->addWidget(refreshModels);
    codexLayout->addLayout(modelRow);
    auto* modelStatus = new QLabel(codexGroup);
    modelStatus->setObjectName(QStringLiteral("p4vpatch.codexModelStatus"));
    modelStatus->setWordWrap(true);
    codexLayout->addWidget(modelStatus);
    layout->addWidget(codexGroup);

    layout->addStretch();
    QObject::connect(browse, &QPushButton::clicked, page, [shell, page] {
        QString path = QFileDialog::getOpenFileName(page, QStringLiteral("Choose pwsh.exe"),
            QString(), QStringLiteral("PowerShell (pwsh.exe);;Executables (*.exe)"));
        if (!path.isEmpty()) shell->setCurrentText(QDir::toNativeSeparators(path));
    });

    QPointer<QWidget> weakPage(page);
    QPointer<QComboBox> weakModelCombo(modelCombo);
    QPointer<QPushButton> weakRefresh(refreshModels);
    QPointer<QLabel> weakStatus(modelStatus);
    auto loadModels = [weakPage, weakModelCombo, weakRefresh, weakStatus] {
        if (!weakPage || !weakModelCombo || !weakRefresh || !weakStatus) return;
        weakRefresh->setEnabled(false);
        weakStatus->setText(QStringLiteral("Loading models from Codex CLI..."));
        fetchCodexModels(weakPage, [weakModelCombo, weakRefresh, weakStatus](
                const QList<CodexModelInfo>& models, const QString& error) {
            if (weakRefresh) weakRefresh->setEnabled(true);
            if (!weakModelCombo || !weakStatus) return;
            if (!error.isEmpty()) {
                weakStatus->setText(error);
                return;
            }
            QString preferred = weakModelCombo->currentText().trimmed();
            if (preferred.isEmpty()) preferred = codexModel();
            const QString defaultModelId = selectDefaultCodexModel(models);

            weakModelCombo->clear();
            int selectIndex = -1;
            int defaultIndex = -1;
            for (int i = 0; i < models.size(); ++i) {
                const auto& m = models[i];
                weakModelCombo->addItem(m.id, m.id);
                QString tooltip = m.displayName;
                if (!m.description.isEmpty()) {
                    if (!tooltip.isEmpty()) tooltip += QStringLiteral(" - ");
                    tooltip += m.description;
                }
                if (!tooltip.isEmpty())
                    weakModelCombo->setItemData(i, tooltip, Qt::ToolTipRole);
                if (!preferred.isEmpty() && m.id == preferred)
                    selectIndex = i;
                if (!defaultModelId.isEmpty() && m.id == defaultModelId && defaultIndex < 0)
                    defaultIndex = i;
            }
            if (!preferred.isEmpty() && selectIndex < 0) {
                selectIndex = weakModelCombo->count();
                weakModelCombo->addItem(preferred, preferred);
            }
            if (selectIndex < 0) selectIndex = defaultIndex >= 0 ? defaultIndex : 0;
            if (weakModelCombo->count() > 0 && selectIndex >= 0)
                weakModelCombo->setCurrentIndex(selectIndex);
            weakStatus->setText(QStringLiteral("Loaded %1 model(s) from Codex CLI.").arg(models.size()));
        });
    };
    QObject::connect(refreshModels, &QPushButton::clicked, page, loadModels);
    loadModels();

    auto* item = new QTreeWidgetItem(tree);
    item->setText(0, QStringLiteral("Patch"));
    stack->addWidget(page);
    // P4V handles its own items; its selection handler does not know this page.
    QObject::connect(tree, &QTreeWidget::currentItemChanged, dialog,
            [tree, stack, item, page](QTreeWidgetItem* current) {
        if (current == item) stack->setCurrentWidget(page);
    });
    QObject::connect(buttons, &QDialogButtonBox::clicked, dialog,
            [dialog, shell, modelCombo, buttons](QAbstractButton* button) {
        if (button != buttons->button(QDialogButtonBox::Ok) &&
            button != buttons->button(QDialogButtonBox::Apply)) return;
        const QString value = shell->currentText().trimmed();
        if (!validShell(value)) {
            QMessageBox::warning(dialog, QStringLiteral("Patch preferences"),
                QStringLiteral("Not saved. Enter cmd.exe, pwsh, or an existing absolute path to pwsh.exe."));
            return;
        }
        const QString selectedModel = modelCombo->currentText().trimmed();
        if (!validCodexModel(selectedModel)) {
            QMessageBox::warning(dialog, QStringLiteral("Patch preferences"),
                QStringLiteral("Not saved. Invalid Codex model name."));
            return;
        }
        QSettings settings(patchSettingsPath(), QSettings::IniFormat);
        settings.setValue(QStringLiteral("Terminal/Shell"), value);
        settings.setValue(QStringLiteral("Codex/Model"), selectedModel);
        settings.sync();
        if (settings.status() != QSettings::NoError)
            QMessageBox::warning(dialog, QStringLiteral("Patch preferences"),
                QStringLiteral("Could not save Patch settings to %1").arg(patchSettingsPath()));
    });
    dialog->setProperty("p4vpatch.preferencesInstalled", true);
}
