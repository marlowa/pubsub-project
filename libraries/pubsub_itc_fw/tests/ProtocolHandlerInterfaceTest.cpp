// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <string>
#include <tuple>

#include <gtest/gtest.h>

#include <pubsub_itc_fw/ProtocolHandlerInterface.hpp>

using namespace pubsub_itc_fw;

namespace {

// A handler that implements only the pure-virtual methods, inheriting the
// interface's default implementations for the rest -- which is exactly what a
// plain-TCP PDU handler does. The defaults are what these tests exercise.
class StubProtocolHandler : public ProtocolHandlerInterface {
  public:
    std::tuple<bool, std::string, bool> on_data_ready() override {
        return {true, "", false};
    }

    std::tuple<bool, std::string> send_prebuilt(ExpandableSlabAllocator*, SlabHandle, void*, uint32_t) override {
        return {true, ""};
    }

    bool has_pending_send() const override {
        return false;
    }

    std::tuple<bool, std::string> continue_send() override {
        return {true, ""};
    }

    void deallocate_pending_send() override {}
};

} // namespaces

TEST(ProtocolHandlerInterfaceTest, DefaultCommitBytesReturnsFalse) {
    StubProtocolHandler handler;

    EXPECT_FALSE(handler.commit_bytes(128));
}

TEST(ProtocolHandlerInterfaceTest, DefaultIsReadsPausedReturnsFalse) {
    StubProtocolHandler handler;

    EXPECT_FALSE(handler.is_reads_paused());
}

TEST(ProtocolHandlerInterfaceTest, DefaultIsHandshakeCompleteReturnsTrue) {
    StubProtocolHandler handler;

    EXPECT_TRUE(handler.is_handshake_complete());
}

TEST(ProtocolHandlerInterfaceTest, DefaultStartOutboundHandshakeSucceeds) {
    StubProtocolHandler handler;

    const auto [ok, error] = handler.start_outbound_handshake();

    EXPECT_TRUE(ok);
    EXPECT_TRUE(error.empty());
}
