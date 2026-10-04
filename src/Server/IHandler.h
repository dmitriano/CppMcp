#pragma once

#include "Server/RequestContext.h"

namespace mcp
{
    class IHandler
    {
    public:

        virtual ~IHandler() = default;

        virtual boost::asio::awaitable<boost::json::value> asyncHandle(
            boost::json::object params, RequestContext context) = 0;
    };
}
