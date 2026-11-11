// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <future>
#include <thread>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include "common/host_command_completion.h"
#include "common/host_command_queue.h"

TEST_CASE("GPU command completion signals only its own request", "[host_command_completion]") {
    auto first = std::make_shared<Common::HostCommandCompletion>();
    auto second = std::make_shared<Common::HostCommandCompletion>();
    auto early = std::async(std::launch::async, [first] { first->Wait(); });
    auto later = std::async(std::launch::async, [second] { second->Wait(); });
    second->Signal();
    const bool later_done = later.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
    const bool early_done = early.wait_for(std::chrono::milliseconds(20)) == std::future_status::ready;
    first->Signal(); // Always release before assertions so a failure cannot leave a waiter behind.
    early.get();
    later.get();
    CHECK(later_done);
    CHECK_FALSE(early_done);
}

TEST_CASE("GPU completion reuse publishes results and survives caller exit", "[host_command_completion]") {
    struct Command {
        unsigned core{}, turn{};
        std::shared_ptr<Common::HostCommandCompletion> completion;
        Command() = default;
        Command(unsigned c, unsigned t, std::shared_ptr<Common::HostCommandCompletion> done)
            : core(c), turn(t), completion(std::move(done)) {}
    };
    constexpr unsigned cores = 6, turns = 4000;
    Common::HostCommandQueue<Command> queue;
    std::barrier start(cores + 1);
    std::array<unsigned, cores> results{};
    std::array<std::shared_ptr<Common::HostCommandCompletion>, cores> identities;
    std::atomic<unsigned> errors{};
    std::jthread consumer([&] {
        start.arrive_and_wait();
        Command command;
        for (unsigned n = 0; n < cores * turns; ++n) {
            queue.PopWait(command, {});
            results[command.core] = command.turn;
            command.completion->Signal();
        }
    });
    std::vector<std::jthread> producers;
    for (unsigned core = 0; core < cores; ++core) {
        producers.emplace_back([&, core] {
            auto completion = Common::HostCommandCompletion::ForCurrentThread();
            identities[core] = completion;
            start.arrive_and_wait();
            for (unsigned turn = 1; turn <= turns; ++turn) {
                if (Common::HostCommandCompletion::ForCurrentThread() != completion) ++errors;
                queue.Emplace(core, turn, completion);
                completion->Wait();
                if (results[core] != turn) ++errors;
            }
        });
    }
    for (auto& producer : producers) producer.join();
    consumer.join();
    CHECK(errors == 0);
    for (unsigned a = 0; a < cores; ++a) {
        CHECK(results[a] == turns);
        for (unsigned b = a + 1; b < cores; ++b) CHECK(identities[a] != identities[b]);
    }
}

TEST_CASE("Queued GPU completion outlives the submitter's local reference", "[host_command_completion]") {
    auto caller = std::make_shared<Common::HostCommandCompletion>();
    std::weak_ptr weak = caller;
    std::jthread gpu([keep = caller] {
        keep->Signal();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        // keep owns the state until notification and callback teardown have finished.
    });
    caller->Wait();
    caller.reset();
    gpu.join();
    CHECK(weak.expired());
}

TEST_CASE("Cancelled GPU completions cannot signal a reused caller", "[host_command_completion]") {
    auto old = Common::HostCommandCompletion::ForCurrentThread();
    std::stop_source stopped;
    stopped.request_stop();
    CHECK_FALSE(old->Wait(stopped.get_token()));
    auto fresh = Common::HostCommandCompletion::ForCurrentThread();
    CHECK(fresh != old);
    auto waiting = std::async(std::launch::async, [fresh] { fresh->Wait(); });
    old->Signal();
    const bool stale_woke_new = waiting.wait_for(std::chrono::milliseconds(20)) == std::future_status::ready;
    fresh->Signal();
    waiting.get();
    CHECK_FALSE(stale_woke_new);
    auto cancelled = std::make_shared<Common::HostCommandCompletion>();
    std::stop_source source;
    auto pending = std::async(std::launch::async, [cancelled, stop = source.get_token()] {
        return cancelled->Wait(stop);
    });
    source.request_stop();
    CHECK_FALSE(pending.get());
}
