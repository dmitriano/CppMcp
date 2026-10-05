#pragma once

#include "Common/JsonSchema.h"
#include "Server/ITool.h"

#include "BoostExtras/Json/JsonUtil.h"

#include "Awl/ILogger.h"

#include <concepts>
#include <format>
#include <utility>

namespace mcp
{
    // Root types are reflected objects; input must be default constructible.
    // Implement asyncExecute() and await all child work during the invocation.
    template <awl::reflectable Input, awl::reflectable Output>
    class TypedTool : public ITool
    {
        static_assert(std::default_initializable<Input>, "TypedTool input must be default constructible.");

    public:

        // Logger is non-null; supply a child logger for this tool at the call site.
        TypedTool(std::string name, std::string description, const std::shared_ptr<awl::ILogger>& logger) :
            _definition{std::move(name), std::move(description), makeJsonSchema<Input>(), makeJsonSchema<Output>()},
            _logger(logger)
        {}

        const ToolDefinition& definition() const final { return _definition; }

        boost::asio::awaitable<ToolResult> asyncCall(
            boost::json::object arguments, RequestContext context) final
        {
            Input input{};
            boost::json::value input_json(std::move(arguments));
            std::optional<std::string> input_error;
            try
            {
                validateJson<Input>(input_json);
                detail::normalizeJsonIntegers<Input>(input_json);
                awl::fromJson(input_json, input);
            }
            catch (const awl::JsonException& error)
            {
                _logger->warning(_T("Invalid tool arguments: {}"), error.message());
                input_error = std::format("Invalid arguments for tool '{}': {}", _definition.name, error.what());
            }

            if (input_error)
            {
                co_return ToolResult{boost::json::array{boost::json::object{
                    {"type", "text"}, {"text", std::move(*input_error)}}}, std::nullopt, true};
            }

            // Execution and output errors propagate to Server's tool error handling.
            // In particular, cancellation must never become an isError response.
            const Output output = co_await asyncExecute(std::move(input), std::move(context));
            boost::json::value output_json = awl::toJson(output);
            validateJson<Output>(output_json);
            const std::string text = boost::json::serialize(output_json);
            co_return ToolResult{boost::json::array{boost::json::object{{"type", "text"}, {"text", text}}},
                std::move(output_json), false};
        }

    protected:

        virtual boost::asio::awaitable<Output> asyncExecute(Input input, RequestContext context) = 0;

        const std::shared_ptr<awl::ILogger>& logger() const { return _logger; }

    private:

        ToolDefinition _definition;
        const std::shared_ptr<awl::ILogger> _logger;
    };
}
