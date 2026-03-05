#include <pubsub_itc_fw/TimerHandler.hpp>
#include <pubsub_itc_fw/Reactor.hpp>

namespace pubsub_itc_fw {

TimerHandler::~TimerHandler() {
    if (fd_ != -1) {
        ::close(fd_);
    }
}

TimerHandler::TimerHandler(const Timer& timer, Reactor& reactor) : timer_(timer), reactor_(reactor) {
    fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd_ == -1) {
        throw std::runtime_error("timerfd_create failed"); // TODO our exception type?
    }

    // Convert microseconds → nanoseconds for timerfd
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timer.get_interval());

    itimerspec spec{};
    spec.it_value.tv_sec  = ns.count() / 1'000'000'000;
    spec.it_value.tv_nsec = ns.count() % 1'000'000'000;

    if (timer.get_type() == TimerType::Recurring) {
        spec.it_interval = spec.it_value;
    }

    if (::timerfd_settime(fd_, 0, &spec, nullptr) == -1) {
        ::close(fd_);
        fd_ = -1;
        throw std::runtime_error("timerfd_settime failed");
    }

// create timer handler allocate fd
}

bool TimerHandler::handle_event(uint32_t events) noexcept {
    std::cerr << fmt::format("{}:{} handle_event\n", __FILE__, __LINE__);
    // 1. Check if the event is actually a 'read' event
    if (!(events & EPOLLIN)) {
    std::cerr << fmt::format("{}:{} handle_event not a read event\n", __FILE__, __LINE__);
        return false;
    }

    // 2. Acknowledge the timerfd (Crucial!)
    // The kernel requires you to read 8 bytes from the timerfd to
    // reset it. If you don't, epoll will fire again immediately.
    uint64_t expirations = 0;
    ssize_t s = ::read(fd_, &expirations, sizeof(expirations));

    if (s != sizeof(expirations)) {
        // This could happen if the FD was set to non-blocking and
        // there was nothing to read, or if the read was interrupted.
    std::cerr << fmt::format("{}:{} handle_event nothing to read\n", __FILE__, __LINE__);
        return false;
    }

    // 3. Dispatch based on the Timer metadata
    // Check if this is the Reactor's own backstop/housekeeping timer
    if (timer_.get_owner_thread_id().get_value() == 0) {
        // This wakes up the Reactor to check for shutdown or perform internal maintenance.
    std::cerr << fmt::format("{}:{} handle_event calling housekeeping function\n", __FILE__, __LINE__);
        reactor_.on_housekeeping_tick();
    } else {
        // This is an application-level timer.
        // Wrap the TimerID in an EventMessage and push it to the owner's queue.
        for (uint64_t i = 0; i < expirations; ++i) {
    std::cerr << fmt::format("{}:{} handle_event will route message\n", __FILE__, __LINE__);
            auto msg = EventMessage::create_timer_event(timer_.get_timer_id());
            reactor_.route_message(timer_.get_owner_thread_id(), std::move(msg));
        }
    }

    return true;
}

} // namespaces
