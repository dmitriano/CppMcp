#include "Server/RequestContext.h"

#include <boost/asio/use_awaitable.hpp>

namespace mcp
{
    boost::asio::awaitable<void> RequestContext::asyncNotify(
        std::string method, boost::json::object params) const
    {
        boost::json::object notification{
            {"jsonrpc", "2.0"}, {"method", std::move(method)}, {"params", std::move(params)}};
        co_await _outputChannel->async_send(boost::system::error_code{},
            OutgoingMessage{_routeId, MessageKind::Notification, boost::json::serialize(notification), _deliveryToken},
            boost::asio::use_awaitable);
    }
}
