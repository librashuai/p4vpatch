// Inert PE/version-resource fixture; never loaded by the tests or the launcher.
extern "C" __declspec(dllexport) int fixtureVersion() { return 1; }
