#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include "Engine/Loader/API/AddonAPI.h"
#include "Engine/Loader/AddonDefinition.h"
#include "Engine/Loader/AddonVersion.h"
#include "Engine/Loader/EAddonFlags.h"
#include "Engine/Loader/EUpdateProvider.h"
#include "Engine/Logging/LogEnum.h"
#include "UI/ERenderType.h"

namespace {

using json = nlohmann::json;

constexpr int kNexusApiVersion = 6;
constexpr signed int kAddonSignature = -486251921;
constexpr const char* kAddonName = "GW2 Story Times";
constexpr const char* kAddonAuthor = "MattyGroch";
constexpr const char* kLogChannel = "GW2StoryTimes";
constexpr const char* kToggleBindIdentifier = "GW2ST_TOGGLE_WINDOW";
constexpr const char* kUpdateLink = "https://github.com/MattyGroch/gw2-storytimes-nexus";
constexpr const wchar_t* kApiHost = L"api.gw2storytimes.com";
constexpr const wchar_t* kUserAgent = L"GW2StoryTimes-Nexus/0.1.2";

struct TimeEstimate {
    double averageMinutes = 0.0;
    int submissions = 0;

    [[nodiscard]] std::string Format() const {
        if (averageMinutes <= 0.0) {
            return "No estimate";
        }

        if (averageMinutes < 60.0) {
            std::ostringstream stream;
            stream << "~" << static_cast<int>(averageMinutes + 0.5) << " min";
            return stream.str();
        }

        const auto hours = static_cast<int>(averageMinutes / 60.0);
        const auto minutes = static_cast<int>(averageMinutes) % 60;

        std::ostringstream stream;
        stream << "~" << hours << "h";
        if (minutes > 0) {
            stream << " " << minutes << "m";
        }

        return stream.str();
    }
};

struct Mission {
    int id = 0;
    int order = 0;
    int storyOrder = 0;
    std::string seasonId;
    std::string seasonName;
    std::string storyName;
    std::string groupName;
    std::string name;
    std::optional<TimeEstimate> fullEstimate;
    std::optional<TimeEstimate> speedEstimate;

    [[nodiscard]] std::string Breadcrumb() const {
        return seasonName + " > " + storyName;
    }
};

struct Season {
    std::string id;
    std::string name;
    int order = 0;
    int missionCount = 0;
    double totalFullMinutes = 0.0;
    double totalSpeedMinutes = 0.0;
    bool detailsLoaded = false;
    bool isLoading = false;
    std::string errorText;
    std::vector<Mission> missions;
};

class TimerState {
public:
    void Start() {
        if (running_) {
            return;
        }

        running_ = true;
        startedAt_ = clock::now();
    }

    void Stop() {
        if (!running_) {
            return;
        }

        accumulated_ += std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - startedAt_);
        running_ = false;
    }

    void Reset() {
        accumulated_ = std::chrono::milliseconds::zero();
        running_ = false;
        startedAt_ = {};
    }

    [[nodiscard]] bool IsRunning() const {
        return running_;
    }

    [[nodiscard]] std::chrono::milliseconds Elapsed() const {
        if (!running_) {
            return accumulated_;
        }

        return accumulated_ + std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - startedAt_);
    }

    [[nodiscard]] std::string FormattedElapsed() const {
        const auto elapsed = Elapsed();
        const auto totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        const auto hours = totalSeconds / 3600;
        const auto minutes = (totalSeconds % 3600) / 60;
        const auto seconds = totalSeconds % 60;

        std::ostringstream stream;
        if (hours > 0) {
            stream << hours << ":";
        }

        if (hours > 0 && minutes < 10) {
            stream << '0';
        }
        stream << minutes << ":";
        if (seconds < 10) {
            stream << '0';
        }
        stream << seconds;

        return stream.str();
    }

private:
    using clock = std::chrono::steady_clock;

    bool running_ = false;
    clock::time_point startedAt_{};
    std::chrono::milliseconds accumulated_{0};
};

struct AppState {
    bool widgetVisible = true;
    bool browserVisible = false;
    bool showFeedbackPrompt = true;
    bool useSpeedCategory = false;
    bool submitPromptOpen = false;
    bool seasonsLoading = false;
    bool submitting = false;
    int selectedSeasonIndex = 0;
    int selectedMissionIndex = -1;
    std::string statusText = "Loading story data...";
    std::string missionSearch;
    std::string seasonsError;
    std::vector<Season> seasons;
    std::optional<Mission> activeMission;
    TimerState timer;
};

struct HttpResponse {
    bool success = false;
    int statusCode = 0;
    std::string body;
    std::string error;
};

AddonAPI6_t* g_api = nullptr;
AppState g_state;
std::recursive_mutex g_stateMutex;
std::vector<std::jthread> g_workers;

std::wstring ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}

std::string ToLower(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string ExtractApiErrorMessage(const HttpResponse& response) {
    if (!response.body.empty()) {
        const auto parsed = json::parse(response.body, nullptr, false);
        if (parsed.is_object()) {
            if (parsed.contains("error") && parsed["error"].is_string()) {
                return parsed["error"].get<std::string>();
            }
            if (parsed.contains("message") && parsed["message"].is_string()) {
                return parsed["message"].get<std::string>();
            }
        }
    }

    if (!response.error.empty()) {
        return response.error;
    }

    std::ostringstream stream;
    stream << "HTTP " << response.statusCode;
    return stream.str();
}

double JsonNumber(const json& value, const char* key, double fallback = 0.0) {
    if (!value.contains(key) || value[key].is_null()) {
        return fallback;
    }
    if (value[key].is_number_float()) {
        return value[key].get<double>();
    }
    if (value[key].is_number_integer() || value[key].is_number_unsigned()) {
        return static_cast<double>(value[key].get<long long>());
    }
    return fallback;
}

int JsonInt(const json& value, const char* key, int fallback = 0) {
    if (!value.contains(key) || value[key].is_null()) {
        return fallback;
    }
    if (value[key].is_number_integer() || value[key].is_number_unsigned()) {
        return value[key].get<int>();
    }
    if (value[key].is_number_float()) {
        return static_cast<int>(value[key].get<double>());
    }
    return fallback;
}

std::string JsonString(const json& value, const char* key, std::string fallback = {}) {
    if (!value.contains(key) || value[key].is_null() || !value[key].is_string()) {
        return fallback;
    }
    return value[key].get<std::string>();
}

void Log(ELogLevel level, std::string_view message) {
    if (g_api == nullptr || g_api->Log == nullptr) {
        return;
    }

    g_api->Log(level, kLogChannel, message.data());
}

void EnsureImGuiBound() {
    if (g_api == nullptr || g_api->ImguiContext == nullptr) {
        return;
    }

    ImGui::SetAllocatorFunctions(
        reinterpret_cast<void* (*)(size_t, void*)>(g_api->ImguiMalloc),
        reinterpret_cast<void (*)(void*, void*)>(g_api->ImguiFree),
        nullptr);
    ImGui::SetCurrentContext(g_api->ImguiContext);
}

HttpResponse PerformRequest(const wchar_t* method, const std::wstring& path, const std::string& body = {}, const wchar_t* contentType = nullptr) {
    HttpResponse response;

    const auto session = WinHttpOpen(kUserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) {
        response.error = "WinHttpOpen failed.";
        return response;
    }

    const auto connection = WinHttpConnect(session, kApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (connection == nullptr) {
        response.error = "WinHttpConnect failed.";
        WinHttpCloseHandle(session);
        return response;
    }

    const auto request = WinHttpOpenRequest(
        connection,
        method,
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);

    if (request == nullptr) {
        response.error = "WinHttpOpenRequest failed.";
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    WinHttpSetTimeouts(request, 5000, 5000, 10000, 10000);

    std::wstring headers = L"Accept: application/json\r\n";
    if (contentType != nullptr) {
        headers += L"Content-Type: ";
        headers += contentType;
        headers += L"\r\n";
    }

    const BOOL sent = WinHttpSendRequest(
        request,
        headers.c_str(),
        static_cast<DWORD>(headers.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
        static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()),
        0);

    if (!sent || !WinHttpReceiveResponse(request, nullptr)) {
        response.error = "HTTP request failed.";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        return response;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
    response.statusCode = static_cast<int>(statusCode);

    std::string bodyBuffer;
    DWORD availableBytes = 0;
    while (WinHttpQueryDataAvailable(request, &availableBytes) && availableBytes > 0) {
        std::string chunk(availableBytes, '\0');
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request, chunk.data(), availableBytes, &bytesRead)) {
            response.error = "Failed reading response body.";
            break;
        }

        chunk.resize(bytesRead);
        bodyBuffer += chunk;
        availableBytes = 0;
    }

    response.body = std::move(bodyBuffer);
    response.success = response.error.empty() && response.statusCode >= 200 && response.statusCode < 300;

    if (!response.success && response.error.empty()) {
        std::ostringstream stream;
        stream << "HTTP " << response.statusCode;
        response.error = stream.str();
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return response;
}

std::optional<TimeEstimate> ParseEstimate(const json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }

    TimeEstimate estimate;
    estimate.averageMinutes = JsonNumber(value, "avg_mins", 0.0);
    if (estimate.averageMinutes <= 0.0) {
        estimate.averageMinutes = JsonNumber(value, "seed_mins", 0.0);
    }
    estimate.submissions = JsonInt(value, "submissions", 0);
    if (estimate.averageMinutes <= 0.0 && estimate.submissions == 0) {
        return std::nullopt;
    }

    return estimate;
}

Mission ParseMission(const json& value, const std::string& seasonId, const std::string& seasonName, const std::string& storyName, int storyOrder) {
    Mission mission;
    mission.id = JsonInt(value, "id", 0);
    mission.order = JsonInt(value, "order", 0);
    mission.storyOrder = storyOrder;
    mission.seasonId = seasonId;
    mission.seasonName = seasonName;
    mission.storyName = JsonString(value, "story_name", storyName);
    mission.groupName = JsonString(value, "group_name", "");
    mission.name = JsonString(value, "name", "Unknown mission");

    if (value.contains("times") && value["times"].is_object()) {
        const auto& times = value["times"];
        if (times.contains("full")) {
            mission.fullEstimate = ParseEstimate(times["full"]);
        }
        if (times.contains("speed")) {
            mission.speedEstimate = ParseEstimate(times["speed"]);
        }
    }

    return mission;
}

Season ParseSeasonSummary(const json& value) {
    Season season;
    season.id = JsonString(value, "id", "");
    season.name = JsonString(value, "name", "Unknown season");
    season.order = JsonInt(value, "order", 0);
    season.missionCount = JsonInt(value, "mission_count", 0);
    season.totalFullMinutes = JsonNumber(value, "total_full_mins", 0.0);
    season.totalSpeedMinutes = JsonNumber(value, "total_speed_mins", 0.0);
    return season;
}

Season ParseSeasonDetail(const json& value) {
    Season season = ParseSeasonSummary(value);
    season.detailsLoaded = true;

    if (value.contains("stories") && value["stories"].is_array()) {
        for (const auto& story : value["stories"]) {
            const std::string storyName = JsonString(story, "name", "Unknown story");
            const int storyOrder = JsonInt(story, "order", 0);
            if (!story.contains("missions") || !story["missions"].is_array()) {
                continue;
            }

            for (const auto& missionValue : story["missions"]) {
                season.missions.push_back(ParseMission(missionValue, season.id, season.name, storyName, storyOrder));
            }
        }
    }

    std::sort(season.missions.begin(), season.missions.end(), [](const Mission& lhs, const Mission& rhs) {
        if (lhs.storyOrder != rhs.storyOrder) {
            return lhs.storyOrder < rhs.storyOrder;
        }
        if (lhs.order != rhs.order) {
            return lhs.order < rhs.order;
        }
        return lhs.name < rhs.name;
    });

    return season;
}

Mission* FindMissionById(std::vector<Season>& seasons, int missionId) {
    for (auto& season : seasons) {
        for (auto& mission : season.missions) {
            if (mission.id == missionId) {
                return &mission;
            }
        }
    }

    return nullptr;
}

std::optional<TimeEstimate> ActiveEstimate() {
    if (!g_state.activeMission.has_value()) {
        return std::nullopt;
    }

    return g_state.useSpeedCategory ? g_state.activeMission->speedEstimate : g_state.activeMission->fullEstimate;
}

std::string BuildPacingText(const std::optional<TimeEstimate>& estimate, std::chrono::milliseconds elapsed) {
    if (!estimate.has_value() || estimate->averageMinutes <= 0.0) {
        return "Timer running without community estimate.";
    }

    const auto elapsedMinutes = static_cast<double>(elapsed.count()) / 60000.0;
    const auto deltaMinutes = elapsedMinutes - estimate->averageMinutes;
    const auto deltaPercent = estimate->averageMinutes <= 0.0 ? 0.0 : std::abs(deltaMinutes) / estimate->averageMinutes;

    std::ostringstream stream;
    if (deltaPercent <= 0.10) {
        stream << "On target";
    } else if (deltaPercent <= 0.25) {
        stream << "Slightly off";
    } else {
        stream << "Well outside estimate";
    }

    stream << " (" << (deltaMinutes >= 0.0 ? "behind" : "ahead") << " by " << static_cast<int>(std::abs(deltaMinutes) + 0.5) << " min)";
    return stream.str();
}

ImVec4 BuildPacingColor(const std::optional<TimeEstimate>& estimate, std::chrono::milliseconds elapsed) {
    if (!estimate.has_value() || estimate->averageMinutes <= 0.0) {
        return ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
    }

    const auto elapsedMinutes = static_cast<double>(elapsed.count()) / 60000.0;
    const auto deltaPercent = std::abs(elapsedMinutes - estimate->averageMinutes) / estimate->averageMinutes;

    if (deltaPercent <= 0.10) {
        return ImVec4(0.34f, 0.77f, 0.32f, 1.0f);
    }

    if (deltaPercent <= 0.25) {
        return ImVec4(0.89f, 0.73f, 0.20f, 1.0f);
    }

    return ImVec4(0.87f, 0.30f, 0.28f, 1.0f);
}

void StartWorker(std::jthread worker) {
    g_workers.push_back(std::move(worker));
}

void SelectMission(const Mission& mission) {
    g_state.activeMission = mission;
    g_state.timer.Reset();
    g_state.submitPromptOpen = false;
    g_state.browserVisible = false;
    g_state.statusText = "Selected " + mission.name + ".";
}

void FetchSeasonsAsync() {
    {
        const std::lock_guard lock(g_stateMutex);
        g_state.seasonsLoading = true;
        g_state.seasonsError.clear();
        g_state.statusText = "Loading seasons from GW2 Story Times...";
    }

    StartWorker(std::jthread([](std::stop_token stopToken) {
        try {
            const auto response = PerformRequest(L"GET", ToWide("/v1/seasons"));
            if (stopToken.stop_requested()) {
                return;
            }

            if (!response.success) {
                const std::lock_guard lock(g_stateMutex);
                g_state.seasonsLoading = false;
                g_state.seasonsError = response.error;
                g_state.statusText = "Failed to load seasons.";
                return;
            }

            const auto parsed = json::parse(response.body, nullptr, false);
            if (!parsed.is_array()) {
                const std::lock_guard lock(g_stateMutex);
                g_state.seasonsLoading = false;
                g_state.seasonsError = "Unexpected seasons payload.";
                g_state.statusText = "Failed to parse season list.";
                return;
            }

            std::vector<Season> seasons;
            for (const auto& item : parsed) {
                seasons.push_back(ParseSeasonSummary(item));
            }

            std::sort(seasons.begin(), seasons.end(), [](const Season& lhs, const Season& rhs) {
                return lhs.order < rhs.order;
            });

            const std::lock_guard lock(g_stateMutex);
            g_state.seasons = std::move(seasons);
            g_state.seasonsLoading = false;
            g_state.seasonsError.clear();
            g_state.selectedSeasonIndex = 0;
            g_state.selectedMissionIndex = -1;
            g_state.statusText = g_state.seasons.empty() ? "No seasons returned from API." : "Choose a mission to begin.";
        } catch (const std::exception& ex) {
            const std::lock_guard lock(g_stateMutex);
            g_state.seasonsLoading = false;
            g_state.seasonsError = ex.what();
            g_state.statusText = "Season load failed.";
        }
    }));
}

void FetchSeasonDetailAsync(const std::string& seasonId) {
    {
        const std::lock_guard lock(g_stateMutex);
        auto it = std::find_if(g_state.seasons.begin(), g_state.seasons.end(), [&](const Season& season) { return season.id == seasonId; });
        if (it == g_state.seasons.end() || it->isLoading || it->detailsLoaded) {
            return;
        }

        it->isLoading = true;
        it->errorText.clear();
    }

    StartWorker(std::jthread([seasonId](std::stop_token stopToken) {
        try {
            const auto response = PerformRequest(L"GET", ToWide("/v1/seasons/" + seasonId));
            if (stopToken.stop_requested()) {
                return;
            }

            if (!response.success) {
                const std::lock_guard lock(g_stateMutex);
                auto it = std::find_if(g_state.seasons.begin(), g_state.seasons.end(), [&](const Season& season) { return season.id == seasonId; });
                if (it != g_state.seasons.end()) {
                    it->isLoading = false;
                    it->errorText = response.error;
                }
                g_state.statusText = "Failed to load missions for season.";
                return;
            }

            const auto parsed = json::parse(response.body, nullptr, false);
            if (!parsed.is_object()) {
                const std::lock_guard lock(g_stateMutex);
                auto it = std::find_if(g_state.seasons.begin(), g_state.seasons.end(), [&](const Season& season) { return season.id == seasonId; });
                if (it != g_state.seasons.end()) {
                    it->isLoading = false;
                    it->errorText = "Unexpected season payload.";
                }
                g_state.statusText = "Failed to parse mission list.";
                return;
            }

            Season detail = ParseSeasonDetail(parsed);

            const std::lock_guard lock(g_stateMutex);
            auto it = std::find_if(g_state.seasons.begin(), g_state.seasons.end(), [&](const Season& season) { return season.id == seasonId; });
            if (it == g_state.seasons.end()) {
                return;
            }

            it->missions = std::move(detail.missions);
            it->detailsLoaded = true;
            it->isLoading = false;
            it->errorText.clear();
            it->missionCount = detail.missionCount;
            it->totalFullMinutes = detail.totalFullMinutes;
            it->totalSpeedMinutes = detail.totalSpeedMinutes;

            if (g_state.activeMission.has_value()) {
                if (auto* refreshedMission = FindMissionById(g_state.seasons, g_state.activeMission->id); refreshedMission != nullptr) {
                    g_state.activeMission = *refreshedMission;
                }
            }
        } catch (const std::exception& ex) {
            const std::lock_guard lock(g_stateMutex);
            auto it = std::find_if(g_state.seasons.begin(), g_state.seasons.end(), [&](const Season& season) { return season.id == seasonId; });
            if (it != g_state.seasons.end()) {
                it->isLoading = false;
                it->errorText = ex.what();
            }
            g_state.statusText = "Mission load failed.";
        }
    }));
}

void SubmitTimeAsync(const std::string& category) {
    if (!g_state.activeMission.has_value() || g_state.submitting) {
        return;
    }

    const Mission mission = *g_state.activeMission;
    const double durationMinutes = static_cast<double>(g_state.timer.Elapsed().count()) / 60000.0;

    g_state.submitting = true;
    g_state.statusText = "Submitting time...";

    StartWorker(std::jthread([mission, durationMinutes, category](std::stop_token stopToken) {
        try {
            json payload = {
                {"category", category},
                {"duration_mins", std::round(durationMinutes * 100.0) / 100.0},
                {"source", "nexus"}
            };

            const auto response = PerformRequest(
                L"POST",
                ToWide("/v1/missions/" + std::to_string(mission.id) + "/submit"),
                payload.dump(),
                L"application/json");

            if (stopToken.stop_requested()) {
                return;
            }

            const std::lock_guard lock(g_stateMutex);
            g_state.submitting = false;

            if (!response.success) {
                g_state.statusText = "Submission failed: " + ExtractApiErrorMessage(response);
                return;
            }

            g_state.submitPromptOpen = false;
            g_state.statusText = "Time submitted successfully.";

            if (g_api != nullptr && g_api->UI.SendAlert != nullptr) {
                g_api->UI.SendAlert("GW2 Story Times submission sent.");
            }

            if (!mission.seasonId.empty()) {
                for (auto& season : g_state.seasons) {
                    if (season.id == mission.seasonId) {
                        season.detailsLoaded = false;
                        season.missions.clear();
                        season.errorText.clear();
                        break;
                    }
                }
            }
        } catch (const std::exception& ex) {
            const std::lock_guard lock(g_stateMutex);
            g_state.submitting = false;
            g_state.statusText = std::string("Submission crashed: ") + ex.what();
        }
    }));
}

bool MissionMatchesFilter(const Mission& mission, const std::string& filter) {
    if (filter.empty()) {
        return true;
    }

    const std::string loweredFilter = ToLower(filter);
    return ToLower(mission.name).find(loweredFilter) != std::string::npos ||
        ToLower(mission.storyName).find(loweredFilter) != std::string::npos ||
        ToLower(mission.seasonName).find(loweredFilter) != std::string::npos;
}

void RenderFeedbackPrompt() {
    if (!g_state.submitPromptOpen || !g_state.activeMission.has_value()) {
        return;
    }

    if (ImGui::BeginPopupModal("Submit Time", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Mission: %s", g_state.activeMission->name.c_str());
        ImGui::Text("Elapsed: %s", g_state.timer.FormattedElapsed().c_str());
        ImGui::TextWrapped("Choose how this run should be categorized for GW2 Story Times.");

        if (ImGui::Button("Submit Full Experience", ImVec2(220.0f, 0.0f))) {
            SubmitTimeAsync("full");
            ImGui::CloseCurrentPopup();
        }

        if (ImGui::Button("Submit Speedrun", ImVec2(220.0f, 0.0f))) {
            SubmitTimeAsync("speed");
            ImGui::CloseCurrentPopup();
        }

        if (ImGui::Button("Cancel", ImVec2(220.0f, 0.0f))) {
            g_state.submitPromptOpen = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
}

void RenderBrowserWindow() {
    if (!g_state.browserVisible) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(760.0f, 520.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Story Times - Browse Missions", &g_state.browserVisible)) {
        ImGui::End();
        return;
    }

    if (g_state.seasonsLoading) {
        ImGui::TextUnformatted("Loading seasons...");
        ImGui::End();
        return;
    }

    if (!g_state.seasonsError.empty()) {
        ImGui::TextWrapped("Failed to load seasons: %s", g_state.seasonsError.c_str());
        if (ImGui::Button("Retry")) {
            FetchSeasonsAsync();
        }
        ImGui::End();
        return;
    }

    if (g_state.seasons.empty()) {
        ImGui::TextUnformatted("No seasons loaded yet.");
        ImGui::End();
        return;
    }

    if (g_state.selectedSeasonIndex < 0 || g_state.selectedSeasonIndex >= static_cast<int>(g_state.seasons.size())) {
        g_state.selectedSeasonIndex = 0;
    }

    Season& selectedSeason = g_state.seasons[g_state.selectedSeasonIndex];
    if (!selectedSeason.detailsLoaded && !selectedSeason.isLoading) {
        FetchSeasonDetailAsync(selectedSeason.id);
    }

    ImGui::TextUnformatted("Choose a season and mission");
    ImGui::Separator();

    ImGui::BeginChild("season_list", ImVec2(230.0f, 0.0f), true);
    for (int index = 0; index < static_cast<int>(g_state.seasons.size()); ++index) {
        const Season& season = g_state.seasons[index];
        const bool isSelected = g_state.selectedSeasonIndex == index;
        if (ImGui::Selectable(season.name.c_str(), isSelected)) {
            g_state.selectedSeasonIndex = index;
            g_state.selectedMissionIndex = -1;
        }

        std::ostringstream detail;
        detail << season.missionCount << " missions";
        const double totalMinutes = g_state.useSpeedCategory ? season.totalSpeedMinutes : season.totalFullMinutes;
        if (totalMinutes > 0.0) {
            detail << " - " << TimeEstimate{totalMinutes, 0}.Format();
        }
        ImGui::TextDisabled("%s", detail.str().c_str());
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginGroup();

    static char searchBuffer[128]{};
    if (g_state.missionSearch.size() >= sizeof(searchBuffer)) {
        g_state.missionSearch.resize(sizeof(searchBuffer) - 1);
    }
    std::strncpy(searchBuffer, g_state.missionSearch.c_str(), sizeof(searchBuffer));
    searchBuffer[sizeof(searchBuffer) - 1] = '\0';
    if (ImGui::InputText("Search missions", searchBuffer, sizeof(searchBuffer))) {
        g_state.missionSearch = searchBuffer;
        g_state.selectedMissionIndex = -1;
    }

    ImGui::BeginChild("mission_list", ImVec2(0.0f, 400.0f), true);
    if (selectedSeason.isLoading) {
        ImGui::TextUnformatted("Loading missions...");
    } else if (!selectedSeason.errorText.empty()) {
        ImGui::TextWrapped("Failed to load missions: %s", selectedSeason.errorText.c_str());
        if (ImGui::Button("Retry Season Load")) {
            selectedSeason.isLoading = false;
            selectedSeason.errorText.clear();
            FetchSeasonDetailAsync(selectedSeason.id);
        }
    } else {
        for (int missionIndex = 0; missionIndex < static_cast<int>(selectedSeason.missions.size()); ++missionIndex) {
            const auto& mission = selectedSeason.missions[missionIndex];
            if (!MissionMatchesFilter(mission, g_state.missionSearch)) {
                continue;
            }

            const bool isActive = g_state.activeMission.has_value() && g_state.activeMission->id == mission.id;
            const bool isSelected = isActive || g_state.selectedMissionIndex == missionIndex;
            if (ImGui::Selectable(mission.name.c_str(), isSelected)) {
                g_state.selectedMissionIndex = missionIndex;
            }

            ImGui::TextDisabled("%s", mission.Breadcrumb().c_str());
            const auto estimate = g_state.useSpeedCategory ? mission.speedEstimate : mission.fullEstimate;
            if (estimate.has_value()) {
                ImGui::SameLine();
                ImGui::TextDisabled("  %s", estimate->Format().c_str());
            }
            ImGui::Separator();
        }
    }
    ImGui::EndChild();

    if (g_state.selectedMissionIndex >= 0 && g_state.selectedMissionIndex < static_cast<int>(selectedSeason.missions.size())) {
        const auto& selectedMission = selectedSeason.missions[g_state.selectedMissionIndex];
        ImGui::Text("Selected: %s", selectedMission.name.c_str());
        ImGui::TextDisabled("%s", selectedMission.Breadcrumb().c_str());
        if (ImGui::Button("Use Mission", ImVec2(140.0f, 0.0f))) {
            SelectMission(selectedMission);
        }
    } else {
        ImGui::TextDisabled("Select a mission to load it into the timer widget.");
    }

    ImGui::SameLine();
    ImGui::Checkbox("Speed estimates", &g_state.useSpeedCategory);
    ImGui::EndGroup();

    ImGui::End();
}

void RenderWidgetWindow() {
    EnsureImGuiBound();

    const std::lock_guard lock(g_stateMutex);

    if (!g_state.widgetVisible) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(360.0f, 175.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.92f);
    const ImGuiWindowFlags widgetFlags =
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoScrollbar;

    if (!ImGui::Begin("Story Times", &g_state.widgetVisible, widgetFlags)) {
        ImGui::End();
        return;
    }

    ImGui::TextUnformatted("Story Times");
    ImGui::SameLine();
    if (ImGui::Button("Browse Missions")) {
        g_state.browserVisible = true;
    }

    if (!g_state.activeMission.has_value()) {
        ImGui::TextUnformatted("No mission selected");
        ImGui::TextDisabled("Open the browser to choose one.");
    } else {
        ImGui::Text("%s", g_state.activeMission->name.c_str());
        ImGui::TextDisabled("%s", g_state.activeMission->Breadcrumb().c_str());
    }

    const auto elapsed = g_state.timer.Elapsed();
    const auto estimate = ActiveEstimate();
    const auto timerColor = BuildPacingColor(estimate, elapsed);

    ImGui::PushStyleColor(ImGuiCol_Text, timerColor);
    ImGui::SetWindowFontScale(1.6f);
    ImGui::Text("%s", g_state.timer.FormattedElapsed().c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();

    if (estimate.has_value()) {
        ImGui::Text("Estimate: %s", estimate->Format().c_str());
    } else {
        ImGui::TextUnformatted("Estimate: No estimate");
    }

    ImGui::TextWrapped("%s", BuildPacingText(estimate, elapsed).c_str());

    if (ImGui::Button(g_state.timer.IsRunning() ? "Pause" : "Start", ImVec2(100.0f, 0.0f))) {
        if (g_state.timer.IsRunning()) {
            g_state.timer.Stop();
            g_state.statusText = "Timer paused.";
        } else {
            g_state.timer.Start();
            g_state.statusText = "Timer running.";
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Reset", ImVec2(100.0f, 0.0f))) {
        g_state.timer.Reset();
        g_state.statusText = "Timer reset.";
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear", ImVec2(80.0f, 0.0f))) {
        g_state.activeMission.reset();
        g_state.timer.Reset();
        g_state.statusText = "Mission cleared.";
    }

    const bool canSubmit = g_state.activeMission.has_value() &&
        !g_state.timer.IsRunning() &&
        !g_state.submitting &&
        std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= 60 &&
        std::chrono::duration_cast<std::chrono::minutes>(elapsed).count() <= 480;

    if (canSubmit) {
        ImGui::SameLine();
        if (ImGui::Button("Submit", ImVec2(100.0f, 0.0f))) {
            g_state.submitPromptOpen = true;
            ImGui::OpenPopup("Submit Time");
        }
    }

    ImGui::Separator();
    if (g_state.activeMission.has_value() &&
        !g_state.timer.IsRunning() &&
        std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() > 0 &&
        !canSubmit) {
        ImGui::TextDisabled("Submission requires a stopped run between 1 and 480 minutes.");
    }
    ImGui::TextWrapped("%s", g_state.statusText.c_str());
    RenderFeedbackPrompt();
    ImGui::End();

    RenderBrowserWindow();
}

void OnInputBind(const char* identifier, bool isRelease) {
    if (isRelease || identifier == nullptr) {
        return;
    }

    if (std::string_view(identifier) == kToggleBindIdentifier) {
        const std::lock_guard lock(g_stateMutex);
        g_state.widgetVisible = !g_state.widgetVisible;
    }
}

void Load(AddonAPI_t* addonApi) {
    g_api = static_cast<AddonAPI6_t*>(addonApi);
    EnsureImGuiBound();

    {
        const std::lock_guard lock(g_stateMutex);
        g_state = {};
        g_state.widgetVisible = true;
        g_state.browserVisible = false;
        g_state.showFeedbackPrompt = true;
        g_state.statusText = "Loading story data...";
    }

    if (g_api != nullptr) {
        g_api->Renderer.Register(ERenderType::Render, RenderWidgetWindow);
        g_api->InputBinds.RegisterWithString(kToggleBindIdentifier, OnInputBind, "ALT+SHIFT+T");
    }

    FetchSeasonsAsync();
    Log(ELogLevel::INFO, "Loaded Nexus port scaffold.");
}

void Unload() {
    if (g_api != nullptr) {
        g_api->Renderer.Deregister(RenderWidgetWindow);
        g_api->InputBinds.Deregister(kToggleBindIdentifier);
    }

    g_workers.clear();
    Log(ELogLevel::INFO, "Unloaded addon.");
    ImGui::SetCurrentContext(nullptr);
    g_api = nullptr;
}

AddonDef_t g_addonDefinition{
    kAddonSignature,
    kNexusApiVersion,
    kAddonName,
    AddonVersion_t{0, 1, 2, 0},
    kAddonAuthor,
    "Guild Wars 2 story mission timing overlay for Raidcore Nexus.",
    Load,
    Unload,
    EAddonFlags::None,
    EUpdateProvider::GitHub,
    kUpdateLink,
};

}  // namespace

extern "C" __declspec(dllexport) AddonDef_t* GetAddonDef() {
    return &g_addonDefinition;
}
