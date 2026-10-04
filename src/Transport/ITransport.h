#pragma once

#include <boost/asio/awaitable.hpp>

namespace mcp
{
    class ITransport
    {
    public:

        virtual ~ITransport() = default;

        // One run per instance. Cancellation stops and joins the transport's I/O.
        // The owner keeps the transport and its executors alive until completion.
        // A transport never closes the application's shared input channel.
        virtual boost::asio::awaitable<void> asyncRun() = 0;
    };
}
