# TabbedTerminal

**A customised fork of [Windows Terminal](https://github.com/microsoft/terminal)** — turning it into a unified workstation shell with a vertical sidebar, TUI session launcher, and integrated hosting for console apps, web views, and legacy Win32 applications.

## Why This Fork?

Windows Terminal is excellent at hosting console/CLI tabs. But a modern developer workstation needs more than that — you need to manage Copilot sessions, SSH connections, Docker containers, serial ports, web admin panels, and legacy GUI tools — ideally all from one window with a persistent sidebar.

TabbedTerminal extends Windows Terminal with:

- **Vertical tab sidebar** with drag-to-resize, color-coded tabs, and right-click management (Move Up/Down, Set Tab Color)
- **TUI launcher** (figlet splash, 8 tabs) for discovering and launching sessions without leaving the terminal
- **Multi-select launch** — check multiple items with Space, launch them all with Enter
- **SSH host management** with persistent config and optional tmux auto-attach
- **Web app bookmarks** for quick access to admin panels and dashboards
- **Win32 app integration** for embedding tools like Calculator, Task Manager, and Notepad
- **Custom themes** (Dusky dark-beige, Garish purple) beyond the stock Light/Dark
- **Copilot CLI integration** — resume sessions, toggle `--allow-all` (YOLO mode), create new sessions in specific folders
- **Serial terminal** with VT pass-through for embedded development
- **Docker/Podman container management** — list, attach, create containers from the launcher

## Architecture

```
┌─────────────────────────────────────────────────┐
│  TabbedTerminal (WindowsTerminal.exe)           │
│  ┌──────────┐  ┌──────────────────────────────┐ │
│  │ Vertical  │  │  Content Area                │ │
│  │ Sidebar   │  │  ┌────────────────────────┐  │ │
│  │           │  │  │ ConPTY Tab (shells)    │  │ │
│  │ [Tab 1]◄──┤  │  │ WebView2 Tab (browser) │  │ │
│  │ [Tab 2]   │  │  │ SetParent Tab (Win32)  │  │ │
│  │ [Tab 3]   │  │  └────────────────────────┘  │ │
│  │           │  │                              │ │
│  │ [+] New   │  │                              │ │
│  │ [⚙] Sess  │  │                              │ │
│  │ [⚙] Sett  │  │                              │ │
│  │ [↻] Upd   │  │                              │ │
│  └──────────┘  └──────────────────────────────┘ │
└─────────────────────────────────────────────────┘
         ▲
         │ Launches via wt-launch.cmd (exit code 42 protocol)
         │
┌────────┴──────────────────────────────────────┐
│  wt-launcher.exe (TUI)                        │
│                                               │
│  Tab 1: Sessions    — Copilot CLI sessions    │
│  Tab 2: Apps        — Shells & tools          │
│  Tab 3: Containers  — Docker/Podman           │
│  Tab 4: New Container — Pull & run images     │
│  Tab 5: Serial      — COM port VT terminal    │
│  Tab 6: SSH         — Managed host list       │
│  Tab 7: Web         — Bookmarked URLs         │
│  Tab 8: Win32       — Desktop apps            │
│                                               │
│  Config: C:\launcher\hosts.cfg                │
│          C:\launcher\webapps.cfg              │
└───────────────────────────────────────────────┘
```

## Launcher Tabs

### 1. Sessions
Enumerates local Copilot CLI sessions from `~/.copilot/session-state/`. Multi-select with Space, launch with Enter. Toggle `--allow-all` (YOLO) mode with `Y`.

### 2. Apps
Quick-launch shells: PowerShell 7, CMD, Git Bash, WSL, Python, Node.js, Copilot CLI.

### 3. Containers
Lists Docker Desktop and WSL docker containers. Attach to running containers or start stopped ones. Auto-detects available engines.

### 4. New Container
Type an image name (e.g. `ubuntu:latest`) and create + attach in one step.

### 5. Serial
Enumerates COM ports, opens an inline VT pass-through serial terminal at 115200 8N1. Designed for embedded development (RP2350, Pi, etc).

### 6. SSH Hosts
Managed list of SSH connections with persistent config (`C:\launcher\hosts.cfg`).

| Feature | Detail |
|---------|--------|
| Add host | Enter on "➕ Add SSH Host" — prompts for alias, hostname, user, port, tmux |
| Delete host | `D` key on selected host |
| tmux integration | Auto-runs `tmux new-session -A -s main` for persistent sessions |
| Multi-select | Space to check, Enter to launch all in separate tabs |
| Config format | `alias|hostname|user|port|tmux(0/1)` pipe-delimited |

### 7. Web Apps
Bookmarked web URLs for admin panels, dashboards, and tools (`C:\launcher\webapps.cfg`).

| Feature | Detail |
|---------|--------|
| Add URL | Enter on "➕ Add Web App" — prompts for name and URL |
| Delete | `D` key on selected entry |
| Auto-scheme | URLs without `://` get `http://` prepended |
| Multi-select | Space to check, Enter to launch all |
| Config format | `alias|url` pipe-delimited |

### 8. Win32 Apps
Launch desktop applications: Calculator, Task Manager, Notepad, Registry Editor, Resource Monitor, Device Manager, Disk Management, Services, Event Viewer.

## Sidebar Features

The vertical tab sidebar replaces the horizontal tab strip in Left/Right tab position modes:

- **Color-coded tabs** — right-click → Set Tab Color shows a color picker flyout anchored to the sidebar. Selected color applies as a 4px indicator bar and a subtle (alpha=40) background tint.
- **Move Up/Down** — right-click context menu to reorder tabs. Correctly syncs the sidebar, the internal tab list, and the TabView items so content panes follow their tabs.
- **Update & Restart** — bottom sidebar button spawns a detached PowerShell that re-registers the AppX package and relaunches Terminal.
- **Resizable** — drag the sidebar edge to resize between 48px and 400px.

## Launch Protocol

The launcher (`wt-launcher.exe`) communicates with the wrapper script (`wt-launch.cmd`) via a file-based protocol:

1. Wrapper sets `WT_LAUNCHER_CMDFILE` env var pointing to a unique temp file
2. Launcher writes command(s) to the file, one per line
3. Launcher exits with code **42** (magic relaunch code)
4. Wrapper reads the file, detects prefixes, and launches:
   - Plain commands → `cmd /k "command"`
   - `WEB:url` → `start "" "url"` (opens in browser; WebView2 embedding planned)
   - `REPARENT:exe` → `start "" "exe"` (launches app; SetParent embedding planned)
   - `CWD=path` → sets working directory for the primary command
   - Multiple lines → first is primary, rest launch as separate processes

## Custom Themes

| Theme | Description |
|-------|-------------|
| **Dusky** | Dark beige chrome — warm muted tones for low-light coding |
| **Garish** | Purple chrome — high-contrast vibrant theme |

## Building

```powershell
# Full solution build (includes packaging for AppX registration)
cd C:\source\terminal
Import-Module .\tools\OpenConsole.psm1
Set-MsBuildDevEnvironment
Invoke-OpenConsoleBuild /t:Build /p:Platform=x64 /p:Configuration=Debug /m

# Launcher only (~3 seconds)
msbuild "src\cascadia\wt-launcher\wt-launcher.vcxproj" /t:Build /p:Platform=x64 /p:Configuration=Debug /p:SolutionDir="C:\source\terminal\"
```

## Deploying

```powershell
# Register the debug AppX package (preserves settings!)
Add-AppxPackage -Register "C:\source\terminal\src\cascadia\CascadiaPackage\bin\x64\Debug\AppX\AppxManifest.xml" -ForceApplicationShutdown

# ⚠️ NEVER use Remove-AppxPackage — it wipes LocalState/settings.json
```

After a full build, if the AppX folder has stale DLLs, copy newer files from the build output:
```powershell
# Compare and copy newer DLLs from bin\x64\Debug\CascadiaPackage\ to AppX\
```

## Launcher Deployment

```powershell
Copy-Item "bin\x64\Debug\wt-launcher\wt-launcher.exe" "C:\launcher\" -Force
Copy-Item "src\cascadia\wt-launcher\wt-launch.cmd" "C:\launcher\" -Force
```

## Roadmap

- [x] Vertical tab sidebar with color, reorder, resize
- [x] TUI launcher with 8 tabs
- [x] SSH host management with tmux
- [x] Web app bookmarks
- [x] Win32 app launcher
- [x] Serial terminal with VT pass-through
- [x] Docker/Podman container management
- [x] Copilot CLI session management with YOLO mode
- [x] Custom themes (Dusky, Garish)
- [ ] WebView2 browser tabs (embedded in Terminal, persistent cookies)
- [ ] SetParent Win32 app reparenting (embedded in Terminal tabs)
- [ ] Signed OTA boot images
- [ ] Upstream merge (76 commits behind microsoft/terminal main)

## Upstream

This fork tracks [microsoft/terminal](https://github.com/microsoft/terminal) as `origin`. The fork remote is `fork` → [WillEastbury/TabbedTerminal](https://github.com/WillEastbury/TabbedTerminal).

## License

Same as upstream — [MIT License](LICENSE).
