#include "Tests/TestTransports.h"

#include "Awl/Exception.h"

#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/experimental/channel_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <utility>

namespace mcp::testing
{
    TestTransport::TestTransport(const std::shared_ptr<InputChannel>& input_channel,
        const std::shared_ptr<OutputChannel>& output_channel,
        const std::shared_ptr<ClientRequestChannel>& client_requests,
        const std::shared_ptr<ClientResponseChannel>& client_responses,
        const std::shared_ptr<awl::ILogger>& logger) :
        _inputChannel(input_channel),
        _outputChannel(output_channel),
        _clientRequests(client_requests),
        _clientResponses(client_responses), _logger(logger)
    {}

    boost::asio::awaitable<void> TestTransport::asyncRun()
    {
        using namespace boost::asio::experimental::awaitable_operators;
        const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
        _logger->info("Transport starting");
        try
        {
            co_await (asyncRead() && asyncWrite());
        }
        catch (const boost::system::system_error& error)
        {
            if (error.code() == boost::asio::error::operation_aborted)
            {
                _logger->debug("Transport cancelled");
            }

            throw;
        }
        catch (const boost::asio::multiple_exceptions&)
        {
            if (cancellation.cancelled() != boost::asio::cancellation_type::none)
            {
                _logger->debug("Transport cancelled");
            }

            throw;
        }

        _logger->info("Transport stopped");
    }

    boost::asio::awaitable<void> TestTransport::asyncRead()
    {
        namespace asio = boost::asio;
        try
        {
            for (;;)
            {
                ClientMessage request = co_await _clientRequests->async_receive(asio::use_awaitable);
                validateRoute(request.routeId);
                _logger->debug("Reading request on route {}", request.routeId);
                co_await _inputChannel->async_send(boost::system::error_code{},
                    IncomingMessage{request.routeId, std::move(request.body), _outputChannel}, asio::use_awaitable);
            }
        }
        catch (const boost::system::system_error& error)
        {
            if (error.code() != asio::experimental::error::channel_closed)
            {
                throw;
            }
        }
    }

    boost::asio::awaitable<void> TestTransport::asyncWrite()
    {
        namespace asio = boost::asio;
        try
        {
            for (;;)
            {
                OutgoingMessage response = co_await _outputChannel->async_receive(asio::use_awaitable);
                validateRoute(response.routeId);
                _logger->debug("Writing message on route {}", response.routeId);
                co_await _clientResponses->async_send(boost::system::error_code{},
                    std::move(response), asio::use_awaitable);
            }
        }
        catch (const boost::system::system_error& error)
        {
            if (error.code() != asio::experimental::error::channel_closed)
            {
                throw;
            }
        }
    }

    void TestStdioTransport::validateRoute(const RouteId route_id) const
    {
        if (route_id != 0)
        {
            throw awl::GeneralException("The test stdio transport has only route 0.");
        }
    }

    void TestHttpTransport::validateRoute(const RouteId route_id) const
    {
        // Each route represents an independent HTTP POST; route 0 is valid too.
        static_cast<void>(route_id);
    }
}
