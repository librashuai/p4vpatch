#pragma once

#include <QString>

class QDialog;

// Stored separately from P4V's ApplicationSettings.xml, in the same ~/.p4qt directory.
QString patchSettingsPath();
QString terminalShell();
QString codexModel();
void installPatchPreferences(QDialog* dialog); // GUI thread only
