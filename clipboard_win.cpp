#include "clipboard_win.hpp"

#include <windows.h>
#include <gdiplus.h>
#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace {

INPUT MakeKeyInput(WORD vk, bool keyUp) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = keyUp ? KEYEVENTF_KEYUP : 0;
    return in;
}

bool GetPngEncoderClsid(CLSID* pClsid) {
    UINT num = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return false;

    std::vector<BYTE> buf(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buf.data());
    Gdiplus::GetImageEncoders(num, size, encoders);

    for (UINT i = 0; i < num; ++i) {
        if (wcscmp(encoders[i].MimeType, L"image/png") == 0) {
            *pClsid = encoders[i].Clsid;
            return true;
        }
    }
    return false;
}

bool SaveBitmapAsPng(Gdiplus::Bitmap* bmp, const std::wstring& destPath) {
    if (!bmp || bmp->GetLastStatus() != Gdiplus::Ok) return false;
    CLSID clsid{};
    if (!GetPngEncoderClsid(&clsid)) return false;
    return bmp->Save(destPath.c_str(), &clsid, nullptr) == Gdiplus::Ok;
}

bool IsSupportedImageExt(const std::wstring& path) {
    auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring ext = path.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext == L".png" || ext == L".jpg" || ext == L".jpeg" ||
           ext == L".bmp" || ext == L".gif" || ext == L".webp";
}

} // namespace

void SendBackspaces(int count) {
    if (count <= 0) return;
    std::vector<INPUT> events;
    events.reserve(static_cast<size_t>(count) * 2);
    for (int i = 0; i < count; ++i) {
        events.push_back(MakeKeyInput(VK_BACK, false));
        events.push_back(MakeKeyInput(VK_BACK, true));
    }
    SendInput(static_cast<UINT>(events.size()), events.data(), sizeof(INPUT));
}

void SendCtrlV() {
    INPUT events[4] = {
        MakeKeyInput(VK_CONTROL, false),
        MakeKeyInput('V', false),
        MakeKeyInput('V', true),
        MakeKeyInput(VK_CONTROL, true),
    };
    SendInput(4, events, sizeof(INPUT));
}

bool SetClipboardTextW(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();

    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) {
        CloseClipboard();
        return false;
    }

    void* ptr = GlobalLock(hMem);
    if (ptr) {
        std::memcpy(ptr, text.c_str(), bytes);
        GlobalUnlock(hMem);
    }
    SetClipboardData(CF_UNICODETEXT, hMem);
    CloseClipboard();
    return true;
}

bool SetClipboardImageFile(const std::wstring& fullPath) {
    Gdiplus::Bitmap src(fullPath.c_str());
    if (src.GetLastStatus() != Gdiplus::Ok) return false;

    UINT w = src.GetWidth();
    UINT h = src.GetHeight();
    if (w == 0 || h == 0) return false;

    // Convert to 24-bit RGB bottom-up DIB for maximum compatibility (Telegram, Chrome, Messenger, etc.)
    Gdiplus::Bitmap rgb(w, h, PixelFormat24bppRGB);
    {
        Gdiplus::Graphics g(&rgb);
        g.Clear(Gdiplus::Color(255, 255, 255, 255));
        g.DrawImage(&src, 0, 0, static_cast<INT>(w), static_cast<INT>(h));
    }

    Gdiplus::BitmapData data{};
    Gdiplus::Rect rect(0, 0, static_cast<INT>(w), static_cast<INT>(h));
    if (rgb.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat24bppRGB, &data) != Gdiplus::Ok) {
        return false;
    }

    DWORD rowStride = ((w * 3 + 3) / 4) * 4;
    DWORD imgSize = rowStride * h;
    DWORD totalSize = sizeof(BITMAPINFOHEADER) + imgSize;

    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, totalSize);
    if (!hMem) {
        rgb.UnlockBits(&data);
        return false;
    }

    BYTE* dst = static_cast<BYTE*>(GlobalLock(hMem));
    if (!dst) {
        GlobalFree(hMem);
        rgb.UnlockBits(&data);
        return false;
    }

    BITMAPINFOHEADER bih{};
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = static_cast<LONG>(w);
    bih.biHeight = static_cast<LONG>(h); // positive = bottom-up DIB
    bih.biPlanes = 1;
    bih.biBitCount = 24;
    bih.biCompression = BI_RGB;
    bih.biSizeImage = imgSize;
    std::memcpy(dst, &bih, sizeof(BITMAPINFOHEADER));

    BYTE* pixelDst = dst + sizeof(BITMAPINFOHEADER);
    const BYTE* pixelSrc = static_cast<const BYTE*>(data.Scan0);
    for (UINT y = 0; y < h; ++y) {
        const BYTE* srcRow = pixelSrc + (h - 1 - y) * data.Stride;
        BYTE* dstRow = pixelDst + y * rowStride;
        std::memcpy(dstRow, srcRow, std::min<DWORD>(rowStride, static_cast<DWORD>(w * 3)));
    }

    GlobalUnlock(hMem);
    rgb.UnlockBits(&data);

    if (!OpenClipboard(nullptr)) {
        GlobalFree(hMem);
        return false;
    }
    EmptyClipboard();
    SetClipboardData(CF_DIB, hMem);
    CloseClipboard();
    return true;
}

bool HasClipboardImage() {
    if (IsClipboardFormatAvailable(CF_DIB) || IsClipboardFormatAvailable(CF_BITMAP)) {
        return true;
    }
    if (IsClipboardFormatAvailable(CF_HDROP)) {
        if (OpenClipboard(nullptr)) {
            HDROP hDrop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
            bool ok = false;
            if (hDrop) {
                wchar_t file[MAX_PATH]{};
                if (DragQueryFileW(hDrop, 0, file, MAX_PATH) > 0) {
                    ok = IsSupportedImageExt(file);
                }
            }
            CloseClipboard();
            return ok;
        }
    }
    return false;
}

bool SaveClipboardImageToPng(const std::wstring& destPath) {
    if (!OpenClipboard(nullptr)) return false;

    bool saved = false;

    if (IsClipboardFormatAvailable(CF_DIB)) {
        HGLOBAL hMem = GetClipboardData(CF_DIB);
        if (hMem) {
            BYTE* ptr = static_cast<BYTE*>(GlobalLock(hMem));
            if (ptr) {
                auto* bih = reinterpret_cast<BITMAPINFOHEADER*>(ptr);
                DWORD paletteColors = bih->biClrUsed;
                if (paletteColors == 0 && bih->biBitCount <= 8) {
                    paletteColors = 1u << bih->biBitCount;
                }
                DWORD offset = bih->biSize + paletteColors * sizeof(RGBQUAD);
                if (bih->biCompression == BI_BITFIELDS && bih->biSize == sizeof(BITMAPINFOHEADER)) {
                    offset += 12;
                }
                Gdiplus::Bitmap bmp(reinterpret_cast<BITMAPINFO*>(ptr), ptr + offset);
                saved = SaveBitmapAsPng(&bmp, destPath);
                GlobalUnlock(hMem);
            }
        }
    } else if (IsClipboardFormatAvailable(CF_BITMAP)) {
        HBITMAP hBmp = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP));
        if (hBmp) {
            Gdiplus::Bitmap bmp(hBmp, nullptr);
            saved = SaveBitmapAsPng(&bmp, destPath);
        }
    } else if (IsClipboardFormatAvailable(CF_HDROP)) {
        HDROP hDrop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
        if (hDrop) {
            wchar_t file[MAX_PATH]{};
            if (DragQueryFileW(hDrop, 0, file, MAX_PATH) > 0 && IsSupportedImageExt(file)) {
                Gdiplus::Bitmap bmp(file);
                saved = SaveBitmapAsPng(&bmp, destPath);
            }
        }
    }

    CloseClipboard();
    return saved;
}

bool ConvertImageFileToPng(const std::wstring& srcPath, const std::wstring& destPath) {
    Gdiplus::Bitmap bmp(srcPath.c_str());
    return SaveBitmapAsPng(&bmp, destPath);
}
