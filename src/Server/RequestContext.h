#pragma once

#include "Transport/TransportChannels.h"
#include "Common/Protocol.h"

#include <boost/asio/awaitable.hpp>
#include <boost/json.hpp>
#include <optional>
#include <string>
#include <utility>

namespace mcp
{
    class RequestContext
    {
    public:

        RequestContext(const RouteId route_id, std::optional<boost::json::value> request_id,
            const std::shared_ptr<OutputChannel>& output_channel, boost::json::object meta = {},
            std::stop_token stop_token = {}, std::stop_token delivery_token = {},
            std::string protocol_version = std::string(protocol::modernVersion)) :
            _routeId(route_id), _requestId(std::move(request_id)), _outputChannel(output_channel),
            _meta(std::move(meta)), _stopToken(stop_token), _deliveryToken(delivery_token),
            _protocolVersion(std::move(protocol_version))
        {}

        RouteId routeId() const { return _routeId; }

        const std::optional<boost::json::value>& requestId() const { return _requestId; }

        std::stop_token stopToken() const { return _stopToken; }

        const std::string& protocolVersion() const { return _protocolVersion; }

        const boost::json::object& meta() const { return _meta; }

        // Await notifications during the handler invocation; do not detach work.
        boost::asio::awaitable<void> asyncNotify(std::string method, boost::json::object params) const;

    private:

        RouteId _routeId;
        std::optional<boost::json::value> _requestId;
        std::shared_ptr<OutputChannel> _outputChannel;
        boost::json::object _meta;
        std::stop_token _stopToken;
        std::stop_token _deliveryToken;
        std::string _protocolVersion;
    };
}
