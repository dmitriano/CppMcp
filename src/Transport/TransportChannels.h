#pragma once

#include "BoostExtras/Channel.h"

#include <boost/system/error_code.hpp>
#include <cstdint>
#include <memory>
#include <string>
#include <stop_token>

namespace mcp
{
    using RouteId = std::uint64_t;

    enum class MessageKind
    {
        Notification,
        Response,
        Accepted
    };

    struct OutgoingMessage
    {
        RouteId routeId;
        MessageKind kind;
        std::string body;
        std::stop_token stopToken;
    };

    // Directions are named from the application's handler perspective.
    using OutputChannel = awl::Channel<void(boost::system::error_code, OutgoingMessage)>;

    struct IncomingMessage
    {
        RouteId routeId;
        std::string body;
        // Required, non-null. Route IDs are local to this output channel.
        std::shared_ptr<OutputChannel> outputChannel;
        std::stop_token stopToken;
        bool requireProtocolMetadata = false;
        // HTTP header or negotiated stdio version; empty for generic test inputs.
        std::string protocolVersion;
    };

    using InputChannel = awl::Channel<void(boost::system::error_code, IncomingMessage)>;
}
