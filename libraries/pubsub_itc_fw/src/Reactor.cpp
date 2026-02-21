#include <pubsub_itc_fw/Reactor.hpp>
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

int Reactor::run() {
    is_finished_.store(false, std::memory_order_release);
    shutdown_reason_ = "";
    // TODO there is no implementation here yet.
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
