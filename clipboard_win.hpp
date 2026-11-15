#pragma once

#include <string>

void SendBackspaces(int count);
void SendCtrlV();

bool SetClipboardTextW(const std::wstring& text);
bool SetClipboardImageFile(const std::wstring& fullPath);

bool HasClipboardImage();
bool SaveClipboardImageToPng(const std::wstring& destPath);
bool ConvertImageFileToPng(const std::wstring& srcPath, const std::wstring& destPath);
