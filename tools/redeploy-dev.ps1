<#
.SYNOPSIS
    Redeploy the locally-built dev TabbedTerminal into its AppX layout and re-register it.

.DESCRIPTION
    The "Update & Restart" button and a manual rebuild both need the SAME thing:
    the freshly-linked binaries copied into the AppX layout, a version bump so
    Windows accepts the re-register, and then Add-AppxPackage -Register.

    The DLLs in the AppX layout are LOCKED while the dev terminal is running, so
    this script first waits for that process to exit (the caller is expected to
    be closing it / -ForceApplicationShutdown handles the rest).

    It NEVER calls Remove-AppxPackage, so settings.json and user state are kept.

.PARAMETER NoRelaunch
    Skip relaunching the app after registering (useful when invoked manually).
#>
[CmdletBinding()]
param(
    [switch]$NoRelaunch
)

$ErrorActionPreference = 'Stop'

$src  = 'C:\source\terminal\src\cascadia\CascadiaPackage\bin\x64\Debug'
$appx = Join-Path $src 'AppX'
$manifest = Join-Path $appx 'AppxManifest.xml'

if (-not (Test-Path $manifest)) {
    Write-Error "AppxManifest not found at $manifest - build the solution first."
    exit 1
}

# 1. Wait (up to ~10s) for any dev terminal running from the AppX layout to exit,
#    otherwise its locked DLLs can't be overwritten.
for ($i = 0; $i -lt 40; $i++) {
    $running = Get-Process WindowsTerminal -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($appx, [System.StringComparison]::OrdinalIgnoreCase) }
    if (-not $running) { break }
    Start-Sleep -Milliseconds 250
}

# 2. Copy any newer build outputs into the AppX layout (skip the AppX folder itself).
$copied = 0
Get-ChildItem $src -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($src.Length).TrimStart('\')
    if ($rel -like 'AppX\*') { return }
    $dest = Join-Path $appx $rel
    if (-not (Test-Path $dest) -or $_.LastWriteTime -gt (Get-Item $dest).LastWriteTime) {
        $destDir = Split-Path $dest -Parent
        if (-not (Test-Path $destDir)) { New-Item -ItemType Directory -Path $destDir -Force | Out-Null }
        try {
            Copy-Item $_.FullName $dest -Force
            $copied++
        } catch {
            Write-Warning "Could not copy $rel ($($_.Exception.Message)) - is the app still running?"
        }
    }
}
Write-Host "Copied $copied newer file(s) into the AppX layout."

# 3. Bump the 4th version component so the re-register is accepted.
$content = Get-Content $manifest -Raw
$content = [regex]::Replace(
    $content,
    '(<Identity\b[^>]*\bVersion=")(\d+)\.(\d+)\.(\d+)\.(\d+)(")',
    {
        param($m)
        $next = [int]$m.Groups[5].Value + 1
        "$($m.Groups[1].Value)$($m.Groups[2].Value).$($m.Groups[3].Value).$($m.Groups[4].Value).$next$($m.Groups[6].Value)"
    },
    [System.Text.RegularExpressions.RegexOptions]::Singleline)
Set-Content $manifest $content -NoNewline
if ($content -match '<Identity\b[^>]*\bVersion="([\d.]+)"') {
    Write-Host "Manifest version is now $($Matches[1])."
}

# 4. Register (keeps settings - does NOT remove the package).
Add-AppxPackage -Register $manifest -ForceApplicationShutdown
Write-Host "Re-registered WindowsTerminalDev."

# 5. Relaunch unless asked not to.
if (-not $NoRelaunch) {
    Start-Sleep -Seconds 1
    Start-Process 'shell:AppsFolder\WindowsTerminalDev_8wekyb3d8bbwe!App'
}
