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
    {16384, "16 KB (16384 B)"}
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

// Parse memory trace string into structured entries
static std::vector<TraceEntry> parseTraceText(const std::string& text) {
    std::vector<TraceEntry> entries;
    std::istringstream stream(text);
    std::string line;

    while (std::getline(stream, line)) {
        size_t c = line.find_first_of("#/;");
        if (c != std::string::npos) line = line.substr(0, c);

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

        status_message = "Loaded " + std::to_string(trace_entries.size()) + " instructions from '" + cleanPath + "'";
        status_is_error = false;
        return true;
    } else {
        status_message = "Error: Could not open trace file '" + cleanPath + "'. Check file path.";
        status_is_error = true;
        return false;
    }
}

// Format execution log entry
static std::string formatLogEntry(const StepResult& res) {
    std::ostringstream ss;
    ss << "[" << (res.op == 'W' ? "WRITE" : "READ ") << "] "
       << "Addr: 0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8) << res.address
       << " -> Set " << std::dec << res.set_index
       << ", Tag 0x" << std::hex << std::uppercase << res.tag << " -> ";

    if (res.hit) {
        ss << "HIT (Way " << std::dec << res.way << ")";
    } else if (res.evicted) {
        ss << "MISS & EVICT (Way " << std::dec << res.evicted_way << ")";
    } else {
        ss << "MISS (Way " << std::dec << res.way << " Filled)";
    }
    return ss.str();
}

static void ApplyUltraTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    
    style.WindowPadding     = ImVec2(12.0f, 12.0f);
    style.FramePadding      = ImVec2(10.0f, 6.0f);
    style.ItemSpacing       = ImVec2(10.0f, 8.0f);
    style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
    style.IndentSpacing     = 18.0f;
    style.ScrollbarSize     = 13.0f;
    style.GrabMinSize       = 13.0f;

    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 7.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 5.0f;
    style.TabRounding       = 5.0f;

    style.WindowBorderSize  = 1.0f;
    style.ChildBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.95f, 0.96f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.52f, 0.58f, 0.65f, 1.00f);
    colors[ImGuiCol_WindowBg]              = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
    colors[ImGuiCol_ChildBg]               = ImVec4(0.10f, 0.13f, 0.17f, 0.98f);
    colors[ImGuiCol_PopupBg]               = ImVec4(0.11f, 0.14f, 0.19f, 1.00f);
    colors[ImGuiCol_Border]                = ImVec4(0.19f, 0.24f, 0.31f, 1.00f);
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.14f, 0.18f, 0.24f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.20f, 0.26f, 0.35f, 1.00f);
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.25f, 0.32f, 0.42f, 1.00f);
    colors[ImGuiCol_TitleBg]               = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
    colors[ImGuiCol_TitleBgActive]         = ImVec4(0.12f, 0.16f, 0.22f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
    colors[ImGuiCol_MenuBarBg]             = ImVec4(0.10f, 0.13f, 0.17f, 1.00f);
    colors[ImGuiCol_ScrollbarBg]           = ImVec4(0.06f, 0.08f, 0.11f, 1.00f);
    colors[ImGuiCol_ScrollbarGrab]         = ImVec4(0.20f, 0.26f, 0.34f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.28f, 0.36f, 0.48f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.34f, 0.44f, 0.58f, 1.00f);
    colors[ImGuiCol_CheckMark]             = ImVec4(0.25f, 0.70f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.25f, 0.70f, 1.00f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.40f, 0.80f, 1.00f, 1.00f);
    colors[ImGuiCol_Button]                = ImVec4(0.15f, 0.21f, 0.29f, 1.00f);
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.22f, 0.31f, 0.44f, 1.00f);
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.28f, 0.40f, 0.56f, 1.00f);
    colors[ImGuiCol_Header]                = ImVec4(0.16f, 0.24f, 0.34f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.24f, 0.34f, 0.48f, 1.00f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.30f, 0.42f, 0.58f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.19f, 0.24f, 0.31f, 1.00f);
    colors[ImGuiCol_SeparatorHovered]      = ImVec4(0.25f, 0.70f, 1.00f, 1.00f);
    colors[ImGuiCol_SeparatorActive]       = ImVec4(0.40f, 0.80f, 1.00f, 1.00f);
    colors[ImGuiCol_ResizeGrip]            = ImVec4(0.20f, 0.26f, 0.34f, 1.00f);
    colors[ImGuiCol_ResizeGripHovered]     = ImVec4(0.25f, 0.70f, 1.00f, 1.00f);
    colors[ImGuiCol_ResizeGripActive]      = ImVec4(0.40f, 0.80f, 1.00f, 1.00f);
    colors[ImGuiCol_Tab]                   = ImVec4(0.10f, 0.14f, 0.20f, 1.00f);
    colors[ImGuiCol_TabHovered]            = ImVec4(0.22f, 0.31f, 0.44f, 1.00f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.18f, 0.26f, 0.38f, 1.00f);
    colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.12f, 0.16f, 0.22f, 1.00f);
    colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.19f, 0.24f, 0.31f, 1.00f);
    colors[ImGuiCol_TableBorderLight]      = ImVec4(0.15f, 0.19f, 0.25f, 1.00f);
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
    SDL_Window* window = SDL_CreateWindow("USTP Digital IC Design - Parameterized Cache Simulator", 
                                          SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 
                                          target_w, target_h, window_flags);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl_context);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ApplyUltraTheme();

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    CacheCore cache;
    int config_mode = 0; // 0: Presets, 1: Custom User Values
    int cache_size_idx = 2; // Default 1024 B
    int block_size_idx = 2; // Default 64 B
    int assoc_idx = 2;      // Default 4-Way
    int selected_policy = 0; // 0: LRU, 1: FIFO, 2: Random

    int custom_cache_size = 1024;
    int custom_block_size = 64;
    int custom_associativity = 4;

    char trace_buffer[65536] = 
        "R 0x1000\n"
        "W 0x1004\n"
        "R 0x2000\n"
        "R 0x1000\n"
        "R 0x1040\n"
        "W 0x3000\n"
        "R 0x1000\n";

    char trace_file_path[256] = "trace.txt";
    std::vector<TraceEntry> trace_entries;
    size_t current_step = 0;

    std::vector<std::string> execution_logs;
    std::string status_message = "Ready. Enter memory trace or click 'RUN ALL' / 'STEP'.";
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

        // Auto-play timer logic
        if (auto_play) {
            auto now = std::chrono::steady_clock::now();
            float elapsed = std::chrono::duration<float, std::milli>(now - last_step_time).count();
            if (elapsed >= auto_play_speed_ms) {
                last_step_time = now;

                if (trace_entries.empty()) {
                    trace_entries = parseTraceText(trace_buffer);
                }

                if (!cache.isConfigured()) {
                    status_message = "Error: Please configure valid cache parameters first.";
                    status_is_error = true;
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
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "  USTP DIGITAL IC DESIGN");
        ImGui::SameLine();
        ImGui::TextDisabled("| Parameterized Cache Architecture Simulator");

        ImGui::SameLine(ImGui::GetWindowWidth() - 360.0f);
        ImGui::TextDisabled("[%dx%d]", (int)io.DisplaySize.x, (int)io.DisplaySize.y);
        ImGui::SameLine();
        if (auto_play) {
            ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.45f, 1.0f), "[AUTO-PLAY ACTIVE]");
        } else if (current_step > 0 && current_step >= trace_entries.size()) {
            ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "[SIMULATION COMPLETE]");
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
            ImGui::SetColumnWidth(0, std::clamp(io.DisplaySize.x * 0.38f, 480.0f, 620.0f));
            init_split = true;
        } else {
            ImGui::Columns(2, "MainLayout", true);
        }

        // ---------------------------------------------------------
        // LEFT COLUMN: CONFIGURATION, ACTION BAR, DECODER, TRACE
        // ---------------------------------------------------------
        
        // 1. Freely Editable Cache Configuration Panel
        ImGui::BeginChild("ConfigPanel", ImVec2(0, 260), true);
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "[CONFIG] CACHE ARCHITECTURE CONFIGURATION");
        ImGui::Separator();

        // Toggle Mode: Preset Dropdowns vs Custom User Manual Inputs
        ImGui::TextDisabled("Input Mode:");
        ImGui::SameLine();
        if (ImGui::RadioButton("Presets Dropdown", config_mode == 0)) {
            config_mode = 0;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Custom User Inputs", config_mode == 1)) {
            config_mode = 1;
        }

        ImGui::Separator();

        if (config_mode == 0) {
            // Preset Dropdowns Mode
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 140.0f);
            
            const char* cs_labels[] = { "256 Bytes", "512 Bytes", "1 KB (1024 B)", "2 KB (2048 B)", "4 KB (4096 B)", "8 KB (8192 B)", "16 KB (16384 B)" };
            if (ImGui::Combo("Cache Size", &cache_size_idx, cs_labels, IM_ARRAYSIZE(cs_labels))) {
                int cs = CACHE_SIZE_OPTIONS[cache_size_idx].value;
                int bs = BLOCK_SIZE_OPTIONS[block_size_idx].value;
                int as = ASSOC_OPTIONS[assoc_idx].value;
                ReplacementPolicy pol = (selected_policy == 1) ? ReplacementPolicy::FIFO : ((selected_policy == 2) ? ReplacementPolicy::RANDOM : ReplacementPolicy::LRU);
                if (!cache.configure(cs, bs, as, pol)) {
                    status_message = "Warning: Selected combination (Size/Block/Assoc) is invalid.";
                    status_is_error = true;
                } else {
                    status_message = "Reconfigured: " + std::to_string(cs) + "B Size, " + std::to_string(bs) + "B Block.";
                    status_is_error = false;
                    execution_logs.clear();
                    current_step = 0;
                }
            }

            const char* bs_labels[] = { "16 Bytes", "32 Bytes", "64 Bytes", "128 Bytes", "256 Bytes" };
            if (ImGui::Combo("Block Size", &block_size_idx, bs_labels, IM_ARRAYSIZE(bs_labels))) {
                int cs = CACHE_SIZE_OPTIONS[cache_size_idx].value;
                int bs = BLOCK_SIZE_OPTIONS[block_size_idx].value;
                int as = ASSOC_OPTIONS[assoc_idx].value;
                ReplacementPolicy pol = (selected_policy == 1) ? ReplacementPolicy::FIFO : ((selected_policy == 2) ? ReplacementPolicy::RANDOM : ReplacementPolicy::LRU);
                if (!cache.configure(cs, bs, as, pol)) {
                    status_message = "Warning: Block size exceeds cache size.";
                    status_is_error = true;
                } else {
                    status_message = "Reconfigured: Block size set to " + std::to_string(bs) + " Bytes.";
                    status_is_error = false;
                    execution_logs.clear();
                    current_step = 0;
                }
            }

            const char* as_labels[] = { "1-Way (Direct Mapped)", "2-Way Set Associative", "4-Way Set Associative", "8-Way Set Associative", "16-Way Set Associative" };
            if (ImGui::Combo("Associativity", &assoc_idx, as_labels, IM_ARRAYSIZE(as_labels))) {
                int cs = CACHE_SIZE_OPTIONS[cache_size_idx].value;
                int bs = BLOCK_SIZE_OPTIONS[block_size_idx].value;
                int as = ASSOC_OPTIONS[assoc_idx].value;
                ReplacementPolicy pol = (selected_policy == 1) ? ReplacementPolicy::FIFO : ((selected_policy == 2) ? ReplacementPolicy::RANDOM : ReplacementPolicy::LRU);
                if (!cache.configure(cs, bs, as, pol)) {
                    status_message = "Warning: Associativity exceeds total available lines.";
                    status_is_error = true;
                } else {
                    status_message = "Reconfigured: Set to " + std::to_string(as) + "-Way Associativity.";
                    status_is_error = false;
                    execution_logs.clear();
                    current_step = 0;
                }
            }

            const char* policy_list[] = { "LRU (Least Recently Used)", "FIFO (First In First Out)", "Random Replacement" };
            if (ImGui::Combo("Policy", &selected_policy, policy_list, IM_ARRAYSIZE(policy_list))) {
                int cs = CACHE_SIZE_OPTIONS[cache_size_idx].value;
                int bs = BLOCK_SIZE_OPTIONS[block_size_idx].value;
                int as = ASSOC_OPTIONS[assoc_idx].value;
                ReplacementPolicy pol = (selected_policy == 1) ? ReplacementPolicy::FIFO : ((selected_policy == 2) ? ReplacementPolicy::RANDOM : ReplacementPolicy::LRU);
                cache.configure(cs, bs, as, pol);
            }

            ImGui::PopItemWidth();
        } else {
            // Freely Editable Custom Numerical Inputs Mode
            ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 160.0f);
            ImGui::InputInt("Cache Size (B)", &custom_cache_size, 256, 1024);
            ImGui::InputInt("Block Size (B)", &custom_block_size, 16, 64);
            ImGui::InputInt("Associativity (n)", &custom_associativity, 1, 2);

            const char* policy_list[] = { "LRU (Least Recently Used)", "FIFO (First In First Out)", "Random Replacement" };
            ImGui::Combo("Policy", &selected_policy, policy_list, IM_ARRAYSIZE(policy_list));
            ImGui::PopItemWidth();

            if (ImGui::Button(" Apply Custom Parameters ", ImVec2(-1, 28))) {
                ReplacementPolicy pol = (selected_policy == 1) ? ReplacementPolicy::FIFO : ((selected_policy == 2) ? ReplacementPolicy::RANDOM : ReplacementPolicy::LRU);
                if (!cache.configure(custom_cache_size, custom_block_size, custom_associativity, pol)) {
                    status_message = "Error: Parameters must be Powers of 2 (Size >= Block Size >= 16, Assoc <= Lines).";
                    status_is_error = true;
                } else {
                    status_message = "Custom parameters applied: " + std::to_string(custom_cache_size) + "B Size, " + std::to_string(custom_block_size) + "B Block, " + std::to_string(custom_associativity) + "-Way.";
                    status_is_error = false;
                    execution_logs.clear();
                    current_step = 0;
                }
            }
        }

        // Architectural parameters summary badge
        if (cache.isConfigured()) {
            uint32_t num_sets = cache.getNumSets();
            uint32_t block_size = cache.getBlockSize();
            uint32_t offset_bits = static_cast<uint32_t>(std::log2(block_size));
            uint32_t index_bits = static_cast<uint32_t>(std::log2(num_sets));
            uint32_t tag_bits = 64 - index_bits - offset_bits;

            ImGui::Separator();
            ImGui::TextDisabled("Derived Specs:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.85f, 1.0f), "Sets: %u  |  Tag: %ub  |  Index: %ub  |  Offset: %ub",
                               num_sets, tag_bits, index_bits, offset_bits);
        }

        ImGui::EndChild();

        // 2. Action Toolbar & Playback Controls Panel
        ImGui::BeginChild("ActionPanel", ImVec2(0, 110), true);
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "[CONTROLS] SIMULATION CONTROLS");
        ImGui::Separator();

        auto ensureTraceLoaded = [&]() {
            if (trace_entries.empty()) {
                trace_entries = parseTraceText(trace_buffer);
            }
            if (trace_entries.empty() && trace_file_path[0] != '\0') {
                loadTraceFromFile(trace_file_path, trace_buffer, sizeof(trace_buffer), trace_entries, cache, current_step, execution_logs, status_message, status_is_error);
            }
        };

        // Proportional Button Sizing
        float avail_w = ImGui::GetContentRegionAvail().x;
        float btn_w = (avail_w - 30.0f) / 4.0f;

        // Run All Button (Emerald Green)
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.12f, 0.55f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.70f, 0.38f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.24f, 0.80f, 0.44f, 1.0f));
        if (ImGui::Button(" RUN ALL ", ImVec2(btn_w, 32))) {
            ensureTraceLoaded();

            if (!cache.isConfigured()) {
                status_message = "Error: Cache configuration invalid.";
                status_is_error = true;
            } else if (trace_entries.empty()) {
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
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Execute all trace instructions instantly to final state.");

        // Step Button (Deep Sky Blue)
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.45f, 0.75f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.20f, 0.58f, 0.88f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.25f, 0.68f, 0.98f, 1.0f));
        if (ImGui::Button(" STEP ", ImVec2(btn_w, 32))) {
            ensureTraceLoaded();

            if (!cache.isConfigured()) {
                status_message = "Error: Cache configuration invalid.";
                status_is_error = true;
            } else if (trace_entries.empty()) {
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
                status_message = "Executed Step " + std::to_string(current_step) + " of " + std::to_string(trace_entries.size());
                status_is_error = false;
            }
        }
        ImGui::PopStyleColor(3);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Execute a single trace instruction step-by-step.");

        // Auto Play / Pause Toggle
        ImGui::SameLine();
        if (auto_play) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.70f, 0.40f, 0.10f, 1.0f));
            if (ImGui::Button(" PAUSE ", ImVec2(btn_w, 32))) auto_play = false;
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.40f, 0.25f, 0.65f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.52f, 0.35f, 0.80f, 1.0f));
            if (ImGui::Button(" AUTO ", ImVec2(btn_w, 32))) {
                ensureTraceLoaded();

                if (!cache.isConfigured()) {
                    status_message = "Error: Cache configuration invalid.";
                    status_is_error = true;
                } else if (trace_entries.empty()) {
                    status_message = "Error: Memory trace instructions are empty.";
                    status_is_error = true;
                } else {
                    auto_play = true;
                }
            }
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Automatically step through trace at set timer interval.");
        }

        // Reset Button (Muted Dark Red)
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.50f, 0.15f, 0.15f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.68f, 0.22f, 0.22f, 1.0f));
        if (ImGui::Button(" RESET ", ImVec2(btn_w, 32))) {
            cache.reset();
            trace_entries = parseTraceText(trace_buffer);
            execution_logs.clear();
            current_step = 0;
            active_set = -1;
            active_way = -1;
            auto_play = false;
            status_message = "Cache reset complete. All state cleared.";
            status_is_error = false;
        }
        ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear cache state and restart trace index to 0.");

        // Speed Slider Inline
        ImGui::PushItemWidth(avail_w - 70.0f);
        ImGui::SliderFloat("Speed", &auto_play_speed_ms, 50.0f, 1000.0f, "%.0f ms / step");
        ImGui::PopItemWidth();

        ImGui::EndChild();

        // 3. Hardware Address Bit Decoder
        ImGui::BeginChild("DecoderPanel", ImVec2(0, 160), true);
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "[DECODER] HARDWARE ADDRESS BIT DECODER");
        ImGui::Separator();

        ImGui::PushItemWidth(150);
        ImGui::InputText("Address", inspect_addr_str, IM_ARRAYSIZE(inspect_addr_str));
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

            // Visual Badges
            ImGui::TextDisabled("Extracted Fields:");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.35f, 0.70f, 1.00f, 1.0f), "[TAG: 0x%llX (%ub)]", (unsigned long long)tag_val, tag_bits);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.85f, 1.0f), "[SET: %u (%ub)]", set_idx, index_bits);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.00f, 0.60f, 0.20f, 1.0f), "[OFFSET: 0x%X (%ub)]", block_offset, offset_bits);

            // Bit-level Binary Display
            ImGui::TextDisabled("64-Bit Binary Sequence:");
            std::string bin_str = std::bitset<64>(dec_addr).to_string();

            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            for (size_t b = 0; b < 64; ++b) {
                size_t bitIndexFromLSB = 63 - b;
                char bitChar[2] = { bin_str[b], '\0' };

                if (bitIndexFromLSB >= (offset_bits + index_bits)) {
                    ImGui::TextColored(ImVec4(0.35f, 0.70f, 1.00f, 1.0f), "%s", bitChar);
                } else if (bitIndexFromLSB >= offset_bits) {
                    ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.85f, 1.0f), "%s", bitChar);
                } else {
                    ImGui::TextColored(ImVec4(1.00f, 0.60f, 0.20f, 1.0f), "%s", bitChar);
                }

                if (b < 63 && (b + 1) % 4 == 0) {
                    ImGui::SameLine();
                    ImGui::TextDisabled(" ");
                }
                if (b < 63) ImGui::SameLine();
            }
            ImGui::PopStyleVar();
        } else {
            ImGui::TextDisabled("Configure cache to inspect address bit fields.");
        }

        ImGui::EndChild();

        // 4. Interactive User Memory Trace Panel
        ImGui::BeginChild("TracePanel", ImVec2(0, 0), true);
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "[TRACE] USER MEMORY TRACE INPUT & EDITOR");
        ImGui::Separator();

        // Interactive Quick Add Instruction Form
        ImGui::TextDisabled("Add Custom Instruction:");
        const char* ops[] = { "READ (R)", "WRITE (W)" };
        ImGui::PushItemWidth(110);
        ImGui::Combo("##QuickOp", &quick_op_idx, ops, IM_ARRAYSIZE(ops));
        ImGui::PopItemWidth();

        ImGui::SameLine();
        ImGui::PushItemWidth(130);
        ImGui::InputText("##QuickAddr", quick_addr_input, IM_ARRAYSIZE(quick_addr_input));
        ImGui::PopItemWidth();

        ImGui::SameLine();
        if (ImGui::Button(" [+] Add Instruction ")) {
            std::string lineStr = std::string(quick_op_idx == 0 ? "R " : "W ") + quick_addr_input + "\n";
            size_t currentLen = strlen(trace_buffer);
            if (currentLen + lineStr.size() < sizeof(trace_buffer)) {
                strcat(trace_buffer, lineStr.c_str());
                trace_entries = parseTraceText(trace_buffer);
                status_message = "Added instruction: " + lineStr;
                status_is_error = false;
            }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Append this access instruction to user trace buffer.");

        ImGui::SameLine();
        if (ImGui::Button(" [-] Clear Trace ")) {
            trace_buffer[0] = '\0';
            trace_entries.clear();
            current_step = 0;
            cache.reset();
            execution_logs.clear();
            status_message = "Memory trace buffer cleared. Ready for custom user trace input.";
            status_is_error = false;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear trace buffer completely to enter new custom trace.");

        ImGui::Separator();

        // Trace File Load & Presets
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x - 170.0f);
        if (ImGui::InputText("File Path", trace_file_path, IM_ARRAYSIZE(trace_file_path), ImGuiInputTextFlags_EnterReturnsTrue)) {
            loadTraceFromFile(trace_file_path, trace_buffer, sizeof(trace_buffer), trace_entries, cache, current_step, execution_logs, status_message, status_is_error);
        }
        ImGui::PopItemWidth();
        
        ImGui::SameLine();
        if (ImGui::Button("Load File")) {
            loadTraceFromFile(trace_file_path, trace_buffer, sizeof(trace_buffer), trace_entries, cache, current_step, execution_logs, status_message, status_is_error);
        }

        ImGui::SameLine();
        if (g_file_dialog_pending) {
            ImGui::Button("Browsing...", ImVec2(80, 0));
        } else {
            if (ImGui::Button(" [Browse] ")) {
                g_file_dialog_future = std::async(std::launch::async, OpenNativeFileDialogSync);
                g_file_dialog_pending = true;
            }
        }

        // Non-blocking file dialog check
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

        // Presets Buttons
        ImGui::TextDisabled("Presets:");
        ImGui::SameLine();
        if (ImGui::Button("[Default]")) {
            snprintf(trace_file_path, sizeof(trace_file_path), "trace.txt");
            loadTraceFromFile(trace_file_path, trace_buffer, sizeof(trace_buffer), trace_entries, cache, current_step, execution_logs, status_message, status_is_error);
        }
        ImGui::SameLine();
        if (ImGui::Button("[Loop]")) {
            snprintf(trace_file_path, sizeof(trace_file_path), "trace_loop.txt");
            loadTraceFromFile(trace_file_path, trace_buffer, sizeof(trace_buffer), trace_entries, cache, current_step, execution_logs, status_message, status_is_error);
        }
        ImGui::SameLine();
        if (ImGui::Button("[Thrash]")) {
            snprintf(trace_file_path, sizeof(trace_file_path), "trace_thrash.txt");
            loadTraceFromFile(trace_file_path, trace_buffer, sizeof(trace_buffer), trace_entries, cache, current_step, execution_logs, status_message, status_is_error);
        }

        // Live Trace Text Area (Parses continuously on keystroke)
        if (ImGui::InputTextMultiline("##TraceEditorArea", trace_buffer, IM_ARRAYSIZE(trace_buffer), 
                                      ImVec2(-1, -26), ImGuiInputTextFlags_AllowTabInput)) {
            trace_entries = parseTraceText(trace_buffer);
            current_step = 0;
        }

        ImGui::TextDisabled("User Trace Count: %zu instructions | Step Index: %zu", trace_entries.size(), current_step);

        ImGui::EndChild();

        ImGui::NextColumn();

        // ---------------------------------------------------------
        // RIGHT COLUMN: METRIC CARDS, VISUALIZER MATRIX & LOGS
        // ---------------------------------------------------------
        ImGui::BeginChild("RightContainer", ImVec2(0, 0), true);

        // Status Callout Banner
        if (!status_message.empty()) {
            ImVec4 bannerBg = status_is_error ? ImVec4(0.40f, 0.12f, 0.12f, 0.9f) : ImVec4(0.12f, 0.35f, 0.20f, 0.9f);
            ImVec4 bannerBorder = status_is_error ? ImVec4(0.90f, 0.30f, 0.30f, 1.0f) : ImVec4(0.25f, 0.80f, 0.45f, 1.0f);
            
            ImGui::PushStyleColor(ImGuiCol_ChildBg, bannerBg);
            ImGui::PushStyleColor(ImGuiCol_Border, bannerBorder);
            ImGui::BeginChild("StatusCallout", ImVec2(0, 34), true, ImGuiWindowFlags_NoScrollbar);
            ImGui::SetCursorPosY(6.0f);
            ImGui::TextColored(bannerBorder, " %s  %s", status_is_error ? "[ERR]" : "[INFO]", status_message.c_str());
            ImGui::EndChild();
            ImGui::PopStyleColor(2);
        }
        
        // 1. Statistics Cards Dashboard
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "[METRICS] PERFORMANCE METRICS DASHBOARD");
        ImGui::Separator();

        uint64_t hits = cache.getHits();
        uint64_t misses = cache.getMisses();
        uint64_t evictions = cache.getEvictions();
        uint64_t total = hits + misses;

        float hit_ratio = total > 0 ? (float)hits / total : 0.0f;
        float miss_ratio = total > 0 ? (float)misses / total : 0.0f;

        // Dynamic 3-Column Responsive Grid
        float card_width = (ImGui::GetContentRegionAvail().x - 20.0f) / 3.0f;

        ImGui::Columns(3, "MetricsGrid", false);
        ImGui::SetColumnWidth(0, card_width + 10.0f);
        ImGui::SetColumnWidth(1, card_width + 10.0f);
        
        // Hits Card
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.22f, 0.15f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.18f, 0.65f, 0.35f, 1.0f));
        ImGui::BeginChild("HitCard", ImVec2(0, 64), true);
        ImGui::TextDisabled("HITS");
        ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.45f, 1.0f), "%llu", (unsigned long long)hits);
        ImGui::SameLine(ImGui::GetWindowWidth() - 65.0f);
        ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.45f, 1.0f), "%.1f%%", hit_ratio * 100.0f);
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::NextColumn();

        // Misses Card
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.24f, 0.18f, 0.08f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.85f, 0.60f, 0.15f, 1.0f));
        ImGui::BeginChild("MissCard", ImVec2(0, 64), true);
        ImGui::TextDisabled("MISSES");
        ImGui::TextColored(ImVec4(0.95f, 0.68f, 0.18f, 1.0f), "%llu", (unsigned long long)misses);
        ImGui::SameLine(ImGui::GetWindowWidth() - 65.0f);
        ImGui::TextColored(ImVec4(0.95f, 0.68f, 0.18f, 1.0f), "%.1f%%", miss_ratio * 100.0f);
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::NextColumn();

        // Evictions Card
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.22f, 0.10f, 0.10f, 0.9f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.80f, 0.25f, 0.25f, 1.0f));
        ImGui::BeginChild("EvictCard", ImVec2(0, 64), true);
        ImGui::TextDisabled("EVICTIONS");
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%llu", (unsigned long long)evictions);
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::NextColumn();

        ImGui::Columns(1);

        // Hit Rate Bar
        ImGui::TextDisabled("Hit Rate Ratio:");
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.20f, 0.75f, 0.40f, 1.0f));
        ImGui::ProgressBar(hit_ratio, ImVec2(-1, 14), "");
        ImGui::PopStyleColor();

        ImGui::Separator();

        // 2. Dynamic Auto-Scaling Cache Visualizer Matrix
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "[MATRIX] CACHE STATE MATRIX (Sets x Ways)");
        if (cache.isConfigured() && cache.getNumSets() > 0) {
            uint32_t num_sets = cache.getNumSets();
            uint32_t assoc = cache.getAssociativity();
            const auto& sets = cache.getSets();

            float remaining_h = ImGui::GetContentRegionAvail().y;
            float table_h = std::max(220.0f, remaining_h * 0.58f);

            if (ImGui::BeginTable("VisualizerTable", assoc + 1, 
                                  ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, 
                                  ImVec2(0, table_h))) {
                
                ImGui::TableSetupScrollFreeze(1, 1);
                ImGui::TableSetupColumn("Set Index", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                for (uint32_t w = 0; w < assoc; ++w) {
                    std::string wayHeader = "Way " + std::to_string(w);
                    ImGui::TableSetupColumn(wayHeader.c_str(), ImGuiTableColumnFlags_WidthStretch);
                }
                ImGui::TableHeadersRow();

                for (uint32_t s = 0; s < num_sets; ++s) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    
                    if (static_cast<int>(s) == active_set) {
                        ImGui::TextColored(ImVec4(0.35f, 0.85f, 0.85f, 1.0f), "Set %u ->", s);
                    } else {
                        ImGui::Text("Set %u", s);
                    }

                    for (uint32_t w = 0; w < assoc; ++w) {
                        ImGui::TableSetColumnIndex(w + 1);
                        const auto& line = sets[s].lines[w];

                        bool isActive = (static_cast<int>(s) == active_set && static_cast<int>(w) == active_way);
                        if (isActive) {
                            if (active_hit) {
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(25, 140, 60, 255));
                            } else if (active_evicted) {
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(190, 40, 40, 255));
                            } else {
                                ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, IM_COL32(200, 140, 20, 255));
                            }
                        }

                        if (line.valid) {
                            if (isActive && active_hit) {
                                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "[HIT] 0x%llX", (unsigned long long)line.tag);
                            } else if (isActive && active_evicted) {
                                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "[EVICT] 0x%llX", (unsigned long long)line.tag);
                            } else if (isActive) {
                                ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "[MISS] 0x%llX", (unsigned long long)line.tag);
                            } else {
                                ImGui::Text("0x%llX", (unsigned long long)line.tag);
                            }

                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Location: Set %u, Way %u\nTag Hex: 0x%llX\nLast Access: Tick #%llu\nInsertion: Tick #%llu", 
                                                  s, w, (unsigned long long)line.tag, 
                                                  (unsigned long long)line.last_access, 
                                                  (unsigned long long)line.insertion_time);
                            }
                        } else {
                            ImGui::TextDisabled("[EMPTY]");
                        }
                    }
                }
                ImGui::EndTable();
            }
        } else {
            ImGui::TextDisabled("Cache matrix inactive.");
        }

        ImGui::Separator();

        // 3. Formatted Execution Log Console
        ImGui::TextColored(ImVec4(0.35f, 0.75f, 1.00f, 1.00f), "[LOGS] EXECUTION LOG & TRACE CONSOLE");
        ImGui::BeginChild("LogConsole", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& logEntry : execution_logs) {
            if (logEntry.find("HIT") != std::string::npos) {
                ImGui::TextColored(ImVec4(0.30f, 0.85f, 0.45f, 1.0f), "%s", logEntry.c_str());
            } else if (logEntry.find("EVICT") != std::string::npos) {
                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s", logEntry.c_str());
            } else {
                ImGui::TextColored(ImVec4(0.95f, 0.70f, 0.20f, 1.0f), "%s", logEntry.c_str());
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
        glClearColor(0.06f, 0.08f, 0.11f, 1.00f);
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
