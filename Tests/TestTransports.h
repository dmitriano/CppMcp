#pragma once

#include "Transport/ITransport.h"
#include "Transport/TransportChannels.h"

#include "Awl/ILogger.h"

#include <memory>
#include <string>

namespace mcp::testing
{
    struct ClientMessage
    {
        RouteId routeId;
        std::string body;
    };

    using ClientRequestChannel = awl::Channel<void(boost::system::error_code, ClientMessage)>;
    using ClientResponseChannel = awl::Channel<void(boost::system::error_code, OutgoingMessage)>;

    class TestTransport : public ITransport
    {
    public:

        TestTransport(const std::shared_ptr<InputChannel>& input_channel,
            const std::shared_ptr<OutputChannel>& output_channel,
            const std::shared_ptr<ClientRequestChannel>& client_requests,
            const std::shared_ptr<ClientResponseChannel>& client_responses,
            const std::shared_ptr<awl::ILogger>& logger);

        boost::asio::awaitable<void> asyncRun() override;

    protected:

        virtual void validateRoute(RouteId route_id) const = 0;

    private:

        boost::asio::awaitable<void> asyncRead();

        boost::asio::awaitable<void> asyncWrite();

        std::shared_ptr<InputChannel> _inputChannel;
        std::shared_ptr<OutputChannel> _outputChannel;
        std::shared_ptr<ClientRequestChannel> _clientRequests;
        std::shared_ptr<ClientResponseChannel> _clientResponses;
        const std::shared_ptr<awl::ILogger> _logger;
    };

    class TestStdioTransport : public TestTransport
    {
    public:

        using TestTransport::TestTransport;

    protected:

        void validateRoute(RouteId route_id) const override;
    };

    class TestHttpTransport : public TestTransport
    {
    public:

        using TestTransport::TestTransport;

    protected:

        void validateRoute(RouteId route_id) const override;
    };
}
