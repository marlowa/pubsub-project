#include <pubsub_itc_fw/Reactor.hpp>

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

} // namespace pubsub_itc_fw
