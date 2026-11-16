#include "ocr_snip.hpp"

#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <sstream>
#include <thread>
#include <vector>

#include "clipboard_win.hpp"
#include "storage.hpp"

namespace {

const wchar_t* kOverlayClass = L"QuickPhraseOcrOverlay";
const wchar_t* kResultClass = L"QuickPhraseOcrResult";
const wchar_t* kToastClass = L"QuickPhraseOcrToast";

constexpr int ID_OCR_EDIT = 401;
constexpr int ID_OCR_COPY = 402;
constexpr int ID_OCR_CLOSE = 403;
constexpr UINT_PTR TIMER_TOAST_HIDE = 5001;

HBRUSH g_resBgBrush = nullptr;
HBRUSH g_resEditBrush = nullptr;

std::string WideToUtf8Local(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWideLocal(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

bool GetPngClsid(CLSID* pClsid) {
    UINT num = 0, size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;
    std::vector<BYTE> buf(size);
    auto* enc = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, enc);
    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(enc[i].MimeType, L"image/png") == 0) {
            *pClsid = enc[i].Clsid;
            return true;
        }
    }
    return false;
}

bool ReadPipeLine(HANDLE hPipe, std::string& outLine) {
    outLine.clear();
    char ch = 0;
    DWORD readBytes = 0;
    while (ReadFile(hPipe, &ch, 1, &readBytes, nullptr) && readBytes == 1) {
        if (ch == '\n') return true;
        if (ch != '\r') outLine.push_back(ch);
    }
    return !outLine.empty();
}

} // namespace

bool OcrSnipTool::Init(HINSTANCE hInst, HWND hostHwnd, TextReadyCallback onTextReady) {
    hInst_ = hInst;
    hostHwnd_ = hostHwnd;
    onTextReady_ = std::move(onTextReady);

    if (!g_resBgBrush) g_resBgBrush = CreateSolidBrush(RGB(17, 24, 39));
    if (!g_resEditBrush) g_resEditBrush = CreateSolidBrush(RGB(15, 23, 42));

    font_ = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    boldFont_ = CreateFontW(-14, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.lpszClassName = kOverlayClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW rc{};
    rc.cbSize = sizeof(rc);
    rc.lpfnWndProc = ResultProc;
    rc.hInstance = hInst;
    rc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    rc.hbrBackground = g_resBgBrush;
    rc.lpszClassName = kResultClass;
    RegisterClassExW(&rc);

    WNDCLASSEXW tc{};
    tc.cbSize = sizeof(tc);
    tc.lpfnWndProc = ToastProc;
    tc.hInstance = hInst;
    tc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    tc.hbrBackground = g_resBgBrush;
    tc.lpszClassName = kToastClass;
    RegisterClassExW(&tc);

    // Pre-warm persistent OCR worker in background so first scan is instant
    std::thread([this]() {
        std::lock_guard<std::mutex> lk(workerMutex_);
        EnsureWorkerRunning();
    }).detach();

    return true;
}

void OcrSnipTool::Shutdown() {
    std::lock_guard<std::mutex> lk(workerMutex_);
    if (workerInWrite_) {
        const char* bye = "EXIT\n";
        DWORD written = 0;
        WriteFile(workerInWrite_, bye, 5, &written, nullptr);
        CloseHandle(workerInWrite_);
        workerInWrite_ = nullptr;
    }
    if (workerOutRead_) {
        CloseHandle(workerOutRead_);
        workerOutRead_ = nullptr;
    }
    if (workerProc_) {
        WaitForSingleObject(workerProc_, 500);
        CloseHandle(workerProc_);
        workerProc_ = nullptr;
    }
}

bool OcrSnipTool::EnsureWorkerRunning() {
    if (workerProc_) {
        DWORD code = 0;
        if (GetExitCodeProcess(workerProc_, &code) && code == STILL_ACTIVE) {
            return true;
        }
        CloseHandle(workerProc_);
        workerProc_ = nullptr;
        if (workerInWrite_) { CloseHandle(workerInWrite_); workerInWrite_ = nullptr; }
        if (workerOutRead_) { CloseHandle(workerOutRead_); workerOutRead_ = nullptr; }
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE hInRead = nullptr, hInWrite = nullptr;
    HANDLE hOutRead = nullptr, hOutWrite = nullptr;
    if (!CreatePipe(&hInRead, &hInWrite, &sa, 0)) return false;
    if (!CreatePipe(&hOutRead, &hOutWrite, &sa, 0)) {
        CloseHandle(hInRead); CloseHandle(hInWrite);
        return false;
    }
    SetHandleInformation(hInWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hOutRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring scriptPath = GetBaseDir() + L"\\ocr_worker.ps1";
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + scriptPath + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = hInRead;
    si.hStdOutput = hOutWrite;
    si.hStdError = hOutWrite;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hInRead);
    CloseHandle(hOutWrite);

    if (!ok) {
        CloseHandle(hInWrite);
        CloseHandle(hOutRead);
        return false;
    }

    CloseHandle(pi.hThread);
    workerProc_ = pi.hProcess;
    workerInWrite_ = hInWrite;
    workerOutRead_ = hOutRead;

    std::string readyLine;
    ReadPipeLine(workerOutRead_, readyLine);
    return true;
}

std::wstring OcrSnipTool::QueryWorker(const std::wstring& pngPath) {
    std::lock_guard<std::mutex> lk(workerMutex_);
    if (!EnsureWorkerRunning()) return {};

    std::string req = WideToUtf8Local(pngPath) + "\n";
    DWORD written = 0;
    if (!WriteFile(workerInWrite_, req.data(), static_cast<DWORD>(req.size()), &written, nullptr)) {
        return {};
    }

    std::string resp;
    if (!ReadPipeLine(workerOutRead_, resp)) {
        return {};
    }

    if (resp.rfind("OK:", 0) == 0) {
        std::string body = resp.substr(3);
        std::wstring wide = Utf8ToWideLocal(body);
        std::wstring unescaped;
        unescaped.reserve(wide.size());
        for (size_t i = 0; i < wide.size(); ++i) {
            if (wide[i] == L'\\' && i + 1 < wide.size() && wide[i + 1] == L'n') {
                unescaped += L'\n';
                ++i;
            } else {
                unescaped += wide[i];
            }
        }
        return unescaped;
    }
    return {};
}

std::wstring OcrSnipTool::ExtractCustomerId(const std::wstring& ocrText) {
    if (ocrText.empty()) return {};

    std::wstring lower = ocrText;
    for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));

    const wchar_t* keys[] = {L"uid", L"id", L"account", L"acc", L"1d", L"ld"};
    for (const wchar_t* k : keys) {
        size_t klen = wcslen(k);
        size_t pos = 0;
        while ((pos = lower.find(k, pos)) != std::wstring::npos) {
            bool leftOk = (pos == 0) || !iswalnum(lower[pos - 1]);
            bool rightOk = (pos + klen >= lower.size()) || !iswalnum(lower[pos + klen]);
            if (leftOk && rightOk) {
                size_t scan = pos + klen;
                while (scan < lower.size() &&
                       (lower[scan] == L':' || lower[scan] == L'\xFF1A' || lower[scan] == L';' ||
                        lower[scan] == L'=' || lower[scan] == L' ' || lower[scan] == L'\t' ||
                        lower[scan] == L'\r' || lower[scan] == L'\n' || lower[scan] == L'.' ||
                        lower[scan] == L'-' || lower[scan] == L'#')) {
                    ++scan;
                }
                std::wstring digits;
                while (scan < lower.size() && iswdigit(lower[scan])) {
                    digits.push_back(lower[scan]);
                    ++scan;
                }
                if (digits.size() >= 5 && digits.size() <= 15) {
                    return digits;
                }
            }
            pos += klen;
        }
    }

    // Fallback: standalone 7-12 digit ID when OCR missed the "ID:" label (e.g. white text on green)
    for (size_t i = 0; i < ocrText.size(); ++i) {
        if (iswdigit(ocrText[i])) {
            size_t start = i;
            while (i < ocrText.size() && iswdigit(ocrText[i])) {
                ++i;
            }
            size_t len = i - start;
            if (len >= 7 && len <= 12) {
                auto isPunctTouch = [](wchar_t c) {
                    return c == L'.' || c == L',' || c == L'/' || c == L':' || c == L'-';
                };
                bool leftOk = (start == 0) || !isPunctTouch(ocrText[start - 1]);
                bool rightOk = (i >= ocrText.size()) || !isPunctTouch(ocrText[i]);
                if (leftOk && rightOk) {
                    return ocrText.substr(start, len);
                }
            }
        }
    }
    return {};
}

bool OcrSnipTool::ExtractCsTarget(const std::wstring& ocrText, std::wstring& outType, std::wstring& outValue) {
    outType.clear();
    outValue.clear();
    if (ocrText.empty()) return false;

    // 1. Check line-by-line from TOP to BOTTOM for Order Number (16..30 digits).
    // Also handle OCR splitting a long order number with spaces (e.g. two >=6-digit chunks).
    std::wstringstream wss(ocrText);
    std::wstring line;
    while (std::getline(wss, line)) {
        for (size_t i = 0; i < line.size(); ++i) {
            if (iswdigit(line[i])) {
                size_t start = i;
                while (i < line.size() && iswdigit(line[i])) ++i;
                std::wstring merged = line.substr(start, i - start);
                if (merged.size() >= 16 && merged.size() <= 30) {
                    outType = L"Order Number";
                    outValue = merged;
                    return true;
                }
                // Check if OCR inserted space(s) inside a long order number (>=6 digits + space + >=6 digits)
                if (merged.size() >= 6) {
                    size_t j = i;
                    while (j < line.size()) {
                        size_t sp = j;
                        while (sp < line.size() && line[sp] == L' ') ++sp;
                        if (sp == j || sp >= line.size() || !iswdigit(line[sp])) break;
                        size_t dStart = sp;
                        while (sp < line.size() && iswdigit(line[sp])) ++sp;
                        if (sp - dStart < 6) break;
                        merged += line.substr(dStart, sp - dStart);
                        j = sp;
                        if (merged.size() >= 16 && merged.size() <= 30) {
                            outType = L"Order Number";
                            outValue = merged;
                            return true;
                        }
                    }
                }
            }
        }
    }

    // 2. Check for ID: / UID: / Account:
    std::wstring id = ExtractCustomerId(ocrText);
    if (!id.empty()) {
        outType = L"ID";
        outValue = id;
        return true;
    }

    return false;
}

void OcrSnipTool::StartSnip() {
    if (overlayHwnd_) return;

    vx_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vy_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vw_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vh_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    screenBmp_ = CreateCompatibleBitmap(screenDC, vw_, vh_);
    HGDIOBJ old = SelectObject(memDC, screenBmp_);
    BitBlt(memDC, 0, 0, vw_, vh_, screenDC, vx_, vy_, SRCCOPY);
    SelectObject(memDC, old);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    dragging_ = false;
    overlayHwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kOverlayClass,
        L"",
        WS_POPUP,
        vx_, vy_, vw_, vh_,
        nullptr, nullptr, hInst_, this);

    ShowWindow(overlayHwnd_, SW_SHOW);
    SetForegroundWindow(overlayHwnd_);
}

bool IsChromeTitleImage(HWND hwnd) {
    wchar_t title[512]{};
    GetWindowTextW(hwnd, title, 512);
    for (wchar_t* p = title; *p; ++p) *p = static_cast<wchar_t>(towlower(*p));
    return wcsstr(title, L".png") != nullptr || wcsstr(title, L".jpg") != nullptr ||
           wcsstr(title, L".jpeg") != nullptr || wcsstr(title, L".webp") != nullptr ||
           wcsstr(title, L".bmp") != nullptr || wcsstr(title, L".gif") != nullptr;
}

bool DetectCenteredImageRect(HBITMAP hBmp, int w, int h, RECT& outCrop) {
    if (!hBmp || w < 160 || h < 160) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint32_t> px(static_cast<size_t>(w) * static_cast<size_t>(h));
    HDC dc = GetDC(nullptr);
    int got = GetDIBits(dc, hBmp, 0, static_cast<UINT>(h), px.data(), &bmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (got == 0) return false;

    auto isDark = [](uint32_t argb) -> bool {
        int r = (argb >> 16) & 0xFF;
        int g = (argb >> 8) & 0xFF;
        int b = argb & 0xFF;
        int m = std::max(r, std::max(g, b));
        return m < 62;
    };

    // 1. Check if outer margins are predominantly dark (lightbox / Media Viewer overlay)
    int borderTotal = 0;
    int borderDark = 0;
    int mx = std::max(8, w / 16);
    int my = std::max(8, h / 16);
    for (int y = 0; y < h; y += 6) {
        for (int x = 0; x < w; x += 6) {
            if (x < mx || x >= w - mx || y < my || y >= h - my) {
                ++borderTotal;
                if (isDark(px[static_cast<size_t>(y) * w + x])) ++borderDark;
            }
        }
    }
    if (borderTotal == 0 || borderDark * 100 < borderTotal * 70) {
        return false;
    }

    // 2. Find full vertical span from topmost active band (>=20px) to bottommost active band (>=20px)
    int x0 = w / 8;
    int x1 = w - w / 8;
    std::vector<bool> rowActive(h, false);
    for (int y = 0; y < h; ++y) {
        int cnt = 0;
        int tot = 0;
        const uint32_t* rowPtr = &px[static_cast<size_t>(y) * w];
        for (int x = x0; x < x1; x += 4) {
            ++tot;
            if (!isDark(rowPtr[x])) ++cnt;
        }
        if (tot > 0 && cnt * 100 >= tot * 4) {
            rowActive[y] = true;
        }
    }

    int top = -1;
    int run = 0;
    for (int y = 0; y < h; ++y) {
        if (rowActive[y]) {
            ++run;
            if (run >= 20) {
                top = y - run + 1;
                break;
            }
        } else {
            run = 0;
        }
    }
    int bottom = -1;
    run = 0;
    for (int y = h - 1; y >= 0; --y) {
        if (rowActive[y]) {
            ++run;
            if (run >= 20) {
                bottom = y + run - 1;
                break;
            }
        } else {
            run = 0;
        }
    }
    if (top < 0 || bottom <= top) return false;

    // 3. Find horizontal band within [top..bottom]
    std::vector<bool> colActive(w, false);
    int bestCol = w / 2;
    int bestColCnt = -1;
    for (int x = 0; x < w; ++x) {
        int cnt = 0;
        int tot = 0;
        for (int y = top; y <= bottom; y += 4) {
            ++tot;
            if (!isDark(px[static_cast<size_t>(y) * w + x])) ++cnt;
        }
        if (tot > 0 && cnt * 100 >= tot * 4) {
            colActive[x] = true;
            if (x >= w / 6 && x <= w - w / 6 && cnt > bestColCnt) {
                bestColCnt = cnt;
                bestCol = x;
            }
        }
    }
    if (bestColCnt <= 0) return false;

    int left = bestCol;
    int gap = 0;
    for (int x = bestCol; x >= 0; --x) {
        if (colActive[x]) {
            left = x;
            gap = 0;
        } else if (++gap > 14) {
            break;
        }
    }
    int right = bestCol;
    gap = 0;
    for (int x = bestCol; x < w; ++x) {
        if (colActive[x]) {
            right = x;
            gap = 0;
        } else if (++gap > 14) {
            break;
        }
    }

    int cw = right - left + 1;
    int ch = bottom - top + 1;
    if (cw < 100 || ch < 100) return false;

    outCrop.left = std::max(0, left - 6);
    outCrop.top = std::max(0, top - 6);
    outCrop.right = std::min(w, right + 7);
    outCrop.bottom = std::min(h, bottom + 7);
    return true;
}

void OcrSnipTool::ScanForegroundWindow(bool imageOnlyMode) {
    HWND fg = GetForegroundWindow();
    if (!fg) return;

    wchar_t cls[128]{};
    GetClassNameW(fg, cls, 128);
    bool isChrome = (wcsstr(cls, L"Chrome_WidgetWin") != nullptr);

    RECT rc{};
    if (isChrome) {
        if (!GetClientRect(fg, &rc)) return;
        POINT tl{rc.left, rc.top};
        POINT br{rc.right, rc.bottom};
        ClientToScreen(fg, &tl);
        ClientToScreen(fg, &br);
        rc = RECT{tl.x, tl.y + 88, br.x, br.y}; // skip browser tabs & address bar
    } else {
        if (!GetWindowRect(fg, &rc)) return;
    }

    int x = rc.left;
    int y = rc.top;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w < 80 || h < 80) return;

    std::wstring tmpPng = GetBaseDir() + L"\\data\\ocr_quick.png";
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bmp = CreateCompatibleBitmap(screenDC, w, h);
    HGDIOBJ old = SelectObject(memDC, bmp);
    BitBlt(memDC, 0, 0, w, h, screenDC, x, y, SRCCOPY);
    SelectObject(memDC, old);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    RECT cropRc{0, 0, w, h};
    bool hasImageBox = DetectCenteredImageRect(bmp, w, h, cropRc);

    if (imageOnlyMode && isChrome && !hasImageBox && !IsChromeTitleImage(fg)) {
        DeleteObject(bmp);
        return; // Not viewing an image in Chrome; do not scan UI/page text
    }

    int cw = cropRc.right - cropRc.left;
    int ch = cropRc.bottom - cropRc.top;

    {
        Gdiplus::Bitmap gdipBmp(bmp, nullptr);
        CLSID clsid{};
        if (GetPngClsid(&clsid)) {
            int outW = (cw <= 1920 && ch <= 1200) ? cw * 2 : cw;
            int outH = (cw <= 1920 && ch <= 1200) ? ch * 2 : ch;
            Gdiplus::Bitmap outBmp(outW, outH, PixelFormat32bppARGB);
            Gdiplus::Graphics g(&outBmp);
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            g.DrawImage(&gdipBmp,
                        Gdiplus::Rect(0, 0, outW, outH),
                        cropRc.left, cropRc.top, cw, ch,
                        Gdiplus::UnitPixel);
            outBmp.Save(tmpPng.c_str(), &clsid, nullptr);
        }
    }
    DeleteObject(bmp);

    RunOcrAsync(tmpPng, true);
}

LRESULT CALLBACK OcrSnipTool::OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    OcrSnipTool* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<OcrSnipTool*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<OcrSnipTool*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleOverlay(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT OcrSnipTool::HandleOverlay(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;

        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;

        case WM_RBUTTONUP:
            DestroyWindow(hwnd);
            return 0;

        case WM_LBUTTONDOWN:
            dragging_ = true;
            startPt_ = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            curPt_ = startPt_;
            SetCapture(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_MOUSEMOVE:
            if (dragging_) {
                curPt_ = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP:
            if (dragging_) {
                dragging_ = false;
                ReleaseCapture();
                curPt_ = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                FinishSelection(hwnd);
            }
            return 0;

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            PaintOverlay(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY:
            overlayHwnd_ = nullptr;
            if (screenBmp_) {
                DeleteObject(screenBmp_);
                screenBmp_ = nullptr;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OcrSnipTool::PaintOverlay(HWND, HDC hdc) {
    HDC backDC = CreateCompatibleDC(hdc);
    HBITMAP backBmp = CreateCompatibleBitmap(hdc, vw_, vh_);
    HGDIOBJ oldBack = SelectObject(backDC, backBmp);

    HDC srcDC = CreateCompatibleDC(hdc);
    HGDIOBJ oldSrc = SelectObject(srcDC, screenBmp_);

    BitBlt(backDC, 0, 0, vw_, vh_, srcDC, 0, 0, SRCCOPY);

    {
        Gdiplus::Graphics g(backDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

        Gdiplus::SolidBrush veil(Gdiplus::Color(135, 2, 6, 23));
        g.FillRectangle(&veil, 0, 0, vw_, vh_);

        if (dragging_) {
            int x1 = std::min(startPt_.x, curPt_.x);
            int y1 = std::min(startPt_.y, curPt_.y);
            int x2 = std::max(startPt_.x, curPt_.x);
            int y2 = std::max(startPt_.y, curPt_.y);
            int rw = x2 - x1;
            int rh = y2 - y1;

            if (rw > 2 && rh > 2) {
                BitBlt(backDC, x1, y1, rw, rh, srcDC, x1, y1, SRCCOPY);

                Gdiplus::LinearGradientBrush grad(
                    Gdiplus::PointF(static_cast<float>(x1), static_cast<float>(y1)),
                    Gdiplus::PointF(static_cast<float>(x2), static_cast<float>(y2)),
                    Gdiplus::Color(255, 59, 130, 246),
                    Gdiplus::Color(255, 139, 92, 246));
                Gdiplus::Pen borderPen(&grad, 2.0f);
                g.DrawRectangle(&borderPen, x1, y1, rw, rh);
            }
        } else {
            Gdiplus::FontFamily ff(L"Segoe UI");
            Gdiplus::Font f(&ff, 15.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush white(Gdiplus::Color(235, 248, 250, 252));
            Gdiplus::StringFormat sf;
            sf.SetAlignment(Gdiplus::StringAlignmentCenter);
            g.DrawString(L"Drag to select area for OCR  (Esc / Right-click to cancel)",
                         -1, &f, Gdiplus::PointF(vw_ * 0.5f, 36.0f), &sf, &white);
        }
    }

    BitBlt(hdc, 0, 0, vw_, vh_, backDC, 0, 0, SRCCOPY);
    SelectObject(srcDC, oldSrc);
    DeleteDC(srcDC);
    SelectObject(backDC, oldBack);
    DeleteObject(backBmp);
    DeleteDC(backDC);
}

void OcrSnipTool::FinishSelection(HWND hwnd) {
    int x1 = std::min(startPt_.x, curPt_.x);
    int y1 = std::min(startPt_.y, curPt_.y);
    int x2 = std::max(startPt_.x, curPt_.x);
    int y2 = std::max(startPt_.y, curPt_.y);
    int rw = x2 - x1;
    int rh = y2 - y1;

    if (rw < 8 || rh < 8) {
        DestroyWindow(hwnd);
        return;
    }

    std::wstring tmpPng = GetBaseDir() + L"\\data\\ocr_tmp.png";
    HDC screenDC = GetDC(nullptr);
    HDC srcDC = CreateCompatibleDC(screenDC);
    HDC dstDC = CreateCompatibleDC(screenDC);
    HGDIOBJ oldSrc = SelectObject(srcDC, screenBmp_);
    HBITMAP cropBmp = CreateCompatibleBitmap(screenDC, rw, rh);
    HGDIOBJ oldDst = SelectObject(dstDC, cropBmp);
    BitBlt(dstDC, 0, 0, rw, rh, srcDC, x1, y1, SRCCOPY);
    SelectObject(dstDC, oldDst);
    SelectObject(srcDC, oldSrc);
    DeleteDC(dstDC);
    DeleteDC(srcDC);
    ReleaseDC(nullptr, screenDC);

    {
        Gdiplus::Bitmap bmp(cropBmp, nullptr);
        CLSID clsid{};
        if (GetPngClsid(&clsid)) {
            bmp.Save(tmpPng.c_str(), &clsid, nullptr);
        }
    }
    DeleteObject(cropBmp);
    DestroyWindow(hwnd);

    RunOcrAsync(tmpPng, false);
}

void OcrSnipTool::RunOcrAsync(const std::wstring& pngPath, bool quickMode) {
    std::thread([this, pngPath, quickMode]() {
        std::wstring text = QueryWorker(pngPath);
        DeleteFileW(pngPath.c_str());
        if (onTextReady_) {
            onTextReady_(text, quickMode);
        }
    }).detach();
}

LRESULT CALLBACK OcrSnipTool::ResultProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<OcrSnipTool*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<OcrSnipTool*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkColor(hdc, RGB(17, 24, 39));
            SetTextColor(hdc, RGB(110, 231, 183));
            return reinterpret_cast<LRESULT>(g_resBgBrush);
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkColor(hdc, RGB(15, 23, 42));
            SetTextColor(hdc, RGB(248, 250, 252));
            return reinterpret_cast<LRESULT>(g_resEditBrush);
        }
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == ID_OCR_COPY) {
                int len = GetWindowTextLengthW(self->resultEdit_);
                std::wstring buf(len + 1, L'\0');
                GetWindowTextW(self->resultEdit_, buf.data(), len + 1);
                buf.resize(len);
                SetClipboardTextW(buf);
            } else if (id == ID_OCR_CLOSE) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            self->resultHwnd_ = nullptr;
            self->resultEdit_ = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OcrSnipTool::ShowResultPopup(const std::wstring& text) {
    if (!text.empty()) {
        SetClipboardTextW(text);
    }

    if (resultHwnd_) {
        DestroyWindow(resultHwnd_);
        resultHwnd_ = nullptr;
    }

    int w = 460;
    int h = 290;
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.right - w - 20;
    int y = wa.bottom - h - 20;

    resultHwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        kResultClass,
        L"QuickPhrase OCR — Copied to Clipboard",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, w, h,
        nullptr, nullptr, hInst_, this);

    HMODULE hDwm = LoadLibraryW(L"dwmapi.dll");
    if (hDwm) {
        using DwmSetAttrFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        auto fn = reinterpret_cast<DwmSetAttrFn>(reinterpret_cast<void*>(GetProcAddress(hDwm, "DwmSetWindowAttribute")));
        if (fn) {
            BOOL dark = TRUE;
            fn(resultHwnd_, 20, &dark, sizeof(dark));
        }
        FreeLibrary(hDwm);
    }

    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int cx, int cy, int cw, int ch, int id, bool bold = false) {
        HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                                 cx, cy, cw, ch, resultHwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                 hInst_, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(bold ? boldFont_ : font_), TRUE);
        return c;
    };

    mk(L"STATIC",
       text.empty() ? L"No text detected in selected region." : L"✓ Text extracted & copied to clipboard!",
       0, 14, 12, 420, 22, 0, true);

    resultEdit_ = mk(L"EDIT", text.c_str(),
                     ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_BORDER,
                     14, 40, 416, 165, ID_OCR_EDIT);

    mk(L"BUTTON", L"Copy", BS_PUSHBUTTON, 240, 214, 90, 30, ID_OCR_COPY, true);
    mk(L"BUTTON", L"Close", BS_PUSHBUTTON, 340, 214, 90, 30, ID_OCR_CLOSE);

    ShowWindow(resultHwnd_, SW_SHOW);
    SetForegroundWindow(resultHwnd_);
}

LRESULT CALLBACK OcrSnipTool::ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<OcrSnipTool*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<OcrSnipTool*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_TIMER:
            if (wp == TIMER_TOAST_HIDE) {
                KillTimer(hwnd, TIMER_TOAST_HIDE);
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            DestroyWindow(hwnd);
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetBkColor(hdc, RGB(17, 24, 39));
            SetTextColor(hdc, RGB(110, 231, 183));
            return reinterpret_cast<LRESULT>(g_resBgBrush);
        }
        case WM_DESTROY:
            self->toastHwnd_ = nullptr;
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OcrSnipTool::ShowQuickToast(const std::wstring& title, const std::wstring& body) {
    if (toastHwnd_) {
        DestroyWindow(toastHwnd_);
        toastHwnd_ = nullptr;
    }

    int w = 340;
    int h = 68;
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int x = wa.right - w - 18;
    int y = wa.bottom - h - 18;

    toastHwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kToastClass,
        L"",
        WS_POPUP | WS_BORDER,
        x, y, w, h,
        nullptr, nullptr, hInst_, this);

    HWND lbl1 = CreateWindowExW(0, L"STATIC", title.c_str(), WS_CHILD | WS_VISIBLE,
                                14, 10, w - 28, 22, toastHwnd_, nullptr, hInst_, nullptr);
    SendMessageW(lbl1, WM_SETFONT, reinterpret_cast<WPARAM>(boldFont_), TRUE);

    HWND lbl2 = CreateWindowExW(0, L"STATIC", body.c_str(), WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
                                14, 34, w - 28, 22, toastHwnd_, nullptr, hInst_, nullptr);
    SendMessageW(lbl2, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

    ShowWindow(toastHwnd_, SW_SHOWNOACTIVATE);
    SetTimer(toastHwnd_, TIMER_TOAST_HIDE, 2600, nullptr);
}
