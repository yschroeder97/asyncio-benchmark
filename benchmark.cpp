#include <cstdint>
#include <iostream>
#include <syncstream>
#include <vector>

#include <asio.hpp>
#include <numeric>
#include <thread>

using asio::ip::tcp;
using namespace std::chrono_literals;

constexpr std::string SERVER_HOST = "127.0.0.1";
constexpr uint16_t SERVER_PORT = 12345;
constexpr size_t FOUR_KiB = 4 * 1024;

std::atomic<int> num_connected = 0;
std::atomic_bool shutdown_signal = false;

auto info() { return std::osyncstream(std::cout); }
auto debug() { return std::osyncstream(std::cerr); }

asio::awaitable<void> send(tcp::socket socket) {
    // info() << "Client IP/PORT: " << socket.remote_endpoint().address() << ":" << socket.remote_endpoint().port()
    //         << std::endl;
    const std::vector buf(FOUR_KiB, 'A');

    while (true) {
        [[maybe_unused]] auto [ec, n] =
                co_await async_write(socket, asio::const_buffer(buf.data(), buf.size()), asio::as_tuple);

        if (ec) {
            socket.shutdown(tcp::socket::shutdown_both, ec);
            // info() << "shutdown: " << ec.message() << std::endl;
            break;
        }
        // info() << "send: " << ec.message() << std::endl;
    }
}

asio::awaitable<void> async_client(const tcp::endpoint client_endpoint, const int num_connections, uint64_t &bytesRead) try {
    const auto executor = co_await asio::this_coro::executor;
    asio::steady_timer timer{executor};
    tcp::socket socket{executor};
    socket.open(tcp::v4());
    socket.bind(client_endpoint);

    co_await socket.async_connect({asio::ip::address_v4::from_string(SERVER_HOST), SERVER_PORT});
    ++num_connected;
    while (num_connected.load() < num_connections) {
        timer.expires_from_now(10ms);
        co_await timer.async_wait(asio::use_awaitable);
    }

    std::vector<char> buf(FOUR_KiB);
    while (!shutdown_signal.load()) {
        auto [ec, n] = co_await async_read(
            socket,
            asio::mutable_buffer(buf.data(), buf.size()),
            asio::as_tuple);
        bytesRead += n;
    }

    asio::error_code ec;
    socket.shutdown(tcp::socket::shutdown_both, ec);
    if (ec) {
        debug() << "shutdown: " << ec.message() << std::endl;
    }
    socket.close();
} catch (asio::system_error const &se) {
    debug() << "async_client: " << se.code().message() << std::endl;
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

    if (const auto errorCode = getaddrinfo(SERVER_HOST.c_str(),
                                           std::to_string(SERVER_PORT).c_str(),
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
        info() << "Could not connect to: " << SERVER_HOST << ":" << SERVER_PORT << std::endl;
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
                // info() << "An error occurred while reading from socket. Error: " << strerror(errno);
                break;
            }
            if (bufferSizeReceived == 0) {
                info() << "No data received from " << SERVER_HOST << ":" << SERVER_PORT;
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
    tcp::acceptor acceptor{executor, {asio::ip::address_v4::from_string("127.0.0.1"), SERVER_PORT}};
    for (int i = 0; i < num_connections; ++i) {
        auto socket = co_await acceptor.async_accept();
        co_spawn(executor, send(std::move(socket)), asio::detached);
    }
} catch (asio::system_error const &se) {
    debug() << "listener: " << se.code().message() << std::endl;
}

int main(const int argc, char **argv) {
    asio::thread_pool ioc;
    const int num_connections = std::stoi(argv[2]);
    std::vector<uint64_t> bytesRead(num_connections, 0);

    if (const std::string executable = argv[1]; executable == "s") {
        co_spawn(ioc, listener(num_connections), asio::detached);
    } else {
        bool stopped = false;
        const int duration = std::stoi(argv[3]);

        if (executable == "ac") {
            int ip_suffix = 2;
            for (int i = 0; i < num_connections; ++i) {
                tcp::endpoint client_endpoint{
                    asio::ip::address_v4::from_string(std::format("127.0.0.{}", ip_suffix)),
                    static_cast<uint16_t>(i + 10000 % UINT16_MAX)
                };
                co_spawn(ioc, async_client(std::move(client_endpoint), num_connections, bytesRead.at(i)),
                         asio::detached);
                if (i % UINT16_MAX == 0) {
                    ip_suffix++;
                }
            }

            while (num_connected < num_connections) {
                std::this_thread::sleep_for(10ms);
            }
            info() << "All clients connected, waiting for " << duration << "s..." << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(duration));
            shutdown_signal.store(true);
        } else if (executable == "bc") {
            std::vector<std::jthread> threads;
            threads.reserve(num_connections);
            for (int i = 0; i < num_connections; ++i) {
                threads.emplace_back(blocking_client, std::ref(stopped), std::ref(bytesRead.at(i)));
            }

            std::this_thread::sleep_for(std::chrono::seconds(duration));
            stopped = true;
        }
        info() << "Total bytes read: " << std::accumulate(bytesRead.begin(), bytesRead.end(), 0ULL) << std::endl;
    }
    ioc.join();
}
