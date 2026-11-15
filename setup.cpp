#include <windows.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <tlhelp32.h>

#include <fstream>
#include <string>

#include "version.hpp"

namespace {

constexpr int IDR_APP_EXE = 101;
constexpr int IDR_OCR_WORKER = 102;
constexpr int IDR_OCR_SCRIPT = 103;

constexpr int ID_CHK_DESKTOP = 101;
constexpr int ID_CHK_STARTUP = 102;
constexpr int ID_BTN_INSTALL = 103;
constexpr int ID_BTN_CANCEL = 104;

HBRUSH g_bgBrush = nullptr;
HFONT g_font = nullptr;
HFONT g_boldFont = nullptr;
HFONT g_titleFont = nullptr;
HWND g_chkDesktop = nullptr;
HWND g_chkStartup = nullptr;
HWND g_lblStatus = nullptr;

void EnableDarkTitleBar(HWND hwnd) {
    HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
    if (!hDwm) return;
    using DwmSetAttrFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto fn = reinterpret_cast<DwmSetAttrFn>(reinterpret_cast<void*>(GetProcAddress(hDwm, "DwmSetWindowAttribute")));
    if (fn) {
        BOOL dark = TRUE;
        fn(hwnd, 20, &dark, sizeof(dark));
    }
    FreeLibrary(hDwm);
}

void KillRunningQuickPhrase() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"QuickPhrase.exe") == 0) {
                HANDLE hp = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pe.th32ProcessID);
                if (hp) {
                    TerminateProcess(hp, 0);
                    WaitForSingleObject(hp, 2000);
                    CloseHandle(hp);
                }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

bool ExtractResourceToFile(HINSTANCE hInst, int resId, const std::wstring& destPath) {
    HRSRC hRes = FindResourceW(hInst, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hRes) return false;
    HGLOBAL hData = LoadResource(hInst, hRes);
    if (!hData) return false;
    DWORD sz = SizeofResource(hInst, hRes);
    const void* ptr = LockResource(hData);
    if (!ptr || sz == 0) return false;

    std::ofstream out(destPath.c_str(), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;
    out.write(static_cast<const char*>(ptr), static_cast<std::streamsize>(sz));
    return out.good();
}

bool CreateShortcut(const std::wstring& targetExe, const std::wstring& lnkPath, const std::wstring& workDir) {
    IShellLinkW* psl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, reinterpret_cast<void**>(&psl));
    if (FAILED(hr) || !psl) return false;

    psl->SetPath(targetExe.c_str());
    psl->SetWorkingDirectory(workDir.c_str());
    psl->SetDescription(L"QuickPhrase — Custom Phrases & CS OCR");

    IPersistFile* ppf = nullptr;
    hr = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf));
    bool ok = false;
    if (SUCCEEDED(hr) && ppf) {
        ok = SUCCEEDED(ppf->Save(lnkPath.c_str(), TRUE));
        ppf->Release();
    }
    psl->Release();
    return ok;
}

std::wstring GetInstallDir() {
    wchar_t localApp[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localApp);
    std::wstring progDir = std::wstring(localApp) + L"\\Programs";
    CreateDirectoryW(progDir.c_str(), nullptr);
    std::wstring dir = progDir + L"\\QuickPhrase";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void RegisterInstalledApp(const std::wstring& installDir, const std::wstring& exePath) {
    HKEY hKey = nullptr;
    const wchar_t* subKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\QuickPhrase";
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
        auto setStr = [&](const wchar_t* name, const std::wstring& val) {
            RegSetValueExW(hKey, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(val.c_str()),
                           static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
        };
        setStr(L"DisplayName", APP_NAME_STR);
        setStr(L"DisplayVersion", APP_VERSION_STR);
        setStr(L"Publisher", APP_PUBLISHER_STR);
        setStr(L"InstallLocation", installDir);
        setStr(L"DisplayIcon", exePath);
        RegCloseKey(hKey);
    }
}

void SetStartup(const std::wstring& exePath, bool enable) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            std::wstring cmd = L"\"" + exePath + L"\"";
            RegSetValueExW(hKey, L"QuickPhraseTrayCpp", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(cmd.c_str()),
                           static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
        } else {
            RegDeleteValueW(hKey, L"QuickPhraseTrayCpp");
        }
        RegCloseKey(hKey);
    }
}

bool PerformInstall(HINSTANCE hInst, bool createDesktop, bool enableStartup) {
    KillRunningQuickPhrase();
    std::wstring dir = GetInstallDir();
    std::wstring exePath = dir + L"\\QuickPhrase.exe";

    if (!ExtractResourceToFile(hInst, IDR_APP_EXE, exePath)) return false;
    ExtractResourceToFile(hInst, IDR_OCR_WORKER, dir + L"\\ocr_worker.ps1");
    ExtractResourceToFile(hInst, IDR_OCR_SCRIPT, dir + L"\\ocr.ps1");

    wchar_t startMenu[MAX_PATH]{};
    SHGetFolderPathW(nullptr, CSIDL_PROGRAMS, nullptr, 0, startMenu);
    CreateShortcut(exePath, std::wstring(startMenu) + L"\\QuickPhrase.lnk", dir);

    if (createDesktop) {
        wchar_t desktop[MAX_PATH]{};
        SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, 0, desktop);
        CreateShortcut(exePath, std::wstring(desktop) + L"\\QuickPhrase.lnk", dir);
    }

    SetStartup(exePath, enableStartup);
    RegisterInstalledApp(dir, exePath);

    ShellExecuteW(nullptr, L"open", exePath.c_str(), nullptr, dir.c_str(), SW_SHOWDEFAULT);
    return true;
}

LRESULT CALLBACK SetupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
            auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id, HFONT f) {
                HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                                         x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                         hInst, nullptr);
                SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
                return c;
            };

            mk(L"STATIC", L"QuickPhrase Setup", 0, 24, 20, 380, 28, 0, g_titleFont);
            mk(L"STATIC", L"Version " APP_VERSION_STR L"  •  Publisher: " APP_PUBLISHER_STR, 0, 24, 52, 380, 22, 0, g_font);
            mk(L"STATIC", L"Install Location: %LOCALAPPDATA%\\Programs\\QuickPhrase", 0, 24, 84, 410, 22, 0, g_font);

            g_chkDesktop = mk(L"BUTTON", L"Create Desktop Shortcut", BS_AUTOCHECKBOX, 24, 122, 300, 24, ID_CHK_DESKTOP, g_font);
            SendMessageW(g_chkDesktop, BM_SETCHECK, BST_CHECKED, 0);

            g_chkStartup = mk(L"BUTTON", L"Start automatically with Windows (Silent Tray)", BS_AUTOCHECKBOX, 24, 152, 360, 24, ID_CHK_STARTUP, g_font);
            SendMessageW(g_chkStartup, BM_SETCHECK, BST_CHECKED, 0);

            g_lblStatus = mk(L"STATIC", L"Ready to install.", 0, 24, 198, 250, 22, 0, g_font);
            mk(L"BUTTON", L"Cancel", BS_PUSHBUTTON, 250, 192, 85, 32, ID_BTN_CANCEL, g_font);
            mk(L"BUTTON", L"Install", BS_DEFPUSHBUTTON, 345, 192, 100, 32, ID_BTN_INSTALL, g_boldFont);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkColor(hdc, RGB(17, 24, 39));
            SetTextColor(hdc, RGB(226, 232, 240));
            return reinterpret_cast<LRESULT>(g_bgBrush);
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == ID_BTN_CANCEL) {
                DestroyWindow(hwnd);
            } else if (id == ID_BTN_INSTALL) {
                bool desk = SendMessageW(g_chkDesktop, BM_GETCHECK, 0, 0) == BST_CHECKED;
                bool start = SendMessageW(g_chkStartup, BM_GETCHECK, 0, 0) == BST_CHECKED;
                SetWindowTextW(g_lblStatus, L"Installing...");
                HINSTANCE hInst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));
                if (PerformInstall(hInst, desk, start)) {
                    MessageBoxW(hwnd,
                                L"QuickPhrase installed successfully!\nLook for the 'QP' icon in your System Tray.",
                                L"QuickPhrase Setup — Mine1918",
                                MB_ICONINFORMATION);
                    DestroyWindow(hwnd);
                } else {
                    MessageBoxW(hwnd, L"Installation failed.", L"Error", MB_ICONERROR);
                }
            }
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (cmdLine && wcsstr(cmdLine, L"/SILENT") != nullptr) {
        PerformInstall(hInst, false, true);
        CoUninitialize();
        return 0;
    }

    g_bgBrush = CreateSolidBrush(RGB(17, 24, 39));
    g_font = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_boldFont = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_titleFont = CreateFontW(-20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SetupWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = L"QuickPhraseSetupWnd";
    RegisterClassExW(&wc);

    int w = 475, h = 280;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;

    HWND hwnd = CreateWindowExW(
        0, L"QuickPhraseSetupWnd",
        L"QuickPhrase Setup — Publisher: " APP_PUBLISHER_STR,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        sx, sy, w, h,
        nullptr, nullptr, hInst, nullptr);

    EnableDarkTitleBar(hwnd);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CoUninitialize();
    return 0;
}
