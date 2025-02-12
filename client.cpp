#ifdef NDEBUG
#define SEHE_TWEAKS
#endif

// #define ASIO_ENABLE_HANDLER_TRACKING 1
#define ASIO_NO_DEPRECATED 1
#define ASIO_HAS_IO_URING 1
#include <asio.hpp>
#include <iostream>
#include <syncstream>

using asio::ip::tcp;
using namespace std::chrono_literals;

constexpr std::string host = "127.0.0.1";
constexpr uint16_t port = 12345;
constexpr size_t FOUR_KiB = 4 * 1024;
static std::atomic_uint64_t g_recvd = 0;
static std::atomic_int32_t num_connections_established = 0;
int num_connections_total = 0;

static auto _cerr() { return std::osyncstream(std::cerr); }
#define errlog() _cerr() << "E " << __FUNCTION__ << ":" << __LINE__ << " "

#ifndef SEHE_TWEAKS
using Executor = asio::any_io_executor;
auto _cout() { return std::osyncstream(std::cout); }
    #define inflog() _cout() << "I " << __FUNCTION__ << ":" << __LINE__ << " "
#else // release
using Executor = asio::thread_pool::executor_type;

// static thread_local std::ostream s_nullstream{nullptr};
struct {
    template<typename T>
    constexpr auto &operator<<(T const &) const { return *this; }

    constexpr auto &operator<<(std::ostream & (*)(std::ostream &)) const { return *this; }
} static constexpr s_nullstream;

static auto &inflog() { return s_nullstream; }
#endif

namespace Client {
    asio::awaitable<void, Executor> asio(std::atomic_uint64_t &bytesRead) try {
        const Executor ex = co_await asio::this_coro::executor;
        tcp::socket socket{ex};
        asio::steady_timer timer{ex, 10ms};

        tcp::resolver res{ex};
        co_await async_connect(socket, res.resolve(host, std::to_string(port)));
        ++num_connections_established;

        while (num_connections_established < num_connections_total) {
            co_await timer.async_wait(asio::use_awaitable_t<Executor>{});
        }

        for (std::vector<char> buf(FOUR_KiB);;) {
            bytesRead += co_await async_read(socket, asio::mutable_buffer(buf.data(), buf.size()));
        }
    } catch (asio::system_error const &se) {
        if (se.code() != asio::error::eof)
            errlog() << se.code().message() << std::endl;
        else
            inflog() << "EOF from " << host << ":" << port << std::endl;
    }

    static int blocking_connect() {
        int sockfd = -1;
        addrinfo hints{};

        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_flags = 0; /// use default behavior
        hints.ai_protocol = 0;
        /// specifying 0 in this field indicates that socket addresses with any protocol can be
        /// returned by ::getaddrinfo();

        addrinfo *result = nullptr;
        if (auto rc = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result); rc != 0) {
            errlog() << "Failed getaddrinfo with error: " << ::gai_strerror(rc) << std::endl;
            return false;
        }

        /// Try each address until we successfully connect
        for (auto rp = result; rp != nullptr; rp = rp->ai_next) {
            sockfd = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (sockfd == -1) {
                rp = rp->ai_next;
                continue;
            }

            if (::connect(sockfd, rp->ai_addr, rp->ai_addrlen) != -1)
                break; /// success
            ::close(sockfd);
            sockfd = -1;
        }
        ::freeaddrinfo(result);

        return sockfd;
    }

    void blocking(const std::stop_token &stopped, std::atomic_uint64_t &bytesRead) {
        int sockfd = blocking_connect();
        if (sockfd == -1) {
            errlog() << "Could not connect to: " << host << ":" << port << std::endl;
            return;
        }
        ++num_connections_established;
        while (num_connections_established < num_connections_total) {
            std::this_thread::sleep_for(20ms);
        }

        std::string buf(FOUR_KiB, '\0');

        while (!stopped.stop_requested()) {
            if (const auto n = ::read(sockfd, buf.data(), buf.size()); n > 0) {
                bytesRead += n;
            } else {
                if (!n) inflog() << "EOF from " << host << ":" << port << std::endl;
                else
                    errlog() << "Error reading from socket: " << strerror(errno) << std::endl;
                break;
            }
        }

        ::shutdown(sockfd, SHUT_RDWR);
        ::close(sockfd);
    }
};

#include <ranges> // split
#include <set>
#include <thread> // jthread
using namespace std::literals;

void stats(const int elapsed_seconds) {
    errlog() << " -- " << std::endl;
    if (g_recvd)
        errlog() << "Total bytes read: " << g_recvd << " " //
                << (g_recvd / 1024.0 / 1024.0 / 1024.0 / elapsed_seconds) << " GiB/s" //
                << std::endl;
}

static inline auto now() { return std::chrono::steady_clock::now(); }

int main(int /*argc*/, char **argv) {
    const auto selection = [&] {
        auto rr = std::views::split(std::string_view(argv[1]), ',');
        return std::set<std::string_view>(rr.begin(), rr.end());
    }();
    auto const duration = std::max(1, std::stoi(argv[3]));

    const size_t njobs = std::stoi(argv[2]);
    num_connections_total = njobs;

    const auto start = now();

    if (selection.contains("asio")) {
        asio::thread_pool client_ctx{1};
        for (size_t i = 0; i < njobs; ++i)
            co_spawn(client_ctx, Client::asio(g_recvd), asio::detached);

        errlog() << "Waiting for " << njobs << " connections" << std::endl;
        while (num_connections_established < num_connections_total) {
            std::this_thread::sleep_for(100ms);
        }
        const auto start_send_time = now();
        errlog() << "All connections established" << std::endl;
        client_ctx.join();

        errlog() << "Total duration: " << (now() - start) / 1.s << "s "
                << "(startup took " << (start_send_time - start) / 1ms
                << "ms)" << std::endl;

        stats(duration);
    }

    if (selection.contains("blocking")) {
        std::vector<std::jthread> threads;
        generate_n(back_inserter(threads), njobs,
                   [&] { return std::jthread(Client::blocking, std::ref(g_recvd)); });

        errlog() << "Waiting for " << njobs << " connections" << std::endl;
        while (num_connections_established < num_connections_total) {
            std::this_thread::sleep_for(100ms);
        }
        const auto start_send_time = now();
        errlog() << "All connections established" << std::endl;
        for (auto &t : threads)
            t.join();

        errlog() << "Total duration: " << (now() - start) / 1.s << "s "
                << "(startup took " << (start_send_time - start) / 1ms
                << "ms)" << std::endl;

        stats(duration);
    }
}
