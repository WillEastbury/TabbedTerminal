// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#pragma once

#include <string>
#include <vector>
#include <functional>

namespace winrt::TerminalApp::implementation
{
    enum class ContainerProvider
    {
        Docker,
        HyperV
    };

    struct ContainerInfo
    {
        std::wstring id;
        std::wstring name;
        std::wstring image;
        std::wstring state;
        ContainerProvider provider;
    };

    struct ContainerEnumerator
    {
        static std::vector<ContainerInfo> EnumerateAll();
        static std::vector<ContainerInfo> EnumerateDocker();
        static std::vector<ContainerInfo> EnumerateHyperV();
        static std::wstring BuildExecCommand(const ContainerInfo& container);
        static bool StartContainer(const ContainerInfo& container);
        static bool CreateAndStartContainer(const std::wstring& image);

    private:
        static std::string _QueryDockerApi(const std::string& path);
        static std::string _PostDockerApi(const std::string& path, const std::string& body = "");
    };
}
