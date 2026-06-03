// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ContainerEnumerator.h"

#include <windows.h>
#include <winhttp.h>
#include <sstream>

namespace winrt::TerminalApp::implementation
{
    // Query Docker Engine API over the named pipe \\.\pipe\docker_engine
    std::string ContainerEnumerator::_QueryDockerApi(const std::string& path)
    {
        // Connect to Docker's named pipe via CreateFile
        auto pipe = CreateFileW(
            L"\\\\.\\pipe\\docker_engine",
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (pipe == INVALID_HANDLE_VALUE)
        {
            return {};
        }

        auto closePipe = wil::scope_exit([&] { CloseHandle(pipe); });

        // Send HTTP request over the pipe
        std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n";
        DWORD written = 0;
        if (!WriteFile(pipe, request.c_str(), static_cast<DWORD>(request.size()), &written, nullptr))
        {
            return {};
        }

        // Read the response
        std::string response;
        char buffer[4096];
        DWORD bytesRead = 0;

        // Read until we have the full response
        while (ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
        {
            response.append(buffer, bytesRead);
            // Simple check: if we got a complete JSON array, stop
            if (response.find("\r\n\r\n") != std::string::npos)
            {
                // Find the body after headers
                auto bodyStart = response.find("\r\n\r\n");
                if (bodyStart != std::string::npos)
                {
                    auto body = response.substr(bodyStart + 4);
                    // For chunked encoding, handle the chunk size line
                    if (response.find("Transfer-Encoding: chunked") != std::string::npos)
                    {
                        auto chunkEnd = body.find("\r\n");
                        if (chunkEnd != std::string::npos)
                        {
                            body = body.substr(chunkEnd + 2);
                        }
                    }
                    // Check if we have a complete JSON array
                    if (!body.empty() && body.front() == '[')
                    {
                        int depth = 0;
                        for (auto c : body)
                        {
                            if (c == '[')
                                depth++;
                            else if (c == ']')
                                depth--;
                            if (depth == 0)
                            {
                                return body.substr(0, &c - body.data() + 1);
                            }
                        }
                    }
                    else if (!body.empty() && body.front() != '[')
                    {
                        // Not a JSON array - possibly an error or empty
                        break;
                    }
                }
            }
        }

        // Fallback: try to extract body from whatever we got
        auto bodyStart = response.find("\r\n\r\n");
        if (bodyStart != std::string::npos)
        {
            return response.substr(bodyStart + 4);
        }
        return {};
    }

    std::vector<ContainerInfo> ContainerEnumerator::EnumerateDocker()
    {
        std::vector<ContainerInfo> results;

        auto json = _QueryDockerApi("/v1.41/containers/json?all=true");
        if (json.empty())
        {
            return results;
        }

        // Simple JSON parsing for the container list
        // Each container object has: "Id", "Names", "Image", "State"
        // We parse minimally without a full JSON library dependency
        size_t pos = 0;
        while ((pos = json.find("\"Id\"", pos)) != std::string::npos)
        {
            ContainerInfo info{};
            info.provider = ContainerProvider::Docker;

            // Extract Id
            auto idStart = json.find('\"', pos + 4) + 1;
            auto idEnd = json.find('\"', idStart);
            if (idStart != std::string::npos && idEnd != std::string::npos)
            {
                auto idStr = json.substr(idStart, idEnd - idStart);
                info.id = std::wstring(idStr.begin(), idStr.end());
                // Truncate to 12 chars like docker does
                if (info.id.size() > 12)
                    info.id = info.id.substr(0, 12);
            }

            // Extract Names (first name, strip leading /)
            auto namesPos = json.find("\"Names\"", pos);
            if (namesPos != std::string::npos && namesPos < json.find("\"Id\"", pos + 4))
            {
                auto nameStart = json.find("\"", json.find("[", namesPos) + 1) + 1;
                auto nameEnd = json.find("\"", nameStart);
                if (nameStart != std::string::npos && nameEnd != std::string::npos)
                {
                    auto nameStr = json.substr(nameStart, nameEnd - nameStart);
                    if (!nameStr.empty() && nameStr[0] == '/')
                        nameStr = nameStr.substr(1);
                    info.name = std::wstring(nameStr.begin(), nameStr.end());
                }
            }

            // Extract Image
            auto imagePos = json.find("\"Image\"", pos);
            if (imagePos != std::string::npos)
            {
                auto imgStart = json.find('\"', imagePos + 7) + 1;
                auto imgEnd = json.find('\"', imgStart);
                if (imgStart != std::string::npos && imgEnd != std::string::npos)
                {
                    auto imgStr = json.substr(imgStart, imgEnd - imgStart);
                    info.image = std::wstring(imgStr.begin(), imgStr.end());
                }
            }

            // Extract State
            auto statePos = json.find("\"State\"", pos);
            if (statePos != std::string::npos)
            {
                auto stStart = json.find('\"', statePos + 7) + 1;
                auto stEnd = json.find('\"', stStart);
                if (stStart != std::string::npos && stEnd != std::string::npos)
                {
                    auto stStr = json.substr(stStart, stEnd - stStart);
                    info.state = std::wstring(stStr.begin(), stStr.end());
                }
            }

            if (!info.id.empty())
            {
                results.push_back(std::move(info));
            }

            pos = idEnd != std::string::npos ? idEnd : pos + 4;
        }

        return results;
    }

    std::vector<ContainerInfo> ContainerEnumerator::EnumerateHyperV()
    {
        std::vector<ContainerInfo> results;

        // Use PowerShell to enumerate Hyper-V VMs
        // We create a pipe to capture output from: powershell -NoProfile -Command "Get-VM | ConvertTo-Json"
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hReadPipe, hWritePipe;
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        {
            return results;
        }

        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hWritePipe;
        si.hStdError = hWritePipe;

        PROCESS_INFORMATION pi{};

        std::wstring cmd = L"powershell.exe -NoProfile -Command \"Get-VM | Select-Object Id, Name, State | ConvertTo-Json -Compress\"";

        if (!CreateProcessW(
                nullptr,
                cmd.data(),
                nullptr,
                nullptr,
                TRUE,
                CREATE_NO_WINDOW,
                nullptr,
                nullptr,
                &si,
                &pi))
        {
            CloseHandle(hReadPipe);
            CloseHandle(hWritePipe);
            return results;
        }

        CloseHandle(hWritePipe);

        // Read output
        std::string output;
        char buffer[4096];
        DWORD bytesRead;
        while (ReadFile(hReadPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
        {
            output.append(buffer, bytesRead);
        }

        CloseHandle(hReadPipe);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        if (output.empty())
        {
            return results;
        }

        // Parse the JSON output from Get-VM
        // Look for "Id", "Name", "State" fields
        size_t pos = 0;
        while ((pos = output.find("\"Name\"", pos)) != std::string::npos)
        {
            ContainerInfo info{};
            info.provider = ContainerProvider::HyperV;

            // Extract Name
            auto nameStart = output.find('\"', pos + 6) + 1;
            auto nameEnd = output.find('\"', nameStart);
            if (nameStart != std::string::npos && nameEnd != std::string::npos)
            {
                auto nameStr = output.substr(nameStart, nameEnd - nameStart);
                info.name = std::wstring(nameStr.begin(), nameStr.end());
            }

            // Extract Id (GUID)
            auto idPos = output.find("\"Id\"", pos);
            if (idPos != std::string::npos)
            {
                auto idStart = output.find('\"', idPos + 4) + 1;
                auto idEnd = output.find('\"', idStart);
                if (idStart != std::string::npos && idEnd != std::string::npos)
                {
                    auto idStr = output.substr(idStart, idEnd - idStart);
                    info.id = std::wstring(idStr.begin(), idStr.end());
                }
            }

            // Extract State (numeric from PowerShell: 2=Running, 3=Off, etc.)
            auto statePos = output.find("\"State\"", pos);
            if (statePos != std::string::npos)
            {
                auto stStart = statePos + 8;
                // State could be a number
                while (stStart < output.size() && (output[stStart] == ' ' || output[stStart] == ':'))
                    stStart++;
                auto stEnd = stStart;
                while (stEnd < output.size() && output[stEnd] != ',' && output[stEnd] != '}')
                    stEnd++;
                auto stStr = output.substr(stStart, stEnd - stStart);
                // Map numeric states
                if (stStr == "2")
                    info.state = L"Running";
                else if (stStr == "3")
                    info.state = L"Off";
                else if (stStr == "6")
                    info.state = L"Saved";
                else
                    info.state = std::wstring(stStr.begin(), stStr.end());
            }

            info.image = L"Hyper-V VM";

            if (!info.name.empty())
            {
                results.push_back(std::move(info));
            }

            pos = nameEnd != std::string::npos ? nameEnd : pos + 6;
        }

        return results;
    }

    std::vector<ContainerInfo> ContainerEnumerator::EnumerateAll()
    {
        auto results = EnumerateDocker();
        auto hyperv = EnumerateHyperV();
        results.insert(results.end(), hyperv.begin(), hyperv.end());
        return results;
    }

    std::wstring ContainerEnumerator::BuildExecCommand(const ContainerInfo& container)
    {
        if (container.provider == ContainerProvider::Docker)
        {
            // docker exec -it <id> /bin/sh (for Linux) or cmd (for Windows containers)
            // Default to /bin/sh; user can override
            return L"docker exec -it " + container.id + L" /bin/sh";
        }
        else
        {
            // Hyper-V: use PowerShell Direct
            return L"powershell.exe -NoProfile -Command \"Enter-PSSession -VMId " + container.id + L"\"";
        }
    }

    std::string ContainerEnumerator::_PostDockerApi(const std::string& path, const std::string& body)
    {
        auto pipe = CreateFileW(
            L"\\\\.\\pipe\\docker_engine",
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr);

        if (pipe == INVALID_HANDLE_VALUE)
        {
            return {};
        }

        auto closePipe = wil::scope_exit([&] { CloseHandle(pipe); });

        std::string request = "POST " + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: " +
                              std::to_string(body.size()) + "\r\n\r\n" + body;
        DWORD written = 0;
        if (!WriteFile(pipe, request.c_str(), static_cast<DWORD>(request.size()), &written, nullptr))
        {
            return {};
        }

        // Read response
        std::string response;
        char buffer[4096];
        DWORD bytesRead = 0;
        while (ReadFile(pipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0)
        {
            response.append(buffer, bytesRead);
            if (response.find("\r\n\r\n") != std::string::npos)
            {
                break;
            }
        }
        return response;
    }

    bool ContainerEnumerator::StartContainer(const ContainerInfo& container)
    {
        if (container.provider == ContainerProvider::Docker)
        {
            // POST /containers/{id}/start
            std::string idNarrow;
            idNarrow.reserve(container.id.size());
            for (auto wc : container.id)
                idNarrow.push_back(static_cast<char>(wc));
            auto response = _PostDockerApi("/v1.41/containers/" + idNarrow + "/start");
            // 204 No Content = success, 304 = already started
            return response.find("204") != std::string::npos ||
                   response.find("304") != std::string::npos;
        }
        else
        {
            // Hyper-V: Start-VM
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            PROCESS_INFORMATION pi{};
            std::wstring cmd = L"powershell.exe -NoProfile -Command \"Start-VM -Id " + container.id + L"\"";
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            {
                WaitForSingleObject(pi.hProcess, 10000);
                DWORD exitCode = 1;
                GetExitCodeProcess(pi.hProcess, &exitCode);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return exitCode == 0;
            }
            return false;
        }
    }

    bool ContainerEnumerator::CreateAndStartContainer(const std::wstring& image)
    {
        // Use docker run -dit <image> to create and start a new container in detached interactive mode
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE hReadPipe, hWritePipe;
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
        {
            return false;
        }
        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hWritePipe;
        si.hStdError = hWritePipe;

        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"docker run -dit " + image;

        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(hReadPipe);
            CloseHandle(hWritePipe);
            return false;
        }

        CloseHandle(hWritePipe);
        WaitForSingleObject(pi.hProcess, 30000);

        DWORD exitCode = 1;
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        CloseHandle(hReadPipe);

        return exitCode == 0;
    }
}
