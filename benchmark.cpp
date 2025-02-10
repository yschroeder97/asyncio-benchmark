#include <cstdint>
#include <iostream>
#include <syncstream>
#include <vector>

#include <boost/system/error_code.hpp>
#include <asio.hpp>
#include <numeric>
#include <thread>

using asio::ip::tcp;
constexpr std::string host = "127.0.0.1";
constexpr uint16_t port = 12345;
constexpr size_t FOUR_KiB = 4 * 1024;

auto info() { return std::osyncstream(std::cout); }

auto debug() { return std::osyncstream(std::cerr); }

asio::awaitable<void> send(tcp::socket socket) {
    std::vector buf(FOUR_KiB, 'A');
    while (true) {
        [[maybe_unused]] auto [ec, n] =
                co_await async_write(socket, asio::const_buffer(buf.data(), buf.size()), asio::as_tuple);

        if (ec) {
            socket.shutdown(tcp::socket::shutdown_both, ec);
            info() << "shutdown: " << ec.message() << std::endl;
            break;
        }
        // info() << "send: " << ec.message() << std::endl;
    }
}

asio::awaitable<void> async_client(const bool &stopped, uint64_t &bytesRead) try {
    tcp::socket socket{co_await asio::this_coro::executor};

    co_await socket.async_connect({{}, port});

    std::vector<char> buf(FOUR_KiB);
    while (!stopped) {
        auto [ec, n] = co_await async_read(
            socket,
            asio::mutable_buffer(buf.data(), buf.size()),
            asio::as_tuple);
        bytesRead += n;
    }
} catch (asio::system_error const &se) {
    debug() << "read:" << se.code().message() << std::endl;
}

void blocking_client(const bool &stopped, uint64_t &bytesRead) {
    int sockfd = -1;
    int connection = -1;
    addrinfo hints{};
    addrinfo *result = nullptr;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = 0; /// use default behavior
    hints.ai_protocol = 0;
    /// specifying 0 in this field indicates that socket addresses with any protocol can be returned by getaddrinfo();

    if (const auto errorCode = getaddrinfo(host.c_str(),
                                           std::to_string(port).c_str(),
                                           &hints,
                                           &result); errorCode != 0) {
        debug() << "Failed getaddrinfo with error: " << gai_strerror(errorCode) << std::endl;
        return;
    }

    /// Try each address until we successfully connect
    while (result != nullptr) {
        sockfd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
        if (sockfd == -1) {
            result = result->ai_next;
            continue;
        }

        constexpr static timeval Timeout{0, 100'000};
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &Timeout, sizeof(Timeout));
        connection = connect(sockfd, result->ai_addr, result->ai_addrlen);

        if (connection != -1) {
            break; /// success
        }
    }
    freeaddrinfo(result);

    if (result == nullptr) {
        info() << "Could not connect to: " << host << ":" << port << std::endl;
    }

    std::vector<char> buf(FOUR_KiB);
    while (!stopped) {
        size_t numReceivedBytes = 0;
        while (numReceivedBytes < FOUR_KiB) {
            const ssize_t bufferSizeReceived = read(sockfd, buf.data() + numReceivedBytes,
                                                    FOUR_KiB - numReceivedBytes);
            numReceivedBytes += bufferSizeReceived;
            if (bufferSizeReceived == -1) {
                /// if read method returned -1 an error occurred during read.
                info() << "An error occurred while reading from socket. Error: " << strerror(errno);
                break;
            }
            if (bufferSizeReceived == 0) {
                info() << "No data received from " << host << ":" << port;
            }
        }
        bytesRead += numReceivedBytes;
    }

    if (connection >= 0) {
        shutdown(connection, SHUT_RDWR);
        close(sockfd);
    }
}

asio::awaitable<void> listener(const int num_connections) try {
    const auto executor = co_await asio::this_coro::executor;
    tcp::acceptor acceptor{executor, {{}, port}};
    for (int i = 0; i < num_connections; ++i) {
        co_spawn(executor, send(co_await acceptor.async_accept()), asio::detached);
    }
} catch (asio::system_error const &se) {
    debug() << "listener: " << se.code().message() << std::endl;
}

int main(const int argc, char **argv) {
    asio::thread_pool ioc;
    const int numConnections = std::stoi(argv[2]);
    std::vector<uint64_t> bytesRead(numConnections, 0);

    if (const std::string executable = argv[1]; executable == "s") {
        co_spawn(ioc, listener(numConnections), asio::detached);
    } else {
        const int duration = std::stoi(argv[3]);
        bool stopped = false;

        if (executable == "ac") {
            for (int i = 0; i < numConnections; ++i) {
                co_spawn(ioc, async_client(stopped, bytesRead.at(i)), asio::detached);
            }
            std::this_thread::sleep_for(std::chrono::seconds(duration));
            stopped = true;
        } else if (executable == "bc") {
            std::vector<std::jthread> threads;
            threads.reserve(numConnections);
            for (int i = 0; i < numConnections; ++i) {
                threads.emplace_back(blocking_client, std::ref(stopped), std::ref(bytesRead.at(i)));
            }

            std::this_thread::sleep_for(std::chrono::seconds(duration));
            stopped = true;
        }
        info() << "Total bytes read: " << std::accumulate(bytesRead.begin(), bytesRead.end(), 0ULL) << std::endl;
    }
    ioc.join();
}
