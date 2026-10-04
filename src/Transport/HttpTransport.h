#pragma once

#include "Transport/ITransport.h"
#include "Transport/TransportChannels.h"

#include "Awl/ILogger.h"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <chrono>
#include <map>
#include <vector>

namespace mcp
{
    struct HttpOptions
    {
        boost::asio::ip::tcp::endpoint endpoint{boost::asio::ip::make_address("127.0.0.1"), 0};
        std::string path = "/mcp";
        std::vector<std::string> allowedOrigins;
        std::size_t maxBodyBytes = 1024 * 1024;
        std::size_t maxConnections = 64;
        std::size_t maxQueuedResponses = 32;
        std::chrono::milliseconds readTimeout{10000};
        std::chrono::milliseconds requestTimeout{30000};
    };

    class HttpTransport : public ITransport
    {
    public:

        // Bind/listen occurs at construction, including port 0 for integration
        // tests. Executor wraps the server strand. Channels/logger are non-null.
        // One POST per connection; the response advertises Connection: close.
        HttpTransport(boost::asio::any_io_executor executor,
            const std::shared_ptr<InputChannel>& input_channel, const std::shared_ptr<OutputChannel>& output_channel,
            const std::shared_ptr<awl::ILogger>& logger, HttpOptions options = {});

        boost::asio::ip::tcp::endpoint localEndpoint() const;

        boost::asio::awaitable<void> asyncRun() override;

    private:

        struct Session;

        boost::asio::awaitable<void> asyncAccept();

        boost::asio::awaitable<void> asyncRoute();

        boost::asio::any_io_executor _executor;
        std::shared_ptr<InputChannel> _inputChannel;
        std::shared_ptr<OutputChannel> _outputChannel;
        const std::shared_ptr<awl::ILogger> _logger;
        HttpOptions _options;
        boost::asio::ip::tcp::acceptor _acceptor;
        std::map<RouteId, std::shared_ptr<Session>> _sessions;
        Channel<void(boost::system::error_code, bool)> _completed;
        RouteId _nextRouteId = 1;
        bool _started = false;
    };
}
