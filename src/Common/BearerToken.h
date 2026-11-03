#pragma once

#include <openssl/crypto.h>
#include <string_view>

namespace mcp
{
    inline bool isValidBearerToken(const std::string_view token)
    {
        if (token.empty())
        {
            return false;
        }

        bool padding = false;

        for (const unsigned char c : token)
        {
            if (c == '=')
            {
                padding = true;
            }
            else if (padding || !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' || c == '+' || c == '/'))
            {
                return false;
            }
        }

        return token.front() != '=';
    }

    inline bool bearerTokensEqual(const std::string_view left, const std::string_view right)
    {
        return left.size() == right.size() && !left.empty() &&
            CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
    }
}
