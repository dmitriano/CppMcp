#include "Transport/TransportServer.h"

#include "Awl/Exception.h"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/deferred.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/experimental/parallel_group.hpp>
#include <boost/asio/multiple_exceptions.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <exception>
#include <utility>

namespace mcp
{
    TransportServer::TransportServer(boost::asio::any_io_executor executor,
        const std::shared_ptr<awl::ILogger>& logger) :
        _executor(std::move(executor)), _logger(logger)
    {}

    void TransportServer::addTransport(std::unique_ptr<ITransport> transport)
    {
        if (_started)
        {
            throw awl::GeneralException("Cannot add a transport after the server has started.");
        }

        _transports.push_back(std::move(transport));
        _logger->debug("Transport registered; count: {}", _transports.size());
    }

    boost::asio::awaitable<void> TransportServer::asyncRun()
    {
        namespace asio = boost::asio;
        co_await asio::dispatch(_executor, asio::use_awaitable);
        if (_started)
        {
            throw awl::GeneralException("The transport server can only run once.");
        }

        if (_transports.empty())
        {
            throw awl::GeneralException("No transports have been added.");
        }

        _started = true;
        const auto cancellation = co_await asio::this_coro::cancellation_state;
        _logger->info("Starting {} transports", _transports.size());
        using Operation = decltype(asio::co_spawn(
            _executor, std::declval<asio::awaitable<void>>(), asio::deferred));
        std::vector<Operation> operations;
        operations.reserve(_transports.size());
        for (const std::unique_ptr<ITransport>& transport : _transports)
        {
            operations.push_back(asio::co_spawn(_executor, transport->asyncRun(), asio::deferred));
        }

        try
        {
            auto [completion_order, exceptions] = co_await asio::experimental::make_parallel_group(
                std::move(operations)).async_wait(asio::experimental::wait_for_one_error(), asio::use_awaitable);
            for (const std::size_t index : completion_order)
            {
                if (exceptions[index])
                {
                    std::rethrow_exception(exceptions[index]);
                }
            }
        }
        catch (const boost::system::system_error& error)
        {
            if (error.code() == asio::error::operation_aborted)
            {
                _logger->debug("Transports cancelled");
            }
            else
            {
                _logger->error("Transport failure: {}", error.what());
            }

            throw;
        }
        catch (const awl::Exception& error)
        {
            _logger->error(_T("Transport failure: {}"), error.message());
            throw;
        }
        catch (const asio::multiple_exceptions& error)
        {
            if (cancellation.cancelled() != asio::cancellation_type::none)
            {
                _logger->debug("Transports cancelled");
            }
            else
            {
                _logger->error("Transport failure: {}", error.what());
            }

            throw;
        }
        catch (const std::exception& error)
        {
            _logger->error("Transport failure: {}", error.what());
            throw;
        }

        _logger->info("Transports stopped");
    }
}
