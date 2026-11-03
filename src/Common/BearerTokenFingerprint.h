#pragma once

#include <array>
#include <cstddef>
#include <format>
#include <openssl/sha.h>
#include <string_view>

namespace mcp
{
    // Borrows the token until formatting; hashing runs only when the log is enabled.
    struct BearerTokenFingerprint
    {
        std::string_view token;
    };
}

namespace std
{
    template <>
    struct formatter<mcp::BearerTokenFingerprint> : formatter<string_view>
    {
        template <class FormatContext>
        auto format(const mcp::BearerTokenFingerprint& value, FormatContext& context) const
        {
            if (value.token.empty())
            {
                return formatter<string_view>::format("missing", context);
            }

            array<unsigned char, SHA256_DIGEST_LENGTH> digest;
            SHA256(reinterpret_cast<const unsigned char*>(value.token.data()), value.token.size(), digest.data());
            constexpr string_view digits = "0123456789abcdef";
            array<char, 16> fingerprint;

            for (size_t i = 0; i < fingerprint.size() / 2; ++i)
            {
                const auto byte = digest[i];
                fingerprint[i * 2] = digits[byte >> 4];
                fingerprint[i * 2 + 1] = digits[byte & 15];
            }

            return formatter<string_view>::format(string_view(fingerprint.data(), fingerprint.size()), context);
        }
    };
}
