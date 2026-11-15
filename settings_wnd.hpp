#pragma once

#include <windows.h>

#include <functional>
#include <string>
#include <vector>

#include "storage.hpp"

class SettingsWindow {
public:
    using DataChangedCallback = std::function<void(const std::vector<Phrase>&, const AppSettings&)>;

    bool Create(HINSTANCE hInst, DataChangedCallback onChanged);
    void Show();
    void Hide();
    void SyncFromExternal(const std::vector<Phrase>& phrases, const AppSettings& settings);

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK TextEditSubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
    LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);

    void InitControls();
    void RefreshList();
    void LoadPhraseIntoForm(const Phrase& p);
    void ClearFormForNew();
    void UpdateImagePreview();
    void HandlePasteImage();
    void HandleBrowseImage();
    void HandleRemoveImage();
    void HandleSavePhrase();
    void HandleDeletePhrase();
    void HandleImportSogou();
    void HandleSettingsChanged();

    HWND hwnd_ = nullptr;
    HINSTANCE hInst_ = nullptr;
    HFONT font_ = nullptr;
    HFONT boldFont_ = nullptr;

    HWND editSearch_ = nullptr;
    HWND listBox_ = nullptr;
    HWND btnNew_ = nullptr;
    HWND btnImport_ = nullptr;

    HWND editShortcut_ = nullptr;
    HWND chkPhraseEnabled_ = nullptr;

    HWND imgPreview_ = nullptr;
    HWND btnPasteImg_ = nullptr;
    HWND btnBrowseImg_ = nullptr;
    HWND btnRemoveImg_ = nullptr;

    HWND editText_ = nullptr;
    HWND btnSave_ = nullptr;
    HWND btnDelete_ = nullptr;
    HWND lblStatus_ = nullptr;

    HWND chkActive_ = nullptr;
    HWND chkAutostart_ = nullptr;
    HWND editDelay_ = nullptr;

    std::vector<Phrase> phrases_;
    std::vector<int> filteredIndices_;
    AppSettings settings_;
    std::wstring currentId_;
    std::wstring stagedImageFile_; // newly pasted/browsed image in data/images/
    std::wstring existingImageFile_;
    DataChangedCallback onChanged_;
};
