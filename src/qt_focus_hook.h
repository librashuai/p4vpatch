#pragma once

// While a foreign page belongs to P4V's tab container, suppress only Qt's
// QApplication::focusChanged signal (not other QApplication signals).
// Failure to install must disable the native page rather than bypass it.
bool installTerminalFocusHook();
void setTerminalFocusFiltering(bool attached);
int terminalFocusSkips();
