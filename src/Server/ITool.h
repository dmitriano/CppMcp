#pragma once

#include "Server/RequestContext.h"

#include <string>

namespace mcp
{
    struct ToolDefinition
    {
        std::string name;
        std::string description;
        boost::json::object inputSchema;
        std::optional<boost::json::object> outputSchema;
    };

    struct ToolResult
    {
        boost::json::array content;
        std::optional<boost::json::value> structuredContent;
        bool isError = false;
    };

    class ITool
    {
    public:

        virtual ~ITool() = default;

        // Server snapshots the definition at registration.
        virtual const ToolDefinition& definition() const = 0;

        // Validate arguments here. Execution failures return isError=true.
        virtual boost::asio::awaitable<ToolResult> asyncCall(
            boost::json::object arguments, RequestContext context) = 0;
    };
}
