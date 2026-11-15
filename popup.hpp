#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "storage.hpp"

class CandidatePopup {
public:
    using SelectCallback = std::function<void(const Phrase&)>;

    bool Create(HINSTANCE hInst, SelectCallback onSelect);
    void ShowCandidates(const std::wstring& typed, const std::vector<Phrase>& matches);
    void Hide();
    bool IsVisible() const;

    bool NextPage();
    bool PrevPage();
    bool GetPageCandidate(size_t pageLocalIdx, Phrase& outPhrase) const;

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void LayoutItems(int& outWidth, int& outHeight);
    void RenderLayered();
    POINT ComputeAnchorPoint(int width, int height) const;
    int TotalPages() const;

    HWND hwnd_ = nullptr;
    std::wstring typed_;
    std::vector<Phrase> matches_;
    std::vector<RECT> itemRects_;
    RECT prevBtnRect_{};
    RECT nextBtnRect_{};
    SelectCallback onSelect_;
    int hoverIndex_ = -1;
    int hoverPageBtn_ = 0; // 0=none, 1=prev, 2=next
    int currentPage_ = 0;

    static constexpr int kPageSize = 9;
    static constexpr int kShadow = 8;
    static constexpr int kHeaderH = 30;
    static constexpr int kRowH = 38;
    static constexpr int kPadX = 12;
    static constexpr int kPadY = 6;
    static constexpr int kMaxBoxW = 720;
    static constexpr int kMinBoxW = 260;
};
