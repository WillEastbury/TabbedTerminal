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
    SetConsoleMode(hIn, ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);

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
    wprintf(L"\x1b[2J\x1b[H");
}

void hideCursor()
{
    wprintf(L"\x1b[?25l");
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
    wprintf(L"\x1b[2m");
}

void setReverse()
{
    wprintf(L"\x1b[7m");
}

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

            ListItem li{};
            auto id = entry.path().filename().wstring();
            li.command = L"gh copilot-cli --resume " + id;

            // Try plan.md for summary
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
            if (li.name.empty())
                li.name = L"Session " + id.substr(0, 8) + L"...";

            li.detail = L"Copilot CLI";

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
        { L"Git Bash", L"C:\\Program Files\\Git\\bin\\bash.exe", L"Git for Windows" },
        { L"WSL (Default)", L"wsl.exe", L"Windows Subsystem for Linux" },
        { L"GitHub Copilot CLI", L"gh copilot-cli", L"Copilot in the CLI" },
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

    // Query Docker via named pipe
    HANDLE pipe = CreateFileW(
        L"\\\\.\\pipe\\docker_engine",
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);

    if (pipe != INVALID_HANDLE_VALUE)
    {
        std::string request = "GET /v1.41/containers/json?all=true HTTP/1.1\r\nHost: localhost\r\n\r\n";
        DWORD written = 0;
        WriteFile(pipe, request.c_str(), (DWORD)request.size(), &written, nullptr);

        std::string response;
        char buffer[8192];
        DWORD bytesRead = 0;
        while (ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
        {
            response.append(buffer, bytesRead);
            if (response.find("\r\n\r\n") != std::string::npos)
            {
                // Simple: check if we have a complete JSON array
                auto bodyStart = response.find("\r\n\r\n") + 4;
                auto body = response.substr(bodyStart);
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

        // Parse container names and IDs (simple extraction)
        size_t pos = 0;
        while ((pos = response.find("\"Id\"", pos)) != std::string::npos)
        {
            ListItem li{};

            auto idStart = response.find('\"', pos + 4) + 1;
            auto idEnd = response.find('\"', idStart);
            std::string id = response.substr(idStart, std::min((size_t)12, idEnd - idStart));

            auto namesPos = response.find("\"Names\"", pos);
            std::string name = id;
            if (namesPos != std::string::npos && namesPos < response.find("\"Id\"", pos + 4))
            {
                auto ns = response.find("\"", response.find("[", namesPos) + 1) + 1;
                auto ne = response.find("\"", ns);
                name = response.substr(ns, ne - ns);
                if (!name.empty() && name[0] == '/') name = name.substr(1);
            }

            auto statePos = response.find("\"State\"", pos);
            std::string state;
            if (statePos != std::string::npos)
            {
                auto ss = response.find('\"', statePos + 7) + 1;
                auto se = response.find('\"', ss);
                state = response.substr(ss, se - ss);
            }

            li.name = std::wstring(name.begin(), name.end());
            li.detail = std::wstring(state.begin(), state.end());
            li.timestamp = L"Docker";
            std::wstring wid(id.begin(), id.end());
            li.command = L"docker exec -it " + wid + L" /bin/sh";

            items.push_back(std::move(li));
            pos = idEnd != std::string::npos ? idEnd : pos + 4;
        }
    }

    return items;
}

// ─── Rendering ──────────────────────────────────────────────────────────────

void renderTabs(Tab currentTab)
{
    moveTo(1, 1);
    resetColor();

    for (int i = 0; i < (int)Tab::COUNT; i++)
    {
        if (i == (int)currentTab)
        {
            setReverse();
            setBold();
            wprintf(L" [%d] %ls ", i + 1, TabNames[i]);
            resetColor();
        }
        else
        {
            setDim();
            wprintf(L"  %d  %ls ", i + 1, TabNames[i]);
            resetColor();
        }
    }
    wprintf(L"\n");

    // Separator line
    resetColor();
    setDim();
    for (int i = 0; i < termWidth; i++)
        wprintf(L"\u2500");
    resetColor();
    wprintf(L"\n");
}

void renderList(const std::vector<ListItem>& items, int selectedIndex, int scrollOffset)
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

void renderFooter()
{
    moveTo(termHeight - 1, 1);
    wprintf(L"\x1b[2K");
    setDim();
    wprintf(L"  [\u2191\u2193] Navigate  [Tab/1-4] Switch tab  [Enter] Select  [Esc] Cancel");
    resetColor();
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
    enum Type { None, Up, Down, Left, Right, Enter, Escape, Tab, Char, Number } type = None;
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
        if (vk == VK_BACK) return { KeyEvent::Char, L'\b' };

        if (ch >= L'1' && ch <= L'4') return { KeyEvent::Number, 0, ch - L'0' };
        if (ch >= 32) return { KeyEvent::Char, ch };
    }
}

// ─── Process Launch ─────────────────────────────────────────────────────────

int launchAndWait(const std::wstring& commandLine)
{
    restoreConsole();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::wstring cmd = commandLine;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
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

int createContainerAndConnect(const std::wstring& image)
{
    restoreConsole();
    wprintf(L"Creating container from image: %ls...\n", image.c_str());

    // docker run -dit <image>
    std::wstring runCmd = L"docker run -dit " + image;
    STARTUPINFOW si{};
    si.cb = sizeof(si);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe, hWritePipe;
    CreatePipe(&hReadPipe, &hWritePipe, &sa, 0);
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, runCmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        wprintf(L"Failed to create container.\n");
        return 1;
    }

    CloseHandle(hWritePipe);
    WaitForSingleObject(pi.hProcess, 30000);

    // Read container ID from output
    char buf[128] = {};
    DWORD bytesRead = 0;
    ReadFile(hReadPipe, buf, sizeof(buf) - 1, &bytesRead, nullptr);
    CloseHandle(hReadPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    std::string containerId(buf, bytesRead);
    // Trim whitespace
    while (!containerId.empty() && (containerId.back() == '\n' || containerId.back() == '\r'))
        containerId.pop_back();

    if (containerId.empty())
    {
        wprintf(L"Failed to get container ID.\n");
        return 1;
    }

    // Now exec into it
    std::string shortId = containerId.substr(0, 12);
    std::wstring execCmd = L"docker exec -it " + std::wstring(shortId.begin(), shortId.end()) + L" /bin/sh";
    wprintf(L"Connecting to %hs...\n", shortId.c_str());
    return launchAndWait(execCmd);
}

// ─── Main Loop ──────────────────────────────────────────────────────────────

int wmain()
{
    _setmode(_fileno(stdout), _O_U16TEXT);
    enableVT();
    hideCursor();

    Tab currentTab = Tab::Sessions;
    int selectedIndex = 0;
    int scrollOffset = 0;
    std::wstring newContainerImage;

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
                else
                    renderList(items, selectedIndex, scrollOffset);
            }

            renderFooter();
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

        case KeyEvent::Enter:
            if (currentTab == Tab::NewContainer)
            {
                if (!newContainerImage.empty())
                {
                    return createContainerAndConnect(newContainerImage);
                }
            }
            else
            {
                const auto& items = tabData[(int)currentTab];
                if (!items.empty() && selectedIndex < (int)items.size())
                {
                    return launchAndWait(items[selectedIndex].command);
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
                needsRedraw = true;
            }
            break;

        default:
            break;
        }
    }

    restoreConsole();
    return 0;
}
