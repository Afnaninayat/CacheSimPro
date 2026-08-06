#include <SDL.h>
#include <SDL_opengl.h>
#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include "imgui_impl_opengl3.h"

#include "CacheCore.hpp"

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <bitset>
#include <cstdio>
#include <future>

struct TraceEntry {
    char op;
    uint64_t address;
};

// Global non-blocking native file dialog state
static std::future<std::string> g_file_dialog_future;
static bool g_file_dialog_pending = false;

// Helper: Trim leading and trailing whitespace
static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

// Open Native Linux OS File Chooser Dialog (Zenity) synchronously in worker thread
static std::string OpenNativeFileDialogSync() {
    std::string filePath = "";
    FILE* pipe = popen("zenity --file-selection --title=\"Select Memory Trace File\" --file-filter=\"Trace Files (*.txt *.trace) | *.txt *.trace\" --file-filter=\"All Files | *\" 2>/dev/null", "r");
    if (pipe) {
        char buffer[2048];
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            filePath = buffer;
            filePath.erase(std::remove(filePath.begin(), filePath.end(), '\n'), filePath.end());
            filePath.erase(std::remove(filePath.begin(), filePath.end(), '\r'), filePath.end());
        }
        pclose(pipe);
    }
    return filePath;
}

// Parse memory trace string into structured entries (Clean & Robust)
static std::vector<TraceEntry> parseTraceText(const std::string& text) {
    std::vector<TraceEntry> entries;
    std::istringstream stream(text);
    std::string line;

    while (std::getline(stream, line)) {
        // Remove inline comments (#, //, ;)
        size_t c = line.find_first_of("#/;");
        if (c != std::string::npos) line = line.substr(0, c);

        // Replace punctuation like commas and colons with spaces
        std::replace(line.begin(), line.end(), ',', ' ');
        std::replace(line.begin(), line.end(), ':', ' ');

        line = trim(line);
        if (line.empty()) continue;

        std::istringstream ss(line);
        std::string token1, token2;
        if (!(ss >> token1)) continue;

        char op = 'R';
        std::string addrStr = token1;

        if (ss >> token2) {
            auto isW = [](const std::string& s) {
                std::string u = s;
                for (char &ch : u) ch = std::toupper(static_cast<unsigned char>(ch));
                return (u == "W" || u == "WRITE" || u == "1" || u == "S" || u == "STORE");
            };
            auto isR = [](const std::string& s) {
                std::string u = s;
                for (char &ch : u) ch = std::toupper(static_cast<unsigned char>(ch));
                return (u == "R" || u == "READ" || u == "0" || u == "2" || u == "L" || u == "LOAD" || u == "I" || u == "FETCH");
            };

            if (isW(token1)) {
                op = 'W';
                addrStr = token2;
            } else if (isR(token1)) {
                op = 'R';
                addrStr = token2;
            } else if (isW(token2)) {
                op = 'W';
                addrStr = token1;
            } else if (isR(token2)) {
                op = 'R';
                addrStr = token1;
            }
        }

        try {
            uint64_t addr = 0;
            if (addrStr.find("0x") == 0 || addrStr.find("0X") == 0) {
                addr = std::stoull(addrStr, nullptr, 16);
            } else {
                try {
                    addr = std::stoull(addrStr, nullptr, 16);
                } catch (...) {
                    addr = std::stoull(addrStr, nullptr, 10);
                }
            }
            entries.push_back({op, addr});
        } catch (...) {}
    }
    return entries;
}

static bool loadTraceFromFile(const std::string& path, char* trace_buffer, size_t buffer_size, std::vector<TraceEntry>& trace_entries, CacheCore& cache, size_t& current_step, std::vector<std::string>& execution_logs, std::string& status_message, bool& status_is_error) {
    std::string cleanPath = trim(path);
    if (cleanPath.empty()) {
        status_message = "Error: Please specify a valid trace file path.";
        status_is_error = true;
        return false;
    }

    std::ifstream file(cleanPath);
    if (!file.is_open()) {
        std::string fullPath = "/home/cl4/Desktop/Afnaninayat/cache_simulator/" + cleanPath;
        file.open(fullPath);
    }

    if (file.is_open()) {
        std::stringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();
        if (content.size() < buffer_size) {
            std::copy(content.begin(), content.end(), trace_buffer);
            trace_buffer[content.size()] = '\0';
        } else {
            std::copy(content.begin(), content.begin() + buffer_size - 1, trace_buffer);
            trace_buffer[buffer_size - 1] = '\0';
        }
        trace_entries = parseTraceText(content);
        current_step = 0;
        cache.reset();
        execution_logs.clear();

        if (trace_entries.empty()) {
            status_message = "Error: File '" + cleanPath + "' opened, but contained 0 valid trace instructions.";
            status_is_error = true;
            return false;
        }

        status_message = "Loaded " + std::to_string(trace_entries.size()) + " instructions from file '" + cleanPath + "'.";
        status_is_error = false;
        return true;
    } else {
        status_message = "Error: Could not open trace file '" + cleanPath + "'. Check file path.";
        status_is_error = true;
        return false;
    }
}

// Format execution log matching exact USTP specification
static std::string formatLogEntry(const StepResult& res) {
    std::ostringstream ss;
    ss << "Address 0x" << std::hex << std::uppercase << res.address
       << " -> Set " << std::dec << res.set_index
       << ", Tag 0x" << std::hex << std::uppercase << res.tag << " -> ";

    if (res.hit) {
        ss << "HIT";
    } else if (res.evicted) {
        ss << "MISS (Evicted Line " << std::dec << res.evicted_way << ")";
    } else {
        ss << "MISS";
    }
    return ss.str();
}

static void ApplyModernTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    
    style.WindowPadding     = ImVec2(12.0f, 12.0f);
    style.FramePadding      = ImVec2(8.0f, 6.0f);
    style.ItemSpacing       = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(6.0f, 6.0f);
    style.IndentSpacing     = 20.0f;
    style.ScrollbarSize     = 14.0f;
    style.GrabMinSize       = 12.0f;

    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 5.0f;

    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.92f, 0.95f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.50f, 0.55f, 0.60f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.09f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.12f, 0.14f, 0.18f, 0.95f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.12f, 0.15f, 0.20f, 1.00f);
    colors[ImGuiCol_Border]                = ImVec4(0.22f, 0.26f, 0.32f, 1.00f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.16f, 0.19f, 0.24f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.22f, 0.27f, 0.34f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.26f, 0.32f, 0.40f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.09f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.14f, 0.18f, 0.24f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.09f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.12f, 0.14f, 0.18f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.09f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.24f, 0.30f, 0.38f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.32f, 0.40f, 0.50f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.38f, 0.48f, 0.60f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.35f, 0.65f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.35f, 0.65f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.50f, 0.75f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.18f, 0.24f, 0.32f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.26f, 0.35f, 0.48f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.32f, 0.44f, 0.60f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.20f, 0.27f, 0.36f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.28f, 0.38f, 0.50f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.35f, 0.46f, 0.60f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.22f, 0.26f, 0.32f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.35f, 0.65f, 1.00f, 1.00f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.50f, 0.75f, 1.00f, 1.00f);
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.24f, 0.30f, 0.38f, 1.00f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.35f, 0.65f, 1.00f, 1.00f);
    colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.50f, 0.75f, 1.00f, 1.00f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.14f, 0.18f, 0.24f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.26f, 0.35f, 0.48f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.22f, 0.30f, 0.42f, 1.00f);
    colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.14f, 0.18f, 0.24f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.22f, 0.26f, 0.32f, 1.00f);
    colors[ImGuiCol_TableBorderLight]      = ImVec4(0.18f, 0.22f, 0.28f, 1.00f);
    colors[ImGuiCol_PlotLines]             = ImVec4(0.35f, 0.65f, 1.00f, 1.00f);
    colors[ImGuiCol_PlotLinesHovered]      = ImVec4(0.50f, 0.75f, 1.00f, 1.00f);
    colors[ImGuiCol_PlotHistogram]         = ImVec4(0.35f, 0.65f, 1.00f, 1.00f);
    colors[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.50f, 0.75f, 1.00f, 1.00f);
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        std::cerr << "Error initializing SDL: " << SDL_GetError() << std::endl;
        return -1;
    }

    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window* window = SDL_CreateWindow("USTP Digital IC Design - Cache Simulator in C++", 
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
                                          1360, 850, window_flags);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ApplyModernTheme();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    CacheCore cache;
    int input_cache_size = 1024;
    int input_block_size = 64;
    int input_associativity = 4;
    int selected_policy = 0; // 0: LRU, 1: FIFO, 2: Random

    char trace_buffer[65536] = 
        "R 0x1000\n"
        "W 0x1004\n"
        "R 0x2000\n"
        "R 0x1000\n";

    char trace_file_path[256] = "trace.txt";
    std::vector<TraceEntry> trace_entries;
    size_t current_step = 0;

    std::vector<std::string> execution_logs;
    std::string status_message = "Ready. Configure parameters and click Run Simulation or Step.";
    bool status_is_error = false;

    int active_set = -1;
    int active_way = -1;
    bool active_hit = false;
    bool active_evicted = false;
    uint64_t active_addr = 0;
    (void)active_addr;

    bool auto_play = false;
    float auto_play_speed_ms = 300.0f;
    auto last_step_time = std::chrono::steady_clock::now();

    char inspect_addr_str[64] = "0x1000";

    cache.configure(input_cache_size, input_block_size, input_associativity, ReplacementPolicy::LRU);
    loadTraceFromFile(trace_file_path, trace_buffer, sizeof(trace_buffer), trace_entries, cache, current_step, execution_logs, status_message, status_is_error);

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
                running = false;
        }

        if (auto_play) {
            auto now = std::chrono::steady_clock::now();
            float elapsed = std::chrono::duration<float, std::milli>(now - last_step_time).count();
            if (elapsed >= auto_play_speed_ms) {
                last_step_time = now;

                if (trace_entries.empty()) {
                    trace_entries = parseTraceText(trace_buffer);
                }

                if (!cache.isConfigured()) {
                    status_message = "Error: Please configure cache parameters first.";
                    status_is_error = true;
                    auto_play = false;
                } else if (trace_entries.empty()) {
                    status_message = "Error: Please load a trace file first.";
                    status_is_error = true;
                    auto_play = false;
                } else {
                    if (current_step >= trace_entries.size()) {
                        current_step = 0;
                        cache.reset();
                        execution_logs.clear();
                    }

                    const auto& entry = trace_entries[current_step++];
                    StepResult res = cache.access(entry.op, entry.address);
                    active_set = res.set_index;
                    active_way = res.way;
                    active_hit = res.hit;
                    active_evicted = res.evicted;
                    active_addr = res.address;
                    execution_logs.push_back(formatLogEntry(res));
                    status_message = "Auto-Play Step " + std::to_string(current_step) + "/" + std::to_string(trace_entries.size());
                    status_is_error = false;
                }
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("RootContainer", nullptr, 
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Header Bar
        ImGui::BeginChild("HeaderBar", ImVec2(0, 50), true);
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "USTP DIGITAL IC DESIGN - PARAMETERIZED CACHE SIMULATOR");
        ImGui::SameLine();
        ImGui::TextDisabled("| C++17 & Dear ImGui Architecture Lab Project");
        
        ImGui::SameLine(ImGui::GetWindowWidth() - 260);
        if (auto_play) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.4f, 1.0f), "[ RUNNING AUTO-PLAY ]");
        } else if (current_step > 0 && current_step >= trace_entries.size()) {
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "[ TRACE COMPLETED ]");
        } else {
            ImGui::TextDisabled("[ STANDBY ]");
        }
        ImGui::EndChild();

        // Status Banner
        if (!status_message.empty()) {
            ImVec4 bannerColor = status_is_error ? ImVec4(1.0f, 0.35f, 0.35f, 1.0f) : ImVec4(0.4f, 0.9f, 0.5f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, bannerColor);
            ImGui::Text("STATUS > %s", status_message.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::Separator();

        ImGui::Columns(2, "MainSplit", true);
        static bool set_width = false;
        if (!set_width) {
            ImGui::SetColumnWidth(0, 480.0f);
            set_width = true;
        }

        // =========================================================
        // LEFT COLUMN: CONTROLS, TRACE EDITOR & ADDRESS DECODER
        // =========================================================
        
        // 1. Configuration & Control Panel
        ImGui::BeginChild("ControlCard", ImVec2(0, 290), true);
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "CONFIGURATION & CONTROL PANEL");
        ImGui::Separator();

        ImGui::PushItemWidth(180);
        ImGui::InputInt("Total Cache Size (B)", &input_cache_size);
        ImGui::InputInt("Block Size (B)", &input_block_size);
        ImGui::InputInt("Associativity (n)", &input_associativity);

        const char* policy_list[] = { "LRU", "FIFO", "Random" };
        ImGui::Combo("Replacement Policy", &selected_policy, policy_list, IM_ARRAYSIZE(policy_list));
        ImGui::PopItemWidth();

        if (ImGui::Button("Apply Config", ImVec2(130, 30))) {
            ReplacementPolicy pol = ReplacementPolicy::LRU;
            if (selected_policy == 1) pol = ReplacementPolicy::FIFO;
            else if (selected_policy == 2) pol = ReplacementPolicy::RANDOM;

            if (!cache.configure(input_cache_size, input_block_size, input_associativity, pol)) {
                status_message = "Error: Please configure cache parameters first. Parameters must be powers of 2.";
                status_is_error = true;
            } else {
                trace_entries = parseTraceText(trace_buffer);
                status_message = "Cache configuration applied & trace parsed (" + std::to_string(trace_entries.size()) + " entries).";
                status_is_error = false;
                execution_logs.clear();
                current_step = 0;
                active_set = -1;
                active_way = -1;
                auto_play = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset", ImVec2(100, 30))) {
            cache.reset();
            trace_entries = parseTraceText(trace_buffer);
            execution_logs.clear();
            current_step = 0;
            active_set = -1;
            active_way = -1;
            auto_play = false;
            status_message = "Reset complete. Cache state and counters cleared.";
            status_is_error = false;
        }

        ImGui::Separator();

        // Action Buttons
        auto ensureTraceLoaded = [&]() {
            if (trace_entries.empty()) {
                trace_entries = parseTraceText(trace_buffer);
            }
            if (trace_entries.empty() && trace_file_path[0] != '\0') {
                loadTraceFromFile(trace_file_path, trace_buffer, sizeof(trace_buffer), trace_entries, cache, current_step, execution_logs, status_message, status_is_error);
            }
        };

        if (ImGui::Button("Run Simulation", ImVec2(140, 32))) {
            ensureTraceLoaded();

            if (!cache.isConfigured()) {
                status_message = "Error: Please configure cache parameters first.";
                status_is_error = true;
            } else if (trace_entries.empty()) {
                status_message = "Error: No valid trace instructions found. Load a trace file or enter instructions in the editor.";
                status_is_error = true;
            } else {
                current_step = 0;
                cache.reset();
                execution_logs.clear();
                while (current_step < trace_entries.size()) {
                    const auto& entry = trace_entries[current_step++];
                    StepResult res = cache.access(entry.op, entry.address);
                    active_set = res.set_index;
                    active_way = res.way;
                    active_hit = res.hit;
                    active_evicted = res.evicted;
                    active_addr = res.address;
                    execution_logs.push_back(formatLogEntry(res));
                }
                status_message = "Run Simulation complete (" + std::to_string(trace_entries.size()) + " instructions processed).";
                status_is_error = false;
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Step", ImVec2(80, 32))) {
            ensureTraceLoaded();

            if (!cache.isConfigured()) {
                status_message = "Error: Please configure cache parameters first.";
                status_is_error = true;
            } else if (trace_entries.empty()) {
                status_message = "Error: No valid trace instructions found. Load a trace file or enter instructions in the editor.";
                status_is_error = true;
            } else {
                if (current_step >= trace_entries.size()) {
                    current_step = 0;
                    cache.reset();
                    execution_logs.clear();
                    active_set = -1;
                    active_way = -1;
                }

                const auto& entry = trace_entries[current_step++];
                StepResult res = cache.access(entry.op, entry.address);
                active_set = res.set_index;
                active_way = res.way;
                active_hit = res.hit;
                active_evicted = res.evicted;
                active_addr = res.address;
                execution_logs.push_back(formatLogEntry(res));
                status_message = "Step " + std::to_string(current_step) + "/" + std::to_string(trace_entries.size()) + " executed.";
                status_is_error = false;
            }
        }
        ImGui::SameLine();
        if (auto_play) {
            if (ImGui::Button("Pause", ImVec2(80, 32))) auto_play = false;
        } else {
            if (ImGui::Button("Auto Play", ImVec2(80, 32))) {
                ensureTraceLoaded();

                if (!cache.isConfigured()) {
                    status_message = "Error: Please configure cache parameters first.";
                    status_is_error = true;
                } else if (trace_entries.empty()) {
                    status_message = "Error: No valid trace instructions found. Load a trace file or enter instructions in the editor.";
                    status_is_error = true;
                } else {
                    auto_play = true;
                }
            }
        }

        ImGui::PushItemWidth(180);
        ImGui::SliderFloat("Speed (ms)", &auto_play_speed_ms, 50.0f, 1000.0f, "%.0f ms");
        ImGui::PopItemWidth();

        ImGui::EndChild();

        // 2. Hardware Bit Decoder Card
        ImGui::BeginChild("DecoderCard", ImVec2(0, 160), true);
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "HARDWARE ADDRESS BIT DECODER");
        ImGui::Separator();

        ImGui::PushItemWidth(140);
        ImGui::InputText("Test Address", inspect_addr_str, IM_ARRAYSIZE(inspect_addr_str));
        ImGui::PopItemWidth();

        uint64_t dec_addr = 0;
        try {
            std::string str = inspect_addr_str;
            if (str.find("0x") == 0 || str.find("0X") == 0) {
                dec_addr = std::stoull(str, nullptr, 16);
            } else {
                dec_addr = std::stoull(str, nullptr, 10);
            }
        } catch (...) {}

        if (cache.isConfigured()) {
            uint32_t block_size = cache.getBlockSize();
            uint32_t num_sets = cache.getNumSets();

            uint32_t offset_bits = static_cast<uint32_t>(std::log2(block_size));
            uint32_t index_bits = static_cast<uint32_t>(std::log2(num_sets));
            uint32_t tag_bits = 64 - index_bits - offset_bits;

            uint32_t set_idx = (dec_addr >> offset_bits) & (num_sets - 1);
            uint64_t tag_val = dec_addr >> (offset_bits + index_bits);
            uint32_t block_offset = dec_addr & (block_size - 1);

            ImGui::Text("Bit Breakdown: [ Tag: %u bits | Set Index: %u bits | Offset: %u bits ]", 
                        tag_bits, index_bits, offset_bits);
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), 
                        "-> Tag: 0x%llX | Set Index: %u | Offset: 0x%X", 
                        (unsigned long long)tag_val, set_idx, block_offset);
        } else {
            ImGui::TextDisabled("Configure cache to view bit decoder.");
        }

        ImGui::EndChild();

        // 3. Trace Editor Panel
        ImGui::BeginChild("TraceCard", ImVec2(0, 0), true);
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "TRACE EDITOR PANEL");
        ImGui::Separator();

        ImGui::PushItemWidth(180);
        if (ImGui::InputText("Trace Path", trace_file_path, IM_ARRAYSIZE(trace_file_path), ImGuiInputTextFlags_EnterReturnsTrue)) {
            loadTraceFromFile(trace_file_path, trace_buffer, sizeof(trace_buffer), trace_entries, cache, current_step, execution_logs, status_message, status_is_error);
        }
        ImGui::PopItemWidth();
        
        ImGui::SameLine();
        if (ImGui::Button("Load Path")) {
            loadTraceFromFile(trace_file_path, trace_buffer, sizeof(trace_buffer), trace_entries, cache, current_step, execution_logs, status_message, status_is_error);
        }

        ImGui::SameLine();
        if (g_file_dialog_pending) {
            ImGui::Button("Browsing...", ImVec2(90, 0));
        } else {
            if (ImGui::Button("Browse...")) {
                g_file_dialog_future = std::async(std::launch::async, OpenNativeFileDialogSync);
                g_file_dialog_pending = true;
            }
        }

        // Non-blocking async result processing
        if (g_file_dialog_pending && g_file_dialog_future.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            std::string selectedFile = g_file_dialog_future.get();
            g_file_dialog_pending = false;
            if (!selectedFile.empty()) {
                if (selectedFile.size() < sizeof(trace_file_path)) {
                    std::copy(selectedFile.begin(), selectedFile.end(), trace_file_path);
                    trace_file_path[selectedFile.size()] = '\0';
                }
                loadTraceFromFile(trace_file_path, trace_buffer, sizeof(trace_buffer), trace_entries, cache, current_step, execution_logs, status_message, status_is_error);
            }
        }

        // Quick Preset Trace File Selectors
        ImGui::TextDisabled("Quick Presets:");
        ImGui::SameLine();
        if (ImGui::SmallButton("trace.txt")) {
            snprintf(trace_file_path, sizeof(trace_file_path), "trace.txt");
            loadTraceFromFile(trace_file_path, trace_buffer, sizeof(trace_buffer), trace_entries, cache, current_step, execution_logs, status_message, status_is_error);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("trace_loop.txt")) {
            snprintf(trace_file_path, sizeof(trace_file_path), "trace_loop.txt");
            loadTraceFromFile(trace_file_path, trace_buffer, sizeof(trace_buffer), trace_entries, cache, current_step, execution_logs, status_message, status_is_error);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("trace_thrash.txt")) {
            snprintf(trace_file_path, sizeof(trace_file_path), "trace_thrash.txt");
            loadTraceFromFile(trace_file_path, trace_buffer, sizeof(trace_buffer), trace_entries, cache, current_step, execution_logs, status_message, status_is_error);
        }

        ImGui::TextDisabled("Memory Trace Text (Format: [Op] [Hex Address]):");
        if (ImGui::InputTextMultiline("##TraceEditor", trace_buffer, IM_ARRAYSIZE(trace_buffer), 
                                      ImVec2(-1, -30), ImGuiInputTextFlags_AllowTabInput)) {
            trace_entries = parseTraceText(trace_buffer);
            current_step = 0;
        }

        ImGui::TextDisabled("Parsed Instructions: %zu | Current Index: %zu", trace_entries.size(), current_step);

        ImGui::EndChild();

        ImGui::NextColumn();

        // =========================================================
        // RIGHT COLUMN: DASHBOARD, VISUALIZER GRID & EXECUTION LOG
        // =========================================================

        ImGui::BeginChild("RightContainer", ImVec2(0, 0), true);
        
        // 1. Statistics Dashboard
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "STATISTICS DASHBOARD");
        ImGui::Separator();

        uint64_t hits = cache.getHits();
        uint64_t misses = cache.getMisses();
        uint64_t evictions = cache.getEvictions();
        uint64_t total = hits + misses;

        float hit_ratio = total > 0 ? (float)hits / total : 0.0f;
        float miss_ratio = total > 0 ? (float)misses / total : 0.0f;

        ImGui::Columns(3, "MetricCols", false);
        ImGui::Text("HITS: %llu", (unsigned long long)hits);
        ImGui::ProgressBar(hit_ratio, ImVec2(-1, 14), ""); ImGui::NextColumn();

        ImGui::Text("MISSES: %llu", (unsigned long long)misses);
        ImGui::ProgressBar(miss_ratio, ImVec2(-1, 14), ""); ImGui::NextColumn();

        ImGui::Text("EVICTIONS: %llu", (unsigned long long)evictions);
        float eviction_ratio = total > 0 ? (float)evictions / total : 0.0f;
        ImGui::ProgressBar(eviction_ratio, ImVec2(-1, 14), ""); ImGui::NextColumn();
        ImGui::Columns(1);

        ImGui::Text("Hit Rate: %.1f%%  |  Miss Rate: %.1f%%", hit_ratio * 100.0f, miss_ratio * 100.0f);
        ImGui::Separator();

        // 2. Cache Visualizer
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "CACHE VISUALIZER (Rows = Sets | Cols = Ways)");
        if (cache.isConfigured() && cache.getNumSets() > 0) {
            uint32_t num_sets = cache.getNumSets();
            uint32_t assoc = cache.getAssociativity();
            const auto& sets = cache.getSets();

            if (ImGui::BeginTable("MatrixTable", assoc + 1, 
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, 
                                  ImVec2(0, 260))) {
                
                ImGui::TableSetupScrollFreeze(1, 1);
                ImGui::TableSetupColumn("Set Index", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                for (uint32_t w = 0; w < assoc; ++w) {
                    std::string wayHeader = "Way " + std::to_string(w);
                    ImGui::TableSetupColumn(wayHeader.c_str(), ImGuiTableColumnFlags_WidthStretch);
                }
                ImGui::TableHeadersRow();

                for (uint32_t s = 0; s < num_sets; ++s) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("Set %u", s);

                    for (uint32_t w = 0; w < assoc; ++w) {
                        ImGui::TableSetColumnIndex(w + 1);
                        const auto& line = sets[s].lines[w];

                        bool isActive = (static_cast<int>(s) == active_set && static_cast<int>(w) == active_way);
                        if (isActive) {
                            if (active_hit) {
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(30, 160, 70, 255)); // Hit Green
                            } else if (active_evicted) {
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(210, 50, 50, 255)); // Eviction Red
                            } else {
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(220, 160, 30, 255)); // Miss Yellow
                            }
                        }

                        if (line.valid) {
                            ImGui::Text("V:1 | Tag: 0x%llX", (unsigned long long)line.tag);
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Set: %u | Way: %u\nTag: 0x%llX\nLast Access: %llu\nInsertion Time: %llu", 
                                                  s, w, (unsigned long long)line.tag, 
                                                  (unsigned long long)line.last_access, 
                                                  (unsigned long long)line.insertion_time);
                            }
                        } else {
                            ImGui::TextDisabled("V:0 | Tag: ---");
                        }
                    }
                }
                ImGui::EndTable();
            }
        } else {
            ImGui::TextDisabled("Visualizer matrix inactive.");
        }

        ImGui::Separator();

        // 3. Execution Log
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "EXECUTION LOG");
        ImGui::BeginChild("LogWindow", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& logEntry : execution_logs) {
            if (logEntry.find("HIT") != std::string::npos) {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "%s", logEntry.c_str());
            } else if (logEntry.find("(Evicted") != std::string::npos || logEntry.find("EVICTION") != std::string::npos) {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f), "%s", logEntry.c_str());
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s", logEntry.c_str());
            }
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        glClearColor(0.07f, 0.08f, 0.11f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();

    SDL_GL_DeleteContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
