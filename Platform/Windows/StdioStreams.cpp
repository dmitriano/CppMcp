#include "Transport/StdioStreams.h"

#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <windows.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <limits>
#include <mutex>

namespace mcp
{
    namespace
    {
        namespace asio = boost::asio;

        // One synchronous syscall per operation. A mutex protects the worker
        // handle so cancellation cannot affect another job after this one ends.
        class Operation : public std::enable_shared_from_this<Operation>
        {
        public:

            Operation(const asio::any_io_executor& executor, const HANDLE file, const asio::mutable_buffer buffer,
                const bool write, std::function<void(boost::system::error_code, std::size_t)> completion) :
                _executor(executor), _timer(executor), _file(file), _buffer(buffer), _write(write),
                _completion(std::move(completion))
            {}

            void run()
            {
                boost::system::error_code error;
                DWORD transferred = 0;
                {
                    const std::lock_guard lock(_mutex);
                    if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentThread(), ::GetCurrentProcess(),
                        &_thread, THREAD_TERMINATE, FALSE, 0))
                    {
                        error = {static_cast<int>(::GetLastError()), boost::system::system_category()};
                    }
                }

                if (!error && !_cancelled.load())
                {
                    const DWORD size = static_cast<DWORD>(std::min<std::size_t>(_buffer.size(), MAXDWORD));
                    const BOOL success = _write ? ::WriteFile(_file, _buffer.data(), size, &transferred, nullptr)
                        : ::ReadFile(_file, _buffer.data(), size, &transferred, nullptr);
                    if (!success)
                    {
                        const DWORD code = ::GetLastError();
                        if (!(!_write && (code == ERROR_BROKEN_PIPE || code == ERROR_HANDLE_EOF)))
                        {
                            error = {static_cast<int>(code), boost::system::system_category()};
                        }
                    }
                }

                {
                    const std::lock_guard lock(_mutex);
                    if (_thread)
                    {
                        ::CloseHandle(_thread);
                        _thread = nullptr;
                    }
                }

                // Cancellation wins a race with a successful syscall. The
                // completion still waits for the worker to release the buffer.
                asio::post(_executor, [self = shared_from_this(), error, transferred]() mutable
                {
                    self->_finished = true;
                    self->_timer.cancel();
                    if (self->_cancelled.load())
                    {
                        error = asio::error::operation_aborted;
                    }

                    self->_completion(error, transferred);
                });
            }

            void cancel()
            {
                _cancelled = true;
                asio::post(_executor, [self = shared_from_this()] { self->cancelWorker(); });
            }

        private:

            void cancelWorker()
            {
                if (_finished)
                {
                    return;
                }

                {
                    const std::lock_guard lock(_mutex);
                    if (_thread)
                    {
                        ::CancelSynchronousIo(_thread);
                    }
                }

                // Retry covers cancellation arriving just before ReadFile or
                // WriteFile starts; ERROR_NOT_FOUND is not completion.
                _timer.expires_after(std::chrono::milliseconds(1));
                _timer.async_wait([self = shared_from_this()](const boost::system::error_code error)
                {
                    if (!error)
                    {
                        self->cancelWorker();
                    }
                });
            }

            asio::any_io_executor _executor;
            asio::steady_timer _timer;
            HANDLE _file;
            asio::mutable_buffer _buffer;
            bool _write;
            std::function<void(boost::system::error_code, std::size_t)> _completion;
            std::atomic<bool> _cancelled = false;
            std::mutex _mutex;
            HANDLE _thread = nullptr;
            bool _finished = false;
        };

        class NativeStream : public IByteStream
        {
        public:

            NativeStream(const asio::any_io_executor& executor, const asio::any_io_executor& blocking_executor,
                const DWORD standard_handle) : _executor(executor), _blockingExecutor(blocking_executor)
            {
                if (!::DuplicateHandle(::GetCurrentProcess(), ::GetStdHandle(standard_handle), ::GetCurrentProcess(),
                    &_file, 0, FALSE, DUPLICATE_SAME_ACCESS))
                {
                    throw boost::system::system_error(static_cast<int>(::GetLastError()), boost::system::system_category());
                }
            }

            ~NativeStream()
            {
                ::CloseHandle(_file);
            }

            asio::awaitable<std::size_t> asyncReadSome(const asio::mutable_buffer buffer) override
            {
                co_return co_await asyncTransfer(buffer, false);
            }

            asio::awaitable<void> asyncWrite(std::string bytes) override
            {
                std::size_t offset = 0;
                while (offset < bytes.size())
                {
                    const auto size = co_await asyncTransfer(asio::buffer(bytes.data() + offset, bytes.size() - offset), true);
                    if (size == 0)
                    {
                        throw boost::system::system_error(asio::error::broken_pipe);
                    }

                    offset += size;
                }
            }

        private:

            asio::awaitable<std::size_t> asyncTransfer(const asio::mutable_buffer buffer, const bool write)
            {
                auto token = asio::use_awaitable;
                co_return co_await asio::async_initiate<decltype(token), void(boost::system::error_code, std::size_t)>(
                    [this, buffer, write](auto handler)
                    {
                        auto slot = asio::get_associated_cancellation_slot(handler);
                        std::shared_ptr<decltype(handler)> completion = std::make_shared<decltype(handler)>(std::move(handler));
                        std::shared_ptr<Operation> operation = std::make_shared<Operation>(_executor, _file, buffer, write,
                            [completion, slot](const boost::system::error_code error, const std::size_t size) mutable
                            {
                                if (slot.is_connected())
                                {
                                    slot.clear();
                                }

                                (*completion)(error, size);
                            });
                        if (slot.is_connected())
                        {
                            slot.assign([weak = std::weak_ptr(operation)](const asio::cancellation_type type)
                            {
                                if (type != asio::cancellation_type::none)
                                {
                                    if (std::shared_ptr<Operation> active = weak.lock())
                                    {
                                        active->cancel();
                                    }
                                }
                            });
                        }

                        asio::post(_blockingExecutor, [operation] { operation->run(); });
                    }, token);
            }

            asio::any_io_executor _executor;
            asio::any_io_executor _blockingExecutor;
            HANDLE _file = nullptr;
        };
    }

    StdioStreams openStdioStreams(const asio::any_io_executor& executor, const asio::any_io_executor& blocking_executor)
    {
        return {std::make_unique<NativeStream>(executor, blocking_executor, STD_INPUT_HANDLE),
            std::make_unique<NativeStream>(executor, blocking_executor, STD_OUTPUT_HANDLE)};
    }
}
