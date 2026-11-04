// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * How long recording a new epoch takes, and how long it can take when the disk is busy (docs/bug_list.md, BUG-0228).
 *
 * When a sequencer or a matching engine moves to a new generation, EpochStore::store writes the epoch to a temporary
 * file, syncs it, renames it into place and syncs the directory, on the application's own thread, before the new
 * generation is acted on. Principle F4 allows that wait because the instance is changing role and handling no orders.
 * But the lease the instance has just been granted keeps running while it waits, and on 4 November 2026 the wait took
 * 3.2 seconds, longer than the lease, while the disk was busy writing a matching engine's open-order region.
 *
 * Three ways of recording the epoch are measured:
 *
 *   epoch_store          EpochStore::store as the venue uses it: a temporary file, fsync, rename, and a sync of the
 *                        directory.
 *   fdatasync_in_place   the value written over the start of one file kept open, then fdatasync. No rename, so a
 *                        crash part-way could leave a damaged value; it is measured to show what the rename costs.
 *   o_dsync_in_place     the same, with the file opened O_DSYNC, so that each write returns only once its data is
 *                        on the disk.
 *
 * Each is measured three ways:
 *
 *   idle                 nothing else writing.
 *   load_same            another thread writing large files, 512 MiB at a time, on the same filesystem as the epoch,
 *                        as a matching engine does when it creates its open-order region.
 *   load_other           the same writer on another filesystem, given by EPOCH_STORE_COST_LOAD_DIRECTORY, to show how
 *                        much of the cost is the filesystem and how much the disk underneath.
 *
 * Under load, measuring starts only once the writer has written 2 GiB, because a disk with a small fast cache writes
 * quickly until that cache is full and slowly afterwards, and the slow part is what matters here.
 *
 * Every record of an epoch is timed on its own, and each benchmark reports the median, the 99th and 99.9th
 * percentiles and the longest, in microseconds, as counters. The mean that Google Benchmark prints is of no use for a
 * stall that happens once in a thousand writes.
 *
 * The epoch's file is kept in the directory given by EPOCH_STORE_COST_DIRECTORY, which should be on the filesystem
 * that holds the WALs, since that is the one being studied; without it, ./tmp/epoch_store_cost under the current
 * directory. The load_other benchmarks run only when EPOCH_STORE_COST_LOAD_DIRECTORY is set. How many records each
 * benchmark times is EPOCH_STORE_COST_SAMPLES, 1000 by default. Run it on a machine set up for measuring, for example:
 *
 *   EPOCH_STORE_COST_DIRECTORY=/mnt/sda2/epoch_store_cost EPOCH_STORE_COST_LOAD_DIRECTORY=/nvme/epoch_store_cost \
 *       bin_benchmarks/epoch_store_cost
 *
 * and then again with the two directories exchanged. Each directory is made if it is absent, and the files the
 * benchmark writes in it are removed when it ends.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include <pubsub_itc_fw/EpochStore.hpp>
#include <pubsub_itc_fw/ThreadWithJoinTimeout.hpp>

namespace {

using Clock = std::chrono::steady_clock;

// The size of each file the background writer writes, the size of a matching engine's open-order region.
constexpr size_t load_file_bytes = 512ULL * 1024 * 1024;
// How much the background writer writes before measuring starts.
constexpr int64_t load_warm_up_bytes = 2LL * 1024 * 1024 * 1024;
constexpr size_t load_chunk_bytes = 1024 * 1024;

std::string directory_from(const char* variable, const char* fallback) {
    const char* given = std::getenv(variable);
    std::string directory = given != nullptr && *given != '\0' ? std::string(given) : std::string(fallback);
    std::filesystem::create_directories(directory);
    return directory;
}

int64_t samples_wanted() {
    const char* given = std::getenv("EPOCH_STORE_COST_SAMPLES");
    return given != nullptr ? std::max<int64_t>(1, std::atoll(given)) : 1000;
}

// The background writer: writes a file of load_file_bytes, syncs it, removes it, and starts again, until told to stop.
class LoadWriter {
  public:
    explicit LoadWriter(std::string directory) : path_(std::move(directory) + "/load_file") {}

    void start() {
        stop_requested_.store(false);
        thread_.start([this] { write_until_stopped(); });
        while (bytes_written_.load() < load_warm_up_bytes) {
            ::usleep(10000);
        }
    }

    void stop() {
        stop_requested_.store(true);
        thread_.join();
        ::unlink(path_.c_str());
    }

  private:
    void write_until_stopped() {
        const std::vector<char> chunk(load_chunk_bytes, 'r');
        while (!stop_requested_.load()) {
            const int fd = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            if (fd < 0) {
                return;
            }
            for (size_t written = 0; written < load_file_bytes && !stop_requested_.load(); written += load_chunk_bytes) {
                if (::write(fd, chunk.data(), chunk.size()) != static_cast<ssize_t>(chunk.size())) {
                    break;
                }
                bytes_written_.fetch_add(static_cast<int64_t>(chunk.size()));
            }
            static_cast<void>(::fsync(fd));
            ::close(fd);
            ::unlink(path_.c_str());
        }
    }

    std::string path_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<int64_t> bytes_written_{0};
    pubsub_itc_fw::ThreadWithJoinTimeout thread_;
};

// The three ways of recording an epoch.
enum class Method { epoch_store, fdatasync_in_place, o_dsync_in_place };

// Where the background writer writes, if anywhere.
enum class Load { idle, same_filesystem, other_filesystem };

// Records one epoch by @p method. @return false if it failed.
class EpochRecorder {
  public:
    ~EpochRecorder() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        ::unlink(in_place_path_.c_str());
        ::unlink(store_.path().c_str());
    }

    EpochRecorder(Method method, const std::string& directory)
        : method_(method), store_(directory + "/epoch.state"), in_place_path_(directory + "/epoch_in_place.state") {
        if (method_ == Method::fdatasync_in_place) {
            fd_ = ::open(in_place_path_.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        } else if (method_ == Method::o_dsync_in_place) {
            fd_ = ::open(in_place_path_.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC | O_DSYNC, 0644);
        }
    }

    EpochRecorder(const EpochRecorder&) = delete;
    EpochRecorder& operator=(const EpochRecorder&) = delete;
    EpochRecorder(EpochRecorder&&) = delete;
    EpochRecorder& operator=(EpochRecorder&&) = delete;

    bool record(int32_t epoch) {
        if (method_ == Method::epoch_store) {
            return store_.store(epoch);
        }
        if (fd_ < 0) {
            return false;
        }
        const std::string text = fmt::format("{:>11}\n", epoch);
        if (::pwrite(fd_, text.data(), text.size(), 0) != static_cast<ssize_t>(text.size())) {
            return false;
        }
        return method_ == Method::o_dsync_in_place || ::fdatasync(fd_) == 0;
    }

  private:
    Method method_;
    pubsub_itc_fw::EpochStore store_;
    std::string in_place_path_;
    int fd_{-1};
};

double percentile(std::vector<double>& sorted, double fraction) {
    const auto index = static_cast<size_t>(fraction * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

void measure(benchmark::State& state, Method method, Load load) {
    const std::string directory = directory_from("EPOCH_STORE_COST_DIRECTORY", "./tmp/epoch_store_cost");
    std::string load_directory = directory;
    if (load == Load::other_filesystem) {
        load_directory = directory_from("EPOCH_STORE_COST_LOAD_DIRECTORY", "./tmp/epoch_store_cost_load");
    }
    EpochRecorder recorder(method, directory);
    LoadWriter writer(load_directory);
    if (load != Load::idle) {
        writer.start();
    }
    std::vector<double> microseconds;
    microseconds.reserve(static_cast<size_t>(state.max_iterations));
    int32_t epoch = 1;
    for (auto _ : state) {
        const Clock::time_point began = Clock::now();
        const bool recorded = recorder.record(epoch++);
        const std::chrono::duration<double> took = Clock::now() - began;
        if (!recorded) {
            state.SkipWithError("recording the epoch failed");
            break;
        }
        state.SetIterationTime(took.count());
        microseconds.push_back(took.count() * 1e6);
    }
    if (load != Load::idle) {
        writer.stop();
    }
    if (microseconds.empty()) {
        return;
    }
    std::sort(microseconds.begin(), microseconds.end());
    state.counters["p50_us"] = percentile(microseconds, 0.50);
    state.counters["p99_us"] = percentile(microseconds, 0.99);
    state.counters["p999_us"] = percentile(microseconds, 0.999);
    state.counters["max_us"] = microseconds.back();
}

void register_all() {
    const int64_t samples = samples_wanted();
    const bool other_filesystem_given = std::getenv("EPOCH_STORE_COST_LOAD_DIRECTORY") != nullptr;
    const struct {
        Method method;
        const char* name;
    } methods[] = {{Method::epoch_store, "epoch_store"}, {Method::fdatasync_in_place, "fdatasync_in_place"}, {Method::o_dsync_in_place, "o_dsync_in_place"}};
    const struct {
        Load load;
        const char* name;
    } loads[] = {{Load::idle, "idle"}, {Load::same_filesystem, "load_same"}, {Load::other_filesystem, "load_other"}};
    for (const auto& method : methods) {
        for (const auto& load : loads) {
            if (load.load == Load::other_filesystem && !other_filesystem_given) {
                continue;
            }
            const Method chosen_method = method.method;
            const Load chosen_load = load.load;
            benchmark::RegisterBenchmark(fmt::format("{}/{}", method.name, load.name),
                                         [chosen_method, chosen_load](benchmark::State& state) { measure(state, chosen_method, chosen_load); })
                ->Iterations(samples)
                ->UseManualTime();
        }
    }
}

} // namespaces

int main(int argc, char** argv) {
    register_all();
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
