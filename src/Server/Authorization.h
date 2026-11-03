#pragma once

#include "Server/IAuthorizer.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mcp
{
    // Request-owned credentials supplied by the transport, never JSON-RPC metadata.
    // Raw tokens have no public getter and must not be logged.
    class Authorization
    {
    public:

        Authorization() = default;

        Authorization(const std::shared_ptr<IAuthorizer>& authorizer, std::string token) :
            _authorizer(authorizer), _token(std::move(token))
        {}

        // Only local trusted transports or an already validated global token use this.
        static Authorization trusted()
        {
            Authorization result;
            result._trusted = true;
            return result;
        }

        boost::asio::awaitable<bool> asyncHasPermissions(std::vector<std::string> permissions) const
        {
            if (permissions.empty() || _trusted)
            {
                co_return true;
            }

            if (!_authorizer || _token.empty())
            {
                co_return false;
            }

            for (const auto& permission : permissions)
            {
                if (!co_await _authorizer->asyncIsAllowed(_token, permission))
                {
                    co_return false;
                }
            }

            co_return true;
        }

    private:

        std::shared_ptr<IAuthorizer> _authorizer;
        std::string _token;
        bool _trusted = false;
    };
}
