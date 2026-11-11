// SPDX-FileCopyrightText: Copyright 2026 Eglantine Contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <memory>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/host_command_queue.h"

TEST_CASE("Host command publication progresses while the consumer is unavailable", "[common]") {
    Common::HostCommandQueue<std::unique_ptr<unsigned>> queue;
    // Exceed the previous GPU ring's 4096 entries without running a consumer.
    // A producer holding a registry lock must be able to publish and release it.
    constexpr unsigned count = 16384;
    for (unsigned i = 0; i < count; ++i) {
        queue.Emplace(std::make_unique<unsigned>(i));
    }
    std::unique_ptr<unsigned> value;
    for (unsigned i = 0; i < count; ++i) {
        REQUIRE(queue.TryPop(value));
        REQUIRE(*value == i);
    }
    REQUIRE_FALSE(queue.TryPop(value));
}

TEST_CASE("Host command queue wakes a waiting consumer and preserves ordering", "[common]") {
    Common::HostCommandQueue<unsigned> queue;
    constexpr unsigned count = 8192;
    bool ordered = true;
    std::jthread consumer{[&](std::stop_token stop) {
        for (unsigned expected = 0; expected < count; ++expected) {
            unsigned value = ~0U;
            queue.PopWait(value, stop);
            ordered &= value == expected;
        }
    }};
    for (unsigned i = 0; i < count; ++i) {
        queue.Emplace(i);
    }
    consumer.join();
    REQUIRE(ordered);
}

TEST_CASE("Host command queue stop wakes an empty consumer without discarding backlog", "[common]") {
    Common::HostCommandQueue<unsigned> queue;
    unsigned value = 77;
    std::jthread consumer{[&](std::stop_token stop) { queue.PopWait(value, stop); }};
    consumer.request_stop();
    consumer.join();
    REQUIRE(value == 77);

    queue.Emplace(123);
    REQUIRE(queue.TryPop(value));
    REQUIRE(value == 123);
}
