#include "popup.hpp"

#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>

namespace {

const wchar_t* kPopupClass = L"QuickPhraseCandidatePopup";

void AddRoundRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& r, float radius) {
    float d = radius * 2.0f;
    path.AddArc(r.X, r.Y, d, d, 180.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y, d, d, 270.0f, 90.0f);
    path.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0.0f, 90.0f);
    path.AddArc(r.X, r.Y + r.Height - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

std::wstring MakePreviewText(const Phrase& p) {
    std::wstring preview = p.text;
    for (auto& ch : preview) {
        if (ch == L'\r' || ch == L'\n') ch = L' ';
    }
    if (preview.size() > 16) {
        preview = preview.substr(0, 16) + L"...";
    }
    if (preview.empty() && !p.imageFile.empty()) {
        preview = L"[Image]";
    }
    return preview;
}

} // namespace

bool CandidatePopup::Create(HINSTANCE hInst, SelectCallback onSelect) {
    onSelect_ = std::move(onSelect);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kPopupClass;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kPopupClass,
        L"",
        WS_POPUP,
        0, 0, kMinBoxW + kShadow * 2, kHeaderH + kRowH + kShadow * 2,
        nullptr, nullptr, hInst, this);

    return hwnd_ != nullptr;
}

bool CandidatePopup::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void CandidatePopup::Hide() {
    if (hwnd_ && IsWindowVisible(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
    }
    hoverIndex_ = -1;
    hoverPageBtn_ = 0;
    currentPage_ = 0;
}

int CandidatePopup::TotalPages() const {
    if (matches_.empty()) return 1;
    return static_cast<int>((matches_.size() + kPageSize - 1) / kPageSize);
}

bool CandidatePopup::NextPage() {
    if (currentPage_ + 1 >= TotalPages()) return false;
    ++currentPage_;
    hoverIndex_ = -1;
    int w = 0, h = 0;
    LayoutItems(w, h);
    POINT pos = ComputeAnchorPoint(w, h);
    SetWindowPos(hwnd_, HWND_TOPMOST, pos.x, pos.y, w, h, SWP_NOACTIVATE);
    RenderLayered();
    return true;
}

bool CandidatePopup::PrevPage() {
    if (currentPage_ <= 0) return false;
    --currentPage_;
    hoverIndex_ = -1;
    int w = 0, h = 0;
    LayoutItems(w, h);
    POINT pos = ComputeAnchorPoint(w, h);
    SetWindowPos(hwnd_, HWND_TOPMOST, pos.x, pos.y, w, h, SWP_NOACTIVATE);
    RenderLayered();
    return true;
}

bool CandidatePopup::GetPageCandidate(size_t pageLocalIdx, Phrase& outPhrase) const {
    size_t globalIdx = static_cast<size_t>(currentPage_) * kPageSize + pageLocalIdx;
    if (globalIdx >= matches_.size() || pageLocalIdx >= kPageSize) return false;
    outPhrase = matches_[globalIdx];
    return true;
}

void CandidatePopup::LayoutItems(int& outWidth, int& outHeight) {
    itemRects_.clear();
    if (matches_.empty()) {
        outWidth = kMinBoxW + kShadow * 2;
        outHeight = kHeaderH + kPadY + kShadow * 2;
        return;
    }

    HDC hdc = GetDC(hwnd_);
    Gdiplus::Graphics g(hdc);
    Gdiplus::FontFamily ff(L"Segoe UI");
    Gdiplus::Font normFont(&ff, 14.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

    size_t startIdx = static_cast<size_t>(currentPage_) * kPageSize;
    size_t endIdx = std::min(matches_.size(), startIdx + kPageSize);

    int x = kShadow + kPadX;
    int y = kShadow + kHeaderH + 5;
    int maxRight = kShadow + kMinBoxW;

    for (size_t i = startIdx; i < endIdx; ++i) {
        const auto& p = matches_[i];
        std::wstring text = MakePreviewText(p);

        Gdiplus::RectF box{};
        g.MeasureString(text.c_str(), -1, &normFont, Gdiplus::PointF(0, 0), &box);

        int itemW = 30 + static_cast<int>(box.Width) + 16;
        if (!p.imageFile.empty()) {
            itemW += 26;
        }
        itemW = std::clamp(itemW, 68, 240);

        if (x > kShadow + kPadX && x + itemW + kPadX > kShadow + kMaxBoxW) {
            x = kShadow + kPadX;
            y += kRowH;
        }

        RECT rc{x, y, x + itemW, y + kRowH - 4};
        itemRects_.push_back(rc);
        x += itemW + 6;
        maxRight = std::max(maxRight, x + kPadX - 6);
    }

    ReleaseDC(hwnd_, hdc);

    int boxW = std::clamp(maxRight - kShadow, kMinBoxW, kMaxBoxW);
    int boxH = (y - kShadow) + kRowH + kPadY - 1;

    prevBtnRect_ = RECT{kShadow + boxW - 56, kShadow + 4, kShadow + boxW - 32, kShadow + kHeaderH - 4};
    nextBtnRect_ = RECT{kShadow + boxW - 30, kShadow + 4, kShadow + boxW - 6, kShadow + kHeaderH - 4};

    outWidth = boxW + kShadow * 2;
    outHeight = boxH + kShadow * 2;
}

POINT CandidatePopup::ComputeAnchorPoint(int width, int height) const {
    POINT pt{100, 100};
    bool found = false;

    HWND fg = GetForegroundWindow();
    if (fg) {
        DWORD tid = GetWindowThreadProcessId(fg, nullptr);
        GUITHREADINFO gti{};
        gti.cbSize = sizeof(gti);
        if (GetGUIThreadInfo(tid, &gti) && gti.hwndCaret) {
            RECT rc = gti.rcCaret;
            if (rc.right > rc.left || rc.bottom > rc.top) {
                POINT caret{rc.left, rc.bottom};
                ClientToScreen(gti.hwndCaret, &caret);
                pt.x = caret.x - kShadow;
                pt.y = caret.y + 4 - kShadow;
                found = true;
            }
        }
    }

    if (!found) {
        GetCursorPos(&pt);
        pt.x -= kShadow;
        pt.y += 18 - kShadow;
    }

    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        RECT wa = mi.rcWork;
        if (pt.x + width > wa.right - 4) pt.x = wa.right - width - 4;
        if (pt.x < wa.left + 4) pt.x = wa.left + 4;
        if (pt.y + height > wa.bottom - 4) pt.y = pt.y - height - 26;
        if (pt.y < wa.top + 4) pt.y = wa.top + 4;
    }
    return pt;
}

void CandidatePopup::ShowCandidates(const std::wstring& typed, const std::vector<Phrase>& matches) {
    if (!hwnd_ || matches.empty()) {
        Hide();
        return;
    }

    typed_ = typed;
    matches_ = matches;
    currentPage_ = 0;
    hoverIndex_ = -1;
    hoverPageBtn_ = 0;

    int w = 0;
    int h = 0;
    LayoutItems(w, h);
    POINT pos = ComputeAnchorPoint(w, h);

    SetWindowPos(hwnd_, HWND_TOPMOST, pos.x, pos.y, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RenderLayered();
}

LRESULT CALLBACK CandidatePopup::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    CandidatePopup* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<CandidatePopup*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<CandidatePopup*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->HandleMessage(msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT CandidatePopup::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_MOUSEMOVE: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            int idx = -1;
            for (int i = 0; i < static_cast<int>(itemRects_.size()); ++i) {
                if (PtInRect(&itemRects_[i], pt)) {
                    idx = i;
                    break;
                }
            }
            int pBtn = 0;
            if (TotalPages() > 1) {
                if (PtInRect(&prevBtnRect_, pt)) pBtn = 1;
                else if (PtInRect(&nextBtnRect_, pt)) pBtn = 2;
            }
            if (idx != hoverIndex_ || pBtn != hoverPageBtn_) {
                hoverIndex_ = idx;
                hoverPageBtn_ = pBtn;
                RenderLayered();
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (TotalPages() > 1) {
                if (PtInRect(&prevBtnRect_, pt)) {
                    PrevPage();
                    return 0;
                }
                if (PtInRect(&nextBtnRect_, pt)) {
                    NextPage();
                    return 0;
                }
            }
            for (int i = 0; i < static_cast<int>(itemRects_.size()); ++i) {
                if (PtInRect(&itemRects_[i], pt)) {
                    Phrase chosen;
                    if (GetPageCandidate(static_cast<size_t>(i), chosen)) {
                        Hide();
                        if (onSelect_) onSelect_(chosen);
                    }
                    break;
                }
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

void CandidatePopup::RenderLayered() {
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP dib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(memDC, dib);

    {
        Gdiplus::Graphics g(memDC);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        float bx = static_cast<float>(kShadow);
        float by = static_cast<float>(kShadow);
        float bw = static_cast<float>(w - kShadow * 2);
        float bh = static_cast<float>(h - kShadow * 2);

        // Antigravity deep outer shadow
        for (int s = kShadow; s >= 1; --s) {
            BYTE alpha = static_cast<BYTE>(18 - s * 2);
            Gdiplus::GraphicsPath sp;
            AddRoundRectPath(sp, Gdiplus::RectF(bx - s * 0.4f, by + s * 0.5f, bw + s * 0.8f, bh + s * 0.8f), 11.0f);
            Gdiplus::SolidBrush sb(Gdiplus::Color(alpha, 2, 6, 23));
            g.FillPath(&sb, &sp);
        }

        // Main dark obsidian card (#0F172A)
        Gdiplus::GraphicsPath cardPath;
        AddRoundRectPath(cardPath, Gdiplus::RectF(bx, by, bw, bh), 10.0f);
        Gdiplus::SolidBrush bgBrush(Gdiplus::Color(246, 15, 23, 42));
        g.FillPath(&bgBrush, &cardPath);

        // Subtle gradient border (Blue -> Violet)
        Gdiplus::LinearGradientBrush borderGrad(
            Gdiplus::PointF(bx, by),
            Gdiplus::PointF(bx + bw, by + bh),
            Gdiplus::Color(220, 59, 130, 246),
            Gdiplus::Color(220, 139, 92, 246));
        Gdiplus::Pen borderPen(&borderGrad, 1.2f);
        g.DrawPath(&borderPen, &cardPath);

        // Subtle divider hairline
        Gdiplus::Pen linePen(Gdiplus::Color(180, 30, 41, 59), 1.0f);
        g.DrawLine(&linePen, bx + 10.0f, by + kHeaderH, bx + bw - 10.0f, by + kHeaderH);

        Gdiplus::FontFamily ff(L"Segoe UI");
        Gdiplus::Font boldFont(&ff, 13.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font normFont(&ff, 14.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        Gdiplus::Font numFont(&ff, 12.0f, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::Font hintFont(&ff, 11.0f, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

        Gdiplus::SolidBrush cyanAccent(Gdiplus::Color(255, 96, 165, 250));
        Gdiplus::SolidBrush whiteText(Gdiplus::Color(255, 248, 250, 252));
        Gdiplus::SolidBrush textBrush(Gdiplus::Color(255, 203, 213, 225));
        Gdiplus::SolidBrush numMuted(Gdiplus::Color(255, 100, 116, 139));
        Gdiplus::SolidBrush hintBrush(Gdiplus::Color(255, 100, 116, 139));

        Gdiplus::StringFormat sfLeft;
        sfLeft.SetAlignment(Gdiplus::StringAlignmentNear);
        sfLeft.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        sfLeft.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
        sfLeft.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);

        Gdiplus::StringFormat sfCenter;
        sfCenter.SetAlignment(Gdiplus::StringAlignmentCenter);
        sfCenter.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        Gdiplus::StringFormat sfRight;
        sfRight.SetAlignment(Gdiplus::StringAlignmentFar);
        sfRight.SetLineAlignment(Gdiplus::StringAlignmentCenter);

        // Typed shortcut pill on top-left with Antigravity gradient
        std::wstring pinyinLine = typed_ + L"|";
        Gdiplus::RectF typedBox(bx + 10.0f, by + 4.0f, 70.0f, static_cast<float>(kHeaderH - 8));
        Gdiplus::GraphicsPath pillPath;
        AddRoundRectPath(pillPath, typedBox, 5.0f);
        Gdiplus::LinearGradientBrush pillGrad(
            Gdiplus::PointF(typedBox.X, typedBox.Y),
            Gdiplus::PointF(typedBox.GetRight(), typedBox.GetBottom()),
            Gdiplus::Color(60, 59, 130, 246),
            Gdiplus::Color(60, 139, 92, 246));
        g.FillPath(&pillGrad, &pillPath);
        g.DrawString(pinyinLine.c_str(), -1, &boldFont, typedBox, &sfCenter, &cyanAccent);

        int pages = TotalPages();
        if (pages > 1) {
            std::wstring pageInfo = std::to_wstring(currentPage_ + 1) + L"/" + std::to_wstring(pages);
            g.DrawString(pageInfo.c_str(), -1, &hintFont,
                         Gdiplus::RectF(bx + bw - 96.0f, by + 2.0f, 36.0f, static_cast<float>(kHeaderH - 4)),
                         &sfRight, &hintBrush);

            auto drawArrow = [&](const RECT& r, const wchar_t* sym, bool hovered, bool enabled) {
                Gdiplus::RectF rf(static_cast<float>(r.left), static_cast<float>(r.top),
                                  static_cast<float>(r.right - r.left), static_cast<float>(r.bottom - r.top));
                if (hovered && enabled) {
                    Gdiplus::GraphicsPath ap;
                    AddRoundRectPath(ap, rf, 4.0f);
                    Gdiplus::SolidBrush hbg(Gdiplus::Color(255, 30, 41, 59));
                    g.FillPath(&hbg, &ap);
                }
                Gdiplus::SolidBrush fg(enabled ? Gdiplus::Color(255, 96, 165, 250) : Gdiplus::Color(255, 51, 65, 85));
                g.DrawString(sym, -1, &boldFont, rf, &sfCenter, &fg);
            };
            drawArrow(prevBtnRect_, L"‹", hoverPageBtn_ == 1, currentPage_ > 0);
            drawArrow(nextBtnRect_, L"›", hoverPageBtn_ == 2, currentPage_ + 1 < pages);
        } else {
            g.DrawString(L"Space · 1-9 · Esc", -1, &hintFont,
                         Gdiplus::RectF(bx + 90.0f, by + 2.0f, bw - 102.0f, static_cast<float>(kHeaderH - 4)),
                         &sfRight, &hintBrush);
        }

        // Horizontal candidate columns
        size_t startIdx = static_cast<size_t>(currentPage_) * kPageSize;
        for (size_t i = 0; i < itemRects_.size(); ++i) {
            size_t globalIdx = startIdx + i;
            if (globalIdx >= matches_.size()) break;
            const Phrase& p = matches_[globalIdx];
            const RECT& ir = itemRects_[i];

            Gdiplus::RectF card(static_cast<float>(ir.left), static_cast<float>(ir.top),
                                static_cast<float>(ir.right - ir.left),
                                static_cast<float>(ir.bottom - ir.top));

            bool isSelected = (static_cast<int>(i) == hoverIndex_) || (hoverIndex_ == -1 && i == 0);
            if (isSelected) {
                Gdiplus::LinearGradientBrush selGrad(
                    Gdiplus::PointF(card.X, card.Y),
                    Gdiplus::PointF(card.GetRight(), card.GetBottom()),
                    Gdiplus::Color(110, 59, 130, 246),
                    Gdiplus::Color(110, 139, 92, 246));
                Gdiplus::Pen selPen(Gdiplus::Color(200, 99, 102, 241), 1.0f);
                Gdiplus::GraphicsPath rp;
                AddRoundRectPath(rp, card, 7.0f);
                g.FillPath(&selGrad, &rp);
                g.DrawPath(&selPen, &rp);
            }

            std::wstring numStr = std::to_wstring(i + 1) + L".";
            g.DrawString(numStr.c_str(), -1, &numFont,
                         Gdiplus::RectF(card.X + 7.0f, card.Y, 18.0f, card.Height),
                         &sfLeft, isSelected ? &cyanAccent : &numMuted);

            float cx = card.X + 25.0f;
            if (!p.imageFile.empty()) {
                std::wstring imgPath = GetImageFullPath(p.imageFile);
                Gdiplus::Bitmap thumb(imgPath.c_str());
                if (thumb.GetLastStatus() == Gdiplus::Ok) {
                    g.DrawImage(&thumb, static_cast<INT>(cx), static_cast<INT>(card.Y + 6.0f), 22, 22);
                    cx += 26.0f;
                }
            }

            std::wstring preview = MakePreviewText(p);
            g.DrawString(preview.c_str(), -1, &normFont,
                         Gdiplus::RectF(cx, card.Y, card.GetRight() - cx - 6.0f, card.Height),
                         &sfLeft, isSelected ? &whiteText : &textBrush);
        }
    }

    POINT ptSrc{0, 0};
    SIZE sz{w, h};
    POINT ptDst{rc.left, rc.top};
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hwnd_, screenDC, &ptDst, &sz, memDC, &ptSrc, 0, &bf, ULW_ALPHA);

    SelectObject(memDC, oldBmp);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
}
