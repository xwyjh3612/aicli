#pragma once
#include <windows.h>

namespace aicli::update {
// All entry points run on the UI thread; network and hashing run on workers.
void Start(HWND main_window);
void Stop();
void CheckForUpdates(HWND owner);
}
