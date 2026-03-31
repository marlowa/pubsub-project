#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.

namespace pubsub_itc_fw {

// A trivial type used only for forcing template instantiation in coverage builds.
// This type is never used in production.
struct CoverageDummy {
    int x;
};

} // namespace pubsub_itc_fw
