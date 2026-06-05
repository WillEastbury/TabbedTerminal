// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// wt-launcher: A TUI launcher for Windows Terminal
// Renders a tabbed interface with VT sequences. On selection, spawns the chosen
// process attached to the same console and waits for it to exit.

#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <io.h>
#include <fcntl.h>

namespace fs = std::filesystem;

// ─── Data Structures ────────────────────────────────────────────────────────

struct ListItem
{
    std::wstring name;
    std::wstring detail;
    std::wstring timestamp;
    std::wstring command; // The command to exec on selection
};

enum class Tab
{
    Sessions = 0,
    Apps,
    Containers,
    NewContainer,
    COUNT
};

static const wchar_t* TabNames[] = { L"Sessions", L"Apps", L"Containers", L"New Container" };

// ─── Terminal Helpers ────────────────────────────────────────────────────────

static HANDLE hOut;
static HANDLE hIn;
static DWORD origOutMode;
static DWORD origInMode;
static int termWidth = 80;
static int termHeight = 24;

void enableVT()
{
    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    hIn = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(hOut, &origOutMode);
    GetConsoleMode(hIn, &origInMode);
    SetConsoleMode(hOut, origOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
    // Use raw console input mode: no line input, no echo, enable window events
    // Do NOT use ENABLE_VIRTUAL_TERMINAL_INPUT — we want KEY_EVENT records with VK codes
    SetConsoleMode(hIn, ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_PROCESSED_INPUT);

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(hOut, &csbi))
    {
        termWidth = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        termHeight = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
}

void restoreConsole()
{
    SetConsoleMode(hOut, origOutMode);
    SetConsoleMode(hIn, origInMode);
    // Show cursor, reset colors
    wprintf(L"\x1b[?25h\x1b[0m\x1b[2J\x1b[H");
}

void moveTo(int row, int col)
{
    wprintf(L"\x1b[%d;%dH", row, col);
}

void clearScreen()
{
    // Dark blue background fill
    wprintf(L"\x1b[48;2;15;23;42m\x1b[2J\x1b[H");
}

void hideCursor()
{
    wprintf(L"\x1b[?25l");
}

// Theme: dark navy background, yellow highlights, white text, cyan accents
void setThemeBg()
{
    wprintf(L"\x1b[48;2;15;23;42m");
}

void setColorYellow()
{
    wprintf(L"\x1b[38;2;255;215;0m\x1b[48;2;15;23;42m");
}

void setColorWhite()
{
    wprintf(L"\x1b[38;2;240;240;240m\x1b[48;2;15;23;42m");
}

void setColorCyan()
{
    wprintf(L"\x1b[38;2;100;220;255m\x1b[48;2;15;23;42m");
}

void setColorDimCyan()
{
    wprintf(L"\x1b[38;2;60;140;180m\x1b[48;2;15;23;42m");
}

void setColorHighlight()
{
    // Selected item: bright yellow on slightly lighter navy
    wprintf(L"\x1b[38;2;255;230;50m\x1b[48;2;30;45;80m");
}

void setColorDim()
{
    wprintf(L"\x1b[38;2;100;120;160m\x1b[48;2;15;23;42m");
}

void setColor(int fg, int bg)
{
    wprintf(L"\x1b[38;5;%dm\x1b[48;5;%dm", fg, bg);
}

void resetColor()
{
    wprintf(L"\x1b[0m");
}

void setBold()
{
    wprintf(L"\x1b[1m");
}

void setDim()
{
    setColorDim();
}

void setReverse()
{
    setColorHighlight();
}

// ─── Splash Screen ──────────────────────────────────────────────────────────

void showSplash()
{
    clearScreen();

    // Figlet-style "LAUNCH" in block characters
    static const wchar_t* logo[] = {
        L"  ██╗      █████╗ ██╗   ██╗███╗   ██╗ ██████╗██╗  ██╗",
        L"  ██║     ██╔══██╗██║   ██║████╗  ██║██╔════╝██║  ██║",
        L"  ██║     ███████║██║   ██║██╔██╗ ██║██║     ███████║",
        L"  ██║     ██╔══██║██║   ██║██║╚██╗██║██║     ██╔══██║",
        L"  ███████╗██║  ██║╚██████╔╝██║ ╚████║╚██████╗██║  ██║",
        L"  ╚══════╝╚═╝  ╚═╝ ╚═════╝ ╚═╝  ╚═══╝ ╚═════╝╚═╝  ╚═╝",
    };

    int startRow = (termHeight / 2) - 5;
    if (startRow < 1) startRow = 1;

    // Animate: reveal line by line with color sweep
    for (int i = 0; i < 6; i++)
    {
        moveTo(startRow + i, 1);
        // Gradient from cyan to yellow across the logo
        float t = (float)i / 5.0f;
        int r = (int)(100 + t * 155);
        int g = (int)(220 - t * 5);
        int b = (int)(255 - t * 205);
        wprintf(L"\x1b[38;2;%d;%d;%dm\x1b[48;2;15;23;42m%ls", r, g, b, logo[i]);
        _flushall();
        Sleep(50);
    }

    // Subtitle
    moveTo(startRow + 7, 1);
    setColorDimCyan();
    wprintf(L"          Terminal Session Launcher");
    _flushall();
    Sleep(100);

    // Brief pause then fade
    Sleep(300);
}

// ─── Docker Helpers ─────────────────────────────────────────────────────────

enum class DockerBackend
{
    None,
    Desktop,  // Docker Desktop (Windows pipe)
    WSL       // Docker Engine inside WSL
};

struct DockerEngine
{
    DockerBackend backend = DockerBackend::None;
    std::wstring label;
};

std::wstring findDockerExe()
{
    static const wchar_t* dockerPaths[] = {
        L"C:\\Program Files\\Docker\\Docker\\resources\\bin\\docker.exe",
        L"C:\\Program Files\\Docker Desktop\\docker.exe",
    };

    for (const auto& p : dockerPaths)
    {
        if (fs::exists(p))
            return p;
    }

    wchar_t* localAppData = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&localAppData, &len, L"LOCALAPPDATA") == 0 && localAppData)
    {
        auto userPath = fs::path(localAppData) / L"Programs" / L"Docker" / L"Docker" / L"resources" / L"bin" / L"docker.exe";
        free(localAppData);
        if (fs::exists(userPath))
            return userPath.wstring();
    }

    return L"docker";
}

bool isDockerDesktopAvailable()
{
    HANDLE pipe = CreateFileW(L"\\\\.\\pipe\\docker_engine",
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE)
    {
        CloseHandle(pipe);
        return true;
    }
    return false;
}

bool isDockerWSLAvailable()
{
    // Check if docker is available inside WSL by running a quick command
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return false;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"wsl.exe -d Ubuntu -- docker info --format {{.ServerVersion}}";
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return false;
    }
    CloseHandle(hWritePipe);

    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return exitCode == 0;
}

bool isDockerAvailable()
{
    return isDockerDesktopAvailable() || isDockerWSLAvailable();
}

std::vector<DockerEngine> getAvailableEngines()
{
    std::vector<DockerEngine> engines;
    if (isDockerDesktopAvailable())
        engines.push_back({ DockerBackend::Desktop, L"Docker Desktop" });
    if (isDockerWSLAvailable())
        engines.push_back({ DockerBackend::WSL, L"Docker (WSL)" });
    return engines;
}

// Build a command prefix for the given backend
std::wstring dockerCmd(DockerBackend backend, const std::wstring& args)
{
    if (backend == DockerBackend::WSL)
    {
        return L"wsl.exe -d Ubuntu -- docker " + args;
    }
    else
    {
        auto exe = findDockerExe();
        return L"\"" + exe + L"\" " + args;
    }
}

// Run a docker command and capture output
std::string runDockerCapture(DockerBackend backend, const std::wstring& args)
{
    std::wstring cmd = dockerCmd(backend, args);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        return {};
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return {};
    }
    CloseHandle(hWritePipe);

    std::string output;
    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(hReadPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
    {
        output.append(buffer, bytesRead);
    }
    CloseHandle(hReadPipe);

    WaitForSingleObject(pi.hProcess, 30000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return output;
}

// Global: which backend is active (selected by user if both available)
static DockerBackend g_activeBackend = DockerBackend::None;

// ─── Data Providers ─────────────────────────────────────────────────────────

std::vector<ListItem> enumerateSessions()
{
    std::vector<ListItem> items;
    try
    {
        wchar_t* userProfile = nullptr;
        size_t len = 0;
        if (_wdupenv_s(&userProfile, &len, L"USERPROFILE") != 0 || !userProfile)
            return items;

        fs::path sessionDir = fs::path(userProfile) / L".copilot" / L"session-state";
        free(userProfile);

        if (!fs::exists(sessionDir))
            return items;

        struct SessionEntry
        {
            ListItem item;
            fs::file_time_type mtime;
        };
        std::vector<SessionEntry> entries;

        for (const auto& entry : fs::directory_iterator(sessionDir))
        {
            if (!entry.is_directory())
                continue;

            auto id = entry.path().filename().wstring();

            // Must have workspace.yaml
            auto workspaceFile = entry.path() / L"workspace.yaml";
            if (!fs::exists(workspaceFile))
                continue;

            // Read workspace.yaml to check client_name and get session name
            std::wifstream wsFile(workspaceFile);
            if (!wsFile.is_open())
                continue;

            std::wstring wsContent;
            std::wstring line;
            std::wstring clientName;
            std::wstring sessionName;
            std::wstring repository;
            int summaryCount = 0;

            while (std::getline(wsFile, line))
            {
                if (line.find(L"client_name:") != std::wstring::npos)
                {
                    auto val = line.substr(line.find(L':') + 1);
                    auto start = val.find_first_not_of(L" \t");
                    if (start != std::wstring::npos)
                        clientName = val.substr(start);
                }
                else if (line.find(L"name:") == 0 || line.find(L"name: ") == 0)
                {
                    auto val = line.substr(line.find(L':') + 1);
                    auto start = val.find_first_not_of(L" \t");
                    if (start != std::wstring::npos)
                        sessionName = val.substr(start);
                }
                else if (line.find(L"repository:") != std::wstring::npos)
                {
                    auto val = line.substr(line.find(L':') + 1);
                    auto start = val.find_first_not_of(L" \t");
                    if (start != std::wstring::npos)
                        repository = val.substr(start);
                }
                else if (line.find(L"summary_count:") != std::wstring::npos)
                {
                    auto val = line.substr(line.find(L':') + 1);
                    auto start = val.find_first_not_of(L" \t");
                    if (start != std::wstring::npos)
                    {
                        try { summaryCount = std::stoi(val.substr(start)); } catch (...) {}
                    }
                }
            }

            // Only include CLI sessions
            if (clientName != L"github/cli")
                continue;

            // Skip sessions with no name and no meaningful interaction
            if (sessionName.empty() && summaryCount < 2)
                continue;

            // Skip sessions that look like scheduled/skill/heartbeat invocations
            if (sessionName.find(L"scheduled") != std::wstring::npos ||
                sessionName.find(L"HEARTBEAT") != std::wstring::npos ||
                sessionName.find(L"heartbeat") != std::wstring::npos ||
                sessionName.find(L"run the") == 0 ||
                sessionName.find(L"Run the") == 0 ||
                sessionName.find(L"run the update") != std::wstring::npos ||
                sessionName.find(L"Trigger ") == 0 ||
                sessionName.size() > 200)
                continue;

            ListItem li{};
            li.command = L"copilot --resume=" + id;

            // Use session name from workspace.yaml first, then plan.md
            if (!sessionName.empty() && sessionName != L"|-")
            {
                li.name = sessionName;
            }
            else
            {
                auto planFile = entry.path() / L"plan.md";
                if (fs::exists(planFile))
                {
                    std::wifstream pf(planFile);
                    std::wstring firstLine;
                    if (std::getline(pf, firstLine))
                    {
                        auto start = firstLine.find_first_not_of(L"# ");
                        if (start != std::wstring::npos)
                            li.name = firstLine.substr(start);
                    }
                }
            }

            if (li.name.empty())
                li.name = L"Session " + id.substr(0, 8) + L"...";

            // Truncate long names for display
            if (li.name.size() > 50)
                li.name = li.name.substr(0, 47) + L"...";

            li.detail = repository.empty() ? L"Copilot CLI" : repository;

            // Get timestamp
            auto mtime = fs::last_write_time(entry.path());
            auto sctp = std::chrono::time_point_cast<std::chrono::seconds>(
                std::chrono::clock_cast<std::chrono::system_clock>(mtime));
            auto timeT = std::chrono::system_clock::to_time_t(sctp);
            struct tm tmBuf {};
            localtime_s(&tmBuf, &timeT);
            wchar_t timeBuf[32];
            wcsftime(timeBuf, 32, L"%b %d %H:%M", &tmBuf);
            li.timestamp = timeBuf;

            entries.push_back({ std::move(li), mtime });
        }

        std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
            return a.mtime > b.mtime;
        });

        // Add "New Session" option at the top
        ListItem newSession{};
        newSession.name = L"\u2795 New Copilot Session (create folder in C:\\source\\)";
        newSession.detail = L"";
        newSession.timestamp = L"";
        newSession.command = L"__NEW_SESSION__";
        items.push_back(std::move(newSession));

        for (auto& e : entries)
            items.push_back(std::move(e.item));
    }
    catch (...)
    {
    }
    return items;
}

std::vector<ListItem> enumerateApps()
{
    std::vector<ListItem> items;

    // Common shells / apps
    struct AppDef
    {
        const wchar_t* name;
        const wchar_t* cmd;
        const wchar_t* detail;
    };
    static const AppDef apps[] = {
        { L"PowerShell", L"pwsh.exe", L"PowerShell 7+" },
        { L"Windows PowerShell", L"powershell.exe", L"PowerShell 5.1" },
        { L"Command Prompt", L"cmd.exe", L"Classic CMD" },
        { L"Serial Console", L"C:\\source\\serial-terminal\\bin\\Release\\net10.0\\win-x64\\native\\serial-terminal.exe", L"Serial Terminal" },
        { L"Git Bash", L"C:\\Program Files\\Git\\bin\\bash.exe", L"Git for Windows" },
        { L"WSL (Default)", L"wsl.exe", L"Windows Subsystem for Linux" },
        { L"GitHub Copilot CLI", L"copilot", L"Copilot in the CLI" },
        { L"Python", L"python.exe", L"Python REPL" },
        { L"Node.js", L"node.exe", L"Node.js REPL" },
    };

    for (const auto& app : apps)
    {
        // Check if the exe exists (for non-absolute paths, just include it)
        std::wstring cmd = app.cmd;
        if (cmd.find(L'\\') != std::wstring::npos)
        {
            if (!fs::exists(cmd))
                continue;
        }
        items.push_back({ app.name, app.detail, L"", cmd });
    }

    return items;
}

std::vector<ListItem> enumerateContainers()
{
    std::vector<ListItem> items;

    auto engines = getAvailableEngines();

    if (engines.empty())
    {
        ListItem li{};
        li.name = L"\u26A0 Docker is not available";
        li.detail = L"Install Docker Desktop or docker.io in WSL";
        li.timestamp = L"";
        li.command = L"__DOCKER_NOT_AVAILABLE__";
        items.push_back(std::move(li));
        return items;
    }

    // If multiple engines, let user pick (shown as items at top)
    if (engines.size() > 1 && g_activeBackend == DockerBackend::None)
    {
        for (const auto& eng : engines)
        {
            ListItem li{};
            li.name = L"\u25B6 Use: " + eng.label;
            li.detail = L"Select engine";
            li.timestamp = L"";
            li.command = (eng.backend == DockerBackend::Desktop) ? L"__SELECT_DOCKER_DESKTOP__" : L"__SELECT_DOCKER_WSL__";
            items.push_back(std::move(li));
        }
        return items;
    }

    // Use first available if only one, or the selected one
    DockerBackend backend = g_activeBackend;
    if (backend == DockerBackend::None)
        backend = engines[0].backend;
    g_activeBackend = backend;

    // Query containers via the selected backend
    std::string output;
    if (backend == DockerBackend::Desktop)
    {
        // Use named pipe for speed
        HANDLE pipe = CreateFileW(L"\\\\.\\pipe\\docker_engine",
            GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
        {
            std::string request = "GET /v1.41/containers/json?all=true HTTP/1.1\r\nHost: localhost\r\n\r\n";
            DWORD written = 0;
            WriteFile(pipe, request.c_str(), (DWORD)request.size(), &written, nullptr);

            char buffer[8192];
            DWORD bytesRead = 0;
            while (ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
            {
                output.append(buffer, bytesRead);
                if (output.find("\r\n\r\n") != std::string::npos)
                {
                    auto bodyStart = output.find("\r\n\r\n") + 4;
                    auto body = output.substr(bodyStart);
                    int depth = 0;
                    bool complete = false;
                    for (auto c : body)
                    {
                        if (c == '[') depth++;
                        else if (c == ']') { depth--; if (depth == 0) { complete = true; break; } }
                    }
                    if (complete) break;
                }
            }
            CloseHandle(pipe);
            // Extract body
            auto bodyStart = output.find("\r\n\r\n");
            if (bodyStart != std::string::npos)
                output = output.substr(bodyStart + 4);
        }
    }
    else
    {
        // WSL: use docker ps with JSON format
        output = runDockerCapture(DockerBackend::WSL, L"ps -a --format \"{{.ID}}\\t{{.Names}}\\t{{.Image}}\\t{{.State}}\"");
    }

    if (output.empty())
    {
        ListItem li{};
        li.name = L"No containers found";
        li.detail = L"Use tab 4 to create one";
        li.timestamp = L"";
        li.command = L"__NO_CONTAINERS__";
        items.push_back(std::move(li));
        return items;
    }

    if (backend == DockerBackend::WSL)
    {
        // Parse tab-separated output: ID\tName\tImage\tState
        std::istringstream stream(output);
        std::string line;
        while (std::getline(stream, line))
        {
            if (line.empty()) continue;
            // Split by tab
            std::vector<std::string> fields;
            std::string field;
            std::istringstream ls(line);
            while (std::getline(ls, field, '\t'))
                fields.push_back(field);

            if (fields.size() >= 4)
            {
                ListItem li{};
                li.name = std::wstring(fields[1].begin(), fields[1].end());
                li.detail = std::wstring(fields[3].begin(), fields[3].end());
                li.timestamp = L"WSL Docker";
                std::wstring wid(fields[0].begin(), fields[0].end());
                li.command = L"wsl.exe -d Ubuntu -- docker exec -it " + wid + L" /bin/sh";
                items.push_back(std::move(li));
            }
        }
    }
    else
    {
        // Parse JSON from Docker Desktop pipe
        size_t pos = 0;
        while ((pos = output.find("\"Id\"", pos)) != std::string::npos)
        {
            ListItem li{};

            auto idStart = output.find('\"', pos + 4) + 1;
            auto idEnd = output.find('\"', idStart);
            std::string id = output.substr(idStart, std::min((size_t)12, idEnd - idStart));

            auto namesPos = output.find("\"Names\"", pos);
            std::string name = id;
            if (namesPos != std::string::npos && namesPos < output.find("\"Id\"", pos + 4))
            {
                auto ns = output.find("\"", output.find("[", namesPos) + 1) + 1;
                auto ne = output.find("\"", ns);
                name = output.substr(ns, ne - ns);
                if (!name.empty() && name[0] == '/') name = name.substr(1);
            }

            auto statePos = output.find("\"State\"", pos);
            std::string state;
            if (statePos != std::string::npos)
            {
                auto ss = output.find('\"', statePos + 7) + 1;
                auto se = output.find('\"', ss);
                state = output.substr(ss, se - ss);
            }

            li.name = std::wstring(name.begin(), name.end());
            li.detail = std::wstring(state.begin(), state.end());
            li.timestamp = L"Docker Desktop";
            std::wstring wid(id.begin(), id.end());
            li.command = dockerCmd(DockerBackend::Desktop, L"exec -it " + wid + L" /bin/sh");

            items.push_back(std::move(li));
            pos = idEnd != std::string::npos ? idEnd : pos + 4;
        }
    }

    if (items.empty())
    {
        ListItem li{};
        li.name = L"No containers found";
        li.detail = L"Use tab 4 to create one";
        li.timestamp = L"";
        li.command = L"__NO_CONTAINERS__";
        items.push_back(std::move(li));
    }

    return items;
}

// ─── Rendering ──────────────────────────────────────────────────────────────

void renderTabs(Tab currentTab)
{
    moveTo(1, 1);
    setThemeBg();
    wprintf(L"\x1b[2K");

    for (int i = 0; i < (int)Tab::COUNT; i++)
    {
        if (i == (int)currentTab)
        {
            setColorYellow();
            setBold();
            wprintf(L" [%d] %ls ", i + 1, TabNames[i]);
        }
        else
        {
            setColorDimCyan();
            wprintf(L"  %d  %ls ", i + 1, TabNames[i]);
        }
    }
    wprintf(L"\n");

    // Separator line
    setColorDimCyan();
    for (int i = 0; i < termWidth; i++)
        wprintf(L"\u2500");
    setThemeBg();
    wprintf(L"\n");
}

void renderList(const std::vector<ListItem>& items, int selectedIndex, int scrollOffset, const std::set<int>* checked = nullptr)
{
    int maxVisible = termHeight - 6; // tabs(2) + footer(2) + padding
    if (maxVisible < 1) maxVisible = 1;

    for (int i = 0; i < maxVisible; i++)
    {
        int idx = scrollOffset + i;
        moveTo(3 + i, 1);

        // Clear line
        wprintf(L"\x1b[2K");

        if (idx >= (int)items.size())
            continue;

        const auto& item = items[idx];

        if (idx == selectedIndex)
        {
            setReverse();
        }

        // Checkbox or arrow indicator
        if (checked)
        {
            bool isChecked = checked->count(idx) > 0;
            wprintf(L" %ls ", isChecked ? L"\u2611" : L"\u2610");
        }
        else if (idx == selectedIndex)
        {
            wprintf(L" \u25B6 ");
        }
        else
        {
            wprintf(L"   ");
        }

        // Name (bold, truncated)
        setBold();
        auto nameDisplay = item.name.substr(0, std::min((size_t)(termWidth / 2), item.name.size()));
        wprintf(L"%-*.*ls", termWidth / 2, termWidth / 2, nameDisplay.c_str());
        resetColor();

        if (idx == selectedIndex)
            setReverse();

        // Detail
        setDim();
        wprintf(L" %-12.12ls", item.detail.c_str());
        resetColor();

        if (idx == selectedIndex)
            setReverse();

        // Timestamp
        setDim();
        wprintf(L" %ls", item.timestamp.c_str());
        resetColor();
    }
}

void renderNewContainerInput(const std::wstring& imageName, bool focused)
{
    moveTo(3, 1);
    wprintf(L"\x1b[2K");

    if (!isDockerAvailable())
    {
        wprintf(L"  \x1b[33m\u26A0 Docker is not available\x1b[0m\n\n");
        moveTo(5, 1);
        wprintf(L"\x1b[2K");
        wprintf(L"  Option 1: Install Docker Desktop\n");
        moveTo(6, 1);
        wprintf(L"\x1b[2K");
        setDim();
        wprintf(L"    winget install Docker.DockerDesktop\n");
        resetColor();
        moveTo(8, 1);
        wprintf(L"\x1b[2K");
        wprintf(L"  Option 2: Install Docker Engine in WSL\n");
        moveTo(9, 1);
        wprintf(L"\x1b[2K");
        setDim();
        wprintf(L"    wsl -d Ubuntu -- sudo apt-get install -y docker.io\n");
        moveTo(10, 1);
        wprintf(L"\x1b[2K");
        wprintf(L"    wsl -d Ubuntu -- sudo service docker start");
        resetColor();
        return;
    }

    setBold();
    wprintf(L"  Create and connect to a new Docker container\n");
    resetColor();

    moveTo(5, 1);
    wprintf(L"\x1b[2K");
    wprintf(L"  Image name: ");
    if (focused)
        setReverse();
    wprintf(L" %ls ", imageName.empty() ? L"(type image e.g. ubuntu:latest)" : imageName.c_str());
    resetColor();
    wprintf(L"\n");

    moveTo(7, 1);
    wprintf(L"\x1b[2K");
    setDim();
    wprintf(L"  Press [Enter] to create, pull, and connect.");
    resetColor();
}

void renderFooter(Tab currentTab, size_t checkedCount)
{
    moveTo(termHeight - 1, 1);
    wprintf(L"\x1b[2K");
    setColorDimCyan();
    if (currentTab == Tab::Sessions)
    {
        if (checkedCount > 0)
        {
            setColorYellow();
            wprintf(L"  [Enter] Launch %zu selected", checkedCount);
            setColorDimCyan();
            wprintf(L"  [\u2191\u2193] Navigate  [Space] Toggle  [Tab] Switch  [Esc] Cancel");
        }
        else
            wprintf(L"  [\u2191\u2193] Navigate  [Space] Multi-select  [Enter] Launch  [Tab] Switch  [Esc] Cancel");
    }
    else
    {
        wprintf(L"  [\u2191\u2193] Navigate  [Tab/1-4] Switch tab  [Enter] Select  [Esc] Cancel");
    }
    setThemeBg();
}

void renderEmpty()
{
    moveTo(4, 1);
    setDim();
    wprintf(L"  No items found.");
    resetColor();
}

// ─── Input Handling ─────────────────────────────────────────────────────────

struct KeyEvent
{
    enum Type { None, Up, Down, Left, Right, Enter, Escape, Tab, Space, Char, Number } type = None;
    wchar_t ch = 0;
    int number = 0;
};

KeyEvent readKey()
{
    INPUT_RECORD rec;
    DWORD read;
    while (true)
    {
        if (!ReadConsoleInputW(hIn, &rec, 1, &read) || read == 0)
            continue;

        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT)
        {
            termWidth = rec.Event.WindowBufferSizeEvent.dwSize.X;
            termHeight = rec.Event.WindowBufferSizeEvent.dwSize.Y;
            return { KeyEvent::None };
        }

        if (rec.EventType != KEY_EVENT || !rec.Event.KeyEvent.bKeyDown)
            continue;

        auto vk = rec.Event.KeyEvent.wVirtualKeyCode;
        auto ch = rec.Event.KeyEvent.uChar.UnicodeChar;

        if (vk == VK_UP) return { KeyEvent::Up };
        if (vk == VK_DOWN) return { KeyEvent::Down };
        if (vk == VK_LEFT) return { KeyEvent::Left };
        if (vk == VK_RIGHT) return { KeyEvent::Right };
        if (vk == VK_RETURN) return { KeyEvent::Enter };
        if (vk == VK_ESCAPE) return { KeyEvent::Escape };
        if (vk == VK_TAB) return { KeyEvent::Tab };
        if (vk == VK_SPACE) return { KeyEvent::Space };
        if (vk == VK_BACK) return { KeyEvent::Char, L'\b' };

        if (ch >= L'1' && ch <= L'4') return { KeyEvent::Number, 0, ch - L'0' };
        if (ch >= 32) return { KeyEvent::Char, ch };
    }
}

// ─── Process Launch ─────────────────────────────────────────────────────────

int launchInDir(const std::wstring& commandLine, const std::wstring& workingDir)
{
    restoreConsole();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::wstring cmd = commandLine;
    const wchar_t* cwd = workingDir.empty() ? nullptr : workingDir.c_str();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, cwd, &si, &pi))
    {
        wprintf(L"Failed to launch: %ls (error %lu)\n", commandLine.c_str(), GetLastError());
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exitCode;
}

int launchAndWait(const std::wstring& commandLine)
{
    return launchInDir(commandLine, L"");
}

int createNewCopilotSession()
{
    restoreConsole();
    wprintf(L"\x1b[?25h"); // show cursor

    wprintf(L"\n  Create a new Copilot CLI session\n");
    wprintf(L"  Folder name (will be created in C:\\source\\): ");
    _flushall();

    // Read folder name from console
    wchar_t folderBuf[256] = {};
    DWORD charsRead = 0;

    // Restore normal console input mode for line reading
    SetConsoleMode(hIn, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    ReadConsoleW(hIn, folderBuf, 255, &charsRead, nullptr);

    // Trim trailing newline
    std::wstring folderName(folderBuf, charsRead);
    while (!folderName.empty() && (folderName.back() == L'\n' || folderName.back() == L'\r' || folderName.back() == L' '))
        folderName.pop_back();

    if (folderName.empty())
    {
        wprintf(L"  Cancelled.\n");
        return 1;
    }

    // Create the folder
    fs::path targetDir = fs::path(L"C:\\source") / folderName;
    try
    {
        fs::create_directories(targetDir);
    }
    catch (...)
    {
        wprintf(L"  Failed to create folder: %ls\n", targetDir.c_str());
        return 1;
    }

    wprintf(L"  Created: %ls\n", targetDir.c_str());
    wprintf(L"  Launching Copilot CLI...\n");
    _flushall();

    // Launch copilot in the new folder
    return launchInDir(L"copilot", targetDir.wstring());
}

int createContainerAndConnect(const std::wstring& image)
{
    restoreConsole();

    if (!isDockerAvailable())
    {
        wprintf(L"\n  \x1b[31mDocker is not available.\x1b[0m\n\n");
        wprintf(L"  Install Docker Desktop:  winget install Docker.DockerDesktop\n");
        wprintf(L"  Or in WSL:  wsl -d Ubuntu -- sudo apt-get install -y docker.io\n\n");
        wprintf(L"  Press any key to exit...\n");
        _flushall();
        SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);
        INPUT_RECORD rec;
        DWORD read;
        ReadConsoleInputW(hIn, &rec, 1, &read);
        return 1;
    }

    // Pick backend if not yet selected
    if (g_activeBackend == DockerBackend::None)
    {
        auto engines = getAvailableEngines();
        g_activeBackend = engines[0].backend;
    }

    wprintf(L"Creating container from image: %ls (%ls)...\n", image.c_str(),
        g_activeBackend == DockerBackend::WSL ? L"WSL" : L"Docker Desktop");
    _flushall();

    // Run: docker run -dit <image>
    auto output = runDockerCapture(g_activeBackend, L"run -dit " + image);

    // Trim whitespace from container ID
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r' || output.back() == ' '))
        output.pop_back();

    if (output.empty() || output.find("Error") != std::string::npos || output.find("error") != std::string::npos)
    {
        wprintf(L"Failed to create container:\n");
        wprintf(L"%hs\n", output.c_str());
        wprintf(L"\nPress any key to exit...\n");
        _flushall();
        SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);
        INPUT_RECORD rec;
        DWORD read;
        ReadConsoleInputW(hIn, &rec, 1, &read);
        return 1;
    }

    std::string shortId = output.substr(0, 12);
    std::wstring wShortId(shortId.begin(), shortId.end());
    std::wstring execCmd = dockerCmd(g_activeBackend, L"exec -it " + wShortId + L" /bin/sh");
    wprintf(L"Connecting to %hs...\n", shortId.c_str());
    _flushall();
    return launchAndWait(execCmd);
}

// ─── Main Loop ──────────────────────────────────────────────────────────────

int wmain()
{
    _setmode(_fileno(stdout), _O_U16TEXT);
    enableVT();
    hideCursor();

    // Show splash animation
    showSplash();

    Tab currentTab = Tab::Sessions;
    int selectedIndex = 0;
    int scrollOffset = 0;
    std::wstring newContainerImage;
    std::set<int> checkedItems; // Multi-select for Sessions tab

    // Pre-load data for all tabs
    std::vector<ListItem> tabData[(int)Tab::COUNT];
    tabData[(int)Tab::Sessions] = enumerateSessions();
    tabData[(int)Tab::Apps] = enumerateApps();
    tabData[(int)Tab::Containers] = enumerateContainers();
    // Tab::NewContainer uses input mode, no list

    bool running = true;
    bool needsRedraw = true;

    while (running)
    {
        if (needsRedraw)
        {
            clearScreen();
            renderTabs(currentTab);

            if (currentTab == Tab::NewContainer)
            {
                renderNewContainerInput(newContainerImage, true);
            }
            else
            {
                const auto& items = tabData[(int)currentTab];
                if (items.empty())
                    renderEmpty();
                else if (currentTab == Tab::Sessions)
                    renderList(items, selectedIndex, scrollOffset, &checkedItems);
                else
                    renderList(items, selectedIndex, scrollOffset);
            }

            renderFooter(currentTab, checkedItems.size());
            _flushall();
            needsRedraw = false;
        }

        auto key = readKey();
        int maxVisible = termHeight - 6;

        switch (key.type)
        {
        case KeyEvent::None:
            needsRedraw = true;
            break;

        case KeyEvent::Escape:
            running = false;
            break;

        case KeyEvent::Tab:
            currentTab = (Tab)(((int)currentTab + 1) % (int)Tab::COUNT);
            selectedIndex = 0;
            scrollOffset = 0;
            needsRedraw = true;
            break;

        case KeyEvent::Number:
            if (key.number >= 1 && key.number <= (int)Tab::COUNT)
            {
                if (currentTab == Tab::NewContainer)
                {
                    // In text input mode, numbers go to input
                    newContainerImage += (wchar_t)(L'0' + key.number);
                    needsRedraw = true;
                }
                else
                {
                    currentTab = (Tab)(key.number - 1);
                    selectedIndex = 0;
                    scrollOffset = 0;
                    needsRedraw = true;
                }
            }
            break;

        case KeyEvent::Up:
            if (currentTab != Tab::NewContainer && selectedIndex > 0)
            {
                selectedIndex--;
                if (selectedIndex < scrollOffset)
                    scrollOffset = selectedIndex;
                needsRedraw = true;
            }
            break;

        case KeyEvent::Down:
            if (currentTab != Tab::NewContainer)
            {
                const auto& items = tabData[(int)currentTab];
                if (selectedIndex < (int)items.size() - 1)
                {
                    selectedIndex++;
                    if (selectedIndex >= scrollOffset + maxVisible)
                        scrollOffset = selectedIndex - maxVisible + 1;
                    needsRedraw = true;
                }
            }
            break;

        case KeyEvent::Space:
            if (currentTab == Tab::Sessions)
            {
                const auto& items = tabData[(int)Tab::Sessions];
                if (selectedIndex < (int)items.size())
                {
                    // Don't allow checking the "__NEW_SESSION__" item
                    if (items[selectedIndex].command != L"__NEW_SESSION__")
                    {
                        if (checkedItems.count(selectedIndex))
                            checkedItems.erase(selectedIndex);
                        else
                            checkedItems.insert(selectedIndex);
                        needsRedraw = true;
                    }
                }
            }
            break;

        case KeyEvent::Enter:
            if (currentTab == Tab::NewContainer)
            {
                if (!newContainerImage.empty() && isDockerAvailable())
                {
                    return createContainerAndConnect(newContainerImage);
                }
            }
            else if (currentTab == Tab::Sessions && !checkedItems.empty())
            {
                // Multi-launch: spawn all checked sessions in separate windows
                restoreConsole();
                const auto& items = tabData[(int)Tab::Sessions];
                for (int idx : checkedItems)
                {
                    if (idx >= (int)items.size()) continue;
                    const auto& cmd = items[idx].command;
                    if (cmd.find(L"__") == 0) continue; // skip placeholders

                    // Launch each in a new Terminal window using wt.exe
                    std::wstring wtCmd = L"wt.exe new-tab -- " + cmd;
                    STARTUPINFOW si{};
                    si.cb = sizeof(si);
                    PROCESS_INFORMATION pi{};
                    CreateProcessW(nullptr, wtCmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi);
                    if (pi.hProcess) CloseHandle(pi.hProcess);
                    if (pi.hThread) CloseHandle(pi.hThread);
                }
                return 0;
            }
            else
            {
                const auto& items = tabData[(int)currentTab];
                if (!items.empty() && selectedIndex < (int)items.size())
                {
                    const auto& cmd = items[selectedIndex].command;
                    if (cmd == L"__NEW_SESSION__")
                    {
                        return createNewCopilotSession();
                    }
                    // Engine selection - set backend and refresh container list
                    if (cmd == L"__SELECT_DOCKER_DESKTOP__")
                    {
                        g_activeBackend = DockerBackend::Desktop;
                        tabData[(int)Tab::Containers] = enumerateContainers();
                        selectedIndex = 0;
                        scrollOffset = 0;
                        needsRedraw = true;
                        break;
                    }
                    if (cmd == L"__SELECT_DOCKER_WSL__")
                    {
                        g_activeBackend = DockerBackend::WSL;
                        tabData[(int)Tab::Containers] = enumerateContainers();
                        selectedIndex = 0;
                        scrollOffset = 0;
                        needsRedraw = true;
                        break;
                    }
                    // Skip placeholder items
                    if (cmd == L"__DOCKER_NOT_AVAILABLE__" || cmd == L"__NO_CONTAINERS__")
                    {
                        break;
                    }
                    return launchAndWait(cmd);
                }
            }
            break;

        case KeyEvent::Char:
            if (currentTab == Tab::NewContainer)
            {
                if (key.ch == L'\b')
                {
                    if (!newContainerImage.empty())
                        newContainerImage.pop_back();
                }
                else
                {
                    newContainerImage += key.ch;
                }
                // Fast update: just redraw the input field, not the whole screen
                moveTo(5, 1);
                wprintf(L"\x1b[2K");
                wprintf(L"  Image name: ");
                setReverse();
                wprintf(L" %ls ", newContainerImage.empty() ? L"(type image e.g. ubuntu:latest)" : newContainerImage.c_str());
                resetColor();
                _flushall();
            }
            break;

        default:
            break;
        }
    }

    restoreConsole();
    return 0;
}
