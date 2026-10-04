# CppMcp

C++23 MCP server project using AWL, Boost.Asio and Boost.Beast, without Qt.
The current executable is a build bootstrap that prints the project and Beast version;
the MCP protocol and server transport will be implemented later.

AWL is a submodule in `lib/Awl`, pinned to the same revision as the reference
`tradeclient` project. Boost and OpenSSL are discovered by AWL's CMake scripts,
with the static runtime and static Boost libraries used by `tradeclient`.

Reference development guidelines: [AGENTS.md](../tradeclient/AGENTS.md) in the `tradeclient` project.

## Windows build

Initialize the bundled AWL checkout:

```powershell
git submodule update --init --recursive
```

Configure and build outside the source directory (adapt the paths to your machine):

```powershell
$cmake = "C:/dev/tools/cmake-4.4.3-windows-x86_64/bin/cmake.exe"

& $cmake -S C:/dev/repos/CppMcp -B C:/dev/build/cppmcp `
    -G "Visual Studio 18 2026" -A x64 `
    -DCMAKE_PREFIX_PATH=C:/dev/libs/boost_1_89_0 `
    -DOPENSSL_ROOT_DIR=C:/dev/libs/OpenSSL `
    -DOPENSSL_USE_STATIC_LIBS=ON

& $cmake --build C:/dev/build/cppmcp --target CppMcp --config RelWithDebInfo
& C:/dev/build/cppmcp/RelWithDebInfo/CppMcp.exe
```

## External AWL checkout

`AWL_ROOT_DIR` is a CMake `CACHE PATH` parameter, as in `SQLiteWrapper`.
It defaults to `lib/Awl` in this repository. To use an external checkout, add this
argument to the configure command:

```powershell
-DAWL_ROOT_DIR=C:/dev/repos/tradeclient/lib/Awl
```

The path must contain `CMake/AwlConfig.cmake` and `CMake/AwlLink.cmake`.
An external checkout does not require initializing the bundled AWL submodule.
Use `-DAWL_ROOT_DIR=...` again to change the path in an existing build directory.
