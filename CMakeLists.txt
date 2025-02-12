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

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING liburing)

if (LIBURING_FOUND)
    message(STATUS "Using system-installed liburing")
    add_library(liburing INTERFACE)
    target_include_directories(liburing INTERFACE ${LIBURING_INCLUDE_DIRS})
    target_link_libraries(liburing INTERFACE ${LIBURING_LIBRARIES})
else()
    message(STATUS "Downloading and building liburing from source")
    include(ExternalProject)
    ExternalProject_Add(
            liburing_git
            GIT_REPOSITORY https://github.com/axboe/liburing.git
            GIT_TAG liburing-2.8
            GIT_PROGRESS 1
            CONFIGURE_COMMAND ./configure
            BUILD_COMMAND make
            BUILD_IN_SOURCE 1
            BUILD_BYPRODUCTS "<SOURCE_DIR>/src/liburing.a"
            INSTALL_COMMAND ""
            TEST_COMMAND ""
            UPDATE_DISCONNECTED 1
    )

    ExternalProject_Get_Property(liburing_git SOURCE_DIR)

    add_library(liburing INTERFACE)
    add_dependencies(liburing liburing_git)
    target_include_directories(liburing INTERFACE ${SOURCE_DIR}/src/include)
    target_link_libraries(liburing INTERFACE "${SOURCE_DIR}/src/liburing.a")
endif()


add_executable(server server.cpp)
target_link_libraries(server PRIVATE liburing)
target_compile_definitions(server PRIVATE ASIO_DISABLE_BOOST ASIO_DISABLE_EPOLL=1, ASIO_HAS_IO_URING=1)
target_include_directories(server PRIVATE ${asio_SOURCE_DIR}/asio/include)


add_executable(client client.cpp)
target_link_libraries(client PRIVATE liburing)
target_compile_definitions(client PRIVATE ASIO_DISABLE_BOOST ASIO_DISABLE_EPOLL=1, ASIO_HAS_IO_URING=1)
target_include_directories(client PRIVATE ${asio_SOURCE_DIR}/asio/include)
