#pragma once

#include "Transport/ITransport.h"
#include "Transport/StdioStreams.h"
#include "Transport/TransportChannels.h"

#include "Awl/ILogger.h"

#include <map>
#include <list>
#include <chrono>
#include <stop_token>
#include <set>

namespace mcp
{
    struct StdioOptions
    {
        std::size_t maxMessageBytes = 1024 * 1024;
        std::size_t maxPendingRequests = 64;
        std::chrono::milliseconds shutdownTimeout{1000};
    };

    class StdioTransport : public ITransport
    {
    public:

        // Executor wraps the server strand. Channels/logger are non-null.
        // Stream objects and their executors remain alive until asyncRun ends.
        StdioTransport(boost::asio::any_io_executor executor, StdioStreams streams,
            const std::shared_ptr<InputChannel>& input_channel, const std::shared_ptr<OutputChannel>& output_channel,
            const std::shared_ptr<awl::ILogger>& logger, StdioOptions options = {});

        boost::asio::awaitable<void> asyncRun() override;

    private:

        boost::asio::awaitable<void> asyncRead();

        boost::asio::awaitable<void> asyncWrite();

        boost::asio::awaitable<void> asyncDeliver(std::string body);

        void completeRequest(std::stop_token token);

        boost::asio::awaitable<void> asyncDrain();

        boost::asio::awaitable<void> asyncDrainTimeout();

        boost::asio::any_io_executor _executor;
        StdioStreams _streams;
        std::shared_ptr<InputChannel> _inputChannel;
        std::shared_ptr<OutputChannel> _outputChannel;
        const std::shared_ptr<awl::ILogger> _logger;
        StdioOptions _options;
        awl::Channel<void(boost::system::error_code, bool)> _drained;
        std::map<std::string, std::stop_source> _requests;
        std::set<std::string> _initializeRequests;
        std::string _protocolVersion;
        std::list<std::stop_source> _pending;
        bool _started = false;
        bool _shutdownTimedOut = false;
    };
}
