#include "settings_wnd.hpp"

#include <commctrl.h>
#include <commdlg.h>
#include <gdiplus.h>

#include <algorithm>

#include "clipboard_win.hpp"

namespace {

const wchar_t* kSettingsClass = L"QuickPhraseSettingsWnd";

enum ControlId {
    ID_EDIT_SEARCH = 100,
    ID_LIST = 101,
    ID_BTN_NEW,
    ID_BTN_IMPORT,
    ID_EDIT_SHORTCUT,
    ID_COMBO_POS,
    ID_CHK_PHRASE_ENABLED,
    ID_IMG_PREVIEW,
    ID_BTN_PASTE_IMG,
    ID_BTN_BROWSE_IMG,
    ID_BTN_REMOVE_IMG,
    ID_EDIT_TEXT,
    ID_BTN_SAVE,
    ID_BTN_DELETE,
    ID_CHK_ACTIVE,
    ID_CHK_AUTOSTART,
    ID_EDIT_DELAY,
};

HBRUSH g_bgBrush = nullptr;
HBRUSH g_cardBrush = nullptr;
HBRUSH g_inputBrush = nullptr;

void EnableDarkTitleBar(HWND hwnd) {
    HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
    if (!hDwm) return;
    using DwmSetAttrFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto fn = reinterpret_cast<DwmSetAttrFn>(reinterpret_cast<void*>(GetProcAddress(hDwm, "DwmSetWindowAttribute")));
    if (fn) {
        BOOL dark = TRUE;
        fn(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    }
    FreeLibrary(hDwm);
}

std::wstring GetWindowTextStr(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring out(len + 1, L'\0');
    GetWindowTextW(hwnd, out.data(), len + 1);
    out.resize(len);
    return out;
}

void AddRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void DrawModernButton(DRAWITEMSTRUCT* dis, const wchar_t* text, int styleType) {
    // styleType: 0 = dark slate secondary, 1 = Antigravity blue->violet gradient, 2 = danger dark rose
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;

    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

    Gdiplus::SolidBrush parentBg(Gdiplus::Color(255, 17, 24, 39));
    g.FillRectangle(&parentBg, 0, 0, w, h);

    Gdiplus::RectF r(1.0f, 1.0f, static_cast<float>(w - 2), static_cast<float>(h - 2));
    Gdiplus::GraphicsPath path;
    AddRoundRect(path, r, 7.0f);

    Gdiplus::Color fg;
    if (styleType == 1) {
        Gdiplus::LinearGradientBrush grad(
            Gdiplus::PointF(r.X, r.Y),
            Gdiplus::PointF(r.GetRight(), r.GetBottom()),
            pressed ? Gdiplus::Color(255, 37, 99, 235) : Gdiplus::Color(255, 59, 130, 246),
            pressed ? Gdiplus::Color(255, 109, 40, 217) : Gdiplus::Color(255, 139, 92, 246));
        g.FillPath(&grad, &path);
        fg = Gdiplus::Color(255, 255, 255, 255);
    } else if (styleType == 2) {
        Gdiplus::SolidBrush fill(pressed ? Gdiplus::Color(255, 127, 29, 29) : Gdiplus::Color(255, 69, 10, 10));
        Gdiplus::Pen border(Gdiplus::Color(255, 153, 27, 27), 1.0f);
        g.FillPath(&fill, &path);
        g.DrawPath(&border, &path);
        fg = Gdiplus::Color(255, 252, 165, 165);
    } else {
        Gdiplus::SolidBrush fill(pressed ? Gdiplus::Color(255, 51, 65, 85) : Gdiplus::Color(255, 30, 41, 59));
        Gdiplus::Pen border(Gdiplus::Color(255, 51, 65, 85), 1.0f);
        g.FillPath(&fill, &path);
        g.DrawPath(&border, &path);
        fg = Gdiplus::Color(255, 226, 232, 240);
    }

    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font font(&ff, 13.0f, styleType == 1 ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::SolidBrush fgBrush(fg);
    Gdiplus::StringFormat sf;
    sf.SetAlignment(Gdiplus::StringAlignmentCenter);
    sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    g.DrawString(text, -1, &font, r, &sf, &fgBrush);
}

std::wstring ToLowerTrimmed(const std::wstring& s) {
    size_t start = s.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return {};
    size_t end = s.find_last_not_of(L" \t\r\n");
    std::wstring out = s.substr(start, end - start + 1);
    if (!out.empty()) {
        CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    }
    return out;
}

bool StartsWithLower(const std::wstring& value, const std::wstring& lowerPrefix) {
    if (lowerPrefix.empty() || value.size() < lowerPrefix.size()) return false;
    return _wcsnicmp(value.c_str(), lowerPrefix.c_str(), lowerPrefix.size()) == 0;
}

bool ContainsLower(const std::wstring& value, const std::wstring& lowerQuery) {
    if (lowerQuery.empty() || value.size() < lowerQuery.size()) return false;
    std::wstring lowerVal = value;
    CharLowerBuffW(lowerVal.data(), static_cast<DWORD>(lowerVal.size()));
    return lowerVal.find(lowerQuery) != std::wstring::npos;
}

} // namespace

bool SettingsWindow::Create(HINSTANCE hInst, DataChangedCallback onChanged) {
    hInst_ = hInst;
    onChanged_ = std::move(onChanged);

    if (!g_bgBrush) g_bgBrush = CreateSolidBrush(RGB(11, 17, 32));      // #0B1120
    if (!g_cardBrush) g_cardBrush = CreateSolidBrush(RGB(17, 24, 39));  // #111827
    if (!g_inputBrush) g_inputBrush = CreateSolidBrush(RGB(15, 23, 42)); // #0F172A

    font_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    boldFont_ = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hIconSm = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = g_bgBrush;
    wc.lpszClassName = kSettingsClass;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        0,
        kSettingsClass,
        L"QuickPhrase — Antigravity Studio",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 880, 610,
        nullptr, nullptr, hInst, this);

    if (hwnd_) {
        EnableDarkTitleBar(hwnd_);
    }

    return hwnd_ != nullptr;
}

void SettingsWindow::Show() {
    if (!hwnd_) return;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
}

void SettingsWindow::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

void SettingsWindow::SyncFromExternal(const std::vector<Phrase>& phrases, const AppSettings& settings) {
    phrases_ = phrases;
    settings_ = settings;
    if (!hwnd_) return;

    SendMessageW(chkActive_, BM_SETCHECK, settings_.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(chkAutostart_, BM_SETCHECK, IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(editDelay_, std::to_wstring(settings_.pasteDelayMs).c_str());
    RefreshList();

    if (!filteredIndices_.empty() && currentId_.empty()) {
        SendMessageW(listBox_, LB_SETCURSEL, 0, 0);
        LoadPhraseIntoForm(phrases_[filteredIndices_[0]]);
    }
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SettingsWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<SettingsWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CALLBACK SettingsWindow::TextEditSubclassProc(
    HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    auto* self = reinterpret_cast<SettingsWindow*>(dwRefData);
    if (msg == WM_PASTE || (msg == WM_KEYDOWN && wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000))) {
        if (HasClipboardImage()) {
            self->HandlePasteImage();
            return 0;
        }
    }
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, TextEditSubclassProc, uIdSubclass);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void SettingsWindow::InitControls() {
    auto mk = [&](const wchar_t* cls, const wchar_t* text, DWORD style, DWORD exStyle,
                  int x, int y, int w, int h, int id, bool bold = false) -> HWND {
        HWND hCtl = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                                    x, y, w, h, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                    hInst_, nullptr);
        SendMessageW(hCtl, WM_SETFONT, reinterpret_cast<WPARAM>(bold ? boldFont_ : font_), TRUE);
        return hCtl;
    };

    // Left Card (x: 16..276, y: 16..512)
    mk(L"STATIC", L"Shortcuts (c,1 / c,2 / c,3...)", 0, 0, 30, 26, 230, 20, 0, true);
    editSearch_ = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, 0, 30, 50, 232, 26, ID_EDIT_SEARCH);
    SendMessageW(editSearch_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search script (e.g. sr)..."));
    listBox_ = mk(L"LISTBOX", L"", LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_VSCROLL | WS_BORDER,
                  0, 30, 82, 232, 370, ID_LIST);
    SendMessageW(listBox_, LB_SETITEMHEIGHT, 0, 42);
    btnNew_ = mk(L"BUTTON", L"+ New", BS_OWNERDRAW, 0, 30, 464, 112, 34, ID_BTN_NEW, true);
    btnImport_ = mk(L"BUTTON", L"Import Sogou", BS_OWNERDRAW, 0, 148, 464, 114, 34, ID_BTN_IMPORT, true);

    // Right Card (x: 292..848, y: 16..512)
    int rx = 310;
    mk(L"STATIC", L"Shortcut", 0, 0, rx, 32, 62, 22, 0, true);
    editShortcut_ = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER, 0, rx + 66, 28, 260, 28, ID_EDIT_SHORTCUT);

    chkPhraseEnabled_ = mk(L"BUTTON", L"Enabled", BS_AUTOCHECKBOX, 0, rx + 364, 30, 88, 24, ID_CHK_PHRASE_ENABLED);
    SendMessageW(chkPhraseEnabled_, BM_SETCHECK, BST_CHECKED, 0);

    // Image above text
    mk(L"STATIC", L"Image (pasted above text, like Telegram)", 0, 0, rx, 70, 265, 22, 0, true);
    btnPasteImg_ = mk(L"BUTTON", L"Paste Image", BS_OWNERDRAW, 0, rx + 275, 65, 92, 28, ID_BTN_PASTE_IMG);
    btnBrowseImg_ = mk(L"BUTTON", L"Browse...", BS_OWNERDRAW, 0, rx + 373, 65, 76, 28, ID_BTN_BROWSE_IMG);
    btnRemoveImg_ = mk(L"BUTTON", L"Remove", BS_OWNERDRAW, 0, rx + 455, 65, 68, 28, ID_BTN_REMOVE_IMG);

    imgPreview_ = mk(L"STATIC", L"", SS_OWNERDRAW, 0, rx, 100, 523, 155, ID_IMG_PREVIEW);

    // Text / Caption
    mk(L"STATIC", L"Text / Caption (Ctrl+V pastes image or text)", 0, 0, rx, 266, 380, 22, 0, true);
    editText_ = mk(L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_BORDER,
                   0, rx, 290, 523, 162, ID_EDIT_TEXT);
    SetWindowSubclass(editText_, TextEditSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    btnDelete_ = mk(L"BUTTON", L"Delete", BS_OWNERDRAW, 0, rx, 464, 90, 34, ID_BTN_DELETE);
    lblStatus_ = mk(L"STATIC", L"Ready — pastes Image + Text and waits for Enter", 0, 0, rx + 102, 471, 295, 22, 0);
    btnSave_ = mk(L"BUTTON", L"Save Phrase", BS_OWNERDRAW, 0, rx + 405, 464, 118, 34, ID_BTN_SAVE, true);

    // Bottom Settings Card (x: 16..848, y: 524..564)
    chkActive_ = mk(L"BUTTON", L"Active (Global)", BS_AUTOCHECKBOX, 0, 30, 532, 130, 24, ID_CHK_ACTIVE);
    chkAutostart_ = mk(L"BUTTON", L"Auto-start on PC Boot (Silent Tray)", BS_AUTOCHECKBOX, 0, 175, 532, 245, 24, ID_CHK_AUTOSTART);
    mk(L"STATIC", L"Image→Text Delay (ms):", 0, 0, 560, 535, 155, 20, 0);
    editDelay_ = mk(L"EDIT", L"350", ES_NUMBER | WS_BORDER, 0, 720, 531, 70, 26, ID_EDIT_DELAY);
}

LRESULT SettingsWindow::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE:
            InitControls();
            return 0;

        case WM_CLOSE:
            Hide();
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd_, &ps);
            Gdiplus::Graphics g(hdc);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

            auto drawCard = [&](float x, float y, float w, float h) {
                Gdiplus::GraphicsPath path;
                AddRoundRect(path, Gdiplus::RectF(x, y, w, h), 10.0f);
                Gdiplus::SolidBrush fill(Gdiplus::Color(255, 17, 24, 39));
                Gdiplus::Pen border(Gdiplus::Color(255, 30, 41, 59), 1.0f);
                g.FillPath(&fill, &path);
                g.DrawPath(&border, &path);
            };

            drawCard(16.0f, 16.0f, 260.0f, 496.0f);
            drawCard(292.0f, 16.0f, 556.0f, 496.0f);
            drawCard(16.0f, 522.0f, 832.0f, 44.0f);

            EndPaint(hwnd_, &ps);
            return 0;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkColor(hdc, RGB(17, 24, 39));
            SetTextColor(hdc, RGB(226, 232, 240));
            return reinterpret_cast<LRESULT>(g_cardBrush);
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkColor(hdc, RGB(15, 23, 42));
            SetTextColor(hdc, RGB(248, 250, 252));
            return reinterpret_cast<LRESULT>(g_inputBrush);
        }

        case WM_CTLCOLORBTN: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkColor(hdc, RGB(17, 24, 39));
            SetTextColor(hdc, RGB(226, 232, 240));
            return reinterpret_cast<LRESULT>(g_cardBrush);
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkColor(hdc, RGB(15, 23, 42));
            SetTextColor(hdc, RGB(248, 250, 252));
            return reinterpret_cast<LRESULT>(g_inputBrush);
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (!dis) break;

            if (dis->CtlID == ID_BTN_NEW) { DrawModernButton(dis, L"+ New", 1); return TRUE; }
            if (dis->CtlID == ID_BTN_IMPORT) { DrawModernButton(dis, L"Import Sogou", 0); return TRUE; }
            if (dis->CtlID == ID_BTN_SAVE) { DrawModernButton(dis, L"Save Phrase", 1); return TRUE; }
            if (dis->CtlID == ID_BTN_DELETE) { DrawModernButton(dis, L"Delete", 2); return TRUE; }
            if (dis->CtlID == ID_BTN_PASTE_IMG) { DrawModernButton(dis, L"Paste Image", 0); return TRUE; }
            if (dis->CtlID == ID_BTN_BROWSE_IMG) { DrawModernButton(dis, L"Browse...", 0); return TRUE; }
            if (dis->CtlID == ID_BTN_REMOVE_IMG) { DrawModernButton(dis, L"Remove", 0); return TRUE; }

            if (dis->CtlID == ID_LIST && dis->itemID != static_cast<UINT>(-1)) {
                int row = static_cast<int>(dis->itemID);
                if (row >= 0 && row < static_cast<int>(filteredIndices_.size())) {
                    int idx = filteredIndices_[row];
                    if (idx >= 0 && idx < static_cast<int>(phrases_.size())) {
                        const Phrase& p = phrases_[idx];
                    bool selected = (dis->itemState & ODS_SELECTED) != 0;
                    RECT rc = dis->rcItem;
                    int w = rc.right - rc.left;
                    int h = rc.bottom - rc.top;

                    HDC memDC = CreateCompatibleDC(dis->hDC);
                    HBITMAP memBmp = CreateCompatibleBitmap(dis->hDC, w, h);
                    HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

                    {
                        Gdiplus::Graphics g(memDC);
                        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

                        Gdiplus::SolidBrush bg(selected ? Gdiplus::Color(255, 30, 41, 59) : Gdiplus::Color(255, 15, 23, 42));
                        g.FillRectangle(&bg, 0, 0, w, h);

                        if (selected) {
                            Gdiplus::LinearGradientBrush bar(
                                Gdiplus::PointF(0, 4),
                                Gdiplus::PointF(0, static_cast<float>(h - 4)),
                                Gdiplus::Color(255, 59, 130, 246),
                                Gdiplus::Color(255, 139, 92, 246));
                            g.FillRectangle(&bar, 0, 4, 3, h - 8);
                        }

                        Gdiplus::FontFamily ff(L"Segoe UI");
                        Gdiplus::Font badgeFont(&ff, 11.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
                        Gdiplus::Font textFont(&ff, 12.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

                        std::wstring scLabel = p.shortcut;
                        Gdiplus::RectF pillRect(8.0f, 10.0f, 48.0f, 22.0f);
                        Gdiplus::GraphicsPath pill;
                        AddRoundRect(pill, pillRect, 5.0f);
                        if (p.enabled) {
                            Gdiplus::LinearGradientBrush pillGrad(
                                Gdiplus::PointF(pillRect.X, pillRect.Y),
                                Gdiplus::PointF(pillRect.GetRight(), pillRect.GetBottom()),
                                Gdiplus::Color(255, 59, 130, 246),
                                Gdiplus::Color(255, 139, 92, 246));
                            g.FillPath(&pillGrad, &pill);
                        } else {
                            Gdiplus::SolidBrush pillOff(Gdiplus::Color(255, 71, 85, 105));
                            g.FillPath(&pillOff, &pill);
                        }

                        Gdiplus::StringFormat sfC;
                        sfC.SetAlignment(Gdiplus::StringAlignmentCenter);
                        sfC.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                        Gdiplus::SolidBrush white(Gdiplus::Color(255, 255, 255, 255));
                        g.DrawString(scLabel.c_str(), -1, &badgeFont, pillRect, &sfC, &white);

                        float tx = 64.0f;
                        if (!p.imageFile.empty()) {
                            Gdiplus::GraphicsPath imgPill;
                            AddRoundRect(imgPill, Gdiplus::RectF(tx, 11.0f, 34.0f, 20.0f), 4.0f);
                            Gdiplus::SolidBrush imgBg(Gdiplus::Color(255, 6, 78, 59));
                            Gdiplus::SolidBrush imgFg(Gdiplus::Color(255, 110, 231, 183));
                            g.FillPath(&imgBg, &imgPill);
                            g.DrawString(L"IMG", -1, &badgeFont, Gdiplus::RectF(tx, 11.0f, 34.0f, 20.0f), &sfC, &imgFg);
                            tx += 40.0f;
                        }

                        std::wstring preview = p.text;
                        for (auto& ch : preview) {
                            if (ch == L'\r' || ch == L'\n') ch = L' ';
                        }
                        if (preview.empty()) preview = L"(Image only)";

                        Gdiplus::StringFormat sfL;
                        sfL.SetAlignment(Gdiplus::StringAlignmentNear);
                        sfL.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                        sfL.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
                        sfL.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
                        Gdiplus::SolidBrush lightText(Gdiplus::Color(255, 226, 232, 240));
                        g.DrawString(preview.c_str(), -1, &textFont,
                                     Gdiplus::RectF(tx, 0.0f, static_cast<float>(w) - tx - 8.0f, static_cast<float>(h)),
                                     &sfL, &lightText);
                    }

                    BitBlt(dis->hDC, rc.left, rc.top, w, h, memDC, 0, 0, SRCCOPY);
                    SelectObject(memDC, oldBmp);
                    DeleteObject(memBmp);
                    DeleteDC(memDC);
                    }
                }
                return TRUE;
            }

            if (dis->CtlID == ID_IMG_PREVIEW) {
                HDC hdc = dis->hDC;
                RECT rc = dis->rcItem;
                int w = rc.right - rc.left;
                int h = rc.bottom - rc.top;

                Gdiplus::Graphics g(hdc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

                Gdiplus::SolidBrush cardClear(Gdiplus::Color(255, 17, 24, 39));
                g.FillRectangle(&cardClear, 0, 0, w, h);

                Gdiplus::GraphicsPath boxPath;
                AddRoundRect(boxPath, Gdiplus::RectF(1.0f, 1.0f, static_cast<float>(w - 2), static_cast<float>(h - 2)), 8.0f);
                Gdiplus::SolidBrush bg(Gdiplus::Color(255, 15, 23, 42));
                Gdiplus::Pen dashPen(Gdiplus::Color(255, 51, 65, 85), 1.5f);
                dashPen.SetDashStyle(Gdiplus::DashStyleDash);
                g.FillPath(&bg, &boxPath);
                g.DrawPath(&dashPen, &boxPath);

                std::wstring activeImg = !stagedImageFile_.empty() ? stagedImageFile_ : existingImageFile_;
                if (!activeImg.empty()) {
                    std::wstring full = GetImageFullPath(activeImg);
                    Gdiplus::Bitmap bmp(full.c_str());
                    if (bmp.GetLastStatus() == Gdiplus::Ok) {
                        float bw = static_cast<float>(bmp.GetWidth());
                        float bh = static_cast<float>(bmp.GetHeight());
                        float scale = std::min((w - 16.0f) / bw, (h - 16.0f) / bh);
                        scale = std::min(scale, 1.0f);
                        float dw = bw * scale;
                        float dh = bh * scale;
                        g.DrawImage(&bmp, (w - dw) * 0.5f, (h - dh) * 0.5f, dw, dh);
                        return TRUE;
                    }
                }

                Gdiplus::FontFamily ff(L"Segoe UI");
                Gdiplus::Font font(&ff, 12.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
                Gdiplus::SolidBrush muted(Gdiplus::Color(255, 148, 163, 184));
                Gdiplus::StringFormat sf;
                sf.SetAlignment(Gdiplus::StringAlignmentCenter);
                sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                g.DrawString(L"No image attached.\nPress Ctrl+V in the Text box below or click Paste Image.",
                             -1, &font, Gdiplus::RectF(0, 0, static_cast<float>(w), static_cast<float>(h)),
                             &sf, &muted);
                return TRUE;
            }
            break;
        }

        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);

            if (id == ID_EDIT_SEARCH && code == EN_CHANGE) {
                RefreshList();
            } else if (id == ID_LIST && code == LBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(listBox_, LB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < static_cast<int>(filteredIndices_.size())) {
                    int idx = filteredIndices_[sel];
                    if (idx >= 0 && idx < static_cast<int>(phrases_.size())) {
                        LoadPhraseIntoForm(phrases_[idx]);
                    }
                }
            } else if (id == ID_BTN_NEW) {
                ClearFormForNew();
            } else if (id == ID_BTN_IMPORT) {
                HandleImportSogou();
            } else if (id == ID_BTN_PASTE_IMG) {
                HandlePasteImage();
            } else if (id == ID_BTN_BROWSE_IMG) {
                HandleBrowseImage();
            } else if (id == ID_BTN_REMOVE_IMG) {
                HandleRemoveImage();
            } else if (id == ID_BTN_SAVE) {
                HandleSavePhrase();
            } else if (id == ID_BTN_DELETE) {
                HandleDeletePhrase();
            } else if (id == ID_CHK_ACTIVE || (id == ID_EDIT_DELAY && code == EN_KILLFOCUS)) {
                HandleSettingsChanged();
            } else if (id == ID_CHK_AUTOSTART) {
                bool on = SendMessageW(chkAutostart_, BM_GETCHECK, 0, 0) == BST_CHECKED;
                SetAutostartEnabled(on);
                SetWindowTextW(lblStatus_, on ? L"Auto-start enabled (silent tray)." : L"Auto-start disabled.");
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void SettingsWindow::RefreshList() {
    std::sort(phrases_.begin(), phrases_.end(), [](const Phrase& a, const Phrase& b) {
        return _wcsicmp(a.shortcut.c_str(), b.shortcut.c_str()) < 0;
    });

    std::wstring query = editSearch_ ? ToLowerTrimmed(GetWindowTextStr(editSearch_)) : L"";
    filteredIndices_.clear();

    if (query.empty()) {
        for (int i = 0; i < static_cast<int>(phrases_.size()); ++i) {
            filteredIndices_.push_back(i);
        }
    } else {
        // Prefer scripts whose shortcut or text starts with the query (e.g. "sr").
        for (int i = 0; i < static_cast<int>(phrases_.size()); ++i) {
            const auto& p = phrases_[i];
            if (StartsWithLower(p.shortcut, query) || StartsWithLower(p.text, query)) {
                filteredIndices_.push_back(i);
            }
        }
        // Fallback to substring match if nothing starts with the query.
        if (filteredIndices_.empty()) {
            for (int i = 0; i < static_cast<int>(phrases_.size()); ++i) {
                const auto& p = phrases_[i];
                if (ContainsLower(p.shortcut, query) || ContainsLower(p.text, query)) {
                    filteredIndices_.push_back(i);
                }
            }
        }
    }

    SendMessageW(listBox_, LB_RESETCONTENT, 0, 0);
    int selectIdx = -1;
    for (int row = 0; row < static_cast<int>(filteredIndices_.size()); ++row) {
        const auto& p = phrases_[filteredIndices_[row]];
        std::wstring rowText = p.shortcut + L" " + p.text;
        SendMessageW(listBox_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(rowText.c_str()));
        if (p.id == currentId_) selectIdx = row;
    }
    if (selectIdx >= 0) {
        SendMessageW(listBox_, LB_SETCURSEL, selectIdx, 0);
    }
}

void SettingsWindow::LoadPhraseIntoForm(const Phrase& p) {
    currentId_ = p.id;
    SetWindowTextW(editShortcut_, p.shortcut.c_str());
    SendMessageW(chkPhraseEnabled_, BM_SETCHECK, p.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTextW(editText_, p.text.c_str());
    stagedImageFile_.clear();
    existingImageFile_ = p.imageFile;
    UpdateImagePreview();
}

void SettingsWindow::ClearFormForNew() {
    currentId_.clear();
    SendMessageW(listBox_, LB_SETCURSEL, -1, 0);
    SetWindowTextW(editShortcut_, L"");
    SendMessageW(chkPhraseEnabled_, BM_SETCHECK, BST_CHECKED, 0);
    SetWindowTextW(editText_, L"");
    stagedImageFile_.clear();
    existingImageFile_.clear();
    UpdateImagePreview();
    SetFocus(editShortcut_);
}

void SettingsWindow::UpdateImagePreview() {
    if (imgPreview_) InvalidateRect(imgPreview_, nullptr, TRUE);
}

void SettingsWindow::HandlePasteImage() {
    if (!HasClipboardImage()) {
        MessageBoxW(hwnd_, L"Your clipboard does not contain an image.", L"No Image", MB_ICONINFORMATION);
        return;
    }
    std::wstring newFile = L"img_" + GenerateId().substr(0, 8) + L".png";
    if (SaveClipboardImageToPng(GetImageFullPath(newFile))) {
        if (!stagedImageFile_.empty()) DeleteImageFile(stagedImageFile_);
        stagedImageFile_ = newFile;
        UpdateImagePreview();
        SetWindowTextW(lblStatus_, L"Image pasted! Click Save Phrase.");
    } else {
        MessageBoxW(hwnd_, L"Could not read image from clipboard.", L"Error", MB_ICONERROR);
    }
}

void SettingsWindow::HandleBrowseImage() {
    wchar_t fileBuf[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.webp\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) return;

    std::wstring newFile = L"img_" + GenerateId().substr(0, 8) + L".png";
    if (ConvertImageFileToPng(fileBuf, GetImageFullPath(newFile))) {
        if (!stagedImageFile_.empty()) DeleteImageFile(stagedImageFile_);
        stagedImageFile_ = newFile;
        UpdateImagePreview();
        SetWindowTextW(lblStatus_, L"Image loaded! Click Save Phrase.");
    } else {
        MessageBoxW(hwnd_, L"Failed to load selected image.", L"Error", MB_ICONERROR);
    }
}

void SettingsWindow::HandleRemoveImage() {
    if (!stagedImageFile_.empty()) {
        DeleteImageFile(stagedImageFile_);
        stagedImageFile_.clear();
    }
    existingImageFile_.clear();
    UpdateImagePreview();
}

void SettingsWindow::HandleSavePhrase() {
    std::wstring sc = GetWindowTextStr(editShortcut_);
    while (!sc.empty() && sc.front() == L' ') sc.erase(sc.begin());
    while (!sc.empty() && sc.back() == L' ') sc.pop_back();

    if (sc.empty() || sc.find(L' ') != std::wstring::npos) {
        MessageBoxW(hwnd_, L"Enter a valid shortcut without spaces (e.g. c).", L"Invalid Shortcut", MB_ICONWARNING);
        return;
    }

    bool enabled = SendMessageW(chkPhraseEnabled_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    std::wstring text = GetWindowTextStr(editText_);
    std::wstring finalImg = !stagedImageFile_.empty() ? stagedImageFile_ : existingImageFile_;

    if (finalImg.empty() && text.empty()) {
        MessageBoxW(hwnd_, L"Please add an image, text, or both.", L"Empty Content", MB_ICONWARNING);
        return;
    }

    auto it = std::find_if(phrases_.begin(), phrases_.end(), [&](const Phrase& p) { return p.id == currentId_; });
    if (it != phrases_.end()) {
        if (!it->imageFile.empty() && it->imageFile != finalImg) {
            DeleteImageFile(it->imageFile);
        }
        it->shortcut = sc;
        it->enabled = enabled;
        it->imageFile = finalImg;
        it->text = text;
    } else {
        Phrase np{GenerateId(), sc, text, finalImg, enabled};
        currentId_ = np.id;
        phrases_.push_back(std::move(np));
    }

    stagedImageFile_.clear();
    existingImageFile_ = finalImg;
    SavePhrases(phrases_);
    HandleSettingsChanged();
    RefreshList();
    SetWindowTextW(lblStatus_, (L"Saved '" + sc + L"'!").c_str());
}

void SettingsWindow::HandleDeletePhrase() {
    if (currentId_.empty()) {
        ClearFormForNew();
        return;
    }
    auto it = std::find_if(phrases_.begin(), phrases_.end(), [&](const Phrase& p) { return p.id == currentId_; });
    if (it != phrases_.end()) {
        if (!it->imageFile.empty()) DeleteImageFile(it->imageFile);
        phrases_.erase(it);
        SavePhrases(phrases_);
        if (onChanged_) onChanged_(phrases_, settings_);
    }
    currentId_.clear();
    RefreshList();
    if (!filteredIndices_.empty()) {
        SendMessageW(listBox_, LB_SETCURSEL, 0, 0);
        LoadPhraseIntoForm(phrases_[filteredIndices_[0]]);
    } else {
        ClearFormForNew();
    }
    SetWindowTextW(lblStatus_, L"Phrase deleted.");
}

void SettingsWindow::HandleSettingsChanged() {
    settings_.enabled = SendMessageW(chkActive_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int delay = _wtoi(GetWindowTextStr(editDelay_).c_str());
    settings_.pasteDelayMs = std::clamp(delay == 0 ? 350 : delay, 100, 3000);
    SaveSettings(settings_);
    if (onChanged_) onChanged_(phrases_, settings_);
}

namespace {

struct ImportDlgState {
    HINSTANCE hInst = nullptr;
    HFONT font = nullptr;
    HFONT boldFont = nullptr;
    HWND editBox = nullptr;
    std::wstring resultText;
    bool accepted = false;
    bool done = false;
};

constexpr int ID_IMP_LOAD = 301;
constexpr int ID_IMP_OK = 302;
constexpr int ID_IMP_CANCEL = 303;
constexpr int ID_IMP_EDIT = 304;

LRESULT CALLBACK ImportDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* st = reinterpret_cast<ImportDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = static_cast<ImportDlgState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        return TRUE;
    }
    if (!st) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CREATE: {
            auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id, bool bold = false) {
                HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                                         x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                         st->hInst, nullptr);
                SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(bold ? st->boldFont : st->font), TRUE);
                return c;
            };
            mk(L"STATIC", L"Paste Sogou lines (e.g. c,1=hello) or load a .txt / .ini file:", 0, 16, 14, 480, 22, 0, true);
            st->editBox = mk(L"EDIT", L"c,1=hello\r\nc,2=សួស្តី!\r\nyx,1=example@gmail.com",
                             ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_BORDER,
                             16, 42, 508, 250, ID_IMP_EDIT);
            mk(L"BUTTON", L"Load File (.txt / .ini)...", BS_OWNERDRAW, 16, 304, 160, 34, ID_IMP_LOAD);
            mk(L"BUTTON", L"Cancel", BS_OWNERDRAW, 318, 304, 92, 34, ID_IMP_CANCEL);
            mk(L"BUTTON", L"Import", BS_OWNERDRAW, 418, 304, 106, 34, ID_IMP_OK, true);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkColor(hdc, RGB(17, 24, 39));
            SetTextColor(hdc, RGB(226, 232, 240));
            return reinterpret_cast<LRESULT>(g_cardBrush);
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkColor(hdc, RGB(15, 23, 42));
            SetTextColor(hdc, RGB(248, 250, 252));
            return reinterpret_cast<LRESULT>(g_inputBrush);
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lp);
            if (!dis) break;
            if (dis->CtlID == ID_IMP_LOAD) { DrawModernButton(dis, L"Load File (.txt/.ini)...", 0); return TRUE; }
            if (dis->CtlID == ID_IMP_CANCEL) { DrawModernButton(dis, L"Cancel", 0); return TRUE; }
            if (dis->CtlID == ID_IMP_OK) { DrawModernButton(dis, L"Import", 1); return TRUE; }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == ID_IMP_LOAD) {
                wchar_t fileBuf[MAX_PATH]{};
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"Sogou Phrase Files (*.txt;*.ini)\0*.txt;*.ini\0All Files (*.*)\0*.*\0";
                ofn.lpstrFile = fileBuf;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    std::wstring content = ReadTextFileAutoEncoding(fileBuf);
                    if (!content.empty()) {
                        SetWindowTextW(st->editBox, content.c_str());
                    } else {
                        MessageBoxW(hwnd, L"Could not read the selected file.", L"Error", MB_ICONERROR);
                    }
                }
            } else if (id == ID_IMP_OK) {
                st->resultText = GetWindowTextStr(st->editBox);
                st->accepted = true;
                st->done = true;
                DestroyWindow(hwnd);
            } else if (id == ID_IMP_CANCEL) {
                st->done = true;
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_CLOSE:
            st->done = true;
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void SettingsWindow::HandleImportSogou() {
    const wchar_t* kImpClass = L"QuickPhraseImportDlg";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ImportDlgProc;
        wc.hInstance = hInst_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = g_cardBrush;
        wc.lpszClassName = kImpClass;
        RegisterClassExW(&wc);
        registered = true;
    }

    ImportDlgState st;
    st.hInst = hInst_;
    st.font = font_;
    st.boldFont = boldFont_;

    RECT parentRc{};
    GetWindowRect(hwnd_, &parentRc);
    int dw = 556;
    int dh = 392;
    int dx = parentRc.left + ((parentRc.right - parentRc.left) - dw) / 2;
    int dy = parentRc.top + ((parentRc.bottom - parentRc.top) - dh) / 2;

    EnableWindow(hwnd_, FALSE);
    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kImpClass,
        L"Import Sogou Phrases (c,1=text)",
        WS_POPUP | WS_CAPTION | WS_SYSMENU,
        dx, dy, dw, dh,
        hwnd_, nullptr, hInst_, &st);
    EnableDarkTitleBar(dlg);
    ShowWindow(dlg, SW_SHOW);

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(hwnd_, TRUE);
    SetForegroundWindow(hwnd_);

    if (st.accepted && !st.resultText.empty()) {
        int count = ImportSogouText(st.resultText, phrases_);
        if (count > 0) {
            SavePhrases(phrases_);
            HandleSettingsChanged();
            RefreshList();
            SetWindowTextW(lblStatus_, (L"Imported " + std::to_wstring(count) + L" phrases!").c_str());
        } else {
            MessageBoxW(hwnd_, L"No valid lines found. Use format: c,1=hello", L"Import", MB_ICONINFORMATION);
        }
    }
}
