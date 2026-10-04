#include "Transport/StdioStreams.h"

#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/system_error.hpp>
#include <unistd.h>
#include <cerrno>

namespace mcp
{
    namespace
    {
        namespace asio = boost::asio;

        class NativeStream : public IByteStream
        {
        public:

            NativeStream(const asio::any_io_executor& executor, const int descriptor) : _stream(executor)
            {
                const int duplicate = ::dup(descriptor);
                if (duplicate < 0)
                {
                    throw boost::system::system_error(errno, boost::system::generic_category());
                }

                boost::system::error_code error;
                _stream.assign(duplicate, error);
                if (error)
                {
                    ::close(duplicate);
                    throw boost::system::system_error(error);
                }
            }

            asio::awaitable<std::size_t> asyncReadSome(const asio::mutable_buffer buffer) override
            {
                boost::system::error_code error;
                const auto size = co_await _stream.async_read_some(buffer, asio::redirect_error(asio::use_awaitable, error));
                if (error && error != asio::error::eof)
                {
                    throw boost::system::system_error(error);
                }

                co_return size;
            }

            asio::awaitable<void> asyncWrite(std::string bytes) override
            {
                co_await asio::async_write(_stream, asio::buffer(bytes), asio::use_awaitable);
            }

        private:

            asio::posix::stream_descriptor _stream;
        };
    }

    StdioStreams openStdioStreams(const asio::any_io_executor& executor, const asio::any_io_executor&)
    {
        return {std::make_unique<NativeStream>(executor, STDIN_FILENO),
            std::make_unique<NativeStream>(executor, STDOUT_FILENO)};
    }
}
