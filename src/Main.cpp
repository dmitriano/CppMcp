#include "Awl/StringFormat.h"

#include <boost/beast/core.hpp>
#include <boost/beast/version.hpp>
#include <iostream>

int main()
{
    std::cout << std::format("{} ({})\n", awl::text("CppMcp"), BOOST_BEAST_VERSION_STRING);
    return 0;
}
