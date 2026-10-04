#pragma once

#include "Awl/Exception.h"

#include <boost/json.hpp>
#include <optional>
#include <string>
#include <utility>

namespace mcp
{
    // Explicit protocol errors; unexpected exceptions become a generic internal error.
    class RpcError : public awl::GeneralException
    {
    public:

        RpcError(const int code, std::string message, std::optional<boost::json::value> data = std::nullopt) :
            awl::GeneralException(std::move(message)), _code(code), _data(std::move(data))
        {}

        RpcError(const int code, std::wstring message, std::optional<boost::json::value> data = std::nullopt) :
            awl::GeneralException(std::move(message)), _code(code), _data(std::move(data))
        {}

        int code() const { return _code; }

        const std::optional<boost::json::value>& data() const { return _data; }

    private:

        int _code;
        std::optional<boost::json::value> _data;
    };
}
