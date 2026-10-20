#include "launcher_theme.h"
#include <windows.h>
#include <dwmapi.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPalette>
#include <QResource>
#include <QWidget>
#include <QXmlStreamReader>

namespace launcher {
P4vAppearance readP4vAppearance(const QString& settingsFile) {
    P4vAppearance appearance;
    QFile file(settingsFile);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) return appearance;
    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        if (xml.name() == QLatin1String("Bool") && xml.attributes().value("varName") == QLatin1String("DarkTheme")) {
            appearance.dark = xml.readElementText() == QLatin1String("true");
        } else if (xml.name() == QLatin1String("Font") && xml.attributes().value("varName") == QLatin1String("AppFont")) {
            while (xml.readNextStartElement()) {
                const auto key = xml.name().toString();
                const auto value = xml.readElementText();
                if (key == "family" && !value.isEmpty() && value.size() < 128) appearance.font.setFamily(value);
                if (key == "pointSize") {
                    bool ok = false;
                    const double points = value.toDouble(&ok);
                    if (ok && points >= 6 && points <= 32) appearance.font.setPointSizeF(points);
                }
                if (key == "weight" && value == "Bold") appearance.font.setBold(true);
                if (key == "italic") appearance.font.setItalic(value == "true");
            }
        }
    }
    return xml.hasError() ? P4vAppearance{} : appearance;
}

LauncherTheme::~LauncherTheme() {
    if (!resource_.isEmpty()) QResource::unregisterResource(resource_, "/p4vpatch-theme");
}

void LauncherTheme::apply(QWidget& window, const QString& executable, const QString& settingsFile) {
    const auto appearance = readP4vAppearance(settingsFile.isEmpty()
        ? QDir::home().filePath(".p4qt/ApplicationSettings.xml") : settingsFile);
    const bool dark = appearance.dark;
    // P4V 2026.2's modern palette; also used if the build directory has no skins.
    const QString base = dark ? "#1E1F22" : "#FAFBFC";
    const QString surface = dark ? "#2B2D30" : "#FFFFFF";
    const QString border = dark ? "#3A3D40" : "#D7DBE0";
    const QString text = dark ? "#E6E8EB" : "#1C1E21";
    const QString muted = dark ? "#9DA0A6" : "#5E646D";
    const QString disabled = dark ? "#5A5D62" : "#A8ADB5";
    const QString accent = dark ? "#3D8BFD" : "#1F6FEB";
    const QString hover = dark ? "#5A9DFF" : "#388BFD";
    const QString selected = dark ? "#264F78" : "#CCE0FF";
    const QString error = dark ? "#FF7B72" : "#C62828";
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(base));
    palette.setColor(QPalette::WindowText, QColor(text));
    palette.setColor(QPalette::Base, QColor(surface));
    palette.setColor(QPalette::AlternateBase, QColor(base));
    palette.setColor(QPalette::Text, QColor(text));
    palette.setColor(QPalette::Button, QColor(surface));
    palette.setColor(QPalette::ButtonText, QColor(text));
    palette.setColor(QPalette::Highlight, QColor(accent));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, QColor(accent));
    palette.setColor(QPalette::ToolTipBase, QColor(surface));
    palette.setColor(QPalette::ToolTipText, QColor(text));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        palette.setColor(QPalette::Disabled, role, QColor(disabled));
    window.setPalette(palette);
    window.setFont(appearance.font);
    window.setProperty("p4vDarkTheme", dark);

    const auto resources = QFileInfo(executable).absolutePath() + "/P4VResources/";
    const auto resource = resources + (dark ? "images-dark.rcc" : "images.rcc");
    if (resource != resource_) {
        if (!resource_.isEmpty()) QResource::unregisterResource(resource_, "/p4vpatch-theme");
        resource_.clear();
        if (QResource::registerResource(resource, "/p4vpatch-theme")) resource_ = resource;
    }
    QFile skin(resources + "skins/" + (dark ? "DarkTheme.qss" : "Default.qss"));
    QString style;
    loadedSkin_ = skin.open(QIODevice::ReadOnly) && skin.size() <= 1024 * 1024;
    if (loadedSkin_) {
        style = QString::fromUtf8(skin.readAll());
        style.replace("url(:/", "url(:/p4vpatch-theme/");
    }
    if (!loadedSkin_) {
        style += QStringLiteral(R"(
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid $border; border-radius: 3px; background: $surface; }
QCheckBox::indicator:checked { background: $accent; border-color: $accent; image: url(:/launcher/check.svg); }
QCheckBox::indicator:disabled { border-color: $disabled; }
)");
    }
    // Only launcher-specific layout/affordances override the installed P4V skin.
    style += QStringLiteral(R"(
QWidget#LauncherWindow { background: $base; color: $text; }
QWidget#LauncherWindow QLabel { color: $text; background: transparent; }
QWidget#LauncherWindow QLabel#appTitle { font-size: 23px; font-weight: 600; }
QWidget#LauncherWindow QLabel#versionBadge { color: $muted; }
QWidget#LauncherWindow QLabel#statusTitle { font-size: 16px; font-weight: 600; }
QWidget#LauncherWindow QLabel#statusSummary,
QWidget#LauncherWindow QLabel#targetCaption { color: $muted; }
QFrame#statusCard { background: $surface; border: 1px solid $border; border-radius: 8px; }
QFrame#statusCard QLabel { border: none; }
QWidget#LauncherWindow QPushButton { background: $surface; color: $text; border: 1px solid $border;
    border-radius: 5px; padding: 7px 14px; min-height: 18px; }
QWidget#LauncherWindow QPushButton:hover { background: $selected; border-color: $accent; }
QWidget#LauncherWindow QPushButton:pressed { background: $selected; }
QWidget#LauncherWindow QPushButton:focus { border-color: $accent; }
QWidget#LauncherWindow QPushButton#primaryAction { background: $accent; color: white; border-color: $accent;
    font-weight: 600; min-width: 84px; }
QWidget#LauncherWindow QPushButton#primaryAction:hover { background: $hover; border-color: $hover; }
QWidget#LauncherWindow QPushButton#updateAction { border-color: $accent; color: $accent; }
QWidget#LauncherWindow QPushButton[secondary="true"] { background: transparent; border-color: transparent; color: $muted; }
QWidget#LauncherWindow QPushButton[secondary="true"]:hover { color: $accent; background: $selected; }
QWidget#LauncherWindow QPushButton[secondary="true"]:focus { border-color: $accent; }
QWidget#LauncherWindow QPushButton:disabled,
QWidget#LauncherWindow QPushButton#primaryAction:disabled,
QWidget#LauncherWindow QPushButton#updateAction:disabled { background: $base; color: $disabled; border-color: $border; }
QWidget#LauncherWindow QCheckBox { color: $muted; spacing: 8px; }
QToolButton#detailsToggle { color: $muted; background: transparent; border: none; padding: 4px; }
QToolButton#detailsToggle:hover { color: $accent; }
QWidget#launcherDetails { background: $surface; border-radius: 5px; }
QWidget#launcherDetails QLabel { color: $muted; }
QTextEdit#releaseNotes { background: $surface; color: $text; border: 1px solid $border;
    border-radius: 6px; padding: 10px; selection-background-color: $accent; selection-color: white; }
QProgressBar#activity { border: none; background: $border; max-height: 2px; min-height: 2px; }
QProgressBar#activity::chunk { background: $accent; }
QWidget#LauncherWindow QLabel#statusTitle[error="true"],
QWidget#LauncherWindow QLabel#statusSummary[error="true"],
QWidget#LauncherWindow QLabel#statusDiagnostic[error="true"],
QWidget#LauncherWindow QMessageBox[error="true"] QLabel { color: $error; }
)");
    for (const auto& pair : {qMakePair("$base", base), qMakePair("$surface", surface), qMakePair("$border", border),
         qMakePair("$text", text), qMakePair("$muted", muted), qMakePair("$disabled", disabled),
         qMakePair("$accent", accent), qMakePair("$hover", hover), qMakePair("$selected", selected), qMakePair("$error", error)})
        style.replace(QString::fromLatin1(pair.first), pair.second);
    window.setStyleSheet(style);
    // Match the light/dark client area without replacing the native Windows frame.
    const BOOL immersive = dark;
    const auto hwnd = reinterpret_cast<HWND>(window.winId());
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &immersive, sizeof(immersive))))
        DwmSetWindowAttribute(hwnd, 19, &immersive, sizeof(immersive));
}
}
