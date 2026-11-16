#include "storage.hpp"

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::wstring EscapeField(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (wchar_t ch : in) {
        switch (ch) {
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            case L'|':  out += L"\\p"; break;
            default:    out += ch; break;
        }
    }
    return out;
}

std::wstring UnescapeField(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == L'\\' && i + 1 < in.size()) {
            wchar_t nxt = in[++i];
            switch (nxt) {
                case L'\\': out += L'\\'; break;
                case L'n':  out += L'\n'; break;
                case L'r':  out += L'\r'; break;
                case L't':  out += L'\t'; break;
                case L'p':  out += L'|'; break;
                default:    out += nxt; break;
            }
        } else {
            out += in[i];
        }
    }
    return out;
}

std::vector<std::wstring> SplitPipes(const std::wstring& line) {
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t ch : line) {
        if (ch == L'|') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur += ch;
        }
    }
    parts.push_back(cur);
    return parts;
}

void EnsureDirs() {
    std::wstring dataDir = GetBaseDir() + L"\\data";
    CreateDirectoryW(dataDir.c_str(), nullptr);
    std::wstring imgDir = GetImagesDir();
    CreateDirectoryW(imgDir.c_str(), nullptr);
}

} // namespace

std::wstring GetBaseDir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    return std::wstring(buf);
}

std::wstring GetImagesDir() {
    return GetBaseDir() + L"\\data\\images";
}

std::wstring GetImageFullPath(const std::wstring& imageFile) {
    if (imageFile.empty()) return {};
    return GetImagesDir() + L"\\" + imageFile;
}

std::wstring GenerateId() {
    GUID guid{};
    CoCreateGuid(&guid);
    wchar_t buf[64]{};
    swprintf_s(buf, L"%08lx%04x%04x%02x%02x%02x%02x%02x%02x%02x%02x",
               guid.Data1, guid.Data2, guid.Data3,
               guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3],
               guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
    return std::wstring(buf);
}

void DeleteImageFile(const std::wstring& imageFile) {
    if (imageFile.empty()) return;
    std::wstring full = GetImageFullPath(imageFile);
    DeleteFileW(full.c_str());
}

std::vector<Phrase> LoadPhrases() {
    EnsureDirs();
    std::wstring path = GetBaseDir() + L"\\data\\phrases.db";
    std::ifstream in(WideToUtf8(path), std::ios::binary);
    if (!in.is_open()) {
        std::vector<Phrase> defaults = {
            {GenerateId(), L"c,1", L"hello", L"", true},
            {GenerateId(), L"c,2", L"សួស្តី!", L"", true},
        };
        SavePhrases(defaults);
        return defaults;
    }

    std::ostringstream ss;
    ss << in.rdbuf();
    std::wstring content = Utf8ToWide(ss.str());

    std::vector<Phrase> result;
    std::wstringstream wss(content);
    std::wstring line;
    while (std::getline(wss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L'#') continue;
        auto cols = SplitPipes(line);
        if (cols.size() < 5) continue;
        Phrase p;
        p.id = UnescapeField(cols[0]);
        p.shortcut = UnescapeField(cols[1]);
        // Read the old group/order format once as a direct shortcut (c + 1 becomes c,1).
        if (cols.size() >= 6) {
            p.shortcut += L"," + cols[2];
            p.enabled = (cols[3] == L"1");
            p.imageFile = UnescapeField(cols[4]);
            p.text = UnescapeField(cols[5]);
        } else {
            p.enabled = (cols[2] == L"1");
            p.imageFile = UnescapeField(cols[3]);
            p.text = UnescapeField(cols[4]);
        }
        result.push_back(std::move(p));
    }
    return result;
}

bool SavePhrases(const std::vector<Phrase>& phrases) {
    EnsureDirs();
    std::wstring path = GetBaseDir() + L"\\data\\phrases.db";
    std::ofstream out(WideToUtf8(path), std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    std::wstring buf = L"# QuickPhrase v2: id|shortcut|enabled|imageFile|text\n";
    for (const auto& p : phrases) {
        buf += EscapeField(p.id) + L"|"
             + EscapeField(p.shortcut) + L"|"
             + (p.enabled ? L"1" : L"0") + L"|"
             + EscapeField(p.imageFile) + L"|"
             + EscapeField(p.text) + L"\n";
    }
    std::string utf8 = WideToUtf8(buf);
    out.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return true;
}

AppSettings LoadSettings() {
    EnsureDirs();
    AppSettings s;
    std::wstring path = GetBaseDir() + L"\\data\\settings.ini";
    s.enabled = GetPrivateProfileIntW(L"General", L"Enabled", 1, path.c_str()) != 0;
    s.pasteDelayMs = std::clamp(
        static_cast<int>(GetPrivateProfileIntW(L"General", L"PasteDelayMs", 350, path.c_str())),
        100, 3000);
    return s;
}

bool SaveSettings(const AppSettings& settings) {
    EnsureDirs();
    std::wstring path = GetBaseDir() + L"\\data\\settings.ini";
    WritePrivateProfileStringW(L"General", L"Enabled", settings.enabled ? L"1" : L"0", path.c_str());
    std::wstring delayStr = std::to_wstring(settings.pasteDelayMs);
    WritePrivateProfileStringW(L"General", L"PasteDelayMs", delayStr.c_str(), path.c_str());
    return true;
}

static const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* kRunValue = L"QuickPhraseTrayCpp";

bool IsAutostartEnabled() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD size = 0;
    LONG res = RegQueryValueExW(hKey, kRunValue, nullptr, &type, nullptr, &size);
    RegCloseKey(hKey);
    return res == ERROR_SUCCESS;
}

bool SetAutostartEnabled(bool enable) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    LONG res = ERROR_SUCCESS;
    if (enable) {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exePath) + L"\"";
        res = RegSetValueExW(hKey, kRunValue, 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(cmd.c_str()),
                             static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        res = RegDeleteValueW(hKey, kRunValue);
        if (res == ERROR_FILE_NOT_FOUND) res = ERROR_SUCCESS;
    }
    RegCloseKey(hKey);
    return res == ERROR_SUCCESS;
}

std::wstring ReadTextFileAutoEncoding(const std::wstring& filePath) {
    std::ifstream in(WideToUtf8(filePath), std::ios::binary);
    if (!in.is_open()) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string bytes = ss.str();
    if (bytes.empty()) return {};

    // UTF-16 LE BOM (FF FE) - common in Windows/Sogou exported files
    if (bytes.size() >= 2 &&
        static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE) {
        size_t wcharCount = (bytes.size() - 2) / 2;
        std::wstring out(wcharCount, L'\0');
        std::memcpy(out.data(), bytes.data() + 2, wcharCount * 2);
        return out;
    }

    // UTF-8 BOM (EF BB BF)
    size_t offset = 0;
    if (bytes.size() >= 3 &&
        static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        offset = 3;
    }
    return Utf8ToWide(bytes.substr(offset));
}

int ImportSogouText(const std::wstring& rawText, std::vector<Phrase>& phrases) {
    std::wstringstream wss(rawText);
    std::wstring line;
    int importedCount = 0;

    auto trim = [](std::wstring& s) {
        while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r')) s.erase(s.begin());
        while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r')) s.pop_back();
    };

    auto isShortcutHeader = [&](const std::wstring& ln, std::wstring& outKey, std::wstring& outVal) -> bool {
        if (ln.empty() || ln[0] == L';' || ln[0] == L'#') return false;
        size_t eq = ln.find(L'=');
        if (eq == std::wstring::npos || eq == 0) return false;

        std::wstring k = ln.substr(0, eq);
        trim(k);
        if (k.empty() || k.size() > 32) return false;
        for (wchar_t ch : k) {
            if (ch == L' ' || ch == L'\t' || ch == L'/' || ch == L'\\' || ch == L':' || ch == L'?' || ch == L'&') {
                return false;
            }
        }
        outKey = k;
        outVal = ln.substr(eq + 1);
        return true;
    };

    std::wstring curKey;
    std::wstring curVal;
    bool hasStartedVal = false;

    auto flushCurrent = [&]() {
        if (curKey.empty()) return;
        while (!curVal.empty() && (curVal.back() == L'\r' || curVal.back() == L'\n')) {
            curVal.pop_back();
        }
        if (!curVal.empty()) {
            auto it = std::find_if(phrases.begin(), phrases.end(), [&](const Phrase& p) {
                return _wcsicmp(p.shortcut.c_str(), curKey.c_str()) == 0;
            });
            if (it != phrases.end()) {
                it->text = curVal;
                it->enabled = true;
            } else {
                phrases.push_back(Phrase{GenerateId(), curKey, curVal, L"", true});
            }
            ++importedCount;
        }
        curKey.clear();
        curVal.clear();
        hasStartedVal = false;
    };

    while (std::getline(wss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();

        std::wstring nextKey, nextVal;
        if (isShortcutHeader(line, nextKey, nextVal)) {
            flushCurrent();
            curKey = nextKey;
            if (!nextVal.empty()) {
                curVal = nextVal;
                hasStartedVal = true;
            } else {
                curVal.clear();
                hasStartedVal = false;
            }
            continue;
        }

        if (curKey.empty()) {
            // Ignore blank lines or comments before the first shortcut entry
            continue;
        }

        if (!hasStartedVal) {
            // If header was "gl,2=" (empty right side), skip leading blank lines before first content line
            std::wstring tmp = line;
            trim(tmp);
            if (tmp.empty()) continue;
            curVal = line;
            hasStartedVal = true;
        } else {
            curVal += L"\r\n";
            curVal += line;
        }
    }
    flushCurrent();

    return importedCount;
}
