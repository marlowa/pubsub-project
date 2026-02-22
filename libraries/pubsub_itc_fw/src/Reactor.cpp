#include <pubsub_itc_fw/Reactor.hpp>

#include <pubsub_itc_fw/Backoff.hpp>
#include <pubsub_itc_fw/LoggingMacros.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>

namespace pubsub_itc_fw {

Reactor::Reactor(const ReactorConfiguration& config, QuillLogger& logger) :
      handlers_{}
    , threads_{}
    , threads_by_thread_id_{}
    , config_(config)
    , logger_(logger)
{
}

void Reactor::route_message(ThreadID target_id, EventMessage message) {
     // Reject attempt to route event message before reactor initialization is complete
    if (!initialization_complete_.load(std::memory_order_acquire)) {
        throw PreconditionAssertion(fmt::format("message posted before Reactor initialization completed, event type {}",
                                                message.type().as_string()), __FILE__, __LINE__);
    }

    // Lookup target thread
    auto it = threads_by_thread_id_.find(target_id);

    if (it == threads_by_thread_id_.end()) {
        // Unknown target: drop safely
        return;
    }

    auto* target = it->second.get();
    // If target is finished, drop safely
    if (!target->is_running()) {
        return;
    }

    auto tag = message.type().as_tag();
    const bool is_reactor_event = tag == EventType::Initial || tag == EventType::AppReady;
    const auto lifecycle_state = target->get_lifecycle_state().as_tag();
    if (!is_reactor_event && lifecycle_state != ThreadLifecycleState::Operational) {
        if (lifecycle_state == ThreadLifecycleState::ShuttingDown || lifecycle_state == ThreadLifecycleState::Terminated) {
            return;
        }
        throw PreconditionAssertion(fmt::format("Attempted to route non-reactor event {} to non-operational thread", message.type().as_string()),
                                    __FILE__, __LINE__);
    }

    // Lookup origin thread
    ThreadID origin_id = message.originating_thread_id();
    auto origin_it = threads_by_thread_id_.find(origin_id);

    // If origin is unknown or finished, drop safely
    if (origin_it == threads_by_thread_id_.end() ||
        !origin_it->second->is_running()) {
        return;
    }

    // Enqueue into target queue
    target->get_queue().enqueue(std::move(message));
}

int Reactor::run() {
    is_finished_.store(false, std::memory_order_release);
    shutdown_reason_ = "";

    // ---------------------------------------------------------------------
    // 1. Start all registered ApplicationThreads
    // ---------------------------------------------------------------------
    for (auto& [name, thread] : threads_) {
        thread->start();
    }

    // ---------------------------------------------------------------------
    // 2. Wait until all threads have entered their run loops
    // ---------------------------------------------------------------------
    for (auto& [name, thread] : threads_) {
        Backoff backoff;
        while (!thread->is_running()) {
            backoff.pause();
        }
    }

    // ---------------------------------------------------------------------
    // 3. Post Initial event to each thread
    // ---------------------------------------------------------------------
    for (auto& [name, thread] : threads_) {
        EventMessage init_msg =
            EventMessage::create_reactor_event(EventType(EventType::Initial));
        thread->post_message(thread->get_thread_id(), std::move(init_msg));
    }

    // ---------------------------------------------------------------------
    // 4. Wait until all threads have processed Initial
    // ---------------------------------------------------------------------
    for (auto& [name, thread] : threads_) {
        Backoff backoff;
        if (thread->get_lifecycle_state().as_tag() != ThreadLifecycleState::Started) {
            throw PreconditionAssertion("Reactor attempted to post Initial before thread reached Started", __FILE__, __LINE__);
        }

        while (thread->get_lifecycle_state().as_tag() < ThreadLifecycleState::InitialProcessed) {
            backoff.pause();
        }
    }

    // ---------------------------------------------------------------------
    // 5. Post AppReady event to each thread
    // ---------------------------------------------------------------------
    for (auto& [name, thread] : threads_) {
        EventMessage ready_msg = EventMessage::create_reactor_event(EventType(EventType::AppReady));
        if (thread->get_lifecycle_state().as_tag() != ThreadLifecycleState::InitialProcessed) {
            throw PreconditionAssertion("Reactor attempted to post AppReady before InitialProcessed", __FILE__, __LINE__);
        }

        thread->post_message(thread->get_thread_id(), std::move(ready_msg));
    }

    // 5.5 Wait until all threads are Operational
    // TODO a misbehaving thread in its early stages could make the reactor wait forever here.
    // I think there needs to be some kind of time limit.
    for (auto& [name, thread] : threads_) {
        Backoff backoff;
        while (thread->get_lifecycle_state().as_tag() < ThreadLifecycleState::Operational) {
            backoff.pause();
        }
    }

    // 5.6 Mark initialization complete
    initialization_complete_.store(true, std::memory_order_release);

    // ---------------------------------------------------------------------
    // 6. Main loop (placeholder until epoll/timers/sockets are added)
    // ---------------------------------------------------------------------
    {
        Backoff backoff;
        while (!is_finished_.load(std::memory_order_acquire)) {
            backoff.pause();
        }
    }

    return 0;
}

void Reactor::shutdown(const std::string& reason)
{
    is_finished_.store(true, std::memory_order_release);
    shutdown_reason_ = reason;
}

std::string Reactor::get_thread_name_from_id(ThreadID id) const
{
    auto it = threads_by_thread_id_.find(id);
    if (it == threads_by_thread_id_.end()) {
        throw PreconditionAssertion("Unknown ThreadID in get_thread_name_from_id",
                                    __FILE__, __LINE__);
    }
    return it->second->get_thread_name();
}

void Reactor::register_thread(std::shared_ptr<ApplicationThread> thread) {
    // Safety check: ensure we didn't get a null pointer
    if (thread == nullptr) {
        throw PreconditionAssertion("Cannot register a null thread", __FILE__, __LINE__);
    }

    const std::string& name = thread->get_thread_name();
    ThreadID id = thread->get_thread_id();

    if (threads_.find(name) != threads_.end()) {
        throw PreconditionAssertion(fmt::format("Thread name '{}' is already registered", name), __FILE__, __LINE__);
    }

    if (threads_by_thread_id_.find(id) != threads_by_thread_id_.end()) {
        throw PreconditionAssertion(fmt::format("Thread ID '{}' is already registered",
                                                id.get_value()), __FILE__, __LINE__);
    }

    threads_[name] = thread;
    threads_by_thread_id_[id] = thread;

    PUBSUB_LOG(logger_, LogLevel::Info, "Registered application thread: {} (ID: {})",
               name, id.get_value());
}

} // namespace pubsub_itc_fw
