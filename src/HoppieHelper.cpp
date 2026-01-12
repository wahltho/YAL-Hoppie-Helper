#include "XPLMDataAccess.h"
#include "XPLMPlugin.h"
#include "XPLMProcessing.h"
#include "XPLMUtilities.h"

#include <curl/curl.h>

#include <atomic>
#include <cstring>
#include <condition_variable>
#include <cctype>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>

namespace {

constexpr const char* kPluginName = "YAL_hoppiehelper";
constexpr const char* kPluginSig = "yal.hoppiehelper";
constexpr const char* kPluginDesc = "HTTP helper for Hoppie ACARS (CPDLC)";
constexpr const char* kHoppieUrl = "https://www.hoppie.nl/acars/system/connect.html";

constexpr float kFlightLoopInterval = 1.0f;
constexpr double kPollIntervalSeconds = 65.0;
constexpr double kPollIntervalShortSeconds = 20.0;

struct DataRefs {
    XPLMDataRef send_queue = nullptr;
    XPLMDataRef send_message_to = nullptr;
    XPLMDataRef send_message_type = nullptr;
    XPLMDataRef send_message_packet = nullptr;
    XPLMDataRef callsign = nullptr;
    XPLMDataRef send_callsign = nullptr;
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

bool g_sendPending = false;
bool g_pollPending = false;
double g_nextPollTime = 0.0;
int g_debugLevel = 1;
std::string g_lastStatus;

enum LogLevel { LOG_ERR = 1, LOG_INFO = 2, LOG_DBG = 3 };

std::string GetDataRefString(XPLMDataRef dr);
void SetDataRefString(XPLMDataRef dr, const std::string& value);
bool GetDataRefBool(XPLMDataRef dr);
void SetDataRefBool(XPLMDataRef dr, bool value);

void Log(LogLevel level, const std::string& msg) {
    if (g_debugLevel < level) {
        return;
    }
    std::string line = std::string("[HoppieHelper] ") + msg + "\n";
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

std::string ToUpperAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string GetDataRefString(XPLMDataRef dr) {
    if (!dr) {
        return "";
    }
    char buffer[2048];
    int len = XPLMGetDatab(dr, buffer, 0, static_cast<int>(sizeof(buffer)));
    if (len <= 0) {
        return "";
    }
    if (len > static_cast<int>(sizeof(buffer))) {
        len = static_cast<int>(sizeof(buffer));
    }
    return std::string(buffer, buffer + len);
}

void SetDataRefString(XPLMDataRef dr, const std::string& value) {
    if (!dr) {
        return;
    }
    if (value.empty()) {
        XPLMSetDatab(dr, "", 0, 0);
        return;
    }
    XPLMSetDatab(dr, value.c_str(), 0, static_cast<int>(value.size()));
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
    XPLMSetDatai(dr, value ? 1 : 0);
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

size_t CurlWrite(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::string* out = static_cast<std::string*>(userp);
    out->append(static_cast<char*>(contents), total);
    return total;
}

bool PostForm(const std::string& postFields, std::string* response, std::string* error, long* httpCode) {
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
}

bool BuildPostFields(const HttpJob& job, std::string* out) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return false;
    }

    auto esc = [&](const std::string& in) -> std::string {
        char* e = curl_easy_escape(curl, in.c_str(), static_cast<int>(in.size()));
        if (!e) return "";
        std::string outStr(e);
        curl_free(e);
        return outStr;
    };

    std::ostringstream ss;
    ss << "logon=" << esc(job.logon)
       << "&from=" << esc(job.from)
       << "&to=" << esc(job.to)
       << "&type=" << esc(job.msg_type);
    if (!job.packet.empty()) {
        ss << "&packet=" << esc(job.packet);
    }

    curl_easy_cleanup(curl);
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
                g_nextPollTime = now + kPollIntervalSeconds;
            } else {
                SetLastError("Poll failed: " + res.error);
                SetLastHttp("poll: " + std::to_string(res.httpCode));
                g_nextPollTime = now + kPollIntervalShortSeconds;
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

void UpdateCallsign() {
    std::string pending = Trim(GetDataRefString(g_dref.send_callsign));
    if (!pending.empty()) {
        pending = ToUpperAscii(pending);
        SetDataRefString(g_dref.callsign, pending);
        SetDataRefString(g_dref.send_callsign, "");
        Log(LOG_INFO, "Callsign set: " + pending);
    }
}

bool IsCommReady(const std::string& logon, const std::string& callsign) {
    bool avionicsOk = true;
    if (g_dref.avionics_on) {
        avionicsOk = XPLMGetDatai(g_dref.avionics_on) != 0;
    }
    return avionicsOk && !logon.empty() && !callsign.empty();
}

void UpdateCommReady(const std::string& logon, const std::string& callsign) {
    bool avionicsOk = true;
    if (g_dref.avionics_on) {
        avionicsOk = XPLMGetDatai(g_dref.avionics_on) != 0;
    }
    bool ready = avionicsOk && !logon.empty() && !callsign.empty();
    SetDataRefBool(g_dref.comm_ready, ready);
    if (g_dref.hbdr_ready) {
        SetDataRefBool(g_dref.hbdr_ready, ready);
    }

    if (!ready) {
        if (!avionicsOk) {
            SetStatus("AVIONICS_OFF");
        } else if (logon.empty()) {
            SetStatus("WAIT_LOGON");
        } else if (callsign.empty()) {
            SetStatus("WAIT_CALLSIGN");
        } else {
            SetStatus("NOT_READY");
        }
    } else {
        SetStatus("READY");
    }
}

float FlightLoopCallback(float, float, int, void*) {
    g_debugLevel = GetDebugLevel();

    DrainResults();
    ClearInboxIfRequested();

    std::string logon = Trim(GetDataRefString(g_dref.logon));
    std::string callsign = Trim(GetDataRefString(g_dref.callsign));

    UpdateCallsign();
    callsign = Trim(GetDataRefString(g_dref.callsign));

    UpdateCommReady(logon, callsign);
    bool commReady = GetDataRefBool(g_dref.comm_ready);

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

    double now = XPLMGetElapsedTime();
    if (commReady && !g_pollPending && now >= g_nextPollTime) {
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
        Log(LOG_ERR, "Missing dataref: YAL/hoppie/logon");
    }
}

}  // namespace

PLUGIN_API int XPluginStart(char* outName, char* outSig, char* outDesc) {
    std::strncpy(outName, kPluginName, 255);
    std::strncpy(outSig, kPluginSig, 255);
    std::strncpy(outDesc, kPluginDesc, 255);

    curl_global_init(CURL_GLOBAL_DEFAULT);
    FindDataRefs();
    return 1;
}

PLUGIN_API void XPluginStop() {
    XPluginDisable();
    curl_global_cleanup();
}

PLUGIN_API int XPluginEnable() {
    g_running.store(true);
    g_worker = std::thread(WorkerLoop);
    g_nextPollTime = XPLMGetElapsedTime() + kPollIntervalSeconds;
    XPLMRegisterFlightLoopCallback(FlightLoopCallback, kFlightLoopInterval, nullptr);
    SetDataRefString(g_dref.last_error, "");
    SetDataRefString(g_dref.last_http, "");
    if (g_dref.send_count) {
        XPLMSetDatai(g_dref.send_count, 0);
    }
    if (g_dref.poll_count) {
        XPLMSetDatai(g_dref.poll_count, 0);
    }
    SetStatus("INIT");
    Log(LOG_INFO, "Enabled");
    return 1;
}

PLUGIN_API void XPluginDisable() {
    XPLMUnregisterFlightLoopCallback(FlightLoopCallback, nullptr);
    g_running.store(false);
    g_cv.notify_all();
    if (g_worker.joinable()) {
        g_worker.join();
    }
    SetDataRefBool(g_dref.comm_ready, false);
    SetDataRefBool(g_dref.hbdr_ready, false);
    SetStatus("DISABLED");
    Log(LOG_INFO, "Disabled");
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int, void*) {}
