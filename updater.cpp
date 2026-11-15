#include "updater.hpp"

#include <winhttp.h>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#include "storage.hpp"
#include "version.hpp"

namespace {

std::wstring Utf8ToWideUpd(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

bool HttpGetBytes(const std::wstring& url, std::string& outBytes) {
    outBytes.clear();

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) {
        return false;
    }

    HINTERNET hSession = WinHttpOpen(L"QuickPhraseUpdater/1.0",
                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    bool ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(hRequest, nullptr);

    if (ok) {
        DWORD status = 0;
        DWORD statusSz = sizeof(status);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSz, WINHTTP_NO_HEADER_INDEX);
        if (status < 200 || status >= 300) {
            ok = false;
        }
    }

    if (ok) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            std::vector<char> buf(avail);
            DWORD read = 0;
            if (WinHttpReadData(hRequest, buf.data(), avail, &read) && read > 0) {
                outBytes.append(buf.data(), read);
            } else {
                break;
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok && !outBytes.empty();
}

std::wstring ExtractJsonField(const std::wstring& json, const std::wstring& key) {
    std::wstring needle = L"\"" + key + L"\"";
    size_t pos = json.find(needle);
    if (pos == std::wstring::npos) return {};
    pos = json.find(L':', pos + needle.size());
    if (pos == std::wstring::npos) return {};
    size_t q1 = json.find(L'"', pos + 1);
    if (q1 == std::wstring::npos) return {};
    size_t q2 = json.find(L'"', q1 + 1);
    if (q2 == std::wstring::npos) return {};
    return json.substr(q1 + 1, q2 - q1 - 1);
}

std::vector<int> ParseVersionParts(const std::wstring& v) {
    std::vector<int> parts;
    std::wstringstream ss(v);
    std::wstring item;
    while (std::getline(ss, item, L'.')) {
        parts.push_back(_wtoi(item.c_str()));
    }
    while (parts.size() < 3) parts.push_back(0);
    return parts;
}

bool IsNewerVersion(const std::wstring& remote, const std::wstring& local) {
    auto r = ParseVersionParts(remote);
    auto l = ParseVersionParts(local);
    for (size_t i = 0; i < std::min(r.size(), l.size()); ++i) {
        if (r[i] > l[i]) return true;
        if (r[i] < l[i]) return false;
    }
    return false;
}

} // namespace

std::wstring GetUpdateUrlSetting() {
    std::wstring iniPath = GetBaseDir() + L"\\data\\settings.ini";
    wchar_t buf[1024]{};
    GetPrivateProfileStringW(L"Update", L"Url", DEFAULT_UPDATE_URL, buf, 1024, iniPath.c_str());
    return std::wstring(buf);
}

UpdateInfo CheckForUpdate(const std::wstring& manifestUrl) {
    UpdateInfo info;
    std::string raw;
    if (!HttpGetBytes(manifestUrl, raw)) {
        return info;
    }

    std::wstring json = Utf8ToWideUpd(raw);
    std::wstring ver = ExtractJsonField(json, L"version");
    std::wstring dl = ExtractJsonField(json, L"url");
    if (ver.empty() || dl.empty()) {
        return info;
    }

    info.version = ver;
    info.downloadUrl = dl;
    info.available = IsNewerVersion(ver, APP_VERSION_STR);
    return info;
}

bool DownloadAndRunUpdate(const std::wstring& downloadUrl) {
    std::string installerBytes;
    if (!HttpGetBytes(downloadUrl, installerBytes)) {
        return false;
    }

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring setupPath = std::wstring(tempDir) + L"QuickPhraseSetup_Update.exe";

    {
        std::ofstream out(setupPath.c_str(), std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write(installerBytes.data(), static_cast<std::streamsize>(installerBytes.size()));
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"open";
    sei.lpFile = setupPath.c_str();
    sei.lpParameters = L"/SILENT";
    sei.nShow = SW_HIDE;
    return ShellExecuteExW(&sei) != FALSE;
}

void CheckForUpdateAsync(HWND hostHwnd, UINT resultMsg, bool silentIfUpToDate) {
    std::thread([hostHwnd, resultMsg, silentIfUpToDate]() {
        std::wstring url = GetUpdateUrlSetting();
        UpdateInfo* info = new UpdateInfo(CheckForUpdate(url));
        PostMessageW(hostHwnd, resultMsg, silentIfUpToDate ? 1 : 0, reinterpret_cast<LPARAM>(info));
    }).detach();
}
