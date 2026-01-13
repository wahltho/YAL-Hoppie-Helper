#include "XPLMDataAccess.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <condition_variable>
#include <cctype>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <thread>

PLUGIN_API void XPluginDisable(void);

namespace {

constexpr const char* kPluginName = "YAL_hoppiehelper";
constexpr const char* kPluginSig = "yal.hoppiehelper";
constexpr const char* kPluginVersion = "1.0.0";
constexpr const char* kPluginDesc = "HTTP helper for Hoppie ACARS (CPDLC) v1.0.0";
constexpr const char* kHoppieUrl = "https://www.hoppie.nl/acars/system/connect.html";

constexpr float kFlightLoopInterval = 1.0f;
constexpr int kPollDefaultMinSeconds = 45;
constexpr int kPollDefaultMaxSeconds = 75;
constexpr int kPollFastMinSeconds = 12;
constexpr int kPollFastMaxSeconds = 18;

struct DataRefs {
    XPLMDataRef send_queue = nullptr;
    XPLMDataRef send_message_to = nullptr;
    XPLMDataRef send_message_type = nullptr;
    XPLMDataRef send_message_packet = nullptr;
    XPLMDataRef callsign = nullptr;
    XPLMDataRef send_callsign = nullptr;
    XPLMDataRef poll_frequency_fast = nullptr;
    XPLMDataRef poll_queue = nullptr;
    XPLMDataRef poll_message_origin = nullptr;
    XPLMDataRef poll_message_from = nullptr;
    XPLMDataRef poll_message_type = nullptr;
    XPLMDataRef poll_message_packet = nullptr;
    XPLMDataRef poll_queue_clear = nullptr;
    XPLMDataRef comm_ready = nullptr;
    XPLMDataRef logon = nullptr;
    XPLMDataRef debug_level = nullptr;
    XPLMDataRef status = nullptr;
    XPLMDataRef last_error = nullptr;
    XPLMDataRef last_http = nullptr;
    XPLMDataRef send_count = nullptr;
    XPLMDataRef poll_count = nullptr;
    XPLMDataRef hbdr_ready = nullptr;
    XPLMDataRef avionics_on = nullptr;
};

constexpr int kNumberDataRefTypes = xplmType_Int | xplmType_Float | xplmType_Double;

struct DataRefSlot {
    std::string text;
    double number = 0.0;
};

struct OwnedDataRef {
    const char* name = nullptr;
    int types = 0;
    bool isString = false;
    DataRefSlot slot;
    XPLMDataRef ref = nullptr;
    bool owned = false;
};

std::array<OwnedDataRef, 14> g_ownedDataRefs = {{
    {"hoppiebridge/send_queue", xplmType_Data, true},
    {"hoppiebridge/send_message_to", xplmType_Data, true},
    {"hoppiebridge/send_message_type", xplmType_Data, true},
    {"hoppiebridge/send_message_packet", xplmType_Data, true},
    {"hoppiebridge/send_callsign", xplmType_Data, true},
    {"hoppiebridge/poll_frequency_fast", kNumberDataRefTypes, false},
    {"hoppiebridge/poll_queue", xplmType_Data, true},
    {"hoppiebridge/poll_message_origin", xplmType_Data, true},
    {"hoppiebridge/poll_message_from", xplmType_Data, true},
    {"hoppiebridge/poll_message_type", xplmType_Data, true},
    {"hoppiebridge/poll_message_packet", xplmType_Data, true},
    {"hoppiebridge/poll_queue_clear", kNumberDataRefTypes, false},
    {"hoppiebridge/callsign", xplmType_Data, true},
    {"hoppiebridge/comm_ready", kNumberDataRefTypes, false},
}};

DataRefs g_dref;
std::atomic<bool> g_running{false};
std::thread g_worker;

enum class JobType { Send, Poll };

struct HttpJob {
    JobType type;
    std::string logon;
    std::string from;
    std::string to;
    std::string msg_type;
    std::string packet;
};

struct HttpResult {
    JobType type;
    bool ok = false;
    std::string response;
    std::string error;
    long httpCode = 0;
};

std::mutex g_mutex;
std::condition_variable g_cv;
std::queue<HttpJob> g_jobs;
std::queue<HttpResult> g_results;
std::mutex g_ownedMutex;
std::mt19937 g_rng{std::random_device{}()};

bool g_sendPending = false;
bool g_pollPending = false;
double g_nextPollTime = 0.0;
int g_debugLevel = 1;
std::string g_lastStatus;
bool g_refsReady = false;
bool g_loggedMissingRefs = false;
bool g_loggedHbdrFound = false;
bool g_loggedHbdrWritable = false;
bool g_loggedHbdrTypes = false;
bool g_loggedStartupSummary = false;
bool g_commReadyKnown = false;
bool g_lastCommReady = false;
bool g_pollModeKnown = false;
bool g_lastFastPoll = false;
bool g_logonKnown = false;
bool g_logonAvailable = false;
std::string g_lastCallsign;
double g_nextRefScanTime = 0.0;
bool g_commEstablished = false;

enum LogLevel { LOG_ERR = 1, LOG_INFO = 2, LOG_DBG = 3 };

std::string GetDataRefString(XPLMDataRef dr);
void SetDataRefString(XPLMDataRef dr, const std::string& value);
bool GetDataRefBool(XPLMDataRef dr);
void SetDataRefBool(XPLMDataRef dr, bool value);
std::string DataRefTypeString(int types);
int GetDataiCB(void* refcon);
void SetDataiCB(void* refcon, int value);
float GetDatafCB(void* refcon);
void SetDatafCB(void* refcon, float value);
double GetDatadCB(void* refcon);
void SetDatadCB(void* refcon, double value);
int GetDatabCB(void* refcon, void* outValue, int offset, int max);
void SetDatabCB(void* refcon, void* inValue, int offset, int max);
bool HasCoreDataRefs();
void RefreshDataRefs(double now);
void RefreshHbdrReady();
void EnsureHoppieDataRefs();
void UnregisterHoppieDataRefs();
double NextPollIntervalSeconds(bool fastPoll);
void ScheduleNextPollTime(double now);
void LogReadySummary();
void LogCallsignState(const std::string& callsign, bool justSet);
void LogCommReadyState(bool ready);
void LogPollModeChange();
void LogLogonState(const std::string& logon);

void Log(LogLevel level, const std::string& msg) {
    if (g_debugLevel < level) {
        return;
    }
    std::string line = std::string("[YAL HoppieHelper] ") + msg + "\n";
    XPLMDebugString(line.c_str());
}

void SetStatus(const std::string& status) {
    if (g_lastStatus == status) {
        return;
    }
    g_lastStatus = status;
    SetDataRefString(g_dref.status, status);
    Log(LOG_INFO, "Status: " + status);
}

void SetLastError(const std::string& err) {
    SetDataRefString(g_dref.last_error, err);
    if (!err.empty()) {
        Log(LOG_ERR, err);
    }
}

void SetLastHttp(const std::string& info) {
    SetDataRefString(g_dref.last_http, info);
    if (!info.empty()) {
        Log(LOG_DBG, info);
    }
}

int GetDebugLevel() {
    if (!g_dref.debug_level) {
        return 1;
    }
    int level = XPLMGetDatai(g_dref.debug_level);
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    return level;
}

std::string Trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return s.substr(start, end - start);
}

std::string UrlEncode(const std::string& input) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(input.size() * 3);
    for (unsigned char c : input) {
        if ((c >= 'a' && c <= 'z')
            || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9')
            || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0F]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return std::wstring();
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) {
        return std::wstring();
    }
    std::wstring out(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], len);
    return out;
}

std::string WinHttpError(DWORD code) {
    return "WinHTTP error " + std::to_string(code);
}
#endif

std::string GetDataRefString(XPLMDataRef dr) {
    if (!dr) {
        return "";
    }
    int len = XPLMGetDatab(dr, nullptr, 0, 0);
    if (len <= 0) {
        return "";
    }
    if (len > 2047) {
        len = 2047;
    }
    std::string out(static_cast<size_t>(len), '\0');
    int read = XPLMGetDatab(dr, &out[0], 0, len);
    if (read <= 0) {
        return "";
    }
    out.resize(static_cast<size_t>(read));
    size_t nullPos = out.find('\0');
    if (nullPos != std::string::npos) {
        out.resize(nullPos);
    }
    return out;
}

void SetDataRefString(XPLMDataRef dr, const std::string& value) {
    if (!dr) {
        return;
    }
    if (value.empty()) {
        const char zero = '\0';
        XPLMSetDatab(dr, const_cast<char*>(&zero), 0, 1);
        return;
    }
    XPLMSetDatab(dr, const_cast<char*>(value.data()), 0, static_cast<int>(value.size()));
}

bool GetDataRefBool(XPLMDataRef dr) {
    if (!dr) {
        return false;
    }
    return XPLMGetDatai(dr) != 0;
}

void SetDataRefBool(XPLMDataRef dr, bool value) {
    if (!dr) {
        return;
    }
    int types = XPLMGetDataRefTypes(dr);
    int iv = value ? 1 : 0;
    float fv = value ? 1.0f : 0.0f;
    double dv = value ? 1.0 : 0.0;
    bool wrote = false;
    if (types & xplmType_Int) {
        XPLMSetDatai(dr, iv);
        wrote = true;
    }
    if (types & xplmType_Float) {
        XPLMSetDataf(dr, fv);
        wrote = true;
    }
    if (types & xplmType_Double) {
        XPLMSetDatad(dr, dv);
        wrote = true;
    }
    if (!wrote) {
        XPLMSetDatai(dr, iv);
    }
}

std::string DataRefTypeString(int types) {
    std::string out;
    if (types & xplmType_Int) out += "int|";
    if (types & xplmType_Float) out += "float|";
    if (types & xplmType_Double) out += "double|";
    if (types & xplmType_Data) out += "data|";
    if (out.empty()) {
        return "unknown";
    }
    out.pop_back();
    return out;
}

int GetDataiCB(void* refcon) {
    auto* slot = static_cast<DataRefSlot*>(refcon);
    std::lock_guard<std::mutex> lock(g_ownedMutex);
    return static_cast<int>(slot->number);
}

void SetDataiCB(void* refcon, int value) {
    auto* slot = static_cast<DataRefSlot*>(refcon);
    std::lock_guard<std::mutex> lock(g_ownedMutex);
    slot->number = static_cast<double>(value);
}

float GetDatafCB(void* refcon) {
    auto* slot = static_cast<DataRefSlot*>(refcon);
    std::lock_guard<std::mutex> lock(g_ownedMutex);
    return static_cast<float>(slot->number);
}

void SetDatafCB(void* refcon, float value) {
    auto* slot = static_cast<DataRefSlot*>(refcon);
    std::lock_guard<std::mutex> lock(g_ownedMutex);
    slot->number = static_cast<double>(value);
}

double GetDatadCB(void* refcon) {
    auto* slot = static_cast<DataRefSlot*>(refcon);
    std::lock_guard<std::mutex> lock(g_ownedMutex);
    return slot->number;
}

void SetDatadCB(void* refcon, double value) {
    auto* slot = static_cast<DataRefSlot*>(refcon);
    std::lock_guard<std::mutex> lock(g_ownedMutex);
    slot->number = value;
}

int GetDatabCB(void* refcon, void* outValue, int offset, int max) {
    auto* slot = static_cast<DataRefSlot*>(refcon);
    std::lock_guard<std::mutex> lock(g_ownedMutex);
    if (offset < 0) {
        return 0;
    }
    int len = static_cast<int>(slot->text.size());
    if (!outValue) {
        return len;
    }
    if (offset >= len) {
        return 0;
    }
    int count = std::min(max, len - offset);
    std::memcpy(outValue, slot->text.data() + offset, static_cast<size_t>(count));
    return count;
}

void SetDatabCB(void* refcon, void* inValue, int offset, int max) {
    auto* slot = static_cast<DataRefSlot*>(refcon);
    std::lock_guard<std::mutex> lock(g_ownedMutex);
    if (offset < 0) {
        return;
    }
    if (!inValue || max <= 0) {
        if (offset == 0) {
            slot->text.clear();
        }
        return;
    }
    const char* data = static_cast<const char*>(inValue);
    if (offset == 0) {
        if (max == 1 && data[0] == '\0') {
            slot->text.clear();
            return;
        }
        slot->text.assign(data, data + max);
        return;
    }
    if (offset > static_cast<int>(slot->text.size())) {
        slot->text.resize(static_cast<size_t>(offset), '\0');
    }
    if (offset + max > static_cast<int>(slot->text.size())) {
        slot->text.resize(static_cast<size_t>(offset + max), '\0');
    }
    std::memcpy(&slot->text[0] + offset, data, static_cast<size_t>(max));
}

void EnsureHoppieDataRefs() {
    int created = 0;
    for (auto& def : g_ownedDataRefs) {
        if (def.owned) {
            continue;
        }
        if (XPLMFindDataRef(def.name)) {
            continue;
        }
        XPLMDataRef ref = XPLMRegisterDataAccessor(
            def.name,
            def.types,
            1,
            def.isString ? nullptr : GetDataiCB,
            def.isString ? nullptr : SetDataiCB,
            def.isString ? nullptr : GetDatafCB,
            def.isString ? nullptr : SetDatafCB,
            def.isString ? nullptr : GetDatadCB,
            def.isString ? nullptr : SetDatadCB,
            nullptr,
            nullptr,
            nullptr,
            nullptr,
            def.isString ? GetDatabCB : nullptr,
            def.isString ? SetDatabCB : nullptr,
            &def.slot,
            &def.slot);
        if (!ref) {
            Log(LOG_ERR, std::string("Failed to register dataref: ") + def.name);
            continue;
        }
        def.ref = ref;
        def.owned = true;
        def.slot.text.clear();
        def.slot.number = 0.0;
        ++created;
    }
    if (created > 0) {
        Log(LOG_INFO, "Registered " + std::to_string(created) + " hoppiebridge datarefs.");
    }
}

void UnregisterHoppieDataRefs() {
    for (auto& def : g_ownedDataRefs) {
        if (!def.owned || !def.ref) {
            continue;
        }
        XPLMUnregisterDataAccessor(def.ref);
        def.ref = nullptr;
        def.owned = false;
    }
}

bool IsFastPollEnabled() {
    return g_dref.poll_frequency_fast && XPLMGetDatai(g_dref.poll_frequency_fast) != 0;
}

double NextPollIntervalSeconds(bool fastPoll) {
    int minSeconds = kPollDefaultMinSeconds;
    int maxSeconds = kPollDefaultMaxSeconds;
    if (fastPoll) {
        minSeconds = kPollFastMinSeconds;
        maxSeconds = kPollFastMaxSeconds;
    }
    if (maxSeconds < minSeconds) {
        std::swap(maxSeconds, minSeconds);
    }
    std::uniform_int_distribution<int> dist(minSeconds, maxSeconds);
    return static_cast<double>(dist(g_rng));
}

void ScheduleNextPollTime(double now) {
    bool fast = IsFastPollEnabled();
    double interval = NextPollIntervalSeconds(fast);
    g_nextPollTime = now + interval;
    Log(LOG_DBG, "Next poll in " + std::to_string(static_cast<int>(interval))
        + "s (" + (fast ? std::string("fast") : std::string("normal")) + ").");
}

void LogReadySummary() {
    if (g_loggedStartupSummary) {
        return;
    }
    g_loggedStartupSummary = true;
    std::string pollMode = IsFastPollEnabled() ? "fast" : "normal";
    std::string hbdrState = g_dref.hbdr_ready ? "found" : "missing";
    std::string hbdrWritable = "unknown";
    if (g_dref.hbdr_ready) {
        hbdrWritable = XPLMCanWriteDataRef(g_dref.hbdr_ready) ? "yes" : "no";
    }
    Log(LOG_INFO, "Helper ready. HBDR_ready " + hbdrState + " writable=" + hbdrWritable
        + ", poll_mode=" + pollMode + ".");
}

void LogCallsignState(const std::string& callsign, bool justSet) {
    if (justSet) {
        g_lastCallsign = callsign;
        return;
    }
    if (callsign == g_lastCallsign) {
        return;
    }
    if (!callsign.empty() && g_lastCallsign.empty()) {
        Log(LOG_INFO, "Callsign available: " + callsign);
    } else if (callsign.empty() && !g_lastCallsign.empty()) {
        Log(LOG_INFO, "Callsign cleared.");
    } else if (!callsign.empty() && !g_lastCallsign.empty()) {
        Log(LOG_INFO, "Callsign changed: " + callsign);
    }
    g_lastCallsign = callsign;
}

void LogCommReadyState(bool ready) {
    if (!g_commReadyKnown) {
        g_commReadyKnown = true;
        g_lastCommReady = ready;
        if (ready) {
            Log(LOG_INFO, "Comm ready.");
        }
        return;
    }
    if (ready == g_lastCommReady) {
        return;
    }
    g_lastCommReady = ready;
    Log(LOG_INFO, ready ? "Comm ready." : "Comm no longer ready.");
}

void LogPollModeChange() {
    bool fast = IsFastPollEnabled();
    if (!g_pollModeKnown) {
        g_pollModeKnown = true;
        g_lastFastPoll = fast;
        return;
    }
    if (fast == g_lastFastPoll) {
        return;
    }
    g_lastFastPoll = fast;
    Log(LOG_INFO, std::string("Poll mode changed: ") + (fast ? "fast" : "normal") + ".");
}

void LogLogonState(const std::string& logon) {
    bool available = !logon.empty();
    if (!g_logonKnown) {
        g_logonKnown = true;
        g_logonAvailable = available;
        if (available) {
            Log(LOG_INFO, "Logon available (len=" + std::to_string(logon.size()) + ").");
        }
        return;
    }
    if (available == g_logonAvailable) {
        return;
    }
    g_logonAvailable = available;
    if (available) {
        Log(LOG_INFO, "Logon available (len=" + std::to_string(logon.size()) + ").");
    } else {
        Log(LOG_INFO, "Logon cleared.");
    }
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

#ifndef _WIN32
size_t CurlWrite(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::string* out = static_cast<std::string*>(userp);
    out->append(static_cast<char*>(contents), total);
    return total;
}
#endif

bool PostForm(const std::string& postFields, std::string* response, std::string* error, long* httpCode) {
#ifdef _WIN32
    if (response) {
        response->clear();
    }
    if (httpCode) {
        *httpCode = 0;
    }

    std::wstring urlW = Utf8ToWide(kHoppieUrl);
    URL_COMPONENTS comps{};
    comps.dwStructSize = sizeof(comps);
    comps.dwSchemeLength = static_cast<DWORD>(-1);
    comps.dwHostNameLength = static_cast<DWORD>(-1);
    comps.dwUrlPathLength = static_cast<DWORD>(-1);
    comps.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(urlW.c_str(), 0, 0, &comps)) {
        if (error) { *error = WinHttpError(GetLastError()); }
        return false;
    }

    std::wstring host(comps.lpszHostName, comps.dwHostNameLength);
    std::wstring path;
    if (comps.dwUrlPathLength && comps.lpszUrlPath) {
        path.assign(comps.lpszUrlPath, comps.dwUrlPathLength);
    }
    if (comps.dwExtraInfoLength && comps.lpszExtraInfo) {
        path.append(comps.lpszExtraInfo, comps.dwExtraInfoLength);
    }
    bool secure = comps.nScheme == INTERNET_SCHEME_HTTPS;

    HINTERNET hSession = WinHttpOpen(L"YAL_hoppiehelper/1.0",
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        if (error) { *error = WinHttpError(GetLastError()); }
        return false;
    }
    WinHttpSetTimeouts(hSession, 10000, 10000, 15000, 15000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), comps.nPort, 0);
    if (!hConnect) {
        if (error) { *error = WinHttpError(GetLastError()); }
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
                                            nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        if (error) { *error = WinHttpError(GetLastError()); }
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    const wchar_t* headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
    BOOL ok = WinHttpSendRequest(hRequest, headers, -1L,
                                 (LPVOID)postFields.data(),
                                 static_cast<DWORD>(postFields.size()),
                                 static_cast<DWORD>(postFields.size()), 0);
    if (!ok) {
        if (error) { *error = WinHttpError(GetLastError()); }
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    ok = WinHttpReceiveResponse(hRequest, nullptr);
    if (!ok) {
        if (error) { *error = WinHttpError(GetLastError()); }
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX)) {
        if (httpCode) {
            *httpCode = static_cast<long>(status);
        }
    }

    std::string resp;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &available)) {
            break;
        }
        if (available == 0) {
            break;
        }
        std::string chunk;
        chunk.resize(available);
        DWORD read = 0;
        if (!WinHttpReadData(hRequest, &chunk[0], available, &read)) {
            break;
        }
        chunk.resize(read);
        resp += chunk;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (response) {
        *response = resp;
    }
    if (status != 200) {
        if (error) { *error = "HTTP " + std::to_string(status); }
        return false;
    }
    return true;
#else
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (error) { *error = "curl_easy_init failed"; }
        return false;
    }

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, kHoppieUrl);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postFields.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        if (error) { *error = curl_easy_strerror(res); }
        curl_easy_cleanup(curl);
        return false;
    }

    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(curl);
    if (httpCode) {
        *httpCode = code;
    }

    if (code != 200) {
        if (error) { *error = "HTTP " + std::to_string(code); }
        return false;
    }

    if (response) {
        *response = resp;
    }
    return true;
#endif
}

bool BuildPostFields(const HttpJob& job, std::string* out) {
    std::ostringstream ss;
    ss << "logon=" << UrlEncode(job.logon)
       << "&from=" << UrlEncode(job.from)
       << "&to=" << UrlEncode(job.to)
       << "&type=" << UrlEncode(job.msg_type);
    if (!job.packet.empty()) {
        ss << "&packet=" << UrlEncode(job.packet);
    }

    *out = ss.str();
    return true;
}

HttpResult RunHttpJob(const HttpJob& job) {
    HttpResult result;
    result.type = job.type;
    std::string post;
    if (!BuildPostFields(job, &post)) {
        result.ok = false;
        result.error = "Failed to build POST fields";
        return result;
    }

    std::string resp;
    std::string err;
    long httpCode = 0;
    bool ok = PostForm(post, &resp, &err, &httpCode);
    result.ok = ok;
    result.response = resp;
    result.error = err;
    result.httpCode = httpCode;
    return result;
}

bool ParseOkPayload(const std::string& raw, std::string* from, std::string* type, std::string* packet) {
    std::string s = Trim(raw);
    if (s.empty()) {
        return false;
    }
    if (s.rfind("ok", 0) != 0) {
        return false;
    }
    size_t brace = s.find('{');
    if (brace == std::string::npos) {
        return false;
    }
    size_t last = s.rfind('}');
    if (last == std::string::npos || last <= brace) {
        return false;
    }
    std::string inner = s.substr(brace + 1, last - brace - 1);
    std::istringstream iss(inner);
    std::string src;
    std::string msgType;
    if (!(iss >> src >> msgType)) {
        return false;
    }
    std::string rest;
    std::getline(iss, rest);
    rest = Trim(rest);
    std::string pkt;
    size_t open = rest.find('{');
    size_t close = rest.rfind('}');
    if (open != std::string::npos && close != std::string::npos && close > open) {
        pkt = rest.substr(open + 1, close - open - 1);
    } else {
        pkt = rest;
    }
    if (from) *from = src;
    if (type) *type = msgType;
    if (packet) *packet = Trim(pkt);
    return true;
}

bool IsPollOkOnly(const std::string& raw) {
    std::string s = Trim(raw);
    if (s.size() != 2) {
        return false;
    }
    return (s[0] == 'o' || s[0] == 'O') && (s[1] == 'k' || s[1] == 'K');
}

bool AreCommPrereqsReady(const std::string& logon, const std::string& callsign, bool* avionicsOkOut) {
    bool avionicsOk = true;
    if (g_dref.avionics_on) {
        avionicsOk = XPLMGetDatai(g_dref.avionics_on) != 0;
    }
    if (avionicsOkOut) {
        *avionicsOkOut = avionicsOk;
    }
    return avionicsOk && !logon.empty() && !callsign.empty();
}

void WorkerLoop() {
    while (g_running.load()) {
        HttpJob job;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait(lock, [] { return !g_jobs.empty() || !g_running.load(); });
            if (!g_running.load()) {
                return;
            }
            job = g_jobs.front();
            g_jobs.pop();
        }
        HttpResult result = RunHttpJob(job);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_results.push(result);
        }
    }
}

void EnqueueJob(const HttpJob& job) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_jobs.push(job);
    g_cv.notify_one();
}

void DrainResults() {
    std::queue<HttpResult> results;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::swap(results, g_results);
    }
    while (!results.empty()) {
        HttpResult res = results.front();
        results.pop();
        if (res.type == JobType::Send) {
            g_sendPending = false;
            if (res.ok) {
                SetDataRefString(g_dref.send_message_to, "");
                SetDataRefString(g_dref.send_message_type, "");
                SetDataRefString(g_dref.send_message_packet, "");
                SetDataRefString(g_dref.send_queue, "");
                if (g_dref.send_count) {
                    XPLMSetDatai(g_dref.send_count, XPLMGetDatai(g_dref.send_count) + 1);
                }
                SetLastError("");
                SetLastHttp("send: " + std::to_string(res.httpCode));
                Log(LOG_INFO, "Send ok: " + Trim(res.response));
            } else {
                SetLastError("Send failed: " + res.error);
                SetLastHttp("send: " + std::to_string(res.httpCode));
            }
        } else if (res.type == JobType::Poll) {
            g_pollPending = false;
            double now = XPLMGetElapsedTime();
            if (res.ok) {
                if (IsPollOkOnly(res.response)) {
                    if (!g_commEstablished) {
                        Log(LOG_INFO, "Poll ok. Communication ready.");
                    }
                    g_commEstablished = true;
                }
                std::string from;
                std::string type;
                std::string packet;
                if (ParseOkPayload(res.response, &from, &type, &packet)) {
                    SetDataRefString(g_dref.poll_message_origin, "poll");
                    SetDataRefString(g_dref.poll_message_from, from);
                    SetDataRefString(g_dref.poll_message_type, type);
                    SetDataRefString(g_dref.poll_message_packet, packet);
                    SetDataRefString(g_dref.poll_queue, std::string("{\"poll\":\"") + JsonEscape(res.response) + "\"}");
                    SetDataRefBool(g_dref.poll_queue_clear, false);
                    if (g_dref.poll_count) {
                        XPLMSetDatai(g_dref.poll_count, XPLMGetDatai(g_dref.poll_count) + 1);
                    }
                    Log(LOG_INFO, "Poll message from " + from + " type " + type);
                }
                SetLastError("");
                SetLastHttp("poll: " + std::to_string(res.httpCode));
                ScheduleNextPollTime(now);
            } else {
                SetLastError("Poll failed: " + res.error);
                SetLastHttp("poll: " + std::to_string(res.httpCode));
                ScheduleNextPollTime(now);
            }
        }
    }
}

void ClearInboxIfRequested() {
    if (!g_dref.poll_queue_clear) {
        return;
    }
    int clear = XPLMGetDatai(g_dref.poll_queue_clear);
    if (clear == 0) {
        return;
    }
    SetDataRefString(g_dref.poll_message_origin, "");
    SetDataRefString(g_dref.poll_message_from, "");
    SetDataRefString(g_dref.poll_message_type, "");
    SetDataRefString(g_dref.poll_message_packet, "");
    SetDataRefString(g_dref.poll_queue, "");
    SetDataRefBool(g_dref.poll_queue_clear, false);
}

bool UpdateCallsign() {
    std::string pending = Trim(GetDataRefString(g_dref.send_callsign));
    if (!pending.empty()) {
        SetDataRefString(g_dref.callsign, pending);
        SetDataRefString(g_dref.send_callsign, "");
        Log(LOG_INFO, "Callsign set: " + pending);
        return true;
    }
    return false;
}

void UpdateCommReady(const std::string& logon, const std::string& callsign) {
    bool avionicsOk = true;
    bool prereqsOk = AreCommPrereqsReady(logon, callsign, &avionicsOk);
    if (!prereqsOk) {
        g_commEstablished = false;
    }
    bool ready = prereqsOk && g_commEstablished;
    SetDataRefBool(g_dref.comm_ready, ready);

    if (!ready) {
        if (!avionicsOk) {
            SetStatus("AVIONICS_OFF");
        } else if (logon.empty()) {
            SetStatus("WAIT_LOGON");
        } else if (callsign.empty()) {
            SetStatus("WAIT_CALLSIGN");
        } else if (!g_commEstablished) {
            SetStatus("WAIT_POLL");
        } else {
            SetStatus("NOT_READY");
        }
    } else {
        SetStatus("READY");
    }
}

float FlightLoopCallback(float, float, int, void*) {
    g_debugLevel = GetDebugLevel();

    double now = XPLMGetElapsedTime();
    RefreshDataRefs(now);
    if (!g_refsReady) {
        return kFlightLoopInterval;
    }

    DrainResults();
    ClearInboxIfRequested();

    std::string logon = Trim(GetDataRefString(g_dref.logon));
    std::string callsign = Trim(GetDataRefString(g_dref.callsign));

    LogLogonState(logon);
    bool callsignUpdated = UpdateCallsign();
    callsign = Trim(GetDataRefString(g_dref.callsign));
    LogCallsignState(callsign, callsignUpdated);

    UpdateCommReady(logon, callsign);
    bool commReady = GetDataRefBool(g_dref.comm_ready);
    LogCommReadyState(commReady);
    LogPollModeChange();
    bool prereqsOk = AreCommPrereqsReady(logon, callsign, nullptr);

    if (commReady && !g_sendPending) {
        std::string to = Trim(GetDataRefString(g_dref.send_message_to));
        std::string type = Trim(GetDataRefString(g_dref.send_message_type));
        std::string packet = Trim(GetDataRefString(g_dref.send_message_packet));
        if (!to.empty() && !type.empty() && !packet.empty()) {
            HttpJob job;
            job.type = JobType::Send;
            job.logon = logon;
            job.from = callsign;
            job.to = to;
            job.msg_type = type;
            job.packet = packet;
            EnqueueJob(job);
            g_sendPending = true;
        }
    }

    if (prereqsOk && !g_pollPending && (!commReady || now >= g_nextPollTime)) {
        std::string existing = Trim(GetDataRefString(g_dref.poll_message_packet));
        if (existing.empty()) {
            HttpJob job;
            job.type = JobType::Poll;
            job.logon = logon;
            job.from = callsign;
            job.to = callsign;
            job.msg_type = "poll";
            EnqueueJob(job);
            g_pollPending = true;
        }
    }

    return kFlightLoopInterval;
}

void FindDataRefs() {
    g_dref.send_queue = XPLMFindDataRef("hoppiebridge/send_queue");
    g_dref.send_message_to = XPLMFindDataRef("hoppiebridge/send_message_to");
    g_dref.send_message_type = XPLMFindDataRef("hoppiebridge/send_message_type");
    g_dref.send_message_packet = XPLMFindDataRef("hoppiebridge/send_message_packet");
    g_dref.callsign = XPLMFindDataRef("hoppiebridge/callsign");
    g_dref.send_callsign = XPLMFindDataRef("hoppiebridge/send_callsign");
    g_dref.poll_frequency_fast = XPLMFindDataRef("hoppiebridge/poll_frequency_fast");
    g_dref.poll_queue = XPLMFindDataRef("hoppiebridge/poll_queue");
    g_dref.poll_message_origin = XPLMFindDataRef("hoppiebridge/poll_message_origin");
    g_dref.poll_message_from = XPLMFindDataRef("hoppiebridge/poll_message_from");
    g_dref.poll_message_type = XPLMFindDataRef("hoppiebridge/poll_message_type");
    g_dref.poll_message_packet = XPLMFindDataRef("hoppiebridge/poll_message_packet");
    g_dref.poll_queue_clear = XPLMFindDataRef("hoppiebridge/poll_queue_clear");
    g_dref.comm_ready = XPLMFindDataRef("hoppiebridge/comm_ready");
    g_dref.logon = XPLMFindDataRef("YAL/hoppie/logon");
    g_dref.debug_level = XPLMFindDataRef("YAL/hoppie/debug_level");
    g_dref.status = XPLMFindDataRef("YAL/hoppie/status");
    g_dref.last_error = XPLMFindDataRef("YAL/hoppie/last_error");
    g_dref.last_http = XPLMFindDataRef("YAL/hoppie/last_http");
    g_dref.send_count = XPLMFindDataRef("YAL/hoppie/send_count");
    g_dref.poll_count = XPLMFindDataRef("YAL/hoppie/poll_count");
    g_dref.hbdr_ready = XPLMFindDataRef("laminar/B738/HBDR_ready");
    g_dref.avionics_on = XPLMFindDataRef("sim/cockpit/electrical/avionics_on");

    if (!g_dref.logon) {
        Log(LOG_DBG, "Missing dataref: YAL/hoppie/logon");
    }
}

bool HasCoreDataRefs() {
    return g_dref.logon
        && g_dref.comm_ready
        && g_dref.send_message_to
        && g_dref.send_message_type
        && g_dref.send_message_packet
        && g_dref.poll_message_packet
        && g_dref.poll_queue_clear
        && g_dref.callsign
        && g_dref.send_callsign;
}

void RefreshHbdrReady() {
    if (!g_dref.hbdr_ready) {
        g_dref.hbdr_ready = XPLMFindDataRef("laminar/B738/HBDR_ready");
        if (g_dref.hbdr_ready && !g_loggedHbdrFound) {
            g_loggedHbdrFound = true;
            Log(LOG_INFO, "Found HBDR_ready dataref.");
        }
    }
    if (!g_dref.hbdr_ready) {
        return;
    }
    if (!g_loggedHbdrTypes) {
        g_loggedHbdrTypes = true;
        Log(LOG_INFO, "HBDR_ready types: " + DataRefTypeString(XPLMGetDataRefTypes(g_dref.hbdr_ready)));
    }
    if (!g_loggedHbdrWritable && !XPLMCanWriteDataRef(g_dref.hbdr_ready)) {
        g_loggedHbdrWritable = true;
        Log(LOG_ERR, "HBDR_ready dataref is not writable.");
    }
    SetDataRefBool(g_dref.hbdr_ready, true);
}

void RefreshDataRefs(double now) {
    RefreshHbdrReady();
    if (g_refsReady) {
        if (!HasCoreDataRefs()) {
            g_refsReady = false;
            g_nextRefScanTime = now;
            g_commEstablished = false;
            SetDataRefBool(g_dref.comm_ready, false);
            g_loggedStartupSummary = false;
            g_commReadyKnown = false;
            g_lastCommReady = false;
            g_pollModeKnown = false;
            g_lastFastPoll = false;
            g_logonKnown = false;
            g_logonAvailable = false;
            g_lastCallsign.clear();
            Log(LOG_ERR, "Lost required datarefs. Will retry.");
        }
        return;
    }

    if (now < g_nextRefScanTime) {
        return;
    }

    EnsureHoppieDataRefs();
    FindDataRefs();
    g_refsReady = HasCoreDataRefs();
    g_nextRefScanTime = now + 2.0;

    if (g_refsReady) {
        ScheduleNextPollTime(now);
        g_loggedMissingRefs = false;
        Log(LOG_INFO, "Required datarefs found. Helper ready.");
        LogReadySummary();
    } else if (!g_loggedMissingRefs) {
        g_loggedMissingRefs = true;
        Log(LOG_INFO, "Waiting for YAL datarefs...");
    }
}

}  // namespace

PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    std::strncpy(outName, kPluginName, 255);
    std::strncpy(outSig, kPluginSig, 255);
    std::strncpy(outDesc, kPluginDesc, 255);
    XPLMDebugString("[YAL HoppieHelper] Starting plugin\n");

#ifndef _WIN32
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
    EnsureHoppieDataRefs();
    FindDataRefs();
    return 1;
}

PLUGIN_API void XPluginStop() {
    XPluginDisable();
    UnregisterHoppieDataRefs();
#ifndef _WIN32
    curl_global_cleanup();
#endif
}

PLUGIN_API int XPluginEnable() {
    g_running.store(true);
    g_worker = std::thread(WorkerLoop);
    EnsureHoppieDataRefs();
    FindDataRefs();
    ScheduleNextPollTime(XPLMGetElapsedTime());
    g_refsReady = HasCoreDataRefs();
    g_loggedMissingRefs = false;
    g_nextRefScanTime = 0.0;
    g_loggedStartupSummary = false;
    g_commReadyKnown = false;
    g_lastCommReady = false;
    g_pollModeKnown = false;
    g_lastFastPoll = false;
    g_logonKnown = false;
    g_logonAvailable = false;
    g_lastCallsign.clear();
    if (!g_refsReady) {
        g_loggedMissingRefs = true;
        Log(LOG_INFO, "Waiting for YAL datarefs...");
    } else {
        Log(LOG_INFO, "Required datarefs found. Helper ready.");
        LogReadySummary();
    }
    RefreshHbdrReady();
    XPLMRegisterFlightLoopCallback(FlightLoopCallback, kFlightLoopInterval, nullptr);
    g_commEstablished = false;
    SetDataRefBool(g_dref.comm_ready, false);
    SetDataRefString(g_dref.last_error, "");
    SetDataRefString(g_dref.last_http, "");
    if (g_dref.send_count) {
        XPLMSetDatai(g_dref.send_count, 0);
    }
    if (g_dref.poll_count) {
        XPLMSetDatai(g_dref.poll_count, 0);
    }
    SetStatus("INIT");
    Log(LOG_INFO, std::string("Enabled v") + kPluginVersion);
    return 1;
}

PLUGIN_API void XPluginDisable() {
    XPLMUnregisterFlightLoopCallback(FlightLoopCallback, nullptr);
    g_running.store(false);
    g_cv.notify_all();
    if (g_worker.joinable()) {
        g_worker.join();
    }
    g_commEstablished = false;
    SetDataRefBool(g_dref.comm_ready, false);
    SetDataRefBool(g_dref.hbdr_ready, false);
    g_loggedHbdrFound = false;
    g_loggedHbdrWritable = false;
    g_loggedHbdrTypes = false;
    g_loggedStartupSummary = false;
    g_commReadyKnown = false;
    g_lastCommReady = false;
    g_pollModeKnown = false;
    g_lastFastPoll = false;
    g_logonKnown = false;
    g_logonAvailable = false;
    g_lastCallsign.clear();
    SetStatus("DISABLED");
    Log(LOG_INFO, "Disabled");
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int, void*) {}
