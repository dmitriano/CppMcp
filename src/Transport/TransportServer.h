#pragma once

#include "Transport/ITransport.h"

#include "Awl/ILogger.h"

#include <boost/asio/any_io_executor.hpp>
#include <memory>
#include <vector>

namespace mcp
{
    class TransportServer
    {
    public:

        // Must wrap a caller-created strand. No threads or strands are created here.
        // Logger is non-null and supplied by the owner.
        TransportServer(boost::asio::any_io_executor executor, const std::shared_ptr<awl::ILogger>& logger);

        // Setup only, before asyncRun(); transfers ownership of a non-null transport.
        void addTransport(std::unique_ptr<ITransport> transport);

        // Runs all added transports. Failure or caller cancellation cancels and
        // joins the children before returning. Keep this server alive until then.
        boost::asio::awaitable<void> asyncRun();

    private:

        boost::asio::any_io_executor _executor;
        const std::shared_ptr<awl::ILogger> _logger;
        std::vector<std::unique_ptr<ITransport>> _transports;
        bool _started = false;
    };
}
