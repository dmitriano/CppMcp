#pragma once

#include <string_view>

namespace mcp::protocol
{
    inline constexpr std::string_view modernVersion = "2026-07-28";
    inline constexpr std::string_view legacyVersion = "2025-06-18";

    inline bool isSupported(const std::string_view version)
    {
        return version == modernVersion || version == legacyVersion;
    }
}
