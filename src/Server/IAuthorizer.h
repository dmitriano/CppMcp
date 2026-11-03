#pragma once

#include <boost/asio/awaitable.hpp>
#include <string>

namespace mcp
{
    class IAuthorizer
    {
    public:

        virtual ~IAuthorizer() = default;

        // Permission names belong to the application. Failures/cancellation propagate.
        virtual boost::asio::awaitable<bool> asyncIsAllowed(
            std::string token, std::string permission) = 0;
    };
}
