#pragma once

namespace pubsub_itc_fw {

// A trivial type used only for forcing template instantiation in coverage builds.
// This type is never used in production.
struct CoverageDummy {
    int x;
};

} // namespace pubsub_itc_fw

