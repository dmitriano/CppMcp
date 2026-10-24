#pragma once

#include "BoostExtras/Channel.h"

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

namespace mcp::testing
{
    using SignalChannel = awl::Channel<void(boost::system::error_code, bool)>;

    class CancellationProbe
    {
    public:

        CancellationProbe(SignalChannel& entered, bool& finished) : _entered(entered), _finished(finished) {}

        boost::asio::awaitable<void> asyncWait()
        {
            struct Guard
            {
                bool& finished;
                ~Guard() { finished = true; }
            } guard{_finished};
            co_await _entered.async_send(boost::system::error_code{}, true, boost::asio::use_awaitable);
            boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
            timer.expires_at(boost::asio::steady_timer::time_point::max());
            co_await timer.async_wait(boost::asio::use_awaitable);
        }

    private:

        SignalChannel& _entered;
        bool& _finished;
    };
}
