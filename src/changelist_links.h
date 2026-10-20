#pragma once

class QApplication;

// Enhance only the verified, read-only Submitted Changelist description browser.
// Must be installed on the QApplication GUI thread.
void installChangelistLinks(QApplication* app);
