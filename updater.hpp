#pragma once

#include <windows.h>

#include <functional>
#include <string>

struct UpdateInfo {
    bool available = false;
    std::wstring version;
    std::wstring downloadUrl;
};

std::wstring GetUpdateUrlSetting();
UpdateInfo CheckForUpdate(const std::wstring& manifestUrl);
bool DownloadAndRunUpdate(const std::wstring& downloadUrl);
void CheckForUpdateAsync(HWND hostHwnd, UINT resultMsg, bool silentIfUpToDate);
