#pragma once

#include "Server/RequestContext.h"

#include <string>
#include <variant>
#include <vector>

namespace mcp
{
    struct ResourceDefinition
    {
        std::string uri;
        std::string name;
        std::string description;
        std::string mimeType;
    };

    struct TextResource
    {
        std::string text;
    };

    struct BlobResource
    {
        std::string base64;
    };

    struct ResourceContents
    {
        std::string uri;
        std::string mimeType;
        std::variant<TextResource, BlobResource> contents;
    };

    class IResource
    {
    public:

        virtual ~IResource() = default;

        // Server snapshots the definition at registration.
        virtual const ResourceDefinition& definition() const = 0;

        virtual boost::asio::awaitable<std::vector<ResourceContents>> asyncRead(RequestContext context) = 0;
    };
}
