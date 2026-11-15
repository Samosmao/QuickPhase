#pragma once

#include <string>
#include <vector>

struct Phrase {
    std::wstring id;
    std::wstring shortcut;
    std::wstring text;
    std::wstring imageFile; // relative filename in data/images/
    bool enabled = true;
};

struct AppSettings {
    bool enabled = true;
    int pasteDelayMs = 350;
};

std::wstring GetBaseDir();
std::wstring GetImagesDir();
std::wstring GetImageFullPath(const std::wstring& imageFile);

std::vector<Phrase> LoadPhrases();
bool SavePhrases(const std::vector<Phrase>& phrases);

AppSettings LoadSettings();
bool SaveSettings(const AppSettings& settings);

std::wstring GenerateId();
void DeleteImageFile(const std::wstring& imageFile);

bool IsAutostartEnabled();
bool SetAutostartEnabled(bool enable);

int ImportSogouText(const std::wstring& rawText, std::vector<Phrase>& phrases);
std::wstring ReadTextFileAutoEncoding(const std::wstring& filePath);
