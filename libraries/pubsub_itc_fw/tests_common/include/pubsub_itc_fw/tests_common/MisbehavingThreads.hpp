#pragma once

// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <pubsub_itc_fw/AllocatorConfiguration.hpp>
#include <pubsub_itc_fw/ApplicationThread.hpp>
#include <pubsub_itc_fw/ApplicationThreadConfiguration.hpp>
#include <pubsub_itc_fw/EventMessage.hpp>
#include <pubsub_itc_fw/QueueConfiguration.hpp>
#include <pubsub_itc_fw/QuillLogger.hpp>
#include <pubsub_itc_fw/Reactor.hpp>
#include <pubsub_itc_fw/ThreadID.hpp>

#include <chrono>
#include <thread>

namespace pubsub_itc_fw::test_support {

// ============================================================================
// A thread that never reaches Started (blocks forever in on_initial_event).
// ============================================================================
class NeverStartingThread : public ApplicationThread {
  public:
    NeverStartingThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, const std::string& thread_name, ThreadID thread_id,
                        const QueueConfiguration& queue_config, const AllocatorConfiguration& allocator_config)
        : ApplicationThread(token, logger, reactor, thread_name, thread_id, queue_config, allocator_config, ApplicationThreadConfiguration{}) {}

  protected:
    void on_initial_event() override {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    void on_itc_message(const EventMessage&) override {}
};

// ============================================================================
// Throws during Initial processing.
// ============================================================================
class ThrowingInitialThread : public ApplicationThread {
  public:
    ThrowingInitialThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, const std::string& thread_name, ThreadID thread_id,
                          const QueueConfiguration& queue_config, const AllocatorConfiguration& allocator_config)
        : ApplicationThread(token, logger, reactor, thread_name, thread_id, queue_config, allocator_config, ApplicationThreadConfiguration{}) {}

  protected:
    void on_initial_event() override {
        throw std::runtime_error("boom in Initial");
    }

    void on_itc_message(const EventMessage&) override {}
};

// ============================================================================
// Throws during AppReady processing.
// ============================================================================
class ThrowingAppReadyThread : public ApplicationThread {
  public:
    ThrowingAppReadyThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, const std::string& thread_name, ThreadID thread_id,
                           const QueueConfiguration& queue_config, const AllocatorConfiguration& allocator_config)
        : ApplicationThread(token, logger, reactor, thread_name, thread_id, queue_config, allocator_config, ApplicationThreadConfiguration{}) {}

  protected:
    void on_initial_event() override {}

    void on_app_ready_event() override {
        throw std::runtime_error("boom in AppReady");
    }

    void on_itc_message(const EventMessage&) override {}
};

// ============================================================================
// Throws during normal ITC message processing (run loop).
// ============================================================================
class ThrowingDuringRunThread : public ApplicationThread {
  public:
    ThrowingDuringRunThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, const std::string& thread_name, ThreadID thread_id,
                            const QueueConfiguration& queue_config, const AllocatorConfiguration& allocator_config)
        : ApplicationThread(token, logger, reactor, thread_name, thread_id, queue_config, allocator_config, ApplicationThreadConfiguration{}) {}

  protected:
    void on_initial_event() override {}
    void on_app_ready_event() override {}

    void on_itc_message(const EventMessage&) override {
        throw std::runtime_error("boom during run loop");
    }
};

// ============================================================================
// Throws during Termination event.
// ============================================================================
class ThrowingTerminationThread : public ApplicationThread {
  public:
    ThrowingTerminationThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, const std::string& thread_name, ThreadID thread_id,
                              const QueueConfiguration& queue_config, const AllocatorConfiguration& allocator_config)
        : ApplicationThread(token, logger, reactor, thread_name, thread_id, queue_config, allocator_config, ApplicationThreadConfiguration{}) {}

  protected:
    void on_initial_event() override {}
    void on_app_ready_event() override {}

    void on_termination_event(const std::string&) override {
        throw std::runtime_error("boom in Termination");
    }

    void on_itc_message(const EventMessage&) override {}
};

// ============================================================================
// Blocks forever in ITC message processing.
// ============================================================================
class RogueITCThread : public ApplicationThread {
  public:
    RogueITCThread(ConstructorToken token, QuillLogger& logger, Reactor& reactor, const std::string& thread_name, ThreadID thread_id,
                   const QueueConfiguration& queue_config, const AllocatorConfiguration& allocator_config)
        : ApplicationThread(token, logger, reactor, thread_name, thread_id, queue_config, allocator_config, ApplicationThreadConfiguration{}) {}

  protected:
    void on_initial_event() override {}
    void on_app_ready_event() override {}

    void on_itc_message(const EventMessage&) override {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
};

} // namespace pubsub_itc_fw::test_support
