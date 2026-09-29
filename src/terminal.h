#pragma once

class QMainWindow;

// Called only on P4V's GUI thread. Installs View -> Terminal when the
// expected P4V 2026.2 Log/Dashboard tab group exists. The added page and
// session are retained when hidden to avoid dangling P4V page references.
void installTerminal(QMainWindow* main);
