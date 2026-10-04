#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <memory>
#include <string>

namespace mcp
{
    class IByteStream
    {
    public:

        virtual ~IByteStream() = default;

        virtual boost::asio::awaitable<std::size_t> asyncReadSome(boost::asio::mutable_buffer buffer) = 0;

        virtual boost::asio::awaitable<void> asyncWrite(std::string bytes) = 0;
    };

    struct StdioStreams
    {
        std::unique_ptr<IByteStream> input;
        std::unique_ptr<IByteStream> output;
    };

    // Owns duplicate handles, leaving the process's standard streams open.
    // Executor wraps a strand. Windows requires a separate blocking executor
    // with at least two workers; both executors outlive all stream operations.
    StdioStreams openStdioStreams(const boost::asio::any_io_executor& executor,
        const boost::asio::any_io_executor& blocking_executor);
}
