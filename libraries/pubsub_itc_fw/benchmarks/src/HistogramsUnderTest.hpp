#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <InterfaceHistogramHandle.hpp>
#include <pubsub_itc_fw/HistogramHandle.hpp>

namespace pubsub_itc_fw_benchmarks {

/**
 * @brief A histogram registered through a PrometheusEndpoint with metrics enabled, as the venue
 *        registers its histograms, and the handle the endpoint returns for it.
 *
 * The endpoint and the histogram are created on the first call and last until the program ends.
 * The endpoint's listener is never started, so nothing scrapes the histogram while it is measured.
 */
[[nodiscard]] pubsub_itc_fw::HistogramHandle concrete_histogram_handle();

/**
 * @brief A histogram of the same kind, with the same bucket bounds, reached through
 *        HistogramInterface, so that every observation is a virtual call.
 *
 * Created on the first call and lasts until the program ends.
 */
[[nodiscard]] InterfaceHistogramHandle interface_histogram_handle();

} // namespaces
