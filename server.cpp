#ifdef NDEBUG
#define SEHE_TWEAKS
#endif

// #define ASIO_ENABLE_HANDLER_TRACKING 1
#define ASIO_NO_DEPRECATED 1
#include <asio.hpp>
#include <iostream>
#include <syncstream>

using asio::ip::tcp;
using namespace std::chrono_literals;

constexpr uint16_t port = 12345;
constexpr size_t FOUR_KiB = 4 * 1024;
static std::atomic_uint64_t g_sent = 0;
static std::atomic_bool g_session_shutdown = false;
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

namespace Server {
    asio::awaitable<void, Executor> session(tcp::socket socket) try {
        asio::steady_timer timer{co_await asio::this_coro::executor, 10ms};
        inflog() << "Connected " << socket.remote_endpoint() << std::endl;
        ++num_connections_established;
        while (num_connections_established < num_connections_total) {
            co_await timer.async_wait(asio::use_awaitable_t<Executor>{});
        }

        for (static std::vector const payload(FOUR_KiB, 'A'); !g_session_shutdown;) {
            auto [ec, n] = co_await async_write(socket, asio::buffer(payload), asio::as_tuple);
            g_sent += n;

            if (ec) {
                socket.shutdown(tcp::socket::shutdown_both, ec);
                if (ec)
                    errlog() << "shutdown: " << ec.message() << std::endl;
                break;
            }
        }
    } catch (asio::system_error const &se) {
        errlog() << se.code().message() << std::endl;
    }

    asio::awaitable<void, Executor> listener() try {
        const auto ex = co_await asio::this_coro::executor;

        for (tcp::acceptor acceptor{ex, {{}, port}};;)
            co_spawn(ex, session(co_await acceptor.async_accept()), asio::detached);
    } catch (asio::system_error const &se) {
        errlog() << se.code().message() << std::endl;
    }
} // namespace Server

using namespace std::literals;

void stats(const int elapsed_seconds) {
    errlog() << " -- " << std::endl;
    if (g_sent)
        errlog() << "Total bytes sent: " << g_sent << " " //
                << (g_sent / 1024.0 / 1024.0 / 1024.0 / elapsed_seconds) << " GiB/s" //
                << std::endl;
}

int main(int /*argc*/, char **argv) {
    auto const duration = std::max(1, std::stoi(argv[2]));
    asio::cancellation_signal stop;

    auto stoppable = asio::bind_cancellation_slot(stop.slot(), asio::detached);
    num_connections_total = std::stoi(argv[1]);

    asio::thread_pool server_ctx(1);
    const Executor ex = server_ctx.get_executor();
    co_spawn(ex, Server::listener(), stoppable);

    while (num_connections_established < num_connections_total) {
        std::this_thread::sleep_for(10ms);
    }

    std::this_thread::sleep_for(1s * duration);

    stop.emit(asio::cancellation_type::all);
    g_session_shutdown = true;

    server_ctx.join(); // allow asio operations to clean up
}
