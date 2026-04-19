#include "XPLMDataAccess.h"
#include "XPLMDisplay.h"
#include "XPLMGraphics.h"
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
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <random>
#include <regex>
#include <sstream>
#include <unordered_set>
#include <system_error>
#include <string>
#include <thread>
#include <vector>

PLUGIN_API void XPluginDisable(void);

namespace {

constexpr const char* kPluginName = "YAL_hoppiehelper";
constexpr const char* kPluginSig = "yal.hoppiehelper";
constexpr const char* kPluginVersion = "2.0";
constexpr const char* kPluginDesc = "HTTP helper for Hoppie ACARS (CPDLC) v2.0";
constexpr const char* kHoppieUrl = "https://www.hoppie.nl/acars/system/connect.html";
constexpr const char* kZiboPluginSig = "zibomod.by.Zibo";

constexpr float kFlightLoopInterval = 1.0f;
constexpr int kPollDefaultMinSeconds = 45;
constexpr int kPollDefaultMaxSeconds = 75;
constexpr int kPollFastMinSeconds = 12;
constexpr int kPollFastMaxSeconds = 18;
constexpr size_t kUpdateLogMaxEntries = 200;
constexpr float kUpdatePopupAutoHideSeconds = 15.0f;
constexpr float kUpdateConfirmTimeoutSeconds = 30.0f;
constexpr int kUpdatePopupMaxLines = 12;
constexpr int kUpdatePopupPadding = 10;
constexpr int kUpdatePopupMargin = 30;
constexpr int kUpdateButtonWidth = 90;
constexpr int kUpdateButtonHeight = 22;
constexpr int kUpdateButtonGap = 10;

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
    XPLMDataRef tailnum = nullptr;
    XPLMDataRef reload_request = nullptr;
    XPLMDataRef voice_seq = nullptr;
    XPLMDataRef voice_text = nullptr;
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

std::array<OwnedDataRef, 16> g_ownedDataRefs = {{
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
    {"YAL/hoppie/voice_seq", xplmType_Int, false},
    {"YAL/hoppie/voice_text", xplmType_Data, true},
}};

DataRefs g_dref;
std::atomic<bool> g_running{false};
std::atomic<bool> g_hoppiePassiveMode{false};
std::atomic<bool> g_workerActive{false};
std::thread g_worker;
XPLMCommandRef g_reloadCmd = nullptr;
bool g_loggedReloadCmdMissing = false;
bool g_loggedReloadDatarefMissing = false;

enum class JobType { Send, Poll, Update, UpdateScan };

struct UpdateCounters {
    size_t copied = 0;
    size_t skipped = 0;
    size_t dirs = 0;
    size_t deleted = 0;
    size_t deleteFailed = 0;
    size_t failed = 0;
    size_t dir_ts = 0;
};

struct HttpJob {
    JobType type;
    std::string logon;
    std::string from;
    std::string to;
    std::string msg_type;
    std::string packet;
    std::string update_source;
    std::string update_target;
    std::vector<std::string> update_excludes;
    bool update_log_details = false;
    size_t update_log_limit = 0;
};

struct LegacyOutboxMessage {
    std::string to;
    std::string type;
    std::string packet;
};

struct HttpResult {
    JobType type;
    bool ok = false;
    std::string response;
    std::string error;
    long httpCode = 0;
    std::vector<std::string> update_log_lines;
    size_t update_log_omitted = 0;
    UpdateCounters update_counts;
};

struct InboundMessage {
    std::string origin;
    std::string raw;
    std::string from;
    std::string type;
    std::string packet;
};

std::mutex g_mutex;
std::condition_variable g_cv;
std::queue<HttpJob> g_jobs;
std::queue<HttpResult> g_results;
std::mutex g_ownedMutex;
std::mt19937 g_rng{std::random_device{}()};
std::deque<InboundMessage> g_pendingInbox;
std::string g_lastBadSendQueue;

bool g_sendPending = false;
bool g_pollPending = false;
double g_nextPollTime = 0.0;
int g_debugLevel = 1;
std::string g_debugLevelSource;
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
bool g_ziboKnown = false;
bool g_lastZiboReady = false;
bool g_ziboPluginKnown = false;
bool g_lastZiboPluginPresent = false;
bool g_tailnumKnown = false;
bool g_lastTailnumMatches = false;
std::string g_lastTailnum;

enum class UpdatePopupMode { None, Info, Confirm, Error };

struct UiRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

XPLMWindowID g_updateWindow = nullptr;
bool g_updatePopupVisible = false;
double g_updatePopupHideAt = 0.0;
std::vector<std::string> g_updatePopupLines;
UpdatePopupMode g_updatePopupMode = UpdatePopupMode::None;
bool g_updateButtonsValid = false;
UiRect g_updateButtonOk;
UiRect g_updateButtonAbort;
int g_updatePopupButtonArea = 0;
bool g_updateDecisionPending = false;
double g_updateDecisionDeadline = 0.0;
bool g_updateErrorPending = false;
bool g_reloadAfterErrorAck = false;
std::string g_updateErrorReloadReason;
bool g_updateScanPending = false;
bool g_reloadRequested = false;
bool g_updateShowResultPopup = false;
std::string g_updatePlanTarget;
bool g_updatePlanValid = false;

struct AutarkConfig {
    bool enabled = false;
    bool hasLogon = false;
    std::string logon;
    bool hasDebug = false;
    int debugLevel = 1;
    bool hasPollFast = false;
    bool pollFast = false;
    bool hasCallsign = false;
    std::string callsign;
};

struct AutarkUpdate {
    bool modeChanged = false;
    bool logonChanged = false;
    bool debugChanged = false;
    bool pollFastChanged = false;
    bool callsignChanged = false;
    bool updateChanged = false;
};

bool g_autarkMode = false;
AutarkConfig g_autarkConfig;
std::string g_autarkAppliedCallsign;
double g_nextPrefScanTime = 0.0;
double g_nextZiboHoppiePrefScanTime = 0.0;

struct ZiboHoppieConfig {
    bool filePresent = false;
    bool enabled = false;
    std::string logon;
};

struct UpdateConfig {
    bool enabled = false;
    std::string source;
    std::vector<std::string> excludes;
};

struct UpdateUiConfig {
    bool hasWindowPos = false;
    int windowLeft = 0;
    int windowTop = 0;
};

UpdateConfig g_updateConfig;
UpdateConfig g_updatePlanConfig;
UpdateUiConfig g_updateUiConfig;
bool g_updatePending = false;
bool g_reloadAfterUpdate = false;
double g_reloadAfterUpdateAt = 0.0;

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
std::string Trim(const std::string& s);
std::string ToLower(const std::string& s);
std::string StripQuotes(const std::string& s);
bool ParseBool(const std::string& s, bool* out);
std::string GetAutarkPrefPath();
std::string GetZiboHoppiePrefPath();
bool LoadZiboHoppieConfig(ZiboHoppieConfig* config);
std::string BuildUpdatePathError(const char* label, const std::filesystem::path& path, const std::error_code& ec);
bool HasCoreDataRefs();
void RefreshDataRefs(double now);
void FindDataRefs();
void RefreshHbdrReady(bool allowReady);
void EnsureHoppieDataRefs();
void UnregisterHoppieDataRefs();
AutarkUpdate UpdateAutarkConfig(double now);
bool UpdateZiboHoppiePassiveMode(double now);
bool TriggerReload(const char* reason);
std::string GetActiveLogon();
bool ApplyAutarkCallsign(std::string* callsign);
void ApplyAutarkPollFastOverride();
double NextPollIntervalSeconds(bool fastPoll);
void ScheduleNextPollTime(double now);
void LogReadySummary();
void LogCallsignState(const std::string& callsign, bool justSet);
void LogCommReadyState(bool ready);
void LogPollModeChange();
void LogLogonState(const std::string& logon);
bool IsZiboPluginLoaded();
bool IsZiboTailnum(const std::string& tailnum);
bool UpdateZiboState();
bool IsHoppiePassiveMode();
void ResetHoppieRuntimeState(double now, bool clearBridgeOutputs);
bool InboxHasMessage();
bool BuildInboundMessage(const std::string& origin, const std::string& raw, InboundMessage* out);
bool BuildInboundMessages(const std::string& origin, const std::string& raw, std::vector<InboundMessage>* out);
void ApplyInboxMessage(const InboundMessage& msg);
bool ParseLegacyOutboxMessage(const std::string& raw, LegacyOutboxMessage* out);
void QueueInboundMessage(const std::string& origin, const std::string& raw);
void DeliverQueuedInboxIfEmpty();
void DropQueuedHoppieJobs();
void EnsureWorkerThread();
void EnqueueJob(const HttpJob& job);
bool IsExcludedRelPath(const std::string& rel, const std::vector<std::string>& excludes);
bool PerformUpdateJob(const HttpJob& job,
    bool dryRun,
    std::string* summary,
    std::string* error,
    std::vector<std::string>* logLines,
    size_t* logOmitted,
    UpdateCounters* counters);
void EnsureUpdateWindow();
void ShowUpdatePopup(const std::vector<std::string>& lines);
void ShowUpdateConfirm(const std::vector<std::string>& lines);
void ShowUpdateError(const std::vector<std::string>& lines, const char* reloadReason);
void UpdatePopupButtonRectsFromWindow(int left, int top, int right, int bottom);
void UpdatePopupVisibility(double now);
void HideUpdatePopup();
void SaveUpdateWindowPosition();
bool WriteUpdateWindowPos(int left, int top);
void HandleUpdateDecision(bool accept, const char* reason);
void HandleUpdateErrorAck(const char* reason);
bool StartUpdateFromPlan(const char* reason);

void Log(LogLevel level, const std::string& msg) {
    if (g_debugLevel < level) {
        return;
    }
    std::string line = std::string("[YAL HoppieHelper] ") + msg + "\n";
    XPLMDebugString(line.c_str());
}

void LogAlways(const std::string& msg) {
    std::string line = std::string("[YAL HoppieHelper] ") + msg + "\n";
    XPLMDebugString(line.c_str());
}

void LogWire(const std::string& msg) {
    Log(LOG_DBG, msg);
}

bool PointInRect(int x, int y, const UiRect& rect) {
    return x >= rect.left && x <= rect.right && y >= rect.bottom && y <= rect.top;
}

int UpdateWindowHandleMouseClick(XPLMWindowID, int x, int y, XPLMMouseStatus status, void*) {
    if (status != xplm_MouseDown) {
        return 0;
    }
    if (g_updatePopupMode == UpdatePopupMode::Confirm || g_updatePopupMode == UpdatePopupMode::Error) {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        XPLMGetWindowGeometry(g_updateWindow, &left, &top, &right, &bottom);
        UpdatePopupButtonRectsFromWindow(left, top, right, bottom);
    }
    if (g_updatePopupMode == UpdatePopupMode::Confirm && g_updateDecisionPending && g_updateButtonsValid) {
        if (PointInRect(x, y, g_updateButtonOk)) {
            HandleUpdateDecision(true, "ok");
            return 1;
        }
        if (PointInRect(x, y, g_updateButtonAbort)) {
            HandleUpdateDecision(false, "abort");
            return 1;
        }
    }
    if (g_updatePopupMode == UpdatePopupMode::Error && g_updateErrorPending && g_updateButtonsValid) {
        if (PointInRect(x, y, g_updateButtonOk)) {
            HandleUpdateErrorAck("ok");
            return 1;
        }
    }
    return 0;
}

int UpdateWindowHandleRightClick(XPLMWindowID, int, int, XPLMMouseStatus, void*) {
    return 0;
}

int UpdateWindowHandleMouseWheel(XPLMWindowID, int, int, int, int, void*) {
    return 0;
}

XPLMCursorStatus UpdateWindowHandleCursor(XPLMWindowID, int, int, void*) {
    return xplm_CursorDefault;
}

void UpdateWindowHandleKey(XPLMWindowID, char, XPLMKeyFlags, char, void*, int) {
}

void UpdatePopupButtonRectsFromWindow(int left, int top, int right, int bottom) {
    if (g_updatePopupMode == UpdatePopupMode::Confirm) {
        int buttonBottom = bottom + kUpdatePopupPadding;
        int buttonTop = buttonBottom + kUpdateButtonHeight;
        int okRight = right - kUpdatePopupPadding;
        int okLeft = okRight - kUpdateButtonWidth;
        int abortRight = okLeft - kUpdateButtonGap;
        int abortLeft = abortRight - kUpdateButtonWidth;
        g_updateButtonOk = {okLeft, buttonTop, okRight, buttonBottom};
        g_updateButtonAbort = {abortLeft, buttonTop, abortRight, buttonBottom};
        g_updateButtonsValid = true;
    } else if (g_updatePopupMode == UpdatePopupMode::Error) {
        int buttonBottom = bottom + kUpdatePopupPadding;
        int buttonTop = buttonBottom + kUpdateButtonHeight;
        int width = right - left;
        int okLeft = left + (width - kUpdateButtonWidth) / 2;
        int okRight = okLeft + kUpdateButtonWidth;
        g_updateButtonOk = {okLeft, buttonTop, okRight, buttonBottom};
        g_updateButtonsValid = true;
    }
}

void UpdateWindowDraw(XPLMWindowID windowId, void*) {
    if (!g_updatePopupVisible || g_updatePopupLines.empty()) {
        return;
    }
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    XPLMGetWindowGeometry(windowId, &left, &top, &right, &bottom);
    if (g_updatePopupMode == UpdatePopupMode::Confirm || g_updatePopupMode == UpdatePopupMode::Error) {
        UpdatePopupButtonRectsFromWindow(left, top, right, bottom);
    }
    XPLMDrawTranslucentDarkBox(left, top, right, bottom);

    int charWidth = 0;
    int charHeight = 0;
    int digitsOnly = 0;
    XPLMGetFontDimensions(xplmFont_Basic, &charWidth, &charHeight, &digitsOnly);
    int lineHeight = charHeight + 2;
    int x = left + kUpdatePopupPadding;
    int y = top - kUpdatePopupPadding - charHeight;
    float color[] = {1.0f, 1.0f, 1.0f};
    int bottomLimit = bottom + kUpdatePopupPadding;
    if (g_updatePopupMode == UpdatePopupMode::Confirm || g_updatePopupMode == UpdatePopupMode::Error) {
        bottomLimit += g_updatePopupButtonArea;
    }
    for (const auto& line : g_updatePopupLines) {
        XPLMDrawString(color, x, y, const_cast<char*>(line.c_str()), nullptr, xplmFont_Basic);
        y -= lineHeight;
        if (y < bottomLimit) {
            break;
        }
    }

    if (g_updateButtonsValid) {
        if (g_updatePopupMode == UpdatePopupMode::Confirm) {
            XPLMDrawTranslucentDarkBox(
                g_updateButtonOk.left, g_updateButtonOk.top, g_updateButtonOk.right, g_updateButtonOk.bottom);
            XPLMDrawTranslucentDarkBox(
                g_updateButtonAbort.left, g_updateButtonAbort.top, g_updateButtonAbort.right, g_updateButtonAbort.bottom);

            const char* okText = "OK";
            const char* abortText = "ABORT";
            int okTextWidth = static_cast<int>(std::strlen(okText)) * charWidth;
            int abortTextWidth = static_cast<int>(std::strlen(abortText)) * charWidth;
            int okX = g_updateButtonOk.left + (g_updateButtonOk.right - g_updateButtonOk.left - okTextWidth) / 2;
            int abortX =
                g_updateButtonAbort.left + (g_updateButtonAbort.right - g_updateButtonAbort.left - abortTextWidth) / 2;
            int textY = g_updateButtonOk.bottom + (kUpdateButtonHeight - charHeight) / 2;
            float buttonColor[] = {0.9f, 0.9f, 0.9f};
            XPLMDrawString(buttonColor, okX, textY, const_cast<char*>(okText), nullptr, xplmFont_Basic);
            XPLMDrawString(buttonColor, abortX, textY, const_cast<char*>(abortText), nullptr, xplmFont_Basic);
        } else if (g_updatePopupMode == UpdatePopupMode::Error) {
            XPLMDrawTranslucentDarkBox(
                g_updateButtonOk.left, g_updateButtonOk.top, g_updateButtonOk.right, g_updateButtonOk.bottom);
            const char* okText = "OK";
            int okTextWidth = static_cast<int>(std::strlen(okText)) * charWidth;
            int okX = g_updateButtonOk.left + (g_updateButtonOk.right - g_updateButtonOk.left - okTextWidth) / 2;
            int textY = g_updateButtonOk.bottom + (kUpdateButtonHeight - charHeight) / 2;
            float buttonColor[] = {0.9f, 0.9f, 0.9f};
            XPLMDrawString(buttonColor, okX, textY, const_cast<char*>(okText), nullptr, xplmFont_Basic);
        }
    }
}

void EnsureUpdateWindow() {
    if (g_updateWindow) {
        return;
    }
    XPLMCreateWindow_t params{};
    params.structSize = sizeof(params);
    int screenLeft = 0;
    int screenTop = 0;
    int screenRight = 0;
    int screenBottom = 0;
    XPLMGetScreenBoundsGlobal(&screenLeft, &screenTop, &screenRight, &screenBottom);
    params.left = screenLeft + kUpdatePopupMargin;
    params.right = params.left + 300;
    params.top = screenTop - kUpdatePopupMargin;
    params.bottom = params.top - 120;
    params.visible = 0;
    params.drawWindowFunc = UpdateWindowDraw;
    params.handleMouseClickFunc = UpdateWindowHandleMouseClick;
    params.handleKeyFunc = UpdateWindowHandleKey;
    params.handleCursorFunc = UpdateWindowHandleCursor;
    params.handleMouseWheelFunc = UpdateWindowHandleMouseWheel;
    params.refcon = nullptr;
#if defined(XPLM301)
    params.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
#endif
#if defined(XPLM300)
    params.layer = xplm_WindowLayerFloatingWindows;
    params.handleRightClickFunc = UpdateWindowHandleRightClick;
#endif
    g_updateWindow = XPLMCreateWindowEx(&params);
#if defined(XPLM300)
    if (g_updateWindow) {
        XPLMSetWindowPositioningMode(g_updateWindow, xplm_WindowPositionFree, -1);
    }
#endif
}

void ComputeUpdateWindowGeometry(int width, int height, int* left, int* top, int* right, int* bottom) {
    if (!left || !top || !right || !bottom) {
        return;
    }
    int screenLeft = 0;
    int screenTop = 0;
    int screenRight = 0;
    int screenBottom = 0;
    XPLMGetScreenBoundsGlobal(&screenLeft, &screenTop, &screenRight, &screenBottom);

    if (g_updateUiConfig.hasWindowPos) {
        *left = g_updateUiConfig.windowLeft;
        *top = g_updateUiConfig.windowTop;
        *right = *left + width;
        *bottom = *top - height;
    } else {
        *right = screenRight - kUpdatePopupMargin;
        *left = *right - width;
        *top = screenTop - kUpdatePopupMargin;
        *bottom = *top - height;
    }

    if (*left < screenLeft + kUpdatePopupMargin) {
        *left = screenLeft + kUpdatePopupMargin;
        *right = *left + width;
    }
    if (*right > screenRight - kUpdatePopupMargin) {
        *right = screenRight - kUpdatePopupMargin;
        *left = *right - width;
    }
    if (*top > screenTop - kUpdatePopupMargin) {
        *top = screenTop - kUpdatePopupMargin;
        *bottom = *top - height;
    }
    if (*bottom < screenBottom + kUpdatePopupMargin) {
        *bottom = screenBottom + kUpdatePopupMargin;
        *top = *bottom + height;
    }
}

void ShowUpdatePopup(const std::vector<std::string>& lines) {
    EnsureUpdateWindow();
    if (!g_updateWindow) {
        return;
    }
    g_updatePopupLines = lines;
    if (g_updatePopupLines.empty()) {
        return;
    }
    g_updatePopupMode = UpdatePopupMode::Info;
    g_updateButtonsValid = false;
    g_updatePopupButtonArea = 0;
    if (g_updatePopupLines.size() > kUpdatePopupMaxLines) {
        size_t keep = kUpdatePopupMaxLines - 1;
        size_t omitted = g_updatePopupLines.size() - keep;
        g_updatePopupLines.resize(keep);
        g_updatePopupLines.push_back("... " + std::to_string(omitted) + " more lines");
    }

    int charWidth = 0;
    int charHeight = 0;
    int digitsOnly = 0;
    XPLMGetFontDimensions(xplmFont_Basic, &charWidth, &charHeight, &digitsOnly);
    int lineHeight = charHeight + 2;
    int maxLen = 0;
    for (const auto& line : g_updatePopupLines) {
        if (static_cast<int>(line.size()) > maxLen) {
            maxLen = static_cast<int>(line.size());
        }
    }
    int width = maxLen * charWidth + kUpdatePopupPadding * 2;
    int height = static_cast<int>(g_updatePopupLines.size()) * lineHeight + kUpdatePopupPadding * 2;

    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    ComputeUpdateWindowGeometry(width, height, &left, &top, &right, &bottom);
    XPLMSetWindowGeometry(g_updateWindow, left, top, right, bottom);
    XPLMSetWindowIsVisible(g_updateWindow, 1);
    g_updatePopupVisible = true;
    g_updatePopupHideAt = XPLMGetElapsedTime() + kUpdatePopupAutoHideSeconds;
}

void ShowUpdateConfirm(const std::vector<std::string>& lines) {
    EnsureUpdateWindow();
    if (!g_updateWindow) {
        return;
    }
    g_updatePopupLines = lines;
    if (g_updatePopupLines.empty()) {
        return;
    }
    g_updatePopupMode = UpdatePopupMode::Confirm;
    g_updateButtonsValid = false;
    g_updatePopupButtonArea = kUpdateButtonHeight + kUpdatePopupPadding * 2;
    if (g_updatePopupLines.size() > kUpdatePopupMaxLines) {
        size_t keep = kUpdatePopupMaxLines - 1;
        size_t omitted = g_updatePopupLines.size() - keep;
        g_updatePopupLines.resize(keep);
        g_updatePopupLines.push_back("... " + std::to_string(omitted) + " more lines");
    }

    int charWidth = 0;
    int charHeight = 0;
    int digitsOnly = 0;
    XPLMGetFontDimensions(xplmFont_Basic, &charWidth, &charHeight, &digitsOnly);
    int lineHeight = charHeight + 2;
    int maxLen = 0;
    for (const auto& line : g_updatePopupLines) {
        if (static_cast<int>(line.size()) > maxLen) {
            maxLen = static_cast<int>(line.size());
        }
    }
    int width = maxLen * charWidth + kUpdatePopupPadding * 2;
    int minWidth = kUpdatePopupPadding * 2 + kUpdateButtonWidth * 2 + kUpdateButtonGap;
    if (width < minWidth) {
        width = minWidth;
    }
    int height =
        static_cast<int>(g_updatePopupLines.size()) * lineHeight + kUpdatePopupPadding * 2 + g_updatePopupButtonArea;

    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    ComputeUpdateWindowGeometry(width, height, &left, &top, &right, &bottom);
    XPLMSetWindowGeometry(g_updateWindow, left, top, right, bottom);
    UpdatePopupButtonRectsFromWindow(left, top, right, bottom);

    XPLMSetWindowIsVisible(g_updateWindow, 1);
    g_updatePopupVisible = true;
    g_updateDecisionPending = true;
    g_updateDecisionDeadline = XPLMGetElapsedTime() + kUpdateConfirmTimeoutSeconds;
}

void ShowUpdateError(const std::vector<std::string>& lines, const char* reloadReason) {
    EnsureUpdateWindow();
    if (!g_updateWindow) {
        return;
    }
    g_updatePopupLines = lines;
    if (g_updatePopupLines.empty()) {
        return;
    }
    g_updatePopupMode = UpdatePopupMode::Error;
    g_updateDecisionPending = false;
    g_updateErrorPending = true;
    g_updateButtonsValid = false;
    g_updatePopupButtonArea = kUpdateButtonHeight + kUpdatePopupPadding * 2;
    if (g_updatePopupLines.size() > kUpdatePopupMaxLines) {
        size_t keep = kUpdatePopupMaxLines - 1;
        size_t omitted = g_updatePopupLines.size() - keep;
        g_updatePopupLines.resize(keep);
        g_updatePopupLines.push_back("... " + std::to_string(omitted) + " more lines");
    }

    int charWidth = 0;
    int charHeight = 0;
    int digitsOnly = 0;
    XPLMGetFontDimensions(xplmFont_Basic, &charWidth, &charHeight, &digitsOnly);
    int lineHeight = charHeight + 2;
    int maxLen = 0;
    for (const auto& line : g_updatePopupLines) {
        if (static_cast<int>(line.size()) > maxLen) {
            maxLen = static_cast<int>(line.size());
        }
    }
    int width = maxLen * charWidth + kUpdatePopupPadding * 2;
    int minWidth = kUpdatePopupPadding * 2 + kUpdateButtonWidth;
    if (width < minWidth) {
        width = minWidth;
    }
    int height =
        static_cast<int>(g_updatePopupLines.size()) * lineHeight + kUpdatePopupPadding * 2 + g_updatePopupButtonArea;

    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    ComputeUpdateWindowGeometry(width, height, &left, &top, &right, &bottom);
    XPLMSetWindowGeometry(g_updateWindow, left, top, right, bottom);
    UpdatePopupButtonRectsFromWindow(left, top, right, bottom);

    XPLMSetWindowIsVisible(g_updateWindow, 1);
    g_updatePopupVisible = true;
    g_reloadAfterErrorAck = true;
    g_updateErrorReloadReason = reloadReason ? reloadReason : "";
}

void HideUpdatePopup() {
    if (!g_updateWindow) {
        g_updatePopupVisible = false;
        return;
    }
    SaveUpdateWindowPosition();
    XPLMSetWindowIsVisible(g_updateWindow, 0);
    g_updatePopupVisible = false;
    g_updatePopupMode = UpdatePopupMode::None;
    g_updateButtonsValid = false;
    g_updatePopupButtonArea = 0;
    g_updateDecisionPending = false;
    g_updateErrorPending = false;
    g_reloadAfterErrorAck = false;
    g_updateErrorReloadReason.clear();
}

void SaveUpdateWindowPosition() {
    if (!g_updateWindow) {
        return;
    }
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    XPLMGetWindowGeometry(g_updateWindow, &left, &top, &right, &bottom);
    if (g_updateUiConfig.hasWindowPos
        && g_updateUiConfig.windowLeft == left
        && g_updateUiConfig.windowTop == top) {
        return;
    }
    g_updateUiConfig.hasWindowPos = true;
    g_updateUiConfig.windowLeft = left;
    g_updateUiConfig.windowTop = top;
    WriteUpdateWindowPos(left, top);
}

bool WriteUpdateWindowPos(int left, int top) {
    std::string path = GetAutarkPrefPath();
    if (path.empty()) {
        return false;
    }

    std::vector<std::string> lines;
    bool leftFound = false;
    bool topFound = false;

    {
        std::ifstream file(path);
        if (file.is_open()) {
            std::string line;
            while (std::getline(file, line)) {
                std::string trimmed = Trim(line);
                if (!trimmed.empty() && trimmed[0] != '#' && trimmed[0] != ';') {
                    size_t eq = trimmed.find('=');
                    if (eq != std::string::npos) {
                        std::string key = Trim(trimmed.substr(0, eq));
                        std::string keyLower = ToLower(key);
                        if (keyLower == "update_window_left") {
                            line = "update_window_left=" + std::to_string(left);
                            leftFound = true;
                        } else if (keyLower == "update_window_top") {
                            line = "update_window_top=" + std::to_string(top);
                            topFound = true;
                        }
                    }
                }
                lines.push_back(line);
            }
        }
    }

    if (!leftFound) {
        lines.push_back("update_window_left=" + std::to_string(left));
    }
    if (!topFound) {
        lines.push_back("update_window_top=" + std::to_string(top));
    }

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        return false;
    }
    for (const auto& line : lines) {
        out << line << "\n";
    }
    return true;
}

void UpdatePopupVisibility(double now) {
    if (g_updatePopupVisible && g_updateWindow && !XPLMGetWindowIsVisible(g_updateWindow)) {
        if (g_updatePopupMode == UpdatePopupMode::Confirm && g_updateDecisionPending) {
            HandleUpdateDecision(false, "closed");
        } else if (g_updatePopupMode == UpdatePopupMode::Error && g_updateErrorPending) {
            HandleUpdateErrorAck("closed");
        } else {
            HideUpdatePopup();
        }
        return;
    }
    if (g_updatePopupVisible && g_updatePopupMode == UpdatePopupMode::Info && now >= g_updatePopupHideAt) {
        HideUpdatePopup();
    }
}

bool StartUpdateFromPlan(const char* reason) {
    if (g_updatePending) {
        LogAlways("Update already in progress; skipping new update.");
        return false;
    }
    if (!g_updatePlanValid || g_updatePlanConfig.source.empty() || g_updatePlanTarget.empty()) {
        LogAlways("Update skipped: missing update plan.");
        return false;
    }
    HttpJob job;
    job.type = JobType::Update;
    job.update_source = g_updatePlanConfig.source;
    job.update_target = g_updatePlanTarget;
    job.update_excludes = g_updatePlanConfig.excludes;
    job.update_log_details = true;
    job.update_log_limit = kUpdateLogMaxEntries;
    EnqueueJob(job);
    g_updatePending = true;
    g_reloadAfterUpdate = true;
    g_reloadAfterUpdateAt = 0.0;
    g_updateShowResultPopup = false;
    g_updatePlanValid = false;
    g_reloadRequested = false;
    std::string msg = "Update confirmed";
    if (reason && *reason) {
        msg += " (" + std::string(reason) + ")";
    }
    LogAlways(msg + ": " + g_updatePlanConfig.source);
    return true;
}

void HandleUpdateDecision(bool accept, const char* reason) {
    if (!g_updateDecisionPending) {
        return;
    }
    g_updateDecisionPending = false;
    HideUpdatePopup();
    if (!accept) {
        std::string msg = "Update aborted";
        if (reason && *reason) {
            msg += " (" + std::string(reason) + ")";
        }
        LogAlways(msg);
        g_updatePlanValid = false;
        g_reloadRequested = false;
        TriggerReload("update aborted");
        return;
    }
    if (!StartUpdateFromPlan(reason)) {
        g_updatePlanValid = false;
        g_reloadRequested = false;
        TriggerReload("update skipped");
    }
}

void HandleUpdateErrorAck(const char* reason) {
    if (!g_updateErrorPending) {
        return;
    }
    g_updateErrorPending = false;
    std::string reloadReason = g_updateErrorReloadReason;
    if (reloadReason.empty() && reason && *reason) {
        reloadReason = reason;
    }
    if (reloadReason.empty()) {
        reloadReason = "update error";
    }
    g_updateErrorReloadReason.clear();
    HideUpdatePopup();
    g_reloadAfterErrorAck = false;
    g_reloadRequested = false;
    TriggerReload(reloadReason.c_str());
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

int ClampDebugLevel(int level) {
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    return level;
}

int GetDebugLevel(std::string* source) {
    if (!g_dref.debug_level) {
        g_dref.debug_level = XPLMFindDataRef("YAL/hoppie/debug_level");
    }
    if (g_dref.debug_level) {
        if (source) {
            *source = "YAL/hoppie/debug_level";
        }
        return ClampDebugLevel(XPLMGetDatai(g_dref.debug_level));
    }
    if (g_autarkMode && g_autarkConfig.hasDebug) {
        if (source) {
            *source = "prefs debug_level";
        }
        return ClampDebugLevel(g_autarkConfig.debugLevel);
    }
    if (source) {
        *source = "default";
    }
    return 1;
}

void RefreshDebugLevel() {
    std::string source;
    int level = GetDebugLevel(&source);
    if (level == g_debugLevel && source == g_debugLevelSource) {
        return;
    }
    g_debugLevel = level;
    g_debugLevelSource = source;
    LogAlways("Debug level set to " + std::to_string(level) + " (" + source + ").");
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

std::string ToLower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

struct UpdateTreeNode {
    std::string name;
    bool isFile = false;
    std::vector<std::string> actions;
    std::map<std::string, UpdateTreeNode> children;
};

bool ParseUpdateLogLine(const std::string& line, std::string* action, std::string* path) {
    if (!action || !path) {
        return false;
    }
    std::string trimmed = Trim(line);
    if (trimmed.rfind("copy ", 0) == 0) {
        *action = "copy";
        *path = Trim(trimmed.substr(5));
        return true;
    }
    if (trimmed.rfind("delete ", 0) == 0) {
        *action = "delete";
        *path = Trim(trimmed.substr(7));
        return true;
    }
    if (trimmed.rfind("dir_ts ", 0) == 0) {
        *action = "dir_ts";
        *path = Trim(trimmed.substr(7));
        return true;
    }
    return false;
}

void SplitPathComponents(const std::string& path, std::vector<std::string>* parts) {
    if (!parts) {
        return;
    }
    parts->clear();
    size_t start = 0;
    while (start < path.size()) {
        size_t pos = path.find('/', start);
        std::string part = (pos == std::string::npos) ? path.substr(start) : path.substr(start, pos - start);
        if (!part.empty() && part != ".") {
            parts->push_back(part);
        }
        if (pos == std::string::npos) {
            break;
        }
        start = pos + 1;
    }
}

void AddUpdateAction(UpdateTreeNode* node, const std::string& action) {
    if (!node) {
        return;
    }
    for (const auto& existing : node->actions) {
        if (existing == action) {
            return;
        }
    }
    node->actions.push_back(action);
}

std::string JoinActions(const std::vector<std::string>& actions) {
    std::ostringstream oss;
    for (size_t i = 0; i < actions.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << actions[i];
    }
    return oss.str();
}

void BuildUpdateTreeLines(const UpdateTreeNode& node, int depth, std::vector<std::string>* out) {
    if (!out) {
        return;
    }
    std::vector<const UpdateTreeNode*> ordered;
    ordered.reserve(node.children.size());
    for (const auto& kv : node.children) {
        ordered.push_back(&kv.second);
    }
    std::sort(ordered.begin(), ordered.end(),
        [](const UpdateTreeNode* a, const UpdateTreeNode* b) {
            bool aDir = !a->isFile || !a->children.empty();
            bool bDir = !b->isFile || !b->children.empty();
            if (aDir != bDir) {
                return aDir > bDir;
            }
            return a->name < b->name;
        });
    for (const auto* child : ordered) {
        std::string line(static_cast<size_t>(depth) * 2, ' ');
        line += child->name;
        if (!child->actions.empty()) {
            line += " [" + JoinActions(child->actions) + "]";
        }
        out->push_back(line);
        if (!child->children.empty()) {
            BuildUpdateTreeLines(*child, depth + 1, out);
        }
    }
}

std::vector<std::string> BuildUpdateHierarchyLines(const std::vector<std::string>& lines) {
    UpdateTreeNode root;
    std::vector<std::string> unknown;
    std::vector<std::string> parts;
    for (const auto& line : lines) {
        std::string action;
        std::string path;
        if (!ParseUpdateLogLine(line, &action, &path)) {
            unknown.push_back(line);
            continue;
        }
        if (path.empty() || path == ".") {
            AddUpdateAction(&root, action);
            continue;
        }
        SplitPathComponents(path, &parts);
        if (parts.empty()) {
            AddUpdateAction(&root, action);
            continue;
        }
        UpdateTreeNode* node = &root;
        for (const auto& part : parts) {
            auto it = node->children.find(part);
            if (it == node->children.end()) {
                UpdateTreeNode child;
                child.name = part;
                it = node->children.emplace(part, std::move(child)).first;
            }
            node = &it->second;
        }
        if (action == "dir_ts") {
            AddUpdateAction(node, action);
        } else {
            node->isFile = true;
            AddUpdateAction(node, action);
        }
    }

    std::vector<std::string> out;
    if (!root.actions.empty() && root.children.empty()) {
        std::string line = "root";
        line += " [" + JoinActions(root.actions) + "]";
        out.push_back(line);
    }
    BuildUpdateTreeLines(root, 0, &out);
    for (const auto& line : unknown) {
        out.push_back(line);
    }
    return out;
}

#ifdef _WIN32
std::string GetWin32ErrorMessage(DWORD code) {
    if (code == 0) {
        return "";
    }
    LPSTR buffer = nullptr;
    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buffer),
        0,
        nullptr);
    std::string msg;
    if (size > 0 && buffer) {
        msg.assign(buffer, size);
        LocalFree(buffer);
    }
    return Trim(msg);
}
#endif

std::string StripQuotes(const std::string& s) {
    if (s.size() < 2) {
        return s;
    }
    char front = s.front();
    char back = s.back();
    if ((front == '"' && back == '"') || (front == '\'' && back == '\'')) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

bool ParseInt(const std::string& s, int* out) {
    if (!out) {
        return false;
    }
    char* end = nullptr;
    long value = std::strtol(s.c_str(), &end, 10);
    if (!end || end == s.c_str()) {
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

bool ParseBool(const std::string& s, bool* out) {
    if (!out) {
        return false;
    }
    std::string lower = ToLower(Trim(s));
    if (lower.empty()) {
        return false;
    }
    if (lower == "1" || lower == "true" || lower == "yes" || lower == "on") {
        *out = true;
        return true;
    }
    if (lower == "0" || lower == "false" || lower == "no" || lower == "off") {
        *out = false;
        return true;
    }
    int value = 0;
    if (ParseInt(lower, &value)) {
        *out = value != 0;
        return true;
    }
    return false;
}

void SkipWhitespace(const std::string& s, size_t* idx) {
    if (!idx) {
        return;
    }
    while (*idx < s.size() && std::isspace(static_cast<unsigned char>(s[*idx]))) {
        ++(*idx);
    }
}

void SkipWhitespaceAndCommas(const std::string& s, size_t* idx) {
    if (!idx) {
        return;
    }
    while (*idx < s.size()) {
        char c = s[*idx];
        if (c == ',' || std::isspace(static_cast<unsigned char>(c))) {
            ++(*idx);
            continue;
        }
        break;
    }
}

char DecodeEscapeChar(char c) {
    switch (c) {
        case 'n': return '\n';
        case 'r': return '\r';
        case 't': return '\t';
        case '\\': return '\\';
        case '"': return '"';
        case '\'': return '\'';
        default: return c;
    }
}

bool ParseQuotedToken(const std::string& s, size_t* idx, std::string* out) {
    if (!idx || !out || *idx >= s.size()) {
        return false;
    }
    char quote = s[*idx];
    if (quote != '"' && quote != '\'') {
        return false;
    }
    ++(*idx);
    out->clear();
    while (*idx < s.size()) {
        char c = s[*idx];
        if (c == '\\' && *idx + 1 < s.size()) {
            char next = s[*idx + 1];
            out->push_back(DecodeEscapeChar(next));
            *idx += 2;
            continue;
        }
        if (c == quote) {
            ++(*idx);
            return true;
        }
        out->push_back(c);
        ++(*idx);
    }
    return false;
}

bool ParseKeyToken(const std::string& s, size_t* idx, std::string* out) {
    if (!idx || !out || *idx >= s.size()) {
        return false;
    }
    char c = s[*idx];
    if (c == '"' || c == '\'') {
        return ParseQuotedToken(s, idx, out);
    }
    size_t start = *idx;
    while (*idx < s.size()) {
        char cur = s[*idx];
        if (cur == ':' || cur == ',' || std::isspace(static_cast<unsigned char>(cur))) {
            break;
        }
        ++(*idx);
    }
    if (*idx == start) {
        return false;
    }
    *out = s.substr(start, *idx - start);
    return true;
}

bool ParseValueToken(const std::string& s, size_t* idx, std::string* out) {
    if (!idx || !out || *idx >= s.size()) {
        return false;
    }
    char c = s[*idx];
    if (c == '"' || c == '\'') {
        return ParseQuotedToken(s, idx, out);
    }
    size_t start = *idx;
    while (*idx < s.size()) {
        char cur = s[*idx];
        if (cur == ',') {
            break;
        }
        ++(*idx);
    }
    if (*idx == start) {
        out->clear();
        return false;
    }
    *out = Trim(s.substr(start, *idx - start));
    return true;
}

bool ParseLegacyOutboxMessage(const std::string& raw, LegacyOutboxMessage* out) {
    if (!out) {
        return false;
    }
    *out = LegacyOutboxMessage{};
    std::string trimmed = Trim(raw);
    if (trimmed.empty()) {
        return false;
    }
    if (trimmed.front() == '{' && trimmed.back() == '}' && trimmed.size() >= 2) {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }
    size_t idx = 0;
    while (idx < trimmed.size()) {
        SkipWhitespaceAndCommas(trimmed, &idx);
        if (idx >= trimmed.size()) {
            break;
        }
        std::string key;
        if (!ParseKeyToken(trimmed, &idx, &key)) {
            break;
        }
        SkipWhitespace(trimmed, &idx);
        if (idx >= trimmed.size() || trimmed[idx] != ':') {
            break;
        }
        ++idx;
        SkipWhitespace(trimmed, &idx);
        std::string value;
        if (!ParseValueToken(trimmed, &idx, &value)) {
            break;
        }
        std::string keyLower = ToLower(Trim(key));
        if (keyLower == "to") {
            out->to = Trim(value);
        } else if (keyLower == "type") {
            out->type = Trim(value);
        } else if (keyLower == "packet") {
            out->packet = value;
        }
    }
    return !out->to.empty() && !out->type.empty() && !out->packet.empty();
}

std::string GetPrefsDir() {
    char path[512] = {};
    XPLMGetPrefsPath(path);
    std::string full(path);
    if (full.empty()) {
        return "";
    }
    size_t pos = full.find_last_of("/\\");
    if (pos == std::string::npos) {
        return "";
    }
    return full.substr(0, pos);
}

std::string GetAutarkPrefPath() {
    std::string dir = GetPrefsDir();
    if (dir.empty()) {
        return "";
    }
    return dir + "/YAL_HoppieHelper.prf";
}

std::string GetZiboHoppiePrefPath() {
    std::string dir = GetPrefsDir();
    if (dir.empty()) {
        return "";
    }
    return dir + "/b738x_hoppie.prf";
}

bool LoadZiboHoppieConfig(ZiboHoppieConfig* config) {
    if (!config) {
        return false;
    }
    *config = ZiboHoppieConfig{};
    std::string path = GetZiboHoppiePrefPath();
    if (path.empty()) {
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    config->filePresent = true;
    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = Trim(line);
        if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string keyLower = ToLower(Trim(trimmed.substr(0, eq)));
        std::string value = Trim(StripQuotes(trimmed.substr(eq + 1)));
        if (keyLower == "enabled") {
            bool enabled = false;
            if (ParseBool(value, &enabled)) {
                config->enabled = enabled;
            }
        } else if (keyLower == "logon") {
            config->logon = Trim(value);
        }
    }
    return true;
}

std::string BuildUpdatePathError(const char* label, const std::filesystem::path& path, const std::error_code& ec) {
    std::ostringstream oss;
    if (label && *label) {
        oss << label << ": ";
    }
    oss << path.string();
    if (ec) {
        oss << " (ec=" << ec.value() << ": " << ec.message() << ")";
    }
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(path.wstring().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        DWORD winerr = GetLastError();
        if (winerr != 0) {
            std::string winmsg = GetWin32ErrorMessage(winerr);
            oss << " (win=" << winerr;
            if (!winmsg.empty()) {
                oss << ": " << winmsg;
            }
            oss << ")";
        }
    }
#endif
    return oss.str();
}

#ifdef _WIN32
bool WinGetPathAttributes(const std::filesystem::path& path, DWORD* attrsOut) {
    if (!attrsOut) {
        return false;
    }
    DWORD attrs = GetFileAttributesW(path.wstring().c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    *attrsOut = attrs;
    return true;
}
#endif

std::string NormalizeUpdateSourcePath(const std::string& path) {
#ifdef _WIN32
    std::string out = path;
    for (char& c : out) {
        if (c == '/') {
            c = '\\';
        }
    }
    if (out.rfind("\\\\?\\", 0) == 0) {
        return out;
    }
    if (out.rfind("\\\\", 0) == 0) {
        return std::string("\\\\?\\UNC\\") + out.substr(2);
    }
    return out;
#else
    return path;
#endif
}

std::string BuildUpdatePathErrorWithRaw(const char* label,
    const std::filesystem::path& path,
    const std::string& raw,
    const std::error_code& ec) {
    std::string msg = BuildUpdatePathError(label, path, ec);
    std::string normalized = path.string();
    if (!raw.empty() && raw != normalized) {
        msg += " (raw: " + raw + ")";
    }
    return msg;
}

bool GetPathInfo(const std::filesystem::path& path, bool* isDirOut, std::error_code* ec) {
    std::error_code localEc;
    bool exists = std::filesystem::exists(path, localEc);
    if (exists && !localEc) {
        if (isDirOut) {
            std::error_code dirEc;
            bool isDir = std::filesystem::is_directory(path, dirEc);
            if (dirEc) {
                if (ec) {
                    *ec = dirEc;
                }
                return false;
            }
            *isDirOut = isDir;
        }
        if (ec) {
            ec->clear();
        }
        return true;
    }
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(path.wstring().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        if (isDirOut) {
            *isDirOut = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }
        if (ec) {
            ec->clear();
        }
        return true;
    }
    DWORD winerr = GetLastError();
    if (ec && winerr != 0) {
        *ec = std::error_code(static_cast<int>(winerr), std::system_category());
    }
#endif
    if (ec) {
        *ec = localEc;
    }
    return false;
}

#ifdef _WIN32
struct WinUpdateEntry {
    std::filesystem::path abs;
    std::filesystem::path rel;
    std::string relStr;
    bool isDir = false;
    uint64_t size = 0;
    FILETIME writeTime = {};
};

struct WinFileInfo {
    bool exists = false;
    bool isDir = false;
    uint64_t size = 0;
    DWORD attrs = 0;
    FILETIME writeTime = {};
};

bool WinGetFileInfo(const std::filesystem::path& path, WinFileInfo* info, std::string* error) {
    if (!info) {
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.wstring().c_str(), GetFileExInfoStandard, &data)) {
        DWORD winerr = GetLastError();
        if (winerr == ERROR_FILE_NOT_FOUND || winerr == ERROR_PATH_NOT_FOUND) {
            *info = WinFileInfo{};
            return true;
        }
        if (error) {
            std::string msg = GetWin32ErrorMessage(winerr);
            std::ostringstream oss;
            oss << "GetFileAttributesExW failed for " << path.string() << " (win=" << winerr;
            if (!msg.empty()) {
                oss << ": " << msg;
            }
            oss << ")";
            *error = oss.str();
        }
        return false;
    }
    info->exists = true;
    info->attrs = data.dwFileAttributes;
    info->isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    ULARGE_INTEGER size;
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;
    info->size = size.QuadPart;
    info->writeTime = data.ftLastWriteTime;
    return true;
}

bool WinSetLastWriteTime(const std::filesystem::path& path, const FILETIME& ft, bool isDir, std::string* error) {
    DWORD flags = isDir ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL;
    HANDLE handle = CreateFileW(
        path.wstring().c_str(),
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        flags,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD winerr = GetLastError();
        if (error) {
            std::string msg = GetWin32ErrorMessage(winerr);
            std::ostringstream oss;
            oss << "CreateFileW failed for " << path.string() << " (win=" << winerr;
            if (!msg.empty()) {
                oss << ": " << msg;
            }
            oss << ")";
            *error = oss.str();
        }
        return false;
    }
    BOOL ok = SetFileTime(handle, nullptr, nullptr, &ft);
    DWORD winerr = ok ? 0 : GetLastError();
    CloseHandle(handle);
    if (!ok) {
        if (error) {
            std::string msg = GetWin32ErrorMessage(winerr);
            std::ostringstream oss;
            oss << "SetFileTime failed for " << path.string() << " (win=" << winerr;
            if (!msg.empty()) {
                oss << ": " << msg;
            }
            oss << ")";
            *error = oss.str();
        }
        return false;
    }
    return true;
}

std::string FormatWinAttributes(DWORD attrs) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::uppercase << attrs;
    return oss.str();
}

bool EnablePrivilege(const char* name, std::string* error) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        DWORD winerr = GetLastError();
        if (error) {
            std::string msg = GetWin32ErrorMessage(winerr);
            std::ostringstream oss;
            oss << "OpenProcessToken failed (win=" << winerr;
            if (!msg.empty()) {
                oss << ": " << msg;
            }
            oss << ")";
            *error = oss.str();
        }
        return false;
    }
    LUID luid{};
    if (!LookupPrivilegeValueA(nullptr, name, &luid)) {
        DWORD winerr = GetLastError();
        CloseHandle(token);
        if (error) {
            std::string msg = GetWin32ErrorMessage(winerr);
            std::ostringstream oss;
            oss << "LookupPrivilegeValue failed (win=" << winerr;
            if (!msg.empty()) {
                oss << ": " << msg;
            }
            oss << ")";
            *error = oss.str();
        }
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        DWORD winerr = GetLastError();
        CloseHandle(token);
        if (error) {
            std::string msg = GetWin32ErrorMessage(winerr);
            std::ostringstream oss;
            oss << "AdjustTokenPrivileges failed (win=" << winerr;
            if (!msg.empty()) {
                oss << ": " << msg;
            }
            oss << ")";
            *error = oss.str();
        }
        return false;
    }
    DWORD winerr = GetLastError();
    CloseHandle(token);
    if (winerr == ERROR_NOT_ALL_ASSIGNED) {
        if (error) {
            *error = "AdjustTokenPrivileges: privilege not assigned";
        }
        return false;
    }
    return true;
}

void EnsureUpdatePrivileges() {
    static bool attempted = false;
    if (attempted) {
        return;
    }
    attempted = true;
    std::string restoreErr;
    std::string backupErr;
    bool okRestore = EnablePrivilege(SE_RESTORE_NAME, &restoreErr);
    bool okBackup = EnablePrivilege(SE_BACKUP_NAME, &backupErr);
    if (!okRestore || !okBackup) {
        std::string msg = "Update privileges: ";
        msg += okRestore ? "SeRestore ok" : "SeRestore failed";
        if (!okRestore && !restoreErr.empty()) {
            msg += " (" + restoreErr + ")";
        }
        msg += ", ";
        msg += okBackup ? "SeBackup ok" : "SeBackup failed";
        if (!okBackup && !backupErr.empty()) {
            msg += " (" + backupErr + ")";
        }
        Log(LOG_INFO, msg);
    } else {
        Log(LOG_INFO, "Update privileges enabled (SeRestore, SeBackup).");
    }
}

bool WinEnumerateUpdateSource(const std::filesystem::path& src,
    const std::vector<std::string>& excludes,
    std::vector<WinUpdateEntry>* out,
    std::string* error) {
    if (!out) {
        return false;
    }
    out->clear();
    struct StackItem {
        std::filesystem::path abs;
        std::filesystem::path rel;
    };
    std::vector<StackItem> stack;
    stack.push_back({src, std::filesystem::path()});
    std::string firstError;
    int skipped = 0;
    while (!stack.empty()) {
        StackItem item = stack.back();
        stack.pop_back();
        std::wstring pattern = item.abs.wstring();
        if (!pattern.empty() && pattern.back() != L'\\' && pattern.back() != L'/') {
            pattern.push_back(L'\\');
        }
        pattern.push_back(L'*');
        WIN32_FIND_DATAW data{};
        HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
        if (handle == INVALID_HANDLE_VALUE) {
            DWORD winerr = GetLastError();
            if (winerr == ERROR_FILE_NOT_FOUND) {
                continue;
            }
            if (firstError.empty()) {
                std::string msg = GetWin32ErrorMessage(winerr);
                std::ostringstream oss;
                oss << "FindFirstFileW failed for " << item.abs.string() << " (win=" << winerr;
                if (!msg.empty()) {
                    oss << ": " << msg;
                }
                oss << ")";
                firstError = oss.str();
            }
            ++skipped;
            if (item.rel.empty()) {
                if (error && !firstError.empty()) {
                    *error = firstError;
                }
                return false;
            }
            continue;
        }
        do {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
                continue;
            }
            std::filesystem::path name(data.cFileName);
            std::filesystem::path childRel = item.rel / name;
            std::string relStr = childRel.generic_string();
            if (IsExcludedRelPath(relStr, excludes)) {
                continue;
            }
            bool isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            bool isReparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            if (isReparse) {
                continue;
            }
            WinUpdateEntry entry;
            entry.abs = item.abs / name;
            entry.rel = childRel;
            entry.relStr = relStr;
            entry.isDir = isDir;
            entry.writeTime = data.ftLastWriteTime;
            ULARGE_INTEGER size;
            size.LowPart = data.nFileSizeLow;
            size.HighPart = data.nFileSizeHigh;
            entry.size = size.QuadPart;
            out->push_back(entry);
            if (isDir) {
                stack.push_back({entry.abs, childRel});
            }
        } while (FindNextFileW(handle, &data));
        FindClose(handle);
    }
    if (error && skipped > 0 && !firstError.empty()) {
        std::ostringstream oss;
        oss << "WinAPI enumeration skipped " << skipped << " directories. First error: " << firstError;
        *error = oss.str();
    }
    return true;
}

bool ShouldCopyFileWin(const WinUpdateEntry& entry, const std::filesystem::path& dst, std::string* error) {
    WinFileInfo dstInfo;
    std::string infoError;
    if (!WinGetFileInfo(dst, &dstInfo, &infoError)) {
        if (error && !infoError.empty()) {
            *error = infoError;
        }
        return true;
    }
    if (!dstInfo.exists || dstInfo.isDir) {
        return true;
    }
    if (dstInfo.size != entry.size) {
        return true;
    }
    if (CompareFileTime(&entry.writeTime, &dstInfo.writeTime) > 0) {
        return true;
    }
    return false;
}
#endif

std::string NormalizeUpdateExcludeToken(std::string token) {
    token = Trim(token);
    if (token.empty()) {
        return token;
    }
    for (char& c : token) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (token.rfind("./", 0) == 0) {
        token.erase(0, 2);
    }
    if (!token.empty() && token.front() == '/') {
        size_t pos = token.find_first_not_of('/');
        if (pos == std::string::npos) {
            return "";
        }
        token = "/" + token.substr(pos);
    }
    if (token.size() >= 2 && token.substr(token.size() - 2) == "/*") {
        token.erase(token.size() - 2);
    }
    while (!token.empty() && token.back() == '/') {
        token.pop_back();
    }
    return token;
}

std::vector<std::string> ParseUpdateExcludeList(const std::string& raw) {
    std::vector<std::string> out;
    std::string token;
    for (char c : raw) {
        if (c == ',' || c == ';') {
            std::string norm = NormalizeUpdateExcludeToken(token);
            if (!norm.empty()) {
                out.push_back(norm);
            }
            token.clear();
            continue;
        }
        token.push_back(c);
    }
    std::string norm = NormalizeUpdateExcludeToken(token);
    if (!norm.empty()) {
        out.push_back(norm);
    }
    return out;
}

void NormalizeUpdateConfig(UpdateConfig* cfg) {
    if (!cfg) {
        return;
    }
    if (cfg->source.empty()) {
        cfg->enabled = false;
    }
    for (std::string& entry : cfg->excludes) {
        entry = NormalizeUpdateExcludeToken(entry);
    }
    std::vector<std::string> normalized;
    normalized.reserve(cfg->excludes.size());
    for (const std::string& entry : cfg->excludes) {
        if (!entry.empty()) {
            normalized.push_back(entry);
        }
    }
    cfg->excludes.swap(normalized);
    auto hasExclude = [&](const std::string& needle) {
        for (const std::string& entry : cfg->excludes) {
            std::string normalized = entry;
            while (!normalized.empty() && normalized.front() == '/') {
                normalized.erase(normalized.begin());
            }
            if (normalized == needle) {
                return true;
            }
        }
        return false;
    };
    if (!hasExclude("data/output")) {
        cfg->excludes.push_back("data/output");
    }
    if (!hasExclude(".ds_store")) {
        cfg->excludes.push_back(".ds_store");
    }
    if (!hasExclude("desktop.ini")) {
        cfg->excludes.push_back("desktop.ini");
    }
    if (!hasExclude("thumbs.db")) {
        cfg->excludes.push_back("thumbs.db");
    }
    if (!hasExclude("agents.md")) {
        cfg->excludes.push_back("agents.md");
    }
    if (!hasExclude(".vscode")) {
        cfg->excludes.push_back(".vscode");
    }
}

bool UpdateConfigEquals(const UpdateConfig& a, const UpdateConfig& b) {
    if (a.enabled != b.enabled) {
        return false;
    }
    if (a.source != b.source) {
        return false;
    }
    if (a.excludes.size() != b.excludes.size()) {
        return false;
    }
    for (size_t i = 0; i < a.excludes.size(); ++i) {
        if (a.excludes[i] != b.excludes[i]) {
            return false;
        }
    }
    return true;
}

bool LoadHelperConfig(AutarkConfig* autark, UpdateConfig* update, UpdateUiConfig* ui) {
    if (!autark) {
        return false;
    }
    *autark = AutarkConfig{};
    if (update) {
        *update = UpdateConfig{};
    }
    if (ui) {
        *ui = UpdateUiConfig{};
    }
    std::string path = GetAutarkPrefPath();
    if (path.empty()) {
        return false;
    }
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }
    bool autarkKeySeen = false;
    bool updateKeySeen = false;
    bool updateEnabledSet = false;
    bool windowLeftSet = false;
    bool windowTopSet = false;
    int windowLeft = 0;
    int windowTop = 0;
    std::string line;
    while (std::getline(file, line)) {
        std::string trimmed = Trim(line);
        if (trimmed.empty()) {
            continue;
        }
        if (trimmed[0] == '#' || trimmed[0] == ';') {
            continue;
        }
        size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = Trim(trimmed.substr(0, eq));
        std::string value = Trim(trimmed.substr(eq + 1));
        value = StripQuotes(value);
        std::string keyLower = ToLower(key);
        if (keyLower == "logon") {
            autarkKeySeen = true;
            autark->hasLogon = true;
            autark->logon = Trim(value);
        } else if (keyLower == "debug_level") {
            int level = 0;
            if (ParseInt(value, &level)) {
                if (level < 0) level = 0;
                if (level > 3) level = 3;
                autarkKeySeen = true;
                autark->hasDebug = true;
                autark->debugLevel = level;
            }
        } else if (keyLower == "poll_fast") {
            bool enabled = false;
            if (ParseBool(value, &enabled)) {
                autarkKeySeen = true;
                autark->hasPollFast = true;
                autark->pollFast = enabled;
            }
        } else if (keyLower == "callsign") {
            autarkKeySeen = true;
            autark->hasCallsign = true;
            autark->callsign = Trim(value);
        } else if (update && keyLower == "update_enabled") {
            bool enabled = false;
            if (ParseBool(value, &enabled)) {
                update->enabled = enabled;
                updateEnabledSet = true;
                updateKeySeen = true;
            }
        } else if (update && keyLower == "update_source") {
            update->source = Trim(value);
            updateKeySeen = true;
        } else if (update && keyLower == "update_exclude") {
            update->excludes = ParseUpdateExcludeList(value);
            updateKeySeen = true;
        }
        if (ui && keyLower == "update_window_left") {
            int left = 0;
            if (ParseInt(value, &left)) {
                windowLeftSet = true;
                windowLeft = left;
            }
        } else if (ui && keyLower == "update_window_top") {
            int top = 0;
            if (ParseInt(value, &top)) {
                windowTopSet = true;
                windowTop = top;
            }
        }
    }
    autark->enabled = autarkKeySeen;
    if (update) {
        if (!updateEnabledSet && !update->source.empty()) {
            update->enabled = true;
        }
        if (!updateKeySeen) {
            *update = UpdateConfig{};
        }
        NormalizeUpdateConfig(update);
    }
    if (ui && windowLeftSet && windowTopSet) {
        ui->hasWindowPos = true;
        ui->windowLeft = windowLeft;
        ui->windowTop = windowTop;
    }
    return true;
}

AutarkUpdate UpdateAutarkConfig(double now) {
    AutarkUpdate update;
    if (now < g_nextPrefScanTime) {
        return update;
    }
    g_nextPrefScanTime = now + 2.0;

    AutarkConfig loaded;
    UpdateConfig updateLoaded;
    UpdateUiConfig uiLoaded;
    LoadHelperConfig(&loaded, &updateLoaded, &uiLoaded);
    if (!UpdateConfigEquals(g_updateConfig, updateLoaded)) {
        update.updateChanged = true;
    }
    bool enabled = loaded.enabled;
    if (enabled != g_autarkMode) {
        update.modeChanged = true;
    }

    if (enabled) {
        const AutarkConfig& prev = g_autarkConfig;
        if (!g_autarkMode || prev.hasLogon != loaded.hasLogon || prev.logon != loaded.logon) {
            update.logonChanged = true;
        }
        if (!g_autarkMode || prev.hasDebug != loaded.hasDebug || prev.debugLevel != loaded.debugLevel) {
            update.debugChanged = true;
        }
        if (!g_autarkMode || prev.hasPollFast != loaded.hasPollFast || prev.pollFast != loaded.pollFast) {
            update.pollFastChanged = true;
        }
        if (!g_autarkMode || prev.hasCallsign != loaded.hasCallsign || prev.callsign != loaded.callsign) {
            update.callsignChanged = true;
        }
    }

    g_autarkMode = enabled;
    if (enabled) {
        g_autarkConfig = loaded;
    } else {
        g_autarkConfig = AutarkConfig{};
    }
    g_updateConfig = updateLoaded;
    g_updateUiConfig = uiLoaded;
    if (update.updateChanged) {
        if (g_updateConfig.enabled) {
            Log(LOG_INFO, "Update config enabled. Source: " + g_updateConfig.source);
        } else if (!g_updateConfig.source.empty()) {
            Log(LOG_INFO, "Update config disabled. Source: " + g_updateConfig.source);
        } else {
            Log(LOG_INFO, "Update config disabled.");
        }
    }
    return update;
}

bool UpdateZiboHoppiePassiveMode(double now) {
    if (now < g_nextZiboHoppiePrefScanTime) {
        return false;
    }
    g_nextZiboHoppiePrefScanTime = now + 2.0;

    ZiboHoppieConfig config;
    LoadZiboHoppieConfig(&config);
    bool passive = config.filePresent && config.enabled && !Trim(config.logon).empty();
    bool changed = passive != g_hoppiePassiveMode.load();
    if (changed) {
        g_hoppiePassiveMode.store(passive);
        LogAlways(passive
            ? "YAL_hoppiehelper: Hoppie passive, internal Zibo Hoppie enabled"
            : "YAL_hoppiehelper: Hoppie active, internal Zibo Hoppie disabled");
    }
    return changed;
}

std::filesystem::path GetXPlaneRootPath() {
    char path[512] = {};
    XPLMGetSystemPath(path);
    std::string root(path);
    if (root.empty()) {
        return std::filesystem::path();
    }
    return std::filesystem::path(root);
}

std::filesystem::path GetYalPluginPath() {
    std::filesystem::path root = GetXPlaneRootPath();
    if (root.empty()) {
        return std::filesystem::path();
    }
    return root / "Resources" / "plugins" / "YAL";
}

std::string NormalizeRelPathForMatch(const std::string& rel) {
    std::string out = rel;
    for (char& c : out) {
        if (c == '\\') {
            c = '/';
        }
    }
    if (out.rfind("./", 0) == 0) {
        out.erase(0, 2);
    }
    while (!out.empty() && out.front() == '/') {
        out.erase(out.begin());
    }
    return ToLower(out);
}

bool IsExcludedRelPath(const std::string& rel, const std::vector<std::string>& excludes) {
    std::string normRel = NormalizeRelPathForMatch(rel);
    if (normRel.empty()) {
        return false;
    }
    std::filesystem::path relPath(normRel);
    std::string baseName = ToLower(relPath.filename().string());
    std::string ext = ToLower(relPath.extension().string());
    std::string topLevel = normRel;
    size_t slashPos = topLevel.find('/');
    if (slashPos != std::string::npos) {
        topLevel = topLevel.substr(0, slashPos);
    }
    for (const std::string& raw : excludes) {
        std::string token = raw;
        if (!token.empty() && token.front() == '/') {
            size_t pos = token.find_first_not_of('/');
            token = (pos == std::string::npos) ? "" : token.substr(pos);
        }
        std::string normEx = NormalizeRelPathForMatch(token);
        if (normEx.empty()) {
            continue;
        }
        if (normEx.find('/') == std::string::npos) {
            if (!baseName.empty() && baseName == normEx) {
                return true;
            }
            if (normEx.rfind("*.", 0) == 0 && !ext.empty()) {
                std::string wantedExt = normEx.substr(1); // ".lua"
                if (ext == wantedExt) {
                    return true;
                }
            }
            if (!topLevel.empty() && topLevel == normEx) {
                return true;
            }
            continue;
        }
        if (normRel == normEx) {
            return true;
        }
        if (normRel.size() > normEx.size() && normRel.compare(0, normEx.size(), normEx) == 0
            && normRel[normEx.size()] == '/') {
            return true;
        }
    }
    return false;
}

bool ShouldCopyFile(const std::filesystem::path& src,
    const std::filesystem::path& dst,
    std::error_code* ec) {
    std::error_code localEc;
    std::error_code* useEc = ec ? ec : &localEc;
    if (!std::filesystem::exists(dst, *useEc)) {
        if (useEc) {
            useEc->clear();
        }
        return true;
    }
    auto srcSize = std::filesystem::file_size(src, *useEc);
    if (*useEc) {
        useEc->clear();
        return true;
    }
    auto dstSize = std::filesystem::file_size(dst, *useEc);
    if (*useEc) {
        useEc->clear();
        return true;
    }
    if (srcSize != dstSize) {
        return true;
    }
    auto srcTime = std::filesystem::last_write_time(src, *useEc);
    if (*useEc) {
        useEc->clear();
        return true;
    }
    auto dstTime = std::filesystem::last_write_time(dst, *useEc);
    if (*useEc) {
        useEc->clear();
        return true;
    }
    return srcTime > dstTime;
}

bool PerformUpdateJob(const HttpJob& job,
    bool dryRun,
    std::string* summary,
    std::string* error,
    std::vector<std::string>* logLines,
    size_t* logOmitted,
    UpdateCounters* counters) {
    std::string srcRaw = job.update_source;
    std::string dstRaw = job.update_target;
    std::string srcNorm = NormalizeUpdateSourcePath(srcRaw);
    std::string dstNorm = NormalizeUpdateSourcePath(dstRaw);
    std::filesystem::path src(srcRaw);
    std::filesystem::path dst(dstRaw);
    bool srcUsedNormalized = false;
    if (logOmitted) {
        *logOmitted = 0;
    }
    const bool logDetails = job.update_log_details && logLines && job.update_log_limit > 0;
    const size_t logLimit = job.update_log_limit;
    auto noteChange = [&](const std::string& prefix, const std::string& rel) {
        if (!logDetails || !logLines) {
            return;
        }
        if (logLines->size() < logLimit) {
            logLines->push_back(prefix + rel);
            return;
        }
        if (logOmitted) {
            ++(*logOmitted);
        }
    };
    std::unordered_set<std::string> changedDirs;
    auto normalizeRelDirKey = [](const std::filesystem::path& relPath) -> std::string {
        if (relPath.empty() || relPath == ".") {
            return ".";
        }
        return relPath.generic_string();
    };
    auto markChangedFileRel = [&](const std::filesystem::path& relFile) {
        std::filesystem::path current = relFile.parent_path();
        while (true) {
            changedDirs.insert(normalizeRelDirKey(current));
            if (current.empty() || current == ".") {
                break;
            }
            current = current.parent_path();
        }
    };
    auto shouldUpdateDirTs = [&](const std::filesystem::path& relPath) -> bool {
        if (changedDirs.empty()) {
            return false;
        }
        std::string key = normalizeRelDirKey(relPath);
        return changedDirs.find(key) != changedDirs.end();
    };
    if (src.empty()) {
        if (error) {
            *error = "Update source is empty";
        }
        return false;
    }
    if (dst.empty()) {
        if (error) {
            *error = "Update target is empty";
        }
        return false;
    }
    std::error_code ec;
    bool srcIsDir = false;
    bool srcExists = GetPathInfo(src, &srcIsDir, &ec);
    if ((!srcExists || ec) && srcNorm != srcRaw) {
        std::filesystem::path alt(srcNorm);
        std::error_code altEc;
        bool altIsDir = false;
        bool altExists = GetPathInfo(alt, &altIsDir, &altEc);
        if (altExists && !altEc) {
            src = alt;
            srcIsDir = altIsDir;
            srcExists = true;
            ec.clear();
            srcUsedNormalized = true;
        }
    }
    if (!srcExists || ec) {
        if (error) {
            *error = BuildUpdatePathErrorWithRaw("Update source not found", src, srcRaw, ec);
        }
        return false;
    }
    if (!srcIsDir) {
        if (error) {
            *error = BuildUpdatePathErrorWithRaw("Update source is not a directory", src, srcRaw, ec);
        }
        return false;
    }
    ec.clear();
    bool dstIsDir = false;
    bool dstExists = GetPathInfo(dst, &dstIsDir, &ec);
    if ((!dstExists || ec) && dstNorm != dstRaw) {
        std::filesystem::path alt(dstNorm);
        std::error_code altEc;
        bool altIsDir = false;
        bool altExists = GetPathInfo(alt, &altIsDir, &altEc);
        if (altExists && !altEc) {
            dst = alt;
            dstIsDir = altIsDir;
            dstExists = true;
            ec.clear();
        }
    }
    if (!dstExists || ec) {
        if (error) {
            *error = BuildUpdatePathErrorWithRaw("YAL plugin path not found", dst, dstRaw, ec);
        }
        return false;
    }
    if (!dstIsDir) {
        if (error) {
            *error = BuildUpdatePathErrorWithRaw("YAL plugin path is not a directory", dst, dstRaw, ec);
        }
        return false;
    }
    ec.clear();
    if (std::filesystem::equivalent(src, dst, ec) && !ec) {
        if (summary) {
            *summary = "Update source equals target; skipped";
        }
        return true;
    }

    size_t copied = 0;
    size_t skipped = 0;
    size_t dirsCreated = 0;
    size_t failed = 0;
    size_t deleted = 0;
    size_t deleteFailed = 0;
    size_t dirTs = 0;
    size_t sourceEntries = 0;
    std::string firstFailure;
    bool loggedFailure = false;
    auto formatEc = [](const std::error_code& err) {
        if (!err) {
            return std::string();
        }
        return std::to_string(err.value()) + ": " + err.message();
    };
    auto recordFailure = [&](const std::string& msg) {
        if (firstFailure.empty()) {
            firstFailure = msg;
        }
        if (!loggedFailure) {
            Log(LOG_ERR, "Update error: " + msg);
            loggedFailure = true;
        }
    };
    const char* copyPrefix = dryRun ? "copy " : "copied ";
    const char* deletePrefix = dryRun ? "delete " : "deleted ";
    std::vector<std::pair<std::filesystem::path, std::filesystem::file_time_type>> sourceDirTimes;
#ifdef _WIN32
    bool usedWinFallback = false;
    std::vector<std::pair<std::filesystem::path, FILETIME>> sourceDirTimesWin;
#endif
    const auto options = std::filesystem::directory_options::skip_permission_denied;
    std::error_code rootTimeEc;
    auto rootTime = std::filesystem::last_write_time(src, rootTimeEc);
    if (!rootTimeEc) {
        sourceDirTimes.emplace_back(std::filesystem::path("."), rootTime);
    }
    std::filesystem::recursive_directory_iterator it(src, options, ec);
    std::filesystem::recursive_directory_iterator end;
    while (it != end) {
        if (ec) {
            ec.clear();
            it.increment(ec);
            continue;
        }
        std::filesystem::path relPath = it->path().lexically_relative(src);
        std::string rel = relPath.generic_string();
        if (IsExcludedRelPath(rel, job.update_excludes)) {
            if (it->is_directory()) {
                it.disable_recursion_pending();
            }
            it.increment(ec);
            continue;
        }
        ++sourceEntries;
        std::filesystem::path dstPath = dst / relPath;
        if (it->is_directory()) {
            std::error_code dirTimeEc;
            auto dirTime = std::filesystem::last_write_time(it->path(), dirTimeEc);
            if (!dirTimeEc) {
                sourceDirTimes.emplace_back(relPath, dirTime);
            }
            if (!dryRun) {
                std::filesystem::create_directories(dstPath, ec);
                if (!ec) {
                    ++dirsCreated;
                } else {
                    if (!dryRun) {
                        recordFailure("mkdir " + rel + " (ec=" + formatEc(ec) + ")");
                    }
                    ec.clear();
                    ++failed;
                }
            }
            it.increment(ec);
            continue;
        }
        if (!it->is_regular_file()) {
            it.increment(ec);
            continue;
        }
        if (!ShouldCopyFile(it->path(), dstPath, &ec)) {
            if (ec) {
                recordFailure("stat " + rel + " (ec=" + formatEc(ec) + ")");
                ec.clear();
                ++failed;
            } else {
                ++skipped;
            }
            it.increment(ec);
            continue;
        }
        if (dryRun) {
            ++copied;
            noteChange(copyPrefix, rel);
            markChangedFileRel(relPath);
            it.increment(ec);
            continue;
        }
        std::filesystem::create_directories(dstPath.parent_path(), ec);
        if (ec) {
            ec.clear();
        }
        bool copyOk = false;
        ec.clear();
        std::filesystem::copy_file(it->path(), dstPath, std::filesystem::copy_options::overwrite_existing, ec);
        if (!ec) {
            copyOk = true;
        }
#ifdef _WIN32
        DWORD winerr = 0;
        if (!copyOk) {
            if (CopyFileW(it->path().wstring().c_str(), dstPath.wstring().c_str(), FALSE)) {
                copyOk = true;
                ec.clear();
            } else {
                winerr = GetLastError();
            }
        }
#endif
        if (!copyOk) {
            std::string msg = "copy " + rel + " (ec=" + formatEc(ec) + ")";
#ifdef _WIN32
            if (winerr != 0) {
                std::string winmsg = GetWin32ErrorMessage(winerr);
                msg += " (win=" + std::to_string(winerr);
                if (!winmsg.empty()) {
                    msg += ": " + winmsg;
                }
                msg += ")";
            }
#endif
            recordFailure(msg);
            ec.clear();
            ++failed;
        } else {
            ++copied;
            noteChange(copyPrefix, rel);
            markChangedFileRel(relPath);
            std::error_code timeEc;
            auto srcTime = std::filesystem::last_write_time(it->path(), timeEc);
            if (!timeEc) {
                std::filesystem::last_write_time(dstPath, srcTime, timeEc);
            }
            if (timeEc) {
                recordFailure("timestamp " + rel + " (ec=" + formatEc(timeEc) + ")");
                ++failed;
            }
        }
        it.increment(ec);
    }

#ifdef _WIN32
    if (sourceEntries == 0) {
        std::vector<WinUpdateEntry> winEntries;
        std::string winEnumError;
        if (WinEnumerateUpdateSource(src, job.update_excludes, &winEntries, &winEnumError) && !winEntries.empty()) {
            usedWinFallback = true;
            std::string firstCopyError;
            std::string firstTimeError;
            int copyErrors = 0;
            int timeErrors = 0;
            if (!winEnumError.empty()) {
                Log(LOG_INFO, "Update scan WinAPI enumeration warning: " + winEnumError);
            }
            Log(LOG_INFO, "Update scan WinAPI enumeration used for source: " + src.string());
            WinFileInfo rootInfo;
            if (WinGetFileInfo(src, &rootInfo, nullptr) && rootInfo.exists) {
                sourceDirTimesWin.emplace_back(std::filesystem::path("."), rootInfo.writeTime);
            }
            for (const auto& entry : winEntries) {
                ++sourceEntries;
                std::filesystem::path dstPath = dst / entry.rel;
                if (entry.isDir) {
                    sourceDirTimesWin.emplace_back(entry.rel, entry.writeTime);
                    if (!dryRun) {
                        std::filesystem::create_directories(dstPath, ec);
                        if (!ec) {
                            ++dirsCreated;
                        } else {
                            ec.clear();
                            ++failed;
                        }
                    }
                    continue;
                }
                if (!ShouldCopyFileWin(entry, dstPath, nullptr)) {
                    ++skipped;
                    continue;
                }
                if (dryRun) {
                    ++copied;
                    noteChange(copyPrefix, entry.relStr);
                    markChangedFileRel(entry.rel);
                    continue;
                }
                std::filesystem::create_directories(dstPath.parent_path(), ec);
                if (ec) {
                    ec.clear();
                }
                if (!CopyFileW(entry.abs.wstring().c_str(), dstPath.wstring().c_str(), FALSE)) {
                    ++failed;
                    ++copyErrors;
                    if (copyErrors == 1) {
                        DWORD winerr = GetLastError();
                        std::string msg = GetWin32ErrorMessage(winerr);
                        std::ostringstream oss;
                        oss << entry.relStr << " (win=" << winerr;
                        if (!msg.empty()) {
                            oss << ": " << msg;
                        }
                        oss << ")";
                        firstCopyError = oss.str();
                        recordFailure("copy " + entry.relStr + " (win=" + std::to_string(winerr)
                            + (msg.empty() ? "" : ": " + msg) + ")");
                    }
                    continue;
                }
                ++copied;
                noteChange(copyPrefix, entry.relStr);
                markChangedFileRel(entry.rel);
                std::string winTimeError;
                if (!WinSetLastWriteTime(dstPath, entry.writeTime, false, &winTimeError)) {
                    ++failed;
                    ++timeErrors;
                    if (timeErrors == 1) {
                        firstTimeError = entry.relStr;
                        if (!winTimeError.empty()) {
                            recordFailure("timestamp " + entry.relStr + " (" + winTimeError + ")");
                        } else {
                            recordFailure("timestamp " + entry.relStr + " (win error)");
                        }
                    }
                }
            }
            if (!dryRun && copyErrors > 0 && !firstCopyError.empty()) {
                Log(LOG_ERR, "Update copy failed (WinAPI). First error: " + firstCopyError);
            }
            if (!dryRun && timeErrors > 0 && !firstTimeError.empty()) {
                Log(LOG_ERR, "Update timestamp failed (WinAPI). First file: " + firstTimeError);
            }
        } else if (!winEnumError.empty()) {
            Log(LOG_INFO, "Update scan WinAPI enumeration failed: " + winEnumError);
        }
    }
#endif

    if (sourceEntries == 0) {
        if (error) {
            std::string msg = "Update source contains no files after excludes";
            if (srcUsedNormalized) {
                msg += " (normalized path used)";
            }
#ifdef _WIN32
            if (usedWinFallback) {
                msg += " (WinAPI fallback used)";
            }
#endif
            *error = msg + "; check update_source/update_exclude";
        }
        return false;
    }

    auto noteDirTimeChange = [&](const std::filesystem::path& relPath) {
        if (!shouldUpdateDirTs(relPath)) {
            return;
        }
        std::string relStr;
        if (relPath.empty() || relPath == ".") {
            relStr = ".";
        } else {
            relStr = relPath.generic_string();
        }
        ++dirTs;
        noteChange("dir_ts ", relStr);
    };

    ec.clear();
    std::filesystem::recursive_directory_iterator dit(dst, options, ec);
    while (dit != end) {
        if (ec) {
            ec.clear();
            dit.increment(ec);
            continue;
        }
        std::filesystem::path relPath = dit->path().lexically_relative(dst);
        std::string rel = relPath.generic_string();
        if (IsExcludedRelPath(rel, job.update_excludes)) {
            if (dit->is_directory()) {
                dit.disable_recursion_pending();
            }
            dit.increment(ec);
            continue;
        }
        if (dit->is_directory()) {
            dit.increment(ec);
            continue;
        }
        if (!dit->is_regular_file()) {
            dit.increment(ec);
            continue;
        }
        std::filesystem::path srcPath = src / relPath;
        std::error_code existsEc;
        bool exists = GetPathInfo(srcPath, nullptr, &existsEc);
        if (existsEc) {
            recordFailure("exists " + rel + " (ec=" + formatEc(existsEc) + ")");
            ++deleteFailed;
            ++failed;
            existsEc.clear();
            dit.increment(ec);
            continue;
        }
        if (!exists) {
            if (dryRun) {
                ++deleted;
                noteChange(deletePrefix, rel);
                markChangedFileRel(relPath);
            } else {
                std::filesystem::remove(dit->path(), ec);
                if (ec) {
                    recordFailure("delete " + rel + " (ec=" + formatEc(ec) + ")");
                    ++deleteFailed;
                    ++failed;
                    ec.clear();
                } else {
                    ++deleted;
                    noteChange(deletePrefix, rel);
                    markChangedFileRel(relPath);
                }
            }
        }
        dit.increment(ec);
    }

    if (dryRun) {
#ifdef _WIN32
        auto getSourceDirTimeWin = [&](const std::filesystem::path& relPath, FILETIME* out) -> bool {
            if (!out) {
                return false;
            }
            std::filesystem::path srcPath;
            if (relPath.empty() || relPath == ".") {
                srcPath = src;
            } else {
                srcPath = src / relPath;
            }
            WinFileInfo srcInfo;
            if (!WinGetFileInfo(srcPath, &srcInfo, nullptr) || !srcInfo.exists || !srcInfo.isDir) {
                return false;
            }
            *out = srcInfo.writeTime;
            return true;
        };
        auto checkDirTimeWin = [&](const std::filesystem::path& relPath) {
            FILETIME srcTime{};
            if (!getSourceDirTimeWin(relPath, &srcTime)) {
                noteDirTimeChange(relPath);
                return;
            }
            std::filesystem::path dstPath = (relPath.empty() || relPath == ".") ? dst : (dst / relPath);
            WinFileInfo dstInfo;
            if (!WinGetFileInfo(dstPath, &dstInfo, nullptr)) {
                noteDirTimeChange(relPath);
                return;
            }
            if (!dstInfo.exists || !dstInfo.isDir) {
                noteDirTimeChange(relPath);
                return;
            }
            if (CompareFileTime(&srcTime, &dstInfo.writeTime) != 0) {
                noteDirTimeChange(relPath);
            }
        };
        if (usedWinFallback && !sourceDirTimesWin.empty()) {
            for (const auto& entry : sourceDirTimesWin) {
                checkDirTimeWin(entry.first);
            }
        } else {
            for (const auto& entry : sourceDirTimes) {
                checkDirTimeWin(entry.first);
            }
        }
#else
        for (const auto& entry : sourceDirTimes) {
            const std::filesystem::path& relPath = entry.first;
            const auto& dirTime = entry.second;
            std::filesystem::path dstPath;
            if (relPath.empty() || relPath == ".") {
                dstPath = dst;
            } else {
                dstPath = dst / relPath;
            }
            std::error_code timeEc;
            if (!std::filesystem::exists(dstPath, timeEc) || timeEc) {
                if (timeEc) {
                    timeEc.clear();
                }
                noteDirTimeChange(relPath);
                continue;
            }
            timeEc.clear();
            if (!std::filesystem::is_directory(dstPath, timeEc) || timeEc) {
                if (timeEc) {
                    timeEc.clear();
                }
                noteDirTimeChange(relPath);
                continue;
            }
            auto dstTime = std::filesystem::last_write_time(dstPath, timeEc);
            if (timeEc) {
                timeEc.clear();
                noteDirTimeChange(relPath);
                continue;
            }
            if (dirTime != dstTime) {
                noteDirTimeChange(relPath);
            }
        }
#endif
    }

    if (!dryRun) {
        std::vector<std::filesystem::path> dirs;
        ec.clear();
        std::filesystem::recursive_directory_iterator dirIt(dst, options, ec);
        while (dirIt != end) {
            if (ec) {
                ec.clear();
                dirIt.increment(ec);
                continue;
            }
            if (!dirIt->is_directory()) {
                dirIt.increment(ec);
                continue;
            }
            std::filesystem::path relPath = dirIt->path().lexically_relative(dst);
            std::string rel = relPath.generic_string();
            if (IsExcludedRelPath(rel, job.update_excludes)) {
                dirIt.disable_recursion_pending();
                dirIt.increment(ec);
                continue;
            }
            dirs.push_back(dirIt->path());
            dirIt.increment(ec);
        }
        std::sort(dirs.begin(), dirs.end(),
            [](const std::filesystem::path& a, const std::filesystem::path& b) {
                return a.native().size() > b.native().size();
            });
        for (const auto& dirPath : dirs) {
            std::filesystem::path relPath = dirPath.lexically_relative(dst);
            std::string rel = relPath.generic_string();
            if (IsExcludedRelPath(rel, job.update_excludes)) {
                continue;
            }
            std::filesystem::path srcPath = src / relPath;
            std::error_code existsEc;
            bool exists = GetPathInfo(srcPath, nullptr, &existsEc);
            if (existsEc) {
                ++deleteFailed;
                ++failed;
                existsEc.clear();
                continue;
            }
            if (exists) {
                continue;
            }
            std::error_code emptyEc;
            if (!std::filesystem::is_empty(dirPath, emptyEc) || emptyEc) {
                if (emptyEc) {
                    recordFailure("is_empty " + rel + " (ec=" + formatEc(emptyEc) + ")");
                    ++deleteFailed;
                    ++failed;
                    emptyEc.clear();
                }
                continue;
            }
            std::filesystem::remove(dirPath, ec);
            if (ec) {
                recordFailure("delete_dir " + rel + " (ec=" + formatEc(ec) + ")");
                ++deleteFailed;
                ++failed;
                ec.clear();
            }
        }

        std::sort(sourceDirTimes.begin(), sourceDirTimes.end(),
            [](const auto& a, const auto& b) {
                return a.first.native().size() > b.first.native().size();
            });
#ifdef _WIN32
        std::sort(sourceDirTimesWin.begin(), sourceDirTimesWin.end(),
            [](const auto& a, const auto& b) {
                return a.first.native().size() > b.first.native().size();
            });
        EnsureUpdatePrivileges();
        auto setDirTimeWin = [&](const std::filesystem::path& relPath) {
            if (!shouldUpdateDirTs(relPath)) {
                return;
            }
            std::filesystem::path srcPath = (relPath.empty() || relPath == ".") ? src : (src / relPath);
            WinFileInfo srcInfo;
            std::string srcErr;
            if (!WinGetFileInfo(srcPath, &srcInfo, &srcErr) || !srcInfo.exists || !srcInfo.isDir) {
                ++failed;
                if (!srcErr.empty()) {
                    recordFailure("timestamp_dir " + relPath.generic_string() + " (" + srcErr + ")");
                } else {
                    recordFailure("timestamp_dir " + relPath.generic_string() + " (source info failed)");
                }
                return;
            }
            std::filesystem::path dstPath = (relPath.empty() || relPath == ".") ? dst : (dst / relPath);
            WinFileInfo dstInfo;
            std::string dstErr;
            if (!WinGetFileInfo(dstPath, &dstInfo, &dstErr)) {
                ++failed;
                if (!dstErr.empty()) {
                    recordFailure("timestamp_dir " + relPath.generic_string() + " (" + dstErr + ")");
                } else {
                    recordFailure("timestamp_dir " + relPath.generic_string() + " (dest info failed)");
                }
                return;
            }
            if (!dstInfo.exists || !dstInfo.isDir) {
                return;
            }
            std::string winTimeError;
            if (!WinSetLastWriteTime(dstPath, srcInfo.writeTime, true, &winTimeError)) {
                ++failed;
                std::string msg = winTimeError.empty() ? "win error" : winTimeError;
                msg += " attrs=" + FormatWinAttributes(dstInfo.attrs);
                recordFailure("timestamp_dir " + relPath.generic_string() + " (" + msg + ")");
                return;
            }
            ++dirTs;
        };
        if (usedWinFallback && !sourceDirTimesWin.empty()) {
            for (const auto& entry : sourceDirTimesWin) {
                setDirTimeWin(entry.first);
            }
        } else {
            for (const auto& entry : sourceDirTimes) {
                setDirTimeWin(entry.first);
            }
        }
#else
        {
            for (const auto& entry : sourceDirTimes) {
                const std::filesystem::path& relPath = entry.first;
                if (!shouldUpdateDirTs(relPath)) {
                    continue;
                }
                const auto& dirTime = entry.second;
                std::filesystem::path dstPath;
                if (relPath.empty() || relPath == ".") {
                    dstPath = dst;
                } else {
                    dstPath = dst / relPath;
                }
                std::error_code timeEc;
                if (!std::filesystem::exists(dstPath, timeEc) || timeEc) {
                    if (timeEc) {
                        ++failed;
                        timeEc.clear();
                    }
                    continue;
                }
                timeEc.clear();
                if (!std::filesystem::is_directory(dstPath, timeEc) || timeEc) {
                    if (timeEc) {
                        ++failed;
                        timeEc.clear();
                    }
                    continue;
                }
                std::filesystem::last_write_time(dstPath, dirTime, timeEc);
                if (timeEc) {
                    recordFailure("timestamp_dir " + relPath.generic_string()
                        + " (ec=" + formatEc(timeEc) + ")");
                    ++failed;
                    timeEc.clear();
                } else {
                    ++dirTs;
                }
            }
        }
#endif
    }

    if (summary) {
        std::ostringstream ss;
        if (dryRun) {
            ss << "scan ";
        }
        ss << "copied=" << copied << " skipped=" << skipped << " dirs=" << dirsCreated
           << " deleted=" << deleted << " del_failed=" << deleteFailed << " failed=" << failed
           << " dir_ts=" << dirTs;
        *summary = ss.str();
    }
    if (counters) {
        counters->copied = copied;
        counters->skipped = skipped;
        counters->dirs = dirsCreated;
        counters->deleted = deleted;
        counters->deleteFailed = deleteFailed;
        counters->failed = failed;
        counters->dir_ts = dirTs;
    }
    if (failed > 0) {
        if (error) {
            *error = firstFailure.empty() ? "Update completed with errors" : firstFailure;
        }
        return false;
    }
    return true;
}

std::string GetActiveLogon() {
    if (g_autarkMode) {
        if (!g_autarkConfig.hasLogon) {
            return "";
        }
        return Trim(g_autarkConfig.logon);
    }
    return Trim(GetDataRefString(g_dref.logon));
}

void ApplyAutarkPollFastOverride() {
    if (!g_autarkMode || !g_autarkConfig.hasPollFast) {
        return;
    }
    SetDataRefBool(g_dref.poll_frequency_fast, g_autarkConfig.pollFast);
}

bool ApplyAutarkCallsign(std::string* callsign) {
    if (!callsign) {
        return false;
    }
    if (!g_autarkMode || !g_autarkConfig.hasCallsign) {
        return false;
    }
    std::string pref = Trim(g_autarkConfig.callsign);
    if (pref.empty()) {
        return false;
    }
    std::string current = Trim(*callsign);
    bool shouldApply = false;
    if (current.empty()) {
        shouldApply = true;
    } else if (!g_autarkAppliedCallsign.empty()
        && g_autarkAppliedCallsign != pref
        && current == g_autarkAppliedCallsign) {
        shouldApply = true;
    }
    if (!shouldApply) {
        return false;
    }
    SetDataRefString(g_dref.callsign, pref);
    *callsign = pref;
    g_autarkAppliedCallsign = pref;
    Log(LOG_INFO, "Callsign set from prefs: " + pref);
    return true;
}

bool IsZiboPluginLoaded() {
    return XPLMFindPluginBySignature(kZiboPluginSig) != XPLM_NO_PLUGIN_ID;
}

bool IsZiboTailnum(const std::string& tailnum) {
    if (tailnum.empty()) {
        return false;
    }
    std::string prefix5 = tailnum.substr(0, std::min<size_t>(5, tailnum.size()));
    if (prefix5 == "ZB738") {
        return true;
    }
    std::string prefix4 = tailnum.substr(0, std::min<size_t>(4, tailnum.size()));
    return prefix4 == "B736" || prefix4 == "B737" || prefix4 == "B738" || prefix4 == "B739" || prefix4 == "738";
}

bool UpdateZiboState() {
    if (!g_dref.tailnum) {
        g_dref.tailnum = XPLMFindDataRef("sim/aircraft/view/acf_tailnum");
    }
    bool pluginPresent = IsZiboPluginLoaded();
    std::string tailnum = Trim(GetDataRefString(g_dref.tailnum));
    bool tailnumOk = IsZiboTailnum(tailnum);
    bool ready = pluginPresent && tailnumOk;

    if (!g_ziboPluginKnown || pluginPresent != g_lastZiboPluginPresent) {
        g_ziboPluginKnown = true;
        g_lastZiboPluginPresent = pluginPresent;
        Log(LOG_DBG, std::string("Zibo plugin ") + (pluginPresent ? "detected." : "not found."));
    }

    if (!g_tailnumKnown || tailnum != g_lastTailnum) {
        g_tailnumKnown = true;
        g_lastTailnum = tailnum;
        Log(LOG_DBG, std::string("Tailnum: ") + (tailnum.empty() ? "-" : tailnum));
    }
    if (g_lastTailnumMatches != tailnumOk) {
        g_lastTailnumMatches = tailnumOk;
        Log(LOG_DBG, std::string("Tailnum match: ") + (tailnumOk ? "yes" : "no") + ".");
    }

    if (!g_ziboKnown || ready != g_lastZiboReady) {
        g_ziboKnown = true;
        g_lastZiboReady = ready;
        if (ready) {
            Log(LOG_INFO, std::string("Zibo ready (tailnum=")
                + (tailnum.empty() ? "-" : tailnum) + ").");
        } else if (!pluginPresent) {
            Log(LOG_INFO, "Zibo not ready (plugin missing).");
        } else if (!tailnumOk) {
            Log(LOG_INFO, std::string("Zibo not ready (tailnum=")
                + (tailnum.empty() ? "-" : tailnum) + ").");
        } else {
            Log(LOG_INFO, "Zibo not ready.");
        }
    }

    return ready;
}

bool IsHoppiePassiveMode() {
    return g_hoppiePassiveMode.load();
}

void ResetHoppieRuntimeState(double now, bool clearBridgeOutputs) {
    g_refsReady = false;
    g_nextRefScanTime = now;
    g_nextPollTime = 0.0;
    g_commEstablished = false;
    g_sendPending = false;
    g_pollPending = false;
    if (clearBridgeOutputs) {
        SetDataRefBool(g_dref.comm_ready, false);
    }
    g_loggedMissingRefs = false;
    g_loggedStartupSummary = false;
    g_commReadyKnown = false;
    g_lastCommReady = false;
    g_pollModeKnown = false;
    g_lastFastPoll = false;
    g_logonKnown = false;
    g_logonAvailable = false;
    g_lastCallsign.clear();
    g_lastBadSendQueue.clear();
    g_autarkAppliedCallsign.clear();
    g_pendingInbox.clear();
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
        Log(LOG_INFO, "Registered " + std::to_string(created) + " helper datarefs.");
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
    if (g_autarkMode && g_autarkConfig.hasPollFast) {
        return g_autarkConfig.pollFast;
    }
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

std::string QuoteForLog(const std::string& s) {
    return "\"" + JsonEscape(s) + "\"";
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
    if (job.type == JobType::Update || job.type == JobType::UpdateScan) {
        std::string summary;
        std::string err;
        std::vector<std::string> updateLines;
        size_t updateOmitted = 0;
        UpdateCounters counts;
        bool ok = PerformUpdateJob(
            job,
            job.type == JobType::UpdateScan,
            &summary,
            &err,
            job.update_log_details ? &updateLines : nullptr,
            job.update_log_details ? &updateOmitted : nullptr,
            &counts);
        result.ok = ok;
        result.response = summary;
        result.error = err;
        result.httpCode = ok ? 200 : 0;
        result.update_log_lines = std::move(updateLines);
        result.update_log_omitted = updateOmitted;
        result.update_counts = counts;
        return result;
    }
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

bool ParseHoppiePayload(const std::string& raw, std::string* from, std::string* type, std::string* packet) {
    std::string s = Trim(raw);
    if (s.empty()) {
        return false;
    }
    if (s == "ok") {
        if (from) from->clear();
        if (type) type->clear();
        if (packet) *packet = "ok";
        return true;
    }
    static const std::regex kHoppiePattern(R"(\{([^\s]+)\s+([^\s]+)\s+\{([\s\S]+?)\}\})");
    std::smatch match;
    if (std::regex_search(s, match, kHoppiePattern)) {
        if (from) *from = match[1].str();
        if (type) *type = match[2].str();
        if (packet) *packet = Trim(match[3].str());
        return true;
    }
    if (from) from->clear();
    if (type) type->clear();
    if (packet) *packet = s;
    return true;
}

bool IsOkOnlyCaseInsensitive(const std::string& raw) {
    std::string s = Trim(raw);
    if (s.size() != 2) {
        return false;
    }
    return (s[0] == 'o' || s[0] == 'O') && (s[1] == 'k' || s[1] == 'K');
}

bool InboxHasMessage() {
    return !Trim(GetDataRefString(g_dref.poll_queue)).empty();
}

bool BuildInboundMessage(const std::string& origin, const std::string& raw, InboundMessage* out) {
    std::string trimmed = Trim(raw);
    if (trimmed.empty()) {
        return false;
    }
    std::string from;
    std::string type;
    std::string packet;
    if (!ParseHoppiePayload(trimmed, &from, &type, &packet)) {
        return false;
    }
    if (out) {
        out->origin = origin;
        out->raw = trimmed;
        out->from = from;
        out->type = type;
        out->packet = packet;
    }
    return true;
}

bool BuildInboundMessages(const std::string& origin, const std::string& raw, std::vector<InboundMessage>* out) {
    if (!out) {
        return false;
    }
    out->clear();
    std::string trimmed = Trim(raw);
    if (trimmed.empty()) {
        return false;
    }
    static const std::regex kHoppiePattern(R"(\{([^\s]+)\s+([^\s]+)\s+\{([\s\S]+?)\}\})");
    std::sregex_iterator it(trimmed.begin(), trimmed.end(), kHoppiePattern);
    std::sregex_iterator end;
    if (it == end) {
        InboundMessage msg;
        if (!BuildInboundMessage(origin, trimmed, &msg)) {
            return false;
        }
        out->push_back(std::move(msg));
        return true;
    }
    for (; it != end; ++it) {
        InboundMessage msg;
        msg.origin = origin;
        msg.raw = Trim(it->str(0));
        msg.from = (*it)[1].str();
        msg.type = (*it)[2].str();
        msg.packet = Trim((*it)[3].str());
        out->push_back(std::move(msg));
    }
    return !out->empty();
}

void ApplyInboxMessage(const InboundMessage& msg) {
    SetDataRefString(g_dref.poll_message_origin, msg.origin);
    SetDataRefString(g_dref.poll_message_from, msg.from);
    SetDataRefString(g_dref.poll_message_type, msg.type);
    SetDataRefString(g_dref.poll_message_packet, msg.packet);
    SetDataRefString(g_dref.poll_queue, std::string("{\"") + msg.origin + "\":\"" + JsonEscape(msg.raw) + "\"}");
    SetDataRefBool(g_dref.poll_queue_clear, false);
    if (g_dref.poll_count) {
        XPLMSetDatai(g_dref.poll_count, XPLMGetDatai(g_dref.poll_count) + 1);
    }
    if (g_dref.voice_seq) {
        XPLMSetDatai(g_dref.voice_seq, XPLMGetDatai(g_dref.voice_seq) + 1);
    }
    if (g_dref.voice_text) {
        std::string voice = msg.packet.empty() ? msg.raw : msg.packet;
        SetDataRefString(g_dref.voice_text, voice);
    }
    std::string label = msg.origin == "response" ? "Response" : "Poll";
    if (!msg.from.empty() || !msg.type.empty()) {
        Log(LOG_INFO, label + " message from " + msg.from + " type " + msg.type);
    } else {
        Log(LOG_INFO, label + " message received.");
    }
}

void QueueInboundMessage(const std::string& origin, const std::string& raw) {
    std::vector<InboundMessage> messages;
    if (!BuildInboundMessages(origin, raw, &messages)) {
        return;
    }
    for (auto& msg : messages) {
        std::string payload = msg.packet.empty() ? msg.raw : msg.packet;
        LogWire("RX message origin=" + msg.origin
            + " from=" + msg.from
            + " type=" + msg.type
            + " payload=" + QuoteForLog(payload));
        if (InboxHasMessage()) {
            g_pendingInbox.push_back(std::move(msg));
            Log(LOG_INFO, "Inbox busy; queued " + origin + " message.");
            continue;
        }
        ApplyInboxMessage(msg);
    }
}

void DeliverQueuedInboxIfEmpty() {
    if (InboxHasMessage() || g_pendingInbox.empty()) {
        return;
    }
    InboundMessage msg = std::move(g_pendingInbox.front());
    g_pendingInbox.pop_front();
    ApplyInboxMessage(msg);
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
    using namespace std::chrono_literals;

    while (g_running.load()) {
        HttpJob job;
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            g_cv.wait_for(lock, 2s, [] { return !g_jobs.empty() || !g_running.load(); });
            if (!g_running.load()) {
                break;
            }
            if (g_jobs.empty()) {
                if (IsHoppiePassiveMode()) {
                    break;
                }
                continue;
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
    g_workerActive.store(false);
}

void DropQueuedHoppieJobs() {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::queue<HttpJob> kept;
    while (!g_jobs.empty()) {
        HttpJob job = g_jobs.front();
        g_jobs.pop();
        if (job.type == JobType::Update || job.type == JobType::UpdateScan) {
            kept.push(std::move(job));
        }
    }
    std::swap(g_jobs, kept);
}

void EnsureWorkerThread() {
    if (!g_running.load()) {
        return;
    }
    if (g_worker.joinable() && !g_workerActive.load()) {
        g_worker.join();
    }
    if (g_worker.joinable()) {
        return;
    }
    g_workerActive.store(true);
    g_worker = std::thread(WorkerLoop);
}

void EnqueueJob(const HttpJob& job) {
    EnsureWorkerThread();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_jobs.push(job);
    g_cv.notify_one();
}

void DrainResults(bool allowHoppieDelivery) {
    std::queue<HttpResult> results;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        std::swap(results, g_results);
    }
    while (!results.empty()) {
        HttpResult res = results.front();
        results.pop();
        if ((res.type == JobType::Send || res.type == JobType::Poll) && !allowHoppieDelivery) {
            if (res.type == JobType::Send) {
                g_sendPending = false;
            } else {
                g_pollPending = false;
            }
            continue;
        }
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
                LogWire("TX result http=" + std::to_string(res.httpCode)
                    + " body=" + QuoteForLog(res.response));
                Log(LOG_INFO, "Send ok: " + Trim(res.response));
                QueueInboundMessage("response", res.response);
            } else {
                SetLastError("Send failed: " + res.error);
                SetLastHttp("send: " + std::to_string(res.httpCode));
                LogWire("TX failed http=" + std::to_string(res.httpCode)
                    + " error=" + QuoteForLog(res.error));
            }
        } else if (res.type == JobType::Poll) {
            g_pollPending = false;
            double now = XPLMGetElapsedTime();
            if (res.ok) {
                bool okOnly = IsOkOnlyCaseInsensitive(res.response);
                bool wasCommEstablished = g_commEstablished;
                if (!g_commEstablished) {
                    Log(LOG_INFO, "Poll ok. Communication ready.");
                }
                // Any successful poll (including polls with messages) proves the link is usable.
                g_commEstablished = true;
                if (!okOnly) {
                    LogWire("RX poll http=" + std::to_string(res.httpCode)
                        + " body=" + QuoteForLog(res.response));
                }
                if (!okOnly || wasCommEstablished) {
                    QueueInboundMessage("poll", res.response);
                }
                SetLastError("");
                SetLastHttp("poll: " + std::to_string(res.httpCode));
            } else {
                SetLastError("Poll failed: " + res.error);
                SetLastHttp("poll: " + std::to_string(res.httpCode));
                LogWire("RX poll failed http=" + std::to_string(res.httpCode)
                    + " error=" + QuoteForLog(res.error));
            }
        } else if (res.type == JobType::UpdateScan) {
            g_updateScanPending = false;
            if (!res.ok) {
                LogAlways("Update scan failed: " + res.error);
                g_updatePlanValid = false;
                if (g_reloadRequested) {
                    g_reloadRequested = false;
                    if (g_updateConfig.enabled && !g_updateConfig.source.empty()) {
                        std::vector<std::string> popupLines;
                        popupLines.push_back("YAL update failed");
                        popupLines.push_back(res.error);
                        ShowUpdateError(popupLines, "update scan failed");
                    } else {
                        TriggerReload("update scan failed");
                    }
                }
                continue;
            }
            std::string summary = Trim(res.response);
            if (summary.empty()) {
                summary = "ok";
            }
            LogAlways("Update scan ok: " + summary);
            for (const auto& line : res.update_log_lines) {
                LogAlways("Update scan " + line);
            }
            if (res.update_log_omitted > 0) {
                std::string omitted =
                    "... " + std::to_string(res.update_log_omitted) + " more changes not listed.";
                LogAlways("Update scan " + omitted);
            }

            const size_t totalChanges = res.update_counts.copied + res.update_counts.deleted
                + res.update_counts.dir_ts + res.update_log_omitted;
            if (totalChanges == 0) {
                LogAlways("Update scan: no changes.");
                g_updatePlanValid = false;
                if (g_reloadRequested) {
                    g_reloadRequested = false;
                    TriggerReload("update no changes");
                }
                continue;
            }
            std::vector<std::string> popupLines;
            std::ostringstream header;
            header << "YAL update pending: " << res.update_counts.copied << " copy, "
                   << res.update_counts.deleted << " delete";
            if (res.update_counts.dir_ts > 0) {
                header << ", " << res.update_counts.dir_ts << " dir-ts";
            }
            if (res.update_log_omitted > 0) {
                header << " (+" << res.update_log_omitted << " more)";
            }
            popupLines.push_back(header.str());
            std::vector<std::string> treeLines = BuildUpdateHierarchyLines(res.update_log_lines);
            for (const auto& line : treeLines) {
                popupLines.push_back(line);
            }
            if (res.update_log_omitted > 0) {
                popupLines.push_back("... " + std::to_string(res.update_log_omitted)
                    + " more changes not listed.");
            }
            g_updatePlanValid = true;
            ShowUpdateConfirm(popupLines);
        } else if (res.type == JobType::Update) {
            g_updatePending = false;
            bool showResultPopup = g_updateShowResultPopup;
            g_updateShowResultPopup = false;
            std::vector<std::string> popupLines;
            if (res.ok) {
                std::string summary = Trim(res.response);
                if (summary.empty()) {
                    summary = "ok";
                }
                LogAlways("Update ok: " + summary);
            } else {
                LogAlways("Update failed: " + res.error);
                if (g_reloadAfterUpdate && g_updateConfig.enabled && !g_updateConfig.source.empty()) {
                    std::vector<std::string> errorLines;
                    errorLines.push_back("YAL update failed");
                    errorLines.push_back(res.error);
                    ShowUpdateError(errorLines, "update failed");
                    g_reloadAfterUpdate = false;
                    g_reloadAfterUpdateAt = 0.0;
                    showResultPopup = false;
                } else if (showResultPopup) {
                    popupLines.push_back("YAL update failed: " + res.error);
                }
            }
            for (const auto& line : res.update_log_lines) {
                LogAlways("Update " + line);
            }
            if (res.update_log_omitted > 0) {
                std::string omitted =
                    "... " + std::to_string(res.update_log_omitted) + " more changes not listed.";
                LogAlways("Update " + omitted);
            }

            if (showResultPopup && !popupLines.empty()) {
                ShowUpdatePopup(popupLines);
            }
            if (g_reloadAfterUpdate) {
                double now = XPLMGetElapsedTime();
                if (g_updatePopupVisible && g_updatePopupMode == UpdatePopupMode::Info) {
                    g_reloadAfterUpdateAt = g_updatePopupHideAt;
                } else {
                    g_reloadAfterUpdateAt = now;
                }
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

bool TriggerReload(const char* reason) {
    if (!g_reloadCmd) {
        g_reloadCmd = XPLMFindCommand("sasl/reload/YAL");
        if (!g_reloadCmd) {
            g_reloadCmd = XPLMFindCommand("sasl/reload/yal");
        }
        if (g_reloadCmd) {
            g_loggedReloadCmdMissing = false;
        }
    }
    if (!g_reloadCmd) {
        if (!g_loggedReloadCmdMissing) {
            g_loggedReloadCmdMissing = true;
            Log(LOG_INFO, "Reload command not found (sasl/reload/YAL or sasl/reload/yal).");
        }
        return false;
    }
    std::string msg = "Reload requested";
    if (reason && *reason) {
        msg += " (" + std::string(reason) + ")";
    }
    Log(LOG_INFO, msg);
    XPLMCommandOnce(g_reloadCmd);
    return true;
}

bool HandleReloadRequest() {
    if (!g_dref.reload_request) {
        g_dref.reload_request = XPLMFindDataRef("YAL/command/reload");
        if (!g_dref.reload_request) {
            g_dref.reload_request = XPLMFindDataRef("yal/command/reload");
        }
        if (g_dref.reload_request) {
            g_loggedReloadDatarefMissing = false;
        }
    }
    if (!g_dref.reload_request) {
        if (!g_loggedReloadDatarefMissing) {
            g_loggedReloadDatarefMissing = true;
            Log(LOG_DBG, "Missing dataref: YAL/command/reload");
        }
        return false;
    }
    int request = XPLMGetDatai(g_dref.reload_request);
    if (request == 0) {
        return false;
    }
    XPLMSetDatai(g_dref.reload_request, 0);
    if (g_updateConfig.enabled && !g_updateConfig.source.empty()) {
        if (g_updateScanPending || g_updateDecisionPending || g_updatePending || g_updateErrorPending) {
            g_reloadRequested = true;
            if (g_updateErrorPending) {
                Log(LOG_INFO, "Reload requested while update error pending; will reload after acknowledgement.");
            } else {
                Log(LOG_INFO, "Reload requested while update pending; will reload after update.");
            }
            return true;
        }
        std::filesystem::path target = GetYalPluginPath();
        if (target.empty()) {
            Log(LOG_ERR, "Update skipped: YAL plugin path not found.");
            return TriggerReload("update skipped");
        }
        HttpJob job;
        job.type = JobType::UpdateScan;
        job.update_source = g_updateConfig.source;
        job.update_target = target.string();
        job.update_excludes = g_updateConfig.excludes;
        job.update_log_details = true;
        job.update_log_limit = kUpdateLogMaxEntries;
        EnqueueJob(job);
        g_updateScanPending = true;
        g_reloadRequested = true;
        g_updatePlanConfig = g_updateConfig;
        g_updatePlanTarget = target.string();
        g_updatePlanValid = true;
        Log(LOG_INFO, "Update scan scheduled from " + g_updateConfig.source);
        return true;
    }
    return TriggerReload("dataref");
}

float FlightLoopCallback(float, float, int, void*) {
    double now = XPLMGetElapsedTime();
    UpdatePopupVisibility(now);
    if (g_updateDecisionPending && g_updatePopupMode == UpdatePopupMode::Confirm
        && now >= g_updateDecisionDeadline) {
        HandleUpdateDecision(true, "timeout");
    }
    if (g_reloadAfterUpdate && g_reloadAfterUpdateAt > 0.0 && now >= g_reloadAfterUpdateAt) {
        g_reloadAfterUpdate = false;
        g_reloadAfterUpdateAt = 0.0;
        TriggerReload("post-update");
    }
    AutarkUpdate autarkUpdate = UpdateAutarkConfig(now);
    bool passiveChanged = UpdateZiboHoppiePassiveMode(now);
    RefreshDebugLevel();

    if (passiveChanged && IsHoppiePassiveMode()) {
        DropQueuedHoppieJobs();
        ResetHoppieRuntimeState(now, false);
        UnregisterHoppieDataRefs();
        FindDataRefs();
        g_cv.notify_all();
    }
    if (passiveChanged && !IsHoppiePassiveMode()) {
        ResetHoppieRuntimeState(now, false);
        FindDataRefs();
    }
    if (autarkUpdate.modeChanged) {
        ResetHoppieRuntimeState(now, !IsHoppiePassiveMode());
        g_nextPollTime = 0.0;
        g_autarkAppliedCallsign.clear();
        Log(LOG_INFO, g_autarkMode
            ? "Autark mode enabled (prefs file found)."
            : "Autark mode disabled (prefs file missing).");
    }
    if (g_autarkMode && autarkUpdate.logonChanged && !IsHoppiePassiveMode()) {
        g_commEstablished = false;
        SetDataRefBool(g_dref.comm_ready, false);
        g_nextPollTime = 0.0;
        g_logonKnown = false;
        g_logonAvailable = false;
    }
    if (!IsHoppiePassiveMode()) {
        ApplyAutarkPollFastOverride();
    }

    if (HandleReloadRequest()) {
        return kFlightLoopInterval;
    }
    if (IsHoppiePassiveMode()) {
        DrainResults(false);
        return kFlightLoopInterval;
    }

    bool ziboReady = UpdateZiboState();
    if (!ziboReady) {
        if (g_refsReady || g_commEstablished || g_sendPending || g_pollPending || !g_pendingInbox.empty()) {
            ResetHoppieRuntimeState(now, true);
        }
        RefreshHbdrReady(false);
        DrainResults(false);
        SetDataRefBool(g_dref.comm_ready, false);
        SetStatus("WAIT_ZIBO");
        return kFlightLoopInterval;
    }

    RefreshDataRefs(now);
    RefreshHbdrReady(g_refsReady);
    if (!g_refsReady) {
        DrainResults(false);
        SetDataRefBool(g_dref.comm_ready, false);
        SetStatus("WAIT_YAL");
        return kFlightLoopInterval;
    }

    DrainResults(true);
    ClearInboxIfRequested();
    DeliverQueuedInboxIfEmpty();

    std::string logon = GetActiveLogon();
    std::string callsign = Trim(GetDataRefString(g_dref.callsign));

    LogLogonState(logon);
    bool callsignUpdated = UpdateCallsign();
    callsign = Trim(GetDataRefString(g_dref.callsign));
    bool callsignAutark = ApplyAutarkCallsign(&callsign);
    LogCallsignState(callsign, callsignUpdated || callsignAutark);

    UpdateCommReady(logon, callsign);
    bool commReady = GetDataRefBool(g_dref.comm_ready);
    LogCommReadyState(commReady);
    LogPollModeChange();
    bool prereqsOk = AreCommPrereqsReady(logon, callsign, nullptr);

    if (!g_sendPending && !g_updatePending && commReady) {
        std::string to = Trim(GetDataRefString(g_dref.send_message_to));
        std::string type = Trim(GetDataRefString(g_dref.send_message_type));
        std::string packet = Trim(GetDataRefString(g_dref.send_message_packet));
        bool usedLegacySendQueue = false;
        bool structuredReady = !to.empty() && !type.empty() && !packet.empty();
        bool structuredPartial = !to.empty() || !type.empty() || !packet.empty();
        if (!structuredReady) {
            if (structuredPartial) {
                Log(LOG_DBG, "Ignoring incomplete structured message fields; falling back to send_queue.");
            }
            std::string legacyRaw = Trim(GetDataRefString(g_dref.send_queue));
            if (!legacyRaw.empty()) {
                LegacyOutboxMessage legacy{};
                if (ParseLegacyOutboxMessage(legacyRaw, &legacy)) {
                    to = legacy.to;
                    type = legacy.type;
                    packet = legacy.packet;
                    structuredReady = true;
                    usedLegacySendQueue = true;
                    g_lastBadSendQueue.clear();
                } else if (legacyRaw != g_lastBadSendQueue) {
                    g_lastBadSendQueue = legacyRaw;
                    Log(LOG_ERR, "send_queue parse failed; ignoring legacy payload.");
                }
            }
        }
        if (structuredReady) {
            HttpJob job;
            job.type = JobType::Send;
            job.logon = logon;
            job.from = callsign;
            job.to = to;
            job.msg_type = type;
            job.packet = packet;
            LogWire("TX enqueue from=" + callsign
                + " to=" + to
                + " type=" + type
                + " source=" + (usedLegacySendQueue ? "send_queue" : "structured")
                + " packet=" + QuoteForLog(packet));
            EnqueueJob(job);
            g_sendPending = true;
            ScheduleNextPollTime(now);
        }
    }

    bool jobPending = g_sendPending || g_pollPending || g_updatePending;
    if (!jobPending && prereqsOk && (!commReady || now >= g_nextPollTime)) {
        HttpJob job;
        job.type = JobType::Poll;
        job.logon = logon;
        job.from = callsign;
        job.to = callsign;
        job.msg_type = "poll";
        EnqueueJob(job);
        g_pollPending = true;
        ScheduleNextPollTime(now);
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
    g_dref.voice_seq = XPLMFindDataRef("YAL/hoppie/voice_seq");
    g_dref.voice_text = XPLMFindDataRef("YAL/hoppie/voice_text");
    g_dref.reload_request = XPLMFindDataRef("YAL/command/reload");
    if (!g_dref.reload_request) {
        g_dref.reload_request = XPLMFindDataRef("yal/command/reload");
    }
    g_dref.hbdr_ready = XPLMFindDataRef("laminar/B738/HBDR_ready");
    g_dref.avionics_on = XPLMFindDataRef("sim/cockpit/electrical/avionics_on");
    g_dref.tailnum = XPLMFindDataRef("sim/aircraft/view/acf_tailnum");

    if (!g_dref.logon && !g_autarkMode && !IsHoppiePassiveMode()) {
        Log(LOG_DBG, "Missing dataref: YAL/hoppie/logon");
    }
}

bool HasCoreDataRefs() {
    if (!g_autarkMode && !g_dref.logon) {
        return false;
    }
    return g_dref.comm_ready
        && g_dref.send_message_to
        && g_dref.send_message_type
        && g_dref.send_message_packet
        && g_dref.poll_message_packet
        && g_dref.poll_queue_clear
        && g_dref.callsign
        && g_dref.send_callsign;
}

void RefreshHbdrReady(bool allowReady) {
    XPLMDataRef ref = XPLMFindDataRef("laminar/B738/HBDR_ready");
    if (ref != g_dref.hbdr_ready) {
        g_dref.hbdr_ready = ref;
        g_loggedHbdrFound = false;
        g_loggedHbdrWritable = false;
        g_loggedHbdrTypes = false;
    }
    if (!g_dref.hbdr_ready) {
        return;
    }
    if (!g_loggedHbdrFound) {
        g_loggedHbdrFound = true;
        Log(LOG_INFO, "Found HBDR_ready dataref.");
    }
    if (!g_loggedHbdrTypes) {
        g_loggedHbdrTypes = true;
        Log(LOG_INFO, "HBDR_ready types: " + DataRefTypeString(XPLMGetDataRefTypes(g_dref.hbdr_ready)));
    }
    if (!g_loggedHbdrWritable && !XPLMCanWriteDataRef(g_dref.hbdr_ready)) {
        g_loggedHbdrWritable = true;
        Log(LOG_ERR, "HBDR_ready dataref is not writable.");
    }
    SetDataRefBool(g_dref.hbdr_ready, allowReady);
}

void RefreshDataRefs(double now) {
    if (IsHoppiePassiveMode()) {
        g_refsReady = false;
        g_nextRefScanTime = now + 2.0;
        return;
    }
    if (g_refsReady) {
        if (!HasCoreDataRefs()) {
            ResetHoppieRuntimeState(now, true);
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
        g_nextPollTime = 0.0;
        g_loggedMissingRefs = false;
        Log(LOG_INFO, "Required datarefs found. Helper ready.");
        LogReadySummary();
    } else if (!g_loggedMissingRefs) {
        g_loggedMissingRefs = true;
        Log(LOG_INFO, g_autarkMode ? "Waiting for core datarefs..." : "Waiting for YAL datarefs...");
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
    double now = XPLMGetElapsedTime();
    g_running.store(true);
    g_workerActive.store(false);
    g_nextPollTime = 0.0;
    g_nextPrefScanTime = 0.0;
    g_nextZiboHoppiePrefScanTime = 0.0;
    g_autarkMode = false;
    g_autarkConfig = AutarkConfig{};
    g_autarkAppliedCallsign.clear();
    g_hoppiePassiveMode.store(false);
    AutarkUpdate autarkUpdate = UpdateAutarkConfig(now);
    UpdateZiboHoppiePassiveMode(now);
    if (!IsHoppiePassiveMode()) {
        EnsureHoppieDataRefs();
    } else {
        UnregisterHoppieDataRefs();
    }
    FindDataRefs();
    g_debugLevelSource.clear();
    RefreshDebugLevel();
    if (autarkUpdate.modeChanged && g_autarkMode) {
        Log(LOG_INFO, "Autark mode enabled (prefs file found).");
    }
    if (!IsHoppiePassiveMode()) {
        ApplyAutarkPollFastOverride();
    }
    g_refsReady = !IsHoppiePassiveMode() && HasCoreDataRefs();
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
    g_pendingInbox.clear();
    g_ziboKnown = false;
    g_lastZiboReady = false;
    g_ziboPluginKnown = false;
    g_lastZiboPluginPresent = false;
    g_tailnumKnown = false;
    g_lastTailnumMatches = false;
    g_lastTailnum.clear();
    if (IsHoppiePassiveMode()) {
        DropQueuedHoppieJobs();
    } else if (!g_refsReady) {
        g_loggedMissingRefs = true;
        Log(LOG_INFO, g_autarkMode ? "Waiting for core datarefs..." : "Waiting for YAL datarefs...");
    } else {
        Log(LOG_INFO, "Required datarefs found. Helper ready.");
        LogReadySummary();
    }
    if (!IsHoppiePassiveMode()) {
        RefreshHbdrReady(g_refsReady);
    }
    XPLMRegisterFlightLoopCallback(FlightLoopCallback, kFlightLoopInterval, nullptr);
    g_commEstablished = false;
    if (!IsHoppiePassiveMode()) {
        SetDataRefBool(g_dref.comm_ready, false);
    }
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
    bool passive = IsHoppiePassiveMode();
    XPLMUnregisterFlightLoopCallback(FlightLoopCallback, nullptr);
    g_running.store(false);
    g_cv.notify_all();
    if (g_worker.joinable()) {
        g_worker.join();
    }
    g_workerActive.store(false);
    g_commEstablished = false;
    if (!passive) {
        SetDataRefBool(g_dref.comm_ready, false);
        SetDataRefBool(g_dref.hbdr_ready, false);
    }
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
    g_pendingInbox.clear();
    g_ziboKnown = false;
    g_lastZiboReady = false;
    g_ziboPluginKnown = false;
    g_lastZiboPluginPresent = false;
    g_tailnumKnown = false;
    g_lastTailnumMatches = false;
    g_lastTailnum.clear();
    g_autarkMode = false;
    g_autarkConfig = AutarkConfig{};
    g_autarkAppliedCallsign.clear();
    g_nextPrefScanTime = 0.0;
    g_nextZiboHoppiePrefScanTime = 0.0;
    g_hoppiePassiveMode.store(false);
    if (g_updateWindow) {
        SaveUpdateWindowPosition();
        XPLMDestroyWindow(g_updateWindow);
        g_updateWindow = nullptr;
    }
    g_updatePopupVisible = false;
    g_updatePopupHideAt = 0.0;
    g_updatePopupLines.clear();
    g_updatePopupMode = UpdatePopupMode::None;
    g_updateButtonsValid = false;
    g_updatePopupButtonArea = 0;
    g_updateDecisionPending = false;
    g_updateDecisionDeadline = 0.0;
    g_updateErrorPending = false;
    g_reloadAfterErrorAck = false;
    g_updateErrorReloadReason.clear();
    g_updateScanPending = false;
    g_reloadRequested = false;
    g_updateShowResultPopup = false;
    g_updatePlanValid = false;
    SetStatus("DISABLED");
    Log(LOG_INFO, "Disabled");
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int, void*) {}
