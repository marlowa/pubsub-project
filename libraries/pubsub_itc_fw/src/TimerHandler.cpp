#include <cstdint>

#include <unistd.h> // for close

#ifdef CLANG_TIDY
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif

#include <sys/timerfd.h>

#include <chrono>
#include <utility>

#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/Timer.hpp>
#include <pubsub_itc_fw/TimerType.hpp>
#include <pubsub_itc_fw/TimerHandler.hpp>
#include <pubsub_itc_fw/ThreadLifecycleState.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>
#include <pubsub_itc_fw/PubSubItcException.hpp>

namespace pubsub_itc_fw {

TimerHandler::~TimerHandler() {
    if (fd_ != -1) {
        ::close(fd_);
    }
}

TimerHandler::TimerHandler(const Timer& timer, Reactor& reactor) : timer_(timer), reactor_(reactor), owner_thread_(nullptr) {
    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    fd_ = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd_ == -1) {
        PUBSUB_LOG_STR(reactor_.get_logger(), LogLevel::Error, "TimerHandler ctor: timerfd_create failed");
        throw PubSubItcException("timerfd_create failed");
    }

    // Resolve the owner ApplicationThread* from the reactor's fast-path map
    const ThreadID owner_id = timer_.get_owner_thread_id();

    // Special case: reactor backstop timer (owner thread ID == 0)
    if (owner_id.get_value() == 0) {
        PUBSUB_LOG(reactor_.get_logger(), LogLevel::Info, "TimerHandler ctor: created backstop timerfd {} for timer '{}'", fd_, timer_.get_name());
    } else {
        owner_thread_ = reactor_.get_fast_path_thread(owner_id);
        if (owner_thread_ == nullptr) {
            PUBSUB_LOG(reactor_.get_logger(), LogLevel::Error,
                       "TimerHandler ctor: owner thread {} not found in fast-path map "
                       "for timer '{}'",
                       owner_id.get_value(), timer_.get_name());
            // This should never happen unless construction order is wrong
            throw PubSubItcException("TimerHandler: owner thread not found");
        }
    }

    // Convert microseconds → nanoseconds for timerfd
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(timer.get_interval());

    itimerspec spec{};
    spec.it_value.tv_sec = ns.count() / 1'000'000'000;
    spec.it_value.tv_nsec = ns.count() % 1'000'000'000;

    if (timer.get_type() == TimerType::Recurring) {
        spec.it_interval = spec.it_value;
    }

    if (::timerfd_settime(fd_, 0, &spec, nullptr) == -1) {
        ::close(fd_);
        fd_ = -1;
        throw PubSubItcException("timerfd_settime failed");
    }
}

bool TimerHandler::handle_event(uint32_t events) {
    PUBSUB_LOG_STR(reactor_.get_logger(), LogLevel::Info, "TimerHandle::handle_event");

    // 1. Must be a read event
    if (!(events & EPOLLIN)) {
        PUBSUB_LOG_STR(reactor_.get_logger(), LogLevel::Error, "handle_event not a read event");
        return false;
    }

    // 2. Drain the timerfd once (non-blocking)
    uint64_t expirations = 0;
    const ssize_t s = ::read(fd_, &expirations, sizeof(expirations));

    if (s != sizeof(expirations)) {
        // Nothing to read (EAGAIN) or interrupted — treat as consumed
        PUBSUB_LOG(reactor_.get_logger(), LogLevel::Info, "handle_event nothing to read (s={}, errno={})", s, errno);
        return true;
    }

    // 3. Reactor-owned housekeeping timer (owner thread ID == 0)
    if (timer_.get_owner_thread_id().get_value() == 0) {
        PUBSUB_LOG_STR(reactor_.get_logger(), LogLevel::Info, "handle_event calling housekeeping function");
        reactor_.on_housekeeping_tick();
        return true;
    }

    if (!reactor_.is_running()) {
        PUBSUB_LOG(reactor_.get_logger(), LogLevel::Info,
                   "TimerHandler::handle_event: reactor is no longer running; dropping {} pending expirations for timer '{}'",
                   expirations, timer_.get_name());
        return true;
    }

    // 4. Application-owned timer
    const ThreadID owner = timer_.get_owner_thread_id();
    auto state = reactor_.get_thread_state(owner);

    // ---- RACE-AVOIDANCE GUARD ----
    // If the owner thread is shutting down or not operational,
    // we drop all pending expirations at once.
    if (state != ThreadLifecycleState::Operational) {
        PUBSUB_LOG(reactor_.get_logger(), LogLevel::Warning,
                   "TimerHandler: dropping {} pending expirations for timer '{}' "
                   "because owner thread {} is in state {}",
                   expirations, timer_.get_name(), owner.get_value(), ThreadLifecycleState::to_string(state));
        return true;
    }
    // ---- END RACE-AVOIDANCE GUARD ----

    // 5. Deliver one event, coalescing multiple expirations (only when Operational)
    if (expirations == 1) {
        PUBSUB_LOG(reactor_.get_logger(), LogLevel::Info, "handle_event single expiration event, sending single timer event {} {}", timer_.get_name(),
                   timer_.get_timer_id().get_value());
    } else {
        PUBSUB_LOG(reactor_.get_logger(), LogLevel::Info, "handle_event coalescing {} expiration events into single timer event {} {}", expirations,
                   timer_.get_name(), timer_.get_timer_id().get_value());
    }

    auto msg = EventMessage::create_timer_event(timer_.get_timer_id());

    // TODO Optional future refinement:
    // if (!reactor_.route_message(owner, std::move(msg))) {
    //     PUBSUB_LOG(reactor_.get_logger(), LogLevel::Warning,
    //                "TimerHandler: route_message dropped event {} "
    //                "for timer '{}'; stopping expiration loop",
    //                i,
    //                timer_.get_name());
    //     break;
    // }

    reactor_.route_message(owner, std::move(msg));

    return true;
}

} // namespace pubsub_itc_fw
