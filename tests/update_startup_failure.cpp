#include <windows.h>
// Carries the real executable version resource, but deliberately fails before startup acknowledgement.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) { return 7; }
