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
#include <cstdio>
#include <cctype>

struct TraceEntry {
    char op;
    uint64_t address;
};

struct ParseResult {
    bool valid = true;
    std::string error_message;
    int error_line = -1;
    std::vector<TraceEntry> entries;
};

// Preset Configuration Options
struct ValueOption {
    int value;
    const char* label;
};

static const ValueOption CACHE_SIZE_OPTIONS[] = {
    {256,   "256 Bytes"},
    {512,   "512 Bytes"},
    {1024,  "1 KB (1024 B)"},
    {2048,  "2 KB (2048 B)"},
    {4096,  "4 KB (4096 B)"},
    {8192,  "8 KB (8192 B)"},
    {16384, "16 KB (16384 B)"},
    {65536, "64 KB (65536 B)"}
};

static const ValueOption BLOCK_SIZE_OPTIONS[] = {
    {16,  "16 Bytes"},
    {32,  "32 Bytes"},
    {64,  "64 Bytes"},
    {128, "128 Bytes"},
    {256, "256 Bytes"}
};

static const ValueOption ASSOC_OPTIONS[] = {
    {1,  "1-Way (Direct Mapped)"},
    {2,  "2-Way Set Associative"},
    {4,  "4-Way Set Associative"},
    {8,  "8-Way Set Associative"},
    {16, "16-Way Set Associative"}
};

// Helper: Trim leading and trailing whitespace
static std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

// Parse memory trace string into structured entries with full line-by-line validation
static ParseResult parseTraceTextEx(const std::string& text) {
    ParseResult res;
    if (trim(text).empty()) {
        res.valid = false;
        res.error_message = "Trace file is empty.";
        return res;
    }

    std::istringstream stream(text);
    std::string raw_line;
    int line_num = 0;

    while (std::getline(stream, raw_line)) {
        line_num++;
        std::string line = raw_line;

        // Strip comments starting with #, //, ;
        size_t c1 = line.find('#');
        if (c1 != std::string::npos) line = line.substr(0, c1);
        size_t c2 = line.find("//");
        if (c2 != std::string::npos) line = line.substr(0, c2);
        size_t c3 = line.find(';');
        if (c3 != std::string::npos) line = line.substr(0, c3);

        // Replace separators ',' and ':' with spaces
        std::replace(line.begin(), line.end(), ',', ' ');
        std::replace(line.begin(), line.end(), ':', ' ');

        line = trim(line);
        if (line.empty()) continue; // blank line or full line comment

        std::istringstream ss(line);
        std::vector<std::string> tokens;
        std::string tok;
        while (ss >> tok) {
            tokens.push_back(tok);
        }
        if (tokens.empty()) continue;

        std::string opToken = tokens[0];
        std::transform(opToken.begin(), opToken.end(), opToken.begin(), [](unsigned char c) { return std::toupper(c); });

        char op = 'R';
        if (opToken == "R" || opToken == "READ" || opToken == "LOAD" || opToken == "FETCH" || opToken == "L" || opToken == "I" || opToken == "0" || opToken == "2") {
            op = 'R';
        } else if (opToken == "W" || opToken == "WRITE" || opToken == "STORE" || opToken == "S" || opToken == "1") {
            op = 'W';
        } else {
            res.valid = false;
            res.error_message = "Line " + std::to_string(line_num) + ": Invalid operation '" + tokens[0] + "'. Use R or W.";
            res.error_line = line_num;
            res.entries.clear();
            return res;
        }

        if (tokens.size() < 2) {
            res.valid = false;
            res.error_message = "Line " + std::to_string(line_num) + ": Expected format: R 0x1000 or W 0x1000";
            res.error_line = line_num;
            res.entries.clear();
            return res;
        }

        std::string addrStr = tokens[1];
        uint64_t addr = 0;
        int base = 10;
        if (addrStr.rfind("0x", 0) == 0 || addrStr.rfind("0X", 0) == 0) {
            base = 16;
        } else {
            bool has_hex_char = false;
            for (char c : addrStr) {
                if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
                    has_hex_char = true;
                    break;
                }
            }
            if (has_hex_char) base = 16;
        }

        try {
            size_t processed = 0;
            addr = std::stoull(addrStr, &processed, base);
            if (processed != addrStr.size()) {
                res.valid = false;
                res.error_message = "Line " + std::to_string(line_num) + ": Invalid address '" + addrStr + "'.";
                res.error_line = line_num;
                res.entries.clear();
                return res;
            }
        } catch (...) {
            res.valid = false;
            res.error_message = "Line " + std::to_string(line_num) + ": Invalid address '" + addrStr + "'.";
            res.error_line = line_num;
            res.entries.clear();
            return res;
        }

        res.entries.push_back({op, addr});
    }

    if (res.entries.empty()) {
        res.valid = false;
        res.error_message = "Trace file is empty.";
    }

    return res;
}


// Load trace from disk file
static bool loadTraceFile(const std::string& filepath, char* trace_buf, size_t buf_size, std::string& status_msg, bool& is_error, std::vector<TraceEntry>& entries, CacheCore& cache, std::vector<std::string>& logs, size_t& current_step, int& active_set, int& active_way, bool& auto_play) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        status_msg = "Error: Cannot open file: " + filepath;
        is_error = true;
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();

    if (trim(content).empty()) {
        status_msg = "Error: Trace file is empty.";
        is_error = true;
        return false;
    }

    ParseResult pr = parseTraceTextEx(content);
    if (!pr.valid) {
        status_msg = "Error: " + pr.error_message;
        is_error = true;
        return false;
    }

    if (content.size() >= buf_size) {
        status_msg = "Error: Trace file too large for editor buffer.";
        is_error = true;
        return false;
    }

    snprintf(trace_buf, buf_size, "%s", content.c_str());
    entries = pr.entries;
    current_step = 0;
    cache.reset();
    logs.clear();
    active_set = -1;
    active_way = -1;
    auto_play = false;
    status_msg = "Loaded " + std::to_string(entries.size()) + " instructions from " + filepath + ".";
    is_error = false;
    logs.push_back("[Info] Loaded " + std::to_string(entries.size()) + " instructions from " + filepath + ".");
    return true;
}

// Format execution log entry
static std::string formatLogEntry(const StepResult& res) {
    std::ostringstream ss;
    ss << "[Sim] Accessing 0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << res.address << ": ";
    if (res.hit) {
        ss << "Cache Hit in Set " << std::dec << res.set_index << ", Way " << res.way;
    } else if (res.evicted) {
        if (res.dirty_eviction) {
            ss << "Cache Miss in Set " << std::dec << res.set_index << "; Evicting Way " << res.evicted_way << " (Dirty eviction) & Loading line...";
        } else {
            ss << "Cache Miss in Set " << std::dec << res.set_index << "; Evicting Way " << res.evicted_way << " & Loading line...";
        }
    } else {
        ss << "Cache Miss in Set " << std::dec << res.set_index << "; Loading line into Way " << res.way << "...";
    }
    return ss.str();
}

static void ApplyCacheSimProTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    
    style.WindowPadding     = ImVec2(14.0f, 14.0f);
    style.FramePadding      = ImVec2(10.0f, 7.0f);
    style.ItemSpacing       = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
    style.IndentSpacing     = 18.0f;
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 12.0f;

    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 8.0f;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 6.0f;

    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.92f, 0.95f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.48f, 0.54f, 0.62f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.07f, 0.09f, 0.12f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.09f, 0.12f, 0.16f, 0.98f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.10f, 0.14f, 0.19f, 1.00f);
    colors[ImGuiCol_Border]                = ImVec4(0.18f, 0.23f, 0.30f, 1.00f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.12f, 0.16f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.18f, 0.24f, 0.32f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.22f, 0.30f, 0.40f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.07f, 0.09f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.10f, 0.14f, 0.19f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.07f, 0.09f, 0.12f, 1.00f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.09f, 0.12f, 0.16f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.07f, 0.09f, 0.12f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.18f, 0.24f, 0.32f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.24f, 0.32f, 0.44f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.30f, 0.40f, 0.54f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.00f, 0.80f, 0.95f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.12f, 0.53f, 0.90f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.00f, 0.80f, 0.95f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.12f, 0.17f, 0.24f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.18f, 0.26f, 0.36f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.24f, 0.34f, 0.48f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.14f, 0.20f, 0.28f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.20f, 0.28f, 0.40f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.26f, 0.36f, 0.50f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.18f, 0.23f, 0.30f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.00f, 0.80f, 0.95f, 1.00f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.00f, 0.90f, 1.00f, 1.00f);
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.18f, 0.24f, 0.32f, 1.00f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.00f, 0.80f, 0.95f, 1.00f);
    colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.00f, 0.90f, 1.00f, 1.00f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.09f, 0.12f, 0.16f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.18f, 0.26f, 0.36f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.14f, 0.20f, 0.28f, 1.00f);
    colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.11f, 0.15f, 0.21f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.18f, 0.23f, 0.30f, 1.00f);
    colors[ImGuiCol_TableBorderLight]      = ImVec4(0.14f, 0.18f, 0.24f, 1.00f);
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

    SDL_DisplayMode dm;
    int target_w = 1600;
    int target_h = 960;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0) {
        if (dm.w > 1200 && dm.h > 800) {
            target_w = std::min(dm.w - 100, 1680);
            target_h = std::min(dm.h - 100, 1000);
        }
    }

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Window* window = SDL_CreateWindow("CacheSimPro - Advanced Cache Simulator", 
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
                                          target_w, target_h, window_flags);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ApplyCacheSimProTheme();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    CacheCore cache;
    int config_mode = 0; // 0: Presets, 1: Custom User Values
    int cache_size_idx = 7; // Default 64 KB
    int block_size_idx = 2; // Default 64 B
    int assoc_idx = 2;      // Default 4-Way
    int selected_policy = 0; // 0: LRU, 1: FIFO, 2: Random

    int custom_cache_size = 65536;
    int custom_block_size = 64;
    int custom_associativity = 4;

    char trace_buffer[65536] = "R 0x1000\nW 0x1004\nR 0x2000\nR 0x1000\nW 0x2004\nR 0x3000\n";
    char file_path_input[256] = "memory_trace.txt";

    ParseResult initial_pr = parseTraceTextEx(trace_buffer);
    std::vector<TraceEntry> trace_entries = initial_pr.entries;
    size_t current_step = 0;

    std::vector<std::string> execution_logs;
    std::string status_message = "Ready. Edit trace or click Run Simulation.";
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

    // Quick User Instruction Input Form state
    int quick_op_idx = 0; // 0: Read 'R', 1: Write 'W'
    char quick_addr_input[64] = "0x1000";

    // Initial cache setup
    int init_cs = CACHE_SIZE_OPTIONS[cache_size_idx].value;
    int init_bs = BLOCK_SIZE_OPTIONS[block_size_idx].value;
    int init_as = ASSOC_OPTIONS[assoc_idx].value;
    cache.configure(init_cs, init_bs, init_as, ReplacementPolicy::LRU);

    auto ensureTraceLoaded = [&]() -> bool {
        ParseResult pr = parseTraceTextEx(trace_buffer);
        if (!pr.valid) {
            status_message = "Error: " + pr.error_message;
            status_is_error = true;
            trace_entries.clear();
            return false;
        }
        trace_entries = pr.entries;
        return true;
    };

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window))
                running = false;
        }

        // Auto-play timer logic
        if (auto_play) {
            auto now = std::chrono::steady_clock::now();
            float elapsed = std::chrono::duration<float, std::milli>(now - last_step_time).count();
            if (elapsed >= auto_play_speed_ms) {
                last_step_time = now;

                if (!cache.isConfigured()) {
                    status_message = "Error: Please configure valid cache parameters first.";
                    status_is_error = true;
                    auto_play = false;
                } else if (!ensureTraceLoaded()) {
                    auto_play = false;
                } else if (trace_entries.empty()) {
                    status_message = "Error: Memory trace is empty. Enter instructions below.";
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
                    snprintf(inspect_addr_str, sizeof(inspect_addr_str), "0x%llX", (unsigned long long)res.address);
                    execution_logs.push_back(formatLogEntry(res));
                    status_message = "Executing Step " + std::to_string(current_step) + " of " + std::to_string(trace_entries.size());
                    status_is_error = false;

                    if (current_step >= trace_entries.size()) {
                        auto_play = false;
                        status_message = "Trace execution complete (" + std::to_string(trace_entries.size()) + " instructions).";
                    }
                }
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        // Root Viewport Container
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("RootContainer", nullptr, 
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

        // =========================================================
        // TOP HEADER BAR
        // =========================================================
        ImGui::BeginChild("HeaderBar", ImVec2(0, 48), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::SetCursorPosY(10.0f);
        
        // Brand Badge Icon & Title
        ImGui::TextColored(ImVec4(0.12f, 0.58f, 0.95f, 1.00f), "  CacheSimPro");
        ImGui::SameLine();
        ImGui::TextDisabled("- Advanced Cache Simulator");

        std::string statusText = "Status: " + status_message;
        float statusWidth = ImGui::CalcTextSize(statusText.c_str()).x;
        float headerWidth = ImGui::GetWindowWidth();
        float centerPos = (headerWidth - statusWidth) * 0.5f;

        ImGui::SameLine(std::max(260.0f, centerPos));
        if (status_is_error) {
            ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.40f, 1.0f), "%s", statusText.c_str());
        } else {
            ImGui::TextColored(ImVec4(0.00f, 0.85f, 0.95f, 1.0f), "%s", statusText.c_str());
        }

        ImGui::SameLine(ImGui::GetWindowWidth() - 220.0f);
        if (auto_play) {
            ImGui::TextColored(ImVec4(0.00f, 0.90f, 0.45f, 1.0f), "[AUTO-PLAY ACTIVE]");
        } else if (current_step > 0 && current_step >= trace_entries.size()) {
            ImGui::TextColored(ImVec4(0.12f, 0.58f, 0.95f, 1.00f), "[SIMULATION COMPLETE]");
        } else {
            ImGui::TextDisabled("[STANDBY READY]");
        }
        ImGui::EndChild();

        // =========================================================
        // FREELY RESIZABLE WORKSPACE SPLIT
        // =========================================================
        static bool init_split = false;
        if (!init_split) {
            ImGui::Columns(2, "MainLayout", true);
            ImGui::SetColumnWidth(0, std::clamp(io.DisplaySize.x * 0.48f, 520.0f, 700.0f));
            init_split = true;
        } else {
            ImGui::Columns(2, "MainLayout", true);
        }

        // ---------------------------------------------------------
        // LEFT COLUMN: CONFIGURATION & TRACE EDITOR
        // ---------------------------------------------------------
        
        // 1. Configuration Panel (Top Left)
        ImGui::BeginChild("ConfigPanel", ImVec2(0, 350), true);
        ImGui::TextColored(ImVec4(0.92f, 0.95f, 0.98f, 1.00f), "Configuration");
        ImGui::Separator();

        // Mode Toggle
        ImGui::TextDisabled("Input Mode:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Presets", config_mode == 0)) {
            config_mode = 0;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Custom", config_mode == 1)) {
            if (config_mode != 1) {
                config_mode = 1;
                custom_cache_size = CACHE_SIZE_OPTIONS[cache_size_idx].value;
                custom_block_size = BLOCK_SIZE_OPTIONS[block_size_idx].value;
                custom_associativity = ASSOC_OPTIONS[assoc_idx].value;
            }
        }

        ImGui::Spacing();

        if (config_mode == 0) {
            // Dropdown Mode
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 170.0f);
            
            const char* cs_labels[] = { "256 Bytes", "512 Bytes", "1 KB", "2 KB", "4 KB", "8 KB", "16 KB", "64 KB" };
            if (ImGui::Combo("Cache Size", &cache_size_idx, cs_labels, IM_ARRAYSIZE(cs_labels))) {
                int cs = CACHE_SIZE_OPTIONS[cache_size_idx].value;
                int bs = BLOCK_SIZE_OPTIONS[block_size_idx].value;
                int as = ASSOC_OPTIONS[assoc_idx].value;
                ReplacementPolicy pol = (selected_policy == 1) ? ReplacementPolicy::FIFO : ((selected_policy == 2) ? ReplacementPolicy::RANDOM : ReplacementPolicy::LRU);
                if (!cache.configure(cs, bs, as, pol)) {
                    status_message = "Error: Invalid cache configuration (check block size & associativity).";
                    status_is_error = true;
                } else {
                    status_message = "Reconfigured: " + std::to_string(cs) + "B Size, " + std::to_string(bs) + "B Block.";
                    status_is_error = false;
                    execution_logs.clear();
                    current_step = 0;
                    active_set = -1;
                    active_way = -1;
                }
            }

            const char* bs_labels[] = { "16 Bytes", "32 Bytes", "64 Bytes", "128 Bytes", "256 Bytes" };
            if (ImGui::Combo("Block Size (Bytes)", &block_size_idx, bs_labels, IM_ARRAYSIZE(bs_labels))) {
                int cs = CACHE_SIZE_OPTIONS[cache_size_idx].value;
                int bs = BLOCK_SIZE_OPTIONS[block_size_idx].value;
                int as = ASSOC_OPTIONS[assoc_idx].value;
                ReplacementPolicy pol = (selected_policy == 1) ? ReplacementPolicy::FIFO : ((selected_policy == 2) ? ReplacementPolicy::RANDOM : ReplacementPolicy::LRU);
                if (!cache.configure(cs, bs, as, pol)) {
                    status_message = "Error: Block size exceeds cache size.";
                    status_is_error = true;
                } else {
                    status_message = "Reconfigured: Block size set to " + std::to_string(bs) + " Bytes.";
                    status_is_error = false;
                    execution_logs.clear();
                    current_step = 0;
                    active_set = -1;
                    active_way = -1;
                }
            }

            const char* as_labels[] = { "1-Way Direct", "2-Way Set Associative", "4-Way Set Associative", "8-Way Set Associative", "16-Way Set Associative" };
            if (ImGui::Combo("Associativity", &assoc_idx, as_labels, IM_ARRAYSIZE(as_labels))) {
                int cs = CACHE_SIZE_OPTIONS[cache_size_idx].value;
                int bs = BLOCK_SIZE_OPTIONS[block_size_idx].value;
                int as = ASSOC_OPTIONS[assoc_idx].value;
                ReplacementPolicy pol = (selected_policy == 1) ? ReplacementPolicy::FIFO : ((selected_policy == 2) ? ReplacementPolicy::RANDOM : ReplacementPolicy::LRU);
                if (!cache.configure(cs, bs, as, pol)) {
                    status_message = "Error: Associativity exceeds total available lines.";
                    status_is_error = true;
                } else {
                    status_message = "Reconfigured: Set to " + std::to_string(as) + "-Way Associativity.";
                    status_is_error = false;
                    execution_logs.clear();
                    current_step = 0;
                    active_set = -1;
                    active_way = -1;
                }
            }

            const char* policy_list[] = { "LRU", "FIFO", "Random" };
            if (ImGui::Combo("Replacement Policy", &selected_policy, policy_list, IM_ARRAYSIZE(policy_list))) {
                int cs = CACHE_SIZE_OPTIONS[cache_size_idx].value;
                int bs = BLOCK_SIZE_OPTIONS[block_size_idx].value;
                int as = ASSOC_OPTIONS[assoc_idx].value;
                ReplacementPolicy pol = (selected_policy == 1) ? ReplacementPolicy::FIFO : ((selected_policy == 2) ? ReplacementPolicy::RANDOM : ReplacementPolicy::LRU);
                cache.configure(cs, bs, as, pol);
            }

            ImGui::PopItemWidth();
        } else {
            // Numerical Custom Inputs
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 170.0f);
            ImGui::InputInt("Cache Size (B)", &custom_cache_size, 256, 1024);
            ImGui::InputInt("Block Size (B)", &custom_block_size, 16, 64);
            ImGui::InputInt("Associativity (n)", &custom_associativity, 1, 2);

            const char* policy_list[] = { "LRU", "FIFO", "Random" };
            ImGui::Combo("Replacement Policy", &selected_policy, policy_list, IM_ARRAYSIZE(policy_list));
            ImGui::PopItemWidth();

            if (ImGui::Button(" Apply Parameters ", ImVec2(-1, 26))) {
                ReplacementPolicy pol = (selected_policy == 1) ? ReplacementPolicy::FIFO : ((selected_policy == 2) ? ReplacementPolicy::RANDOM : ReplacementPolicy::LRU);
                if (!cache.configure(custom_cache_size, custom_block_size, custom_associativity, pol)) {
                    status_message = "Error: Parameters must be Powers of 2 & valid cache math constraints.";
                    status_is_error = true;
                } else {
                    status_message = "Applied custom parameters.";
                    status_is_error = false;
                    execution_logs.clear();
                    current_step = 0;
                    active_set = -1;
                    active_way = -1;
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Toolbar Buttons (Load Trace, Run Simulation, Step, Auto Play, Reset)
        float avail_w = ImGui::GetContentRegionAvail().x;
        float item_spacing = ImGui::GetStyle().ItemSpacing.x;
        float btn_w = (avail_w - (item_spacing * 4.0f)) / 5.0f;

        // 1. Load Trace Button (Blue) -> Parses user-entered trace instructions
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.53f, 0.90f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.62f, 0.98f, 1.0f));
        if (ImGui::Button("Load Trace", ImVec2(btn_w, 34))) {
            if (ensureTraceLoaded()) {
                current_step = 0;
                cache.reset();
                execution_logs.clear();
                active_set = -1;
                active_way = -1;
                auto_play = false;
                if (trace_entries.empty()) {
                    status_message = "Error: Trace editor is empty. Please enter memory instructions.";
                    status_is_error = true;
                } else {
                    status_message = "Loaded " + std::to_string(trace_entries.size()) + " instructions.";
                    status_is_error = false;
                    execution_logs.push_back("[Info] Loaded " + std::to_string(trace_entries.size()) + " user trace instructions.");
                }
            }
        }
        ImGui::PopStyleColor(2);

        // 2. Run Simulation Button (Emerald Green)
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.00f, 0.78f, 0.38f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.00f, 0.90f, 0.45f, 1.0f));
        if (ImGui::Button("Run Sim", ImVec2(btn_w, 34))) {
            if (!cache.isConfigured()) {
                status_message = "Error: Cache configuration invalid.";
                status_is_error = true;
            } else if (ensureTraceLoaded()) {
                if (trace_entries.empty()) {
                    status_message = "Error: Memory trace instructions are empty.";
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
                        snprintf(inspect_addr_str, sizeof(inspect_addr_str), "0x%llX", (unsigned long long)res.address);
                        execution_logs.push_back(formatLogEntry(res));
                    }
                    status_message = "Ran all " + std::to_string(trace_entries.size()) + " instructions successfully.";
                    status_is_error = false;
                }
            }
        }
        ImGui::PopStyleColor(2);

        // 3. Step Button
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.24f, 0.32f, 1.0f));
        if (ImGui::Button("Step", ImVec2(btn_w, 34))) {
            if (!cache.isConfigured()) {
                status_message = "Error: Cache configuration invalid.";
                status_is_error = true;
            } else if (ensureTraceLoaded()) {
                if (trace_entries.empty()) {
                    status_message = "Error: Memory trace instructions are empty.";
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
                    snprintf(inspect_addr_str, sizeof(inspect_addr_str), "0x%llX", (unsigned long long)res.address);
                    execution_logs.push_back(formatLogEntry(res));
                    if (current_step >= trace_entries.size()) {
                        status_message = "Trace execution complete (" + std::to_string(trace_entries.size()) + " instructions).";
                    } else {
                        status_message = "Executed Step " + std::to_string(current_step) + " of " + std::to_string(trace_entries.size());
                    }
                    status_is_error = false;
                }
            }
        }
        ImGui::PopStyleColor();

        // 4. Auto Play / Pause Toggle Button
        ImGui::SameLine();
        if (auto_play) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.40f, 0.10f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.95f, 0.50f, 0.15f, 1.0f));
            if (ImGui::Button("Pause", ImVec2(btn_w, 34))) {
                auto_play = false;
                status_message = "Auto-play paused.";
                status_is_error = false;
            }
            ImGui::PopStyleColor(2);
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.35f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.55f, 0.45f, 1.0f));
            if (ImGui::Button("Auto Play", ImVec2(btn_w, 34))) {
                if (!cache.isConfigured()) {
                    status_message = "Error: Configure cache parameters first.";
                    status_is_error = true;
                } else if (ensureTraceLoaded()) {
                    if (trace_entries.empty()) {
                        status_message = "Error: Memory trace is empty.";
                        status_is_error = true;
                    } else {
                        auto_play = true;
                        last_step_time = std::chrono::steady_clock::now();
                        status_message = "Auto-play started.";
                        status_is_error = false;
                    }
                }
            }
            ImGui::PopStyleColor(2);
        }

        // 5. Reset Button
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.24f, 0.32f, 1.0f));
        if (ImGui::Button("Reset", ImVec2(btn_w, 34))) {
            cache.reset();
            ensureTraceLoaded();
            execution_logs.clear();
            current_step = 0;
            active_set = -1;
            active_way = -1;
            auto_play = false;
            status_message = "Cache reset complete.";
            status_is_error = false;
        }
        ImGui::PopStyleColor();

        ImGui::EndChild();

        // 2. Trace Editor Panel (Bottom Left)
        ImGui::BeginChild("TraceEditorPanel", ImVec2(0, 0), true);
        ImGui::TextColored(ImVec4(0.92f, 0.95f, 0.98f, 1.00f), "Trace Editor");
        ImGui::SameLine(ImGui::GetWindowWidth() - 40.0f);
        ImGui::TextDisabled("...");
        ImGui::Separator();

        // File Path Loader Row
        ImGui::TextDisabled("Trace File:");
        ImGui::SameLine();
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 100.0f);
        ImGui::InputText("##FilePathInput", file_path_input, sizeof(file_path_input));
        ImGui::PopItemWidth();

        ImGui::SameLine();
        if (ImGui::Button("Load File")) {
            loadTraceFile(file_path_input, trace_buffer, sizeof(trace_buffer), status_message, status_is_error, trace_entries, cache, execution_logs, current_step, active_set, active_way, auto_play);
        }

        ImGui::TextDisabled("Presets:");
        ImGui::SameLine();
        if (ImGui::SmallButton("memory_trace.txt")) {
            snprintf(file_path_input, sizeof(file_path_input), "memory_trace.txt");
            loadTraceFile(file_path_input, trace_buffer, sizeof(trace_buffer), status_message, status_is_error, trace_entries, cache, execution_logs, current_step, active_set, active_way, auto_play);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("trace.txt")) {
            snprintf(file_path_input, sizeof(file_path_input), "trace.txt");
            loadTraceFile(file_path_input, trace_buffer, sizeof(trace_buffer), status_message, status_is_error, trace_entries, cache, execution_logs, current_step, active_set, active_way, auto_play);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("trace_loop.txt")) {
            snprintf(file_path_input, sizeof(file_path_input), "trace_loop.txt");
            loadTraceFile(file_path_input, trace_buffer, sizeof(trace_buffer), status_message, status_is_error, trace_entries, cache, execution_logs, current_step, active_set, active_way, auto_play);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("trace_thrash.txt")) {
            snprintf(file_path_input, sizeof(file_path_input), "trace_thrash.txt");
            loadTraceFile(file_path_input, trace_buffer, sizeof(trace_buffer), status_message, status_is_error, trace_entries, cache, execution_logs, current_step, active_set, active_way, auto_play);
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Quick Instruction Entry Form
        ImGui::TextDisabled("Add Instruction:");
        ImGui::SameLine();
        const char* ops[] = { "R", "W" };
        ImGui::PushItemWidth(55);
        ImGui::Combo("##OpSelect", &quick_op_idx, ops, IM_ARRAYSIZE(ops));
        ImGui::PopItemWidth();

        ImGui::SameLine();
        ImGui::PushItemWidth(95);
        ImGui::InputText("##AddrInput", quick_addr_input, IM_ARRAYSIZE(quick_addr_input));
        ImGui::PopItemWidth();

        ImGui::SameLine();
        if (ImGui::Button(" Add ")) {
            std::string addrTrimmed = trim(quick_addr_input);
            if (!addrTrimmed.empty()) {
                std::string lineStr = std::string(quick_op_idx == 0 ? "R " : "W ") + addrTrimmed + "\n";
                size_t currentLen = strlen(trace_buffer);
                if (currentLen + lineStr.size() < sizeof(trace_buffer)) {
                    strcat(trace_buffer, lineStr.c_str());
                    ensureTraceLoaded();
                    status_message = "Added instruction: " + trim(lineStr);
                    status_is_error = false;
                }
            }
        }

        ImGui::SameLine();
        if (ImGui::Button(" Clear ")) {
            trace_buffer[0] = '\0';
            trace_entries.clear();
            current_step = 0;
            active_set = -1;
            active_way = -1;
            cache.reset();
            execution_logs.clear();
            auto_play = false;
            status_message = "Trace editor and cache state cleared.";
            status_is_error = false;
        }

        ImGui::Spacing();

        // Line numbers gutter + Monospaced text area split
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 0.0f));
        
        float full_avail_y = std::max(100.0f, ImGui::GetContentRegionAvail().y - 24.0f);
        
        // Line Gutter Panel
        ImGui::BeginChild("LineGutter", ImVec2(38, full_avail_y), false, ImGuiWindowFlags_NoScrollbar);
        std::istringstream gstream(trace_buffer);
        std::string gline;
        int lnum = 1;
        while (std::getline(gstream, gline)) {
            char lbuf[16];
            snprintf(lbuf, sizeof(lbuf), "%02d", lnum++);
            if (lnum - 1 == (int)current_step + 1) {
                ImGui::TextColored(ImVec4(0.00f, 0.80f, 0.95f, 1.0f), "● %s", lbuf);
            } else {
                ImGui::TextDisabled("  %s", lbuf);
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        // Multiline Text Buffer
        if (ImGui::InputTextMultiline("##TraceEditorArea", trace_buffer, IM_ARRAYSIZE(trace_buffer), 
                                      ImVec2(-1, full_avail_y), ImGuiInputTextFlags_AllowTabInput)) {
            ParseResult pr = parseTraceTextEx(trace_buffer);
            if (!pr.valid) {
                status_message = "Warning: " + pr.error_message;
                status_is_error = true;
                trace_entries.clear();
            } else {
                trace_entries = pr.entries;
                status_message = "Trace updated (" + std::to_string(trace_entries.size()) + " valid instructions).";
                status_is_error = false;
            }
            current_step = 0;
        }
        ImGui::PopStyleVar();

        ImGui::TextDisabled("Trace Count: %zu instructions", trace_entries.size());

        ImGui::EndChild();

        ImGui::NextColumn();

        // ---------------------------------------------------------
        // RIGHT COLUMN: RESULTS PANEL & CACHE VISUALIZATION
        // ---------------------------------------------------------
        ImGui::BeginChild("RightContainer", ImVec2(0, 0), true);

        // 1. Results Panel (Top Right Dashboard Cards)
        ImGui::TextColored(ImVec4(0.92f, 0.95f, 0.98f, 1.00f), "Results Panel");
        ImGui::Separator();

        uint64_t hits = cache.getHits();
        uint64_t misses = cache.getMisses();
        uint64_t evictions = cache.getEvictions();
        uint64_t total = hits + misses;

        float hit_ratio = total > 0 ? (float)hits / total : 0.0f;
        float miss_ratio = total > 0 ? (float)misses / total : 0.0f;
        (void)miss_ratio;

        float c_width = (ImGui::GetContentRegionAvail().x - 20.0f) / 2.0f;

        ImGui::Columns(2, "ResultsGrid", false);
        ImGui::SetColumnWidth(0, c_width + 10.0f);

        // Cache Hits Card (Cyan/Emerald Highlight Card matching mockup)
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.20f, 0.26f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.00f, 0.70f, 0.90f, 1.0f));
        ImGui::BeginChild("HitCard", ImVec2(0, 68), true);
        ImGui::TextColored(ImVec4(0.00f, 0.85f, 0.95f, 1.0f), "[v] Cache Hits");
        ImGui::TextColored(ImVec4(1.00f, 1.00f, 1.00f, 1.0f), "%llu", (unsigned long long)hits);
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::NextColumn();

        // Cache Misses Card (Red/Dark Card matching mockup)
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.18f, 0.12f, 0.14f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.65f, 0.20f, 0.25f, 1.0f));
        ImGui::BeginChild("MissCard", ImVec2(0, 68), true);
        ImGui::TextColored(ImVec4(0.85f, 0.40f, 0.45f, 1.0f), "[!] Cache Misses");
        ImGui::TextColored(ImVec4(1.00f, 1.00f, 1.00f, 1.0f), "%llu", (unsigned long long)misses);
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::NextColumn();

        // Evictions Card
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.20f, 0.15f, 0.08f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.80f, 0.50f, 0.15f, 1.0f));
        ImGui::BeginChild("EvictCard", ImVec2(0, 68), true);
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.20f, 1.0f), "[T] Evictions");
        ImGui::TextColored(ImVec4(1.00f, 1.00f, 1.00f, 1.0f), "%llu", (unsigned long long)evictions);
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::NextColumn();

        // Hit Rate % Card
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.08f, 0.18f, 0.18f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.00f, 0.75f, 0.60f, 1.0f));
        ImGui::BeginChild("HitRateCard", ImVec2(0, 68), true);
        ImGui::TextColored(ImVec4(0.00f, 0.85f, 0.70f, 1.0f), "[~] Hit Rate %%");
        ImGui::TextColored(ImVec4(1.00f, 1.00f, 1.00f, 1.0f), "%.1f%%", hit_ratio * 100.0f);
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::NextColumn();

        ImGui::Columns(1);

        // Total Accesses Card (Full Width)
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.12f, 0.16f, 0.22f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.20f, 0.30f, 0.42f, 1.0f));
        ImGui::BeginChild("TotalCard", ImVec2(0, 46), true);
        ImGui::TextDisabled("Total Accesses:");
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.90f, 0.95f, 1.00f, 1.0f), "%llu instructions", (unsigned long long)total);
        ImGui::EndChild();
        ImGui::PopStyleColor(2);

        ImGui::Separator();

        // 2. Cache Visualization Table (Bottom Right)
        ImGui::TextColored(ImVec4(0.92f, 0.95f, 0.98f, 1.00f), "Cache Visualization");
        ImGui::SameLine(ImGui::GetWindowWidth() - 40.0f);
        ImGui::TextDisabled("...");

        if (cache.isConfigured() && cache.getNumSets() > 0) {
            uint32_t num_sets = cache.getNumSets();
            uint32_t assoc = cache.getAssociativity();
            const auto& sets = cache.getSets();

            float remaining_h = ImGui::GetContentRegionAvail().y;
            float table_h = std::max(200.0f, remaining_h * 0.58f);

            if (ImGui::BeginTable("CacheMatrixTable", assoc + 1, 
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, 
                                  ImVec2(0, table_h))) {
                
                ImGui::TableSetupScrollFreeze(1, 1);
                ImGui::TableSetupColumn("Sets", ImGuiTableColumnFlags_WidthFixed, 80.0f);
                for (uint32_t w = 0; w < assoc; ++w) {
                    std::string wayHeader = "Way " + std::to_string(w);
                    ImGui::TableSetupColumn(wayHeader.c_str(), ImGuiTableColumnFlags_WidthStretch);
                }
                ImGui::TableHeadersRow();

                for (uint32_t s = 0; s < num_sets; ++s) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    
                    char slabel[32];
                    snprintf(slabel, sizeof(slabel), "Set %u  S%u", s, s);
                    if (static_cast<int>(s) == active_set) {
                        ImGui::TextColored(ImVec4(0.00f, 0.85f, 0.95f, 1.0f), "%s", slabel);
                    } else {
                        ImGui::Text("%s", slabel);
                    }

                    for (uint32_t w = 0; w < assoc; ++w) {
                        ImGui::TableSetColumnIndex(w + 1);
                        const auto& line = sets[s].lines[w];

                        bool isActive = (static_cast<int>(s) == active_set && static_cast<int>(w) == active_way);
                        
                        // Custom Cell Backgrounds matching Mockup Image
                        if (isActive) {
                            if (active_hit) {
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(0, 200, 115, 255)); // Emerald Hit
                            } else if (active_evicted) {
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(230, 60, 60, 255)); // Red Miss/Evict
                            } else {
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(255, 140, 0, 255)); // Orange Eviction
                            }
                        } else if (line.valid) {
                            ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(15, 85, 130, 255)); // Electric Blue Loaded Block
                        }

                        if (line.valid) {
                            if (isActive && active_hit) {
                                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Hit");
                            } else if (isActive && active_evicted) {
                                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "Miss");
                            } else {
                                ImGui::Text("V:1 Tag:0x%X", (unsigned int)line.tag);
                            }

                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Set %u, Way %u\nTag Hex: 0x%X\nLast Used: Tick #%llu", 
                                                  s, w, (unsigned int)line.tag, 
                                                  (unsigned long long)line.last_access);
                            }
                        } else {
                            ImGui::TextDisabled("V:0 Invalid");
                        }
                    }
                }
                ImGui::EndTable();
            }
        } else {
            ImGui::TextDisabled("Cache matrix inactive.");
        }

        ImGui::Separator();

        // 3. Execution Log (Below Matrix)
        ImGui::TextColored(ImVec4(0.92f, 0.95f, 0.98f, 1.00f), "Execution Log");
        ImGui::BeginChild("ExecutionLogConsole", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& logEntry : execution_logs) {
            if (logEntry.find("Hit") != std::string::npos) {
                ImGui::TextColored(ImVec4(0.00f, 0.85f, 0.45f, 1.0f), "%s", logEntry.c_str());
            } else if (logEntry.find("Evicting") != std::string::npos) {
                ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.40f, 1.0f), "%s", logEntry.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.90f, 0.70f, 0.20f, 1.0f), "%s", logEntry.c_str());
            }
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) {
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        ImGui::EndChild();

        ImGui::End();

        ImGui::Render();
        int display_w, display_h;
        SDL_GL_GetDrawableSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.07f, 0.09f, 0.12f, 1.00f);
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
