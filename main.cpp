#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <algorithm>
#include <string>
#include <vector>

#include "clipboard_win.hpp"
#include "ocr_snip.hpp"
#include "popup.hpp"
#include "settings_wnd.hpp"
#include "storage.hpp"
#include "updater.hpp"
#include "version.hpp"

namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 1;
constexpr UINT WM_APP_UPDATE_POPUP = WM_APP + 2;
constexpr UINT WM_APP_HIDE_POPUP = WM_APP + 3;
constexpr UINT WM_APP_SELECT_IDX = WM_APP + 4;
constexpr UINT WM_APP_NEXT_PAGE = WM_APP + 5;
constexpr UINT WM_APP_PREV_PAGE = WM_APP + 6;
constexpr UINT WM_APP_OCR_RESULT = WM_APP + 7;
constexpr UINT WM_APP_UPDATE_RESULT = WM_APP + 8;

constexpr UINT_PTR TIMER_PASTE_IMAGE = 1001;
constexpr UINT_PTR TIMER_PASTE_TEXT = 1002;
constexpr UINT_PTR TIMER_CHECK_TG_PREVIEW = 1003;

enum TrayMenuId {
    ID_TRAY_SETTINGS = 2001,
    ID_TRAY_OCR,
    ID_TRAY_QUICK_SCAN,
    ID_TRAY_AUTO_TG_OCR,
    ID_TRAY_ACTIVE,
    ID_TRAY_AUTOSTART,
    ID_TRAY_CHECK_UPDATE,
    ID_TRAY_QUIT,
};

const wchar_t* kHostClass = L"QuickPhraseTrayHost";

HINSTANCE g_hInst = nullptr;
HWND g_hostHwnd = nullptr;
HHOOK g_kbHook = nullptr;
HHOOK g_mouseHook = nullptr;

NOTIFYICONDATAW g_nid{};
HICON g_iconActive = nullptr;
HICON g_iconPaused = nullptr;

std::vector<Phrase> g_phrases;
AppSettings g_settings;
std::wstring g_buffer;
std::vector<Phrase> g_currentMatches;
bool g_expanding = false;

Phrase g_pendingPhrase;
int g_pendingDeleteLen = 0;

CandidatePopup g_popup;
SettingsWindow g_settingsWnd;
OcrSnipTool g_ocrTool;
bool g_autoTelegramOcr = false;
HWND g_lastTgOcrHwnd = nullptr;
bool g_lastScanWasAuto = false;

std::wstring GetWindowProcessName(HWND hwnd) {
    if (!hwnd) return {};
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid) return {};
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProc) return {};
    wchar_t path[MAX_PATH]{};
    DWORD sz = MAX_PATH;
    std::wstring res;
    if (QueryFullProcessImageNameW(hProc, 0, path, &sz)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        res = name ? (name + 1) : path;
    }
    CloseHandle(hProc);
    return res;
}

bool IsTelegramWindow(HWND hwnd) {
    return _wcsicmp(GetWindowProcessName(hwnd).c_str(), L"Telegram.exe") == 0;
}

bool IsChromeWindow(HWND hwnd) {
    return _wcsicmp(GetWindowProcessName(hwnd).c_str(), L"chrome.exe") == 0;
}

bool IsAllowedOcrApp(HWND hwnd) {
    return IsTelegramWindow(hwnd) || IsChromeWindow(hwnd);
}

bool IsTelegramMediaViewer(HWND hwnd) {
    if (!IsTelegramWindow(hwnd)) return false;
    wchar_t title[256]{};
    GetWindowTextW(hwnd, title, 256);
    for (wchar_t* p = title; *p; ++p) *p = static_cast<wchar_t>(towlower(*p));
    if (wcsstr(title, L"media") != nullptr || wcsstr(title, L"viewer") != nullptr) {
        return true;
    }
    // Telegram's main chat window has WS_CAPTION; the Media Viewer overlay does not.
    LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if ((style & WS_CAPTION) != 0) {
        return false;
    }
    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return false;
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        int ww = rc.right - rc.left;
        int wh = rc.bottom - rc.top;
        int mw = mi.rcMonitor.right - mi.rcMonitor.left;
        int mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        if (ww >= mw * 0.90 && wh >= mh * 0.90) {
            return true;
        }
    }
    return false;
}

HICON CreateBadgeIcon(bool active) {
    Gdiplus::Bitmap bmp(32, 32, PixelFormat32bppARGB);
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    if (active) {
        Gdiplus::LinearGradientBrush brush(
            Gdiplus::PointF(1, 1),
            Gdiplus::PointF(31, 31),
            Gdiplus::Color(255, 59, 130, 246),
            Gdiplus::Color(255, 139, 92, 246));
        g.FillEllipse(&brush, 1, 1, 30, 30);
    } else {
        Gdiplus::SolidBrush brush(Gdiplus::Color(255, 71, 85, 105));
        g.FillEllipse(&brush, 1, 1, 30, 30);
    }

    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff, 13.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    g.DrawString(L"QP", -1, &font, Gdiplus::RectF(0, 0, 32, 32), &sf, &white);

    HICON hIcon = nullptr;
    bmp.GetHICON(&hIcon);
    return hIcon;
}

void UpdateTrayIcon() {
    g_nid.hIcon = g_settings.enabled ? g_iconActive : g_iconPaused;
    wcscpy_s(g_nid.szTip, g_settings.enabled ? L"QuickPhrase (Active)" : L"QuickPhrase (Paused)");
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

std::vector<Phrase> FindMatches(const std::wstring& typed) {
    std::vector<Phrase> matches;
    if (typed.empty()) return matches;

    for (const auto& p : g_phrases) {
        if (!p.enabled || p.shortcut.empty()) continue;
        if (_wcsnicmp(p.shortcut.c_str(), typed.c_str(), typed.size()) == 0) {
            matches.push_back(p);
        }
    }
    std::sort(matches.begin(), matches.end(), [](const Phrase& a, const Phrase& b) {
        return _wcsicmp(a.shortcut.c_str(), b.shortcut.c_str()) < 0;
    });
    return matches;
}

void StartExpansion(const Phrase& phrase, int deleteCount) {
    g_expanding = true;
    g_popup.Hide();
    g_buffer.clear();
    g_currentMatches.clear();

    g_pendingPhrase = phrase;
    g_pendingDeleteLen = deleteCount;

    if (deleteCount > 0) {
        SendBackspaces(deleteCount);
    }

    if (!g_pendingPhrase.imageFile.empty()) {
        SetTimer(g_hostHwnd, TIMER_PASTE_IMAGE, 25, nullptr);
    } else if (!g_pendingPhrase.text.empty()) {
        SetTimer(g_hostHwnd, TIMER_PASTE_TEXT, 25, nullptr);
    } else {
        g_expanding = false;
    }
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HC_ACTION && !g_expanding && g_settings.enabled) {
        auto* kb = reinterpret_cast<KBDLLHOOKSTRUCT*>(lp);
        if ((kb->flags & LLKHF_INJECTED) == 0 && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
            bool modDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
                           (GetAsyncKeyState(VK_MENU) & 0x8000) ||
                           (GetAsyncKeyState(VK_LWIN) & 0x8000) ||
                           (GetAsyncKeyState(VK_RWIN) & 0x8000);

            if (modDown) {
                g_buffer.clear();
                PostMessageW(g_hostHwnd, WM_APP_HIDE_POPUP, 0, 0);
                return CallNextHookEx(g_kbHook, nCode, wp, lp);
            }

            DWORD vk = kb->vkCode;

            // If candidate popup is visible, handle Sogou-style Space / 1-5 / - = / Esc
            if (g_popup.IsVisible() && !g_currentMatches.empty()) {
                if (vk == VK_SPACE) {
                    PostMessageW(g_hostHwnd, WM_APP_SELECT_IDX, 0, 0);
                    return 1; // suppress Space
                }
                if (vk >= '1' && vk <= '9') {
                    size_t idx = static_cast<size_t>(vk - '1');
                    Phrase tmp;
                    if (g_popup.GetPageCandidate(idx, tmp)) {
                        PostMessageW(g_hostHwnd, WM_APP_SELECT_IDX, static_cast<WPARAM>(idx), 0);
                        return 1; // suppress digit
                    }
                }
                if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) {
                    size_t idx = static_cast<size_t>(vk - VK_NUMPAD1);
                    Phrase tmp;
                    if (g_popup.GetPageCandidate(idx, tmp)) {
                        PostMessageW(g_hostHwnd, WM_APP_SELECT_IDX, static_cast<WPARAM>(idx), 0);
                        return 1; // suppress digit
                    }
                }
                if (vk == VK_OEM_PLUS || vk == VK_NEXT) {
                    PostMessageW(g_hostHwnd, WM_APP_NEXT_PAGE, 0, 0);
                    return 1;
                }
                if (vk == VK_OEM_MINUS || vk == VK_PRIOR) {
                    PostMessageW(g_hostHwnd, WM_APP_PREV_PAGE, 0, 0);
                    return 1;
                }
                if (vk == VK_RETURN || vk == VK_ESCAPE) {
                    g_buffer.clear();
                    PostMessageW(g_hostHwnd, WM_APP_HIDE_POPUP, 0, 0);
                    return 1; // keep current typed text, close popup without sending Enter
                }
            }

            if (vk == VK_BACK) {
                if (!g_buffer.empty()) g_buffer.pop_back();
                PostMessageW(g_hostHwnd, WM_APP_UPDATE_POPUP, 0, 0);
                return CallNextHookEx(g_kbHook, nCode, wp, lp);
            }

            if (vk == VK_SPACE || vk == VK_RETURN || vk == VK_TAB || vk == VK_ESCAPE ||
                vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN ||
                vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT || vk == VK_DELETE) {
                g_buffer.clear();
                PostMessageW(g_hostHwnd, WM_APP_HIDE_POPUP, 0, 0);
                return CallNextHookEx(g_kbHook, nCode, wp, lp);
            }

            // Translate virtual key to character using foreground thread layout
            BYTE keyState[256]{};
            GetKeyboardState(keyState);
            if (GetAsyncKeyState(VK_SHIFT) & 0x8000) keyState[VK_SHIFT] = 0x80;
            if (GetKeyState(VK_CAPITAL) & 0x0001) keyState[VK_CAPITAL] = 0x01;

            HWND fg = GetForegroundWindow();
            HKL layout = GetKeyboardLayout(fg ? GetWindowThreadProcessId(fg, nullptr) : 0);
            wchar_t chBuf[4]{};
            int res = ToUnicodeEx(vk, kb->scanCode, keyState, chBuf, 4, 0, layout);
            if (res == 1 && iswprint(chBuf[0])) {
                g_buffer.push_back(chBuf[0]);
                if (g_buffer.size() > 32) {
                    g_buffer.erase(0, g_buffer.size() - 32);
                }
                PostMessageW(g_hostHwnd, WM_APP_UPDATE_POPUP, 0, 0);
            }
        }
    }
    return CallNextHookEx(g_kbHook, nCode, wp, lp);
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wp, LPARAM lp) {
    if (nCode == HC_ACTION && wp == WM_LBUTTONDOWN && !g_expanding) {
        auto* ms = reinterpret_cast<MSLLHOOKSTRUCT*>(lp);
        HWND hit = WindowFromPoint(ms->pt);
        wchar_t cls[64]{};
        if (hit) GetClassNameW(hit, cls, 64);
        if (wcscmp(cls, L"QuickPhraseCandidatePopup") != 0) {
            g_buffer.clear();
            PostMessageW(g_hostHwnd, WM_APP_HIDE_POPUP, 0, 0);
        }
        if (g_autoTelegramOcr && IsAllowedOcrApp(GetForegroundWindow())) {
            SetTimer(g_hostHwnd, TIMER_CHECK_TG_PREVIEW, 400, nullptr);
        }
    }
    return CallNextHookEx(g_mouseHook, nCode, wp, lp);
}

void ShowTrayMenu() {
    POINT pt{};
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_SETTINGS, L"Settings / Custom Phrases...");
    SetMenuDefaultItem(menu, ID_TRAY_SETTINGS, FALSE);
    AppendMenuW(menu, MF_STRING, ID_TRAY_QUICK_SCAN, L"Quick Scan ID/Order (F8)");
    AppendMenuW(menu, MF_STRING, ID_TRAY_OCR, L"Screenshot OCR (Ctrl+Alt+O)");
    AppendMenuW(menu, MF_STRING | (g_autoTelegramOcr ? MF_CHECKED : 0), ID_TRAY_AUTO_TG_OCR, L"Auto-OCR (Telegram & Chrome)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_settings.enabled ? MF_CHECKED : 0), ID_TRAY_ACTIVE, L"Active");
    AppendMenuW(menu, MF_STRING | (IsAutostartEnabled() ? MF_CHECKED : 0), ID_TRAY_AUTOSTART, L"Auto-start on PC Boot");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_CHECK_UPDATE, L"Check for Updates (v" APP_VERSION_STR L")...");
    AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit");

    SetForegroundWindow(g_hostHwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hostHwnd, nullptr);
    PostMessageW(g_hostHwnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK HostWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_TRAY:
            if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
                ShowTrayMenu();
            } else if (lp == WM_LBUTTONDBLCLK || lp == WM_LBUTTONUP) {
                if (lp == WM_LBUTTONDBLCLK) {
                    g_settingsWnd.Show();
                }
            }
            return 0;

        case WM_APP_UPDATE_POPUP: {
            g_currentMatches = FindMatches(g_buffer);
            auto exact = std::find_if(g_currentMatches.begin(), g_currentMatches.end(), [](const Phrase& p) {
                return _wcsicmp(p.shortcut.c_str(), g_buffer.c_str()) == 0;
            });
            if (exact != g_currentMatches.end()) {
                StartExpansion(*exact, static_cast<int>(g_buffer.size()));
            } else if (!g_currentMatches.empty()) {
                g_popup.ShowCandidates(g_buffer, g_currentMatches);
            } else {
                g_popup.Hide();
            }
            return 0;
        }

        case WM_APP_HIDE_POPUP:
            g_currentMatches.clear();
            g_popup.Hide();
            return 0;

        case WM_APP_SELECT_IDX: {
            size_t idx = static_cast<size_t>(wp);
            Phrase chosen;
            if (g_popup.GetPageCandidate(idx, chosen)) {
                int delCount = static_cast<int>(g_buffer.size());
                StartExpansion(chosen, delCount);
            }
            return 0;
        }

        case WM_APP_NEXT_PAGE:
            g_popup.NextPage();
            return 0;

        case WM_APP_PREV_PAGE:
            g_popup.PrevPage();
            return 0;

        case WM_APP_OCR_RESULT: {
            bool quickMode = (wp != 0);
            auto* textPtr = reinterpret_cast<std::wstring*>(lp);
            if (textPtr) {
                if (quickMode) {
                    std::wstring targetType;
                    std::wstring targetVal;
                    if (OcrSnipTool::ExtractCsTarget(*textPtr, targetType, targetVal)) {
                        SetClipboardTextW(targetVal);
                        std::wstring title = L"✓ " + targetType + L" Copied!";
                        g_ocrTool.ShowQuickToast(title, targetVal);
                    } else if (!g_lastScanWasAuto) {
                        g_ocrTool.ShowQuickToast(L"No ID or Order Number", L"Try F8 or Ctrl+Alt+O to select manually.");
                    }
                    g_lastScanWasAuto = false;
                } else {
                    g_ocrTool.ShowResultPopup(*textPtr);
                }
                delete textPtr;
            }
            return 0;
        }

        case WM_APP_UPDATE_RESULT: {
            bool silentIfUpToDate = (wp != 0);
            auto* info = reinterpret_cast<UpdateInfo*>(lp);
            if (info) {
                if (info->available) {
                    std::wstring msg = L"New version " + info->version + L" (Publisher: " APP_PUBLISHER_STR
                                       L") is available!\n\nDownload and install update now?";
                    if (MessageBoxW(nullptr, msg.c_str(), L"QuickPhrase Update", MB_YESNO | MB_ICONINFORMATION | MB_TOPMOST) == IDYES) {
                        if (DownloadAndRunUpdate(info->downloadUrl)) {
                            DestroyWindow(hwnd);
                        } else {
                            MessageBoxW(nullptr, L"Failed to download update.", L"QuickPhrase Update", MB_ICONERROR | MB_TOPMOST);
                        }
                    }
                } else if (!silentIfUpToDate) {
                    MessageBoxW(nullptr, L"You are running the latest version (" APP_VERSION_STR L").",
                                L"QuickPhrase Update", MB_ICONINFORMATION | MB_TOPMOST);
                }
                delete info;
            }
            return 0;
        }

        case WM_HOTKEY:
            if (wp == 1) {
                g_ocrTool.StartSnip();
            } else if (wp == 2) {
                HWND fg = GetForegroundWindow();
                if (IsAllowedOcrApp(fg)) {
                    g_lastScanWasAuto = false;
                    g_ocrTool.ScanForegroundWindow(false);
                }
            }
            return 0;

        case WM_TIMER:
            if (wp == TIMER_CHECK_TG_PREVIEW) {
                KillTimer(hwnd, TIMER_CHECK_TG_PREVIEW);
                if (g_autoTelegramOcr) {
                    HWND fg = GetForegroundWindow();
                    if (IsTelegramMediaViewer(fg) && fg != g_lastTgOcrHwnd) {
                        g_lastTgOcrHwnd = fg;
                        g_lastScanWasAuto = true;
                        g_ocrTool.ScanForegroundWindow(true);
                    } else if (IsChromeWindow(fg)) {
                        g_lastTgOcrHwnd = nullptr;
                        g_lastScanWasAuto = true;
                        g_ocrTool.ScanForegroundWindow(true);
                    } else if (!IsTelegramMediaViewer(fg)) {
                        g_lastTgOcrHwnd = nullptr;
                    }
                }
                return 0;
            }
            if (wp == TIMER_PASTE_IMAGE) {
                KillTimer(hwnd, TIMER_PASTE_IMAGE);
                std::wstring fullImg = GetImageFullPath(g_pendingPhrase.imageFile);
                if (SetClipboardImageFile(fullImg)) {
                    SendCtrlV();
                }
                if (!g_pendingPhrase.text.empty()) {
                    SetTimer(hwnd, TIMER_PASTE_TEXT, static_cast<UINT>(g_settings.pasteDelayMs), nullptr);
                } else {
                    g_expanding = false;
                }
                return 0;
            }
            if (wp == TIMER_PASTE_TEXT) {
                KillTimer(hwnd, TIMER_PASTE_TEXT);
                if (SetClipboardTextW(g_pendingPhrase.text)) {
                    SendCtrlV();
                }
                g_expanding = false;
                return 0;
            }
            break;

        case WM_COMMAND: {
            switch (LOWORD(wp)) {
                case ID_TRAY_SETTINGS:
                    g_settingsWnd.Show();
                    break;
                case ID_TRAY_OCR:
                    g_ocrTool.StartSnip();
                    break;
                case ID_TRAY_QUICK_SCAN:
                    g_ocrTool.ScanForegroundWindow(IsTelegramMediaViewer(GetForegroundWindow()));
                    break;
                case ID_TRAY_AUTO_TG_OCR:
                    g_autoTelegramOcr = !g_autoTelegramOcr;
                    break;
                case ID_TRAY_ACTIVE:
                    g_settings.enabled = !g_settings.enabled;
                    SaveSettings(g_settings);
                    UpdateTrayIcon();
                    g_settingsWnd.SyncFromExternal(g_phrases, g_settings);
                    break;
                case ID_TRAY_AUTOSTART:
                    SetAutostartEnabled(!IsAutostartEnabled());
                    g_settingsWnd.SyncFromExternal(g_phrases, g_settings);
                    break;
                case ID_TRAY_CHECK_UPDATE:
                    CheckForUpdateAsync(g_hostHwnd, WM_APP_UPDATE_RESULT, false);
                    break;
                case ID_TRAY_QUIT:
                    DestroyWindow(hwnd);
                    break;
            }
            return 0;
        }

        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
#ifdef QUICKPHRASE_PREVIEW
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"QuickPhraseWebPreviewSingleton");
#else
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, L"QuickPhraseDirectShortcutSingleton");
#endif
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (hMutex) CloseHandle(hMutex);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    Gdiplus::GdiplusStartupInput gdiInput;
    ULONG_PTR gdiToken = 0;
    Gdiplus::GdiplusStartup(&gdiToken, &gdiInput, nullptr);

    g_hInst = hInst;
    g_phrases = LoadPhrases();
    g_settings = LoadSettings();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kHostClass;
    RegisterClassExW(&wc);

    g_hostHwnd = CreateWindowExW(0, kHostClass, L"QuickPhraseHost", 0,
                                 0, 0, 0, 0, HWND_MESSAGE, nullptr, hInst, nullptr);

    g_iconActive = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    if (!g_iconActive) g_iconActive = CreateBadgeIcon(true);
    g_iconPaused = CreateBadgeIcon(false);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hostHwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_settings.enabled ? g_iconActive : g_iconPaused;
    wcscpy_s(g_nid.szTip, g_settings.enabled ? L"QuickPhrase (Active)" : L"QuickPhrase (Paused)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    g_popup.Create(hInst, [](const Phrase& chosen) {
        int delCount = static_cast<int>(g_buffer.size());
        StartExpansion(chosen, delCount);
    });

    g_settingsWnd.Create(hInst, [](const std::vector<Phrase>& updatedPhrases, const AppSettings& updatedSettings) {
        g_phrases = updatedPhrases;
        g_settings = updatedSettings;
        UpdateTrayIcon();
    });
    g_settingsWnd.SyncFromExternal(g_phrases, g_settings);
#ifdef QUICKPHRASE_PREVIEW
    g_settingsWnd.Show();
#endif

    g_ocrTool.Init(hInst, g_hostHwnd, [](const std::wstring& text, bool quickMode) {
        PostMessageW(g_hostHwnd, WM_APP_OCR_RESULT, quickMode ? 1 : 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
    });
    RegisterHotKey(g_hostHwnd, 1, MOD_CONTROL | MOD_ALT, 'O');
    RegisterHotKey(g_hostHwnd, 2, 0, VK_F8);

    // Silent background update check on startup
    CheckForUpdateAsync(g_hostHwnd, WM_APP_UPDATE_RESULT, true);

    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, hInst, 0);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, hInst, 0);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnregisterHotKey(g_hostHwnd, 1);
    UnregisterHotKey(g_hostHwnd, 2);
    g_ocrTool.Shutdown();

    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    if (g_iconActive) DestroyIcon(g_iconActive);
    if (g_iconPaused) DestroyIcon(g_iconPaused);

    Gdiplus::GdiplusShutdown(gdiToken);
    CoUninitialize();
    if (hMutex) CloseHandle(hMutex);
    return static_cast<int>(msg.wParam);
}
