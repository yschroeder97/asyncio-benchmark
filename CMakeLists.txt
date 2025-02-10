cmake_minimum_required(VERSION 3.29)
project(asyncio_benchmark)

set(CMAKE_CXX_STANDARD 20)

include(FetchContent)
FetchContent_Declare(
        asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG        asio-1-32-0
)

FetchContent_MakeAvailable(asio)

add_executable(asyncio_benchmark benchmark.cpp)
target_compile_definitions(asyncio_benchmark PRIVATE ASIO_DISABLE_BOOST)

target_include_directories(asyncio_benchmark PRIVATE ${asio_SOURCE_DIR}/asio/include)
