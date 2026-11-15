#pragma once

#include <windows.h>

#include <functional>
#include <mutex>
#include <string>

class OcrSnipTool {
public:
    using TextReadyCallback = std::function<void(const std::wstring&, bool /*quickMode*/)>;

    bool Init(HINSTANCE hInst, HWND hostHwnd, TextReadyCallback onTextReady);
    void Shutdown();

    void StartSnip();
    void ScanForegroundWindow(bool imageOnlyMode);

    void ShowResultPopup(const std::wstring& text);
    void ShowQuickToast(const std::wstring& title, const std::wstring& body);

    static std::wstring ExtractCustomerId(const std::wstring& ocrText);
    static bool ExtractCsTarget(const std::wstring& ocrText, std::wstring& outType, std::wstring& outValue);

private:
    static LRESULT CALLBACK OverlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK ResultProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK ToastProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    LRESULT HandleOverlay(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void PaintOverlay(HWND hwnd, HDC hdc);
    void FinishSelection(HWND hwnd);
    void RunOcrAsync(const std::wstring& pngPath, bool quickMode);

    bool EnsureWorkerRunning();
    std::wstring QueryWorker(const std::wstring& pngPath);

    HINSTANCE hInst_ = nullptr;
    HWND hostHwnd_ = nullptr;
    HWND overlayHwnd_ = nullptr;
    HWND resultHwnd_ = nullptr;
    HWND resultEdit_ = nullptr;
    HWND toastHwnd_ = nullptr;
    HFONT font_ = nullptr;
    HFONT boldFont_ = nullptr;

    HBITMAP screenBmp_ = nullptr;
    int vx_ = 0;
    int vy_ = 0;
    int vw_ = 0;
    int vh_ = 0;

    bool dragging_ = false;
    POINT startPt_{};
    POINT curPt_{};

    std::mutex workerMutex_;
    HANDLE workerProc_ = nullptr;
    HANDLE workerInWrite_ = nullptr;
    HANDLE workerOutRead_ = nullptr;

    TextReadyCallback onTextReady_;
};
