#pragma once

#include <boost/asio/experimental/concurrent_channel.hpp>

namespace mcp
{
    template <typename ExecutorOrSignature, typename... Signatures>
    using Channel = boost::asio::experimental::concurrent_channel<
        ExecutorOrSignature, Signatures...>;
}
