// Copyright (c) 2024-2026 Andrew Peter Marlow. All rights reserved.
// SPDX-License-Identifier: Apache-2.0

/*
 * What it costs the leading sequencer to check a command against its WAL before giving it a number
 * (docs/availability/one_check_against_sequencing_twice.md, section 8, step 5).
 *
 * Every command the leader receives is checked. The check makes a 64-bit number from the command's
 * identity (the member's comp id, the gateway protocol and the ClOrdID) and looks the number up in the
 * record of command identifiers, a hash map reserved for the whole day when the sequencer starts. For
 * almost every command the number is not there, and the check ends. When it is there, the WAL may hold
 * the command, and the leader reads the one WAL record at the position the map keeps beside the number
 * and compares the identities. Every command written is also added to the map.
 *
 * Three costs are measured.
 *
 *   Making the number. identity_to_number makes the number from identities of the size the venue's
 *   identities have.
 *
 *   Looking the number up, and adding it. find_absent and add_new use a map of the size the venue
 *   reserves, 200 million identities, which is 2^28 slots and 6 GiB, holding 50 million identities (a
 *   normal day) or 100 million (the day the venue is tested with). A map of 10 thousand, small enough
 *   to stay in the processor's caches, is measured beside them, to show how much of the cost is memory.
 *   Each lookup or add is followed by an LFENCE instruction, which makes the processor finish it before
 *   it starts the next. That measures how long one takes, which is what an order waits for, rather than
 *   how many the processor can have under way at once: the sequencer makes one lookup per order, among
 *   other work, so lookups on the order path do not overlap. Without the fence, the processor guesses
 *   that a number is not in the map before the lookup has finished, starts the next lookup, and several
 *   reads of memory overlap; a lookup in the map of 6 GiB then appears to take about 12 nanoseconds,
 *   far less than one read of main memory. fenced_empty_loop measures the loop and the fence with no
 *   lookup, to be subtracted from the others.
 *
 *   Reading one record. read_one_record does what the leader does to read the record at a position:
 *   it opens a WalCursor at the position and reads the next record. It is measured with the segment in
 *   the page cache ("warm") and with the segment's pages dropped from the page cache first ("cold"), so
 *   that the read must go to the disk, and with the cursor asking the kernel to read the whole segment
 *   ahead ("read_only_pages" 0) or only the pages it reads (1), which is how the leader reads one record.
 *   Each is measured with the WAL's directory holding 20 segment files, 4000 and 16000, to show that the
 *   time does not depend on how many segments the WAL has. A record in
 *   the sequencer's WAL averages 292 bytes, so 50 million records fill about 3500 segments of 4 MiB; a
 *   day of 50 million orders writes a record for each order and each report, and for each cancel and its
 *   report, so 4000 files is a day with few cancels and 16000 a day with many. The cold case checks, with mincore,
 *   that none of the segment's pages was in the page cache before each read, and reports how many were
 *   as pages_resident_before_read, which must be zero for the figure to mean a read from the disk.
 *
 *   Finding the segment that holds a record. find_segment_of_record times Wal::scan_start_for, which
 *   the leader uses to start reading at a record it knows only by its number, for records in the last
 *   segments of the same three directories, with the segments in the page cache.
 *
 * The WAL is written in a directory given by the environment variable COMMAND_CHECK_COST_WAL_DIRECTORY,
 * on the disk whose read is to be measured; the directory is removed and written again at the start.
 * Without the variable, it is ./tmp/command_check_cost_wal under the current directory. The map takes
 * 6 GiB of memory, and the program builds one such map at a time.
 *
 * Run it pinned to one core, on a machine set up for measuring, from the installation directory:
 *   python3 scripts/measurement_machine.py --on      (as root)
 *   COMMAND_CHECK_COST_WAL_DIRECTORY=<a directory on the WAL's disk> \
 *       LD_LIBRARY_PATH=lib taskset -c 6 bin_benchmarks/command_check_cost
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <immintrin.h>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#include <pubsub_itc_fw/Crc32.hpp>
#include <pubsub_itc_fw/LoggedCommandIdentifiers.hpp>
#include <pubsub_itc_fw/Wal.hpp>
#include <pubsub_itc_fw/WalCursor.hpp>
#include <pubsub_itc_fw/WalPosition.hpp>
#include <pubsub_itc_fw/WalWriter.hpp>

namespace {

using pubsub_itc_fw::LoggedCommandIdentifiers;
using pubsub_itc_fw::WalPosition;

// The final mixing step of SplitMix64, used to make numbers spread evenly over the map's slots.
uint64_t mix(uint64_t value) {
    value ^= value >> 30U;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27U;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31U;
    return value;
}

// Numbers added to a map are mix(i) for i from 1; numbers looked for and not present are mix(i) with the top
// bit of i set, which no map here is filled far enough to reach.
constexpr uint64_t absent_bit = uint64_t{1} << 63U;

constexpr size_t venue_capacity = 200'000'000;

// Making the number from a command's identity.

// An identity laid out as the sequencer lays it out: the comp id's length in two bytes, the comp id, the
// protocol in two bytes, and the ClOrdID.
std::string make_identity(int member, int64_t order) {
    const std::string comp_id = fmt::format("MEMBER{:04}", member);
    const std::string cl_ord_id = fmt::format("ORD{:016}", order);
    std::string identity;
    const auto comp_id_size = static_cast<uint16_t>(comp_id.size());
    identity.append(reinterpret_cast<const char*>(&comp_id_size), 2);
    identity.append(comp_id);
    const int16_t protocol = 1;
    identity.append(reinterpret_cast<const char*>(&protocol), 2);
    identity.append(cl_ord_id);
    return identity;
}

void identity_to_number(benchmark::State& state) {
    constexpr size_t count = 4096;
    std::vector<std::string> identities;
    identities.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        identities.push_back(make_identity(static_cast<int>(index % 500), static_cast<int64_t>(index) * 7919));
    }
    size_t index = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(LoggedCommandIdentifiers::identifier(identities[index]));
        index = (index + 1) & (count - 1);
    }
    state.counters["identity_bytes"] = static_cast<double>(identities[0].size());
}

// Looking the number up in the record of command identifiers, and adding it.

// One map at a time, rebuilt when a benchmark asks for a different size, because two maps of the venue's
// size would take 12 GiB.
struct MapUnderTest {
    size_t capacity{0};
    size_t filled{0};
    std::unique_ptr<LoggedCommandIdentifiers> map;
    uint64_t next_to_add{0};
};

MapUnderTest& map_of(size_t capacity, size_t filled) {
    static MapUnderTest current;
    if (current.map == nullptr || current.capacity != capacity || current.filled != filled) {
        current.map.reset();
        current.map = std::make_unique<LoggedCommandIdentifiers>(capacity, nullptr);
        for (uint64_t index = 1; index <= filled; ++index) {
            current.map->add(mix(index), WalPosition{index / 12'000, (index % 12'000) * 350});
        }
        current.capacity = capacity;
        current.filled = filled;
        current.next_to_add = filled + 1;
    }
    return current;
}

void fenced_empty_loop(benchmark::State& state) {
    uint64_t chain = 1;
    for (auto _ : state) {
        benchmark::DoNotOptimize(mix(chain | absent_bit));
        _mm_lfence();
        ++chain;
    }
}

void find_absent(benchmark::State& state) {
    MapUnderTest& under_test = map_of(static_cast<size_t>(state.range(0)), static_cast<size_t>(state.range(1)));
    uint64_t chain = 1;
    for (auto _ : state) {
        LoggedCommandIdentifiers::Lookup found = under_test.map->find(mix(chain | absent_bit));
        benchmark::DoNotOptimize(found.may_hold);
        _mm_lfence();
        ++chain;
    }
    state.counters["slots"] = static_cast<double>(under_test.map->slot_count());
    state.counters["held"] = static_cast<double>(under_test.map->size());
}

void add_new(benchmark::State& state) {
    MapUnderTest& under_test = map_of(static_cast<size_t>(state.range(0)), static_cast<size_t>(state.range(1)));
    for (auto _ : state) {
        if (under_test.map->full()) {
            state.SkipWithError("the map is full");
            break;
        }
        const uint64_t index = under_test.next_to_add++;
        benchmark::DoNotOptimize(under_test.map->add(mix(index), WalPosition{index / 12'000, (index % 12'000) * 350}));
        _mm_lfence();
    }
    // The repetitions add to the same map, rather than each building it again, which takes seconds; five
    // repetitions of a million adds move a map of 50 million by a tenth.
    state.counters["held_at_end"] = static_cast<double>(under_test.map->size());
}

// Reading the one WAL record at a position.

constexpr size_t segment_size = 4 * 1024 * 1024;
constexpr size_t record_size = 350;
constexpr size_t segments_written = 20;

std::string wal_directory_of(size_t segment_files) {
    const char* given = std::getenv("COMMAND_CHECK_COST_WAL_DIRECTORY");
    const std::filesystem::path base = given != nullptr ? std::filesystem::path(given) : std::filesystem::current_path() / "tmp" / "command_check_cost_wal";
    return (base / fmt::format("segments_{}", segment_files)).string();
}

std::string segment_path(const std::string& directory, uint64_t segment) {
    return fmt::format("{}/wal_{:06}.log", directory, segment);
}

// A WAL of segments_written full segments, the last segment_files of a directory holding segment_files files.
// Each file before them holds one record, numbered in order, and nothing else: they stand for the earlier segments of
// a day, which the readers measured here must not have to read, without taking 4 MiB of disk each.
struct WrittenWal {
    std::string directory;
    uint64_t first_segment{0};
    int64_t first_full_segment_record{0};
    std::vector<WalPosition> positions;
};

// Writes a segment file holding the one record @p record_id, framed as WalWriter frames an entry: magic, payload size,
// record id and eight reserved bytes, the payload, and a CRC32 of the header and the payload.
void write_one_record_segment(const std::string& path, int64_t record_id) {
    std::array<uint8_t, 24 + record_size + 4> entry{};
    const uint32_t magic = pubsub_itc_fw::WalWriter::entry_magic;
    const auto payload_size = static_cast<uint32_t>(record_size);
    std::memcpy(entry.data(), &magic, sizeof(magic));
    std::memcpy(entry.data() + 4, &payload_size, sizeof(payload_size));
    std::memcpy(entry.data() + 8, &record_id, sizeof(record_id));
    const uint32_t checksum = pubsub_itc_fw::Crc32::compute(entry.data(), 24 + record_size);
    std::memcpy(entry.data() + 24 + record_size, &checksum, sizeof(checksum));
    const int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) {
        throw std::runtime_error("cannot create " + path);
    }
    const ssize_t written = ::write(fd, entry.data(), entry.size());
    ::close(fd);
    if (written != static_cast<ssize_t>(entry.size())) {
        throw std::runtime_error("cannot write " + path);
    }
}

WrittenWal& wal_of(size_t segment_files) {
    static std::vector<std::unique_ptr<WrittenWal>> written;
    for (const std::unique_ptr<WrittenWal>& wal : written) {
        if (wal->directory == wal_directory_of(segment_files)) {
            return *wal;
        }
    }
    auto wal = std::make_unique<WrittenWal>();
    wal->directory = wal_directory_of(segment_files);
    wal->first_segment = segment_files - segments_written;
    std::filesystem::remove_all(wal->directory);
    std::filesystem::create_directories(wal->directory);
    for (uint64_t segment = 0; segment < wal->first_segment; ++segment) {
        write_one_record_segment(segment_path(wal->directory, segment), static_cast<int64_t>(segment) + 1);
    }
    wal->first_full_segment_record = static_cast<int64_t>(wal->first_segment) + 1;
    {
        pubsub_itc_fw::WalWriter writer;
        writer.open(wal->directory, segment_size, WalPosition{wal->first_segment, 0});
        std::array<uint8_t, record_size> payload{};
        int64_t record_id = wal->first_full_segment_record;
        while (writer.current_position().segment < wal->first_segment + segments_written) {
            payload.fill(static_cast<uint8_t>(record_id));
            const WalPosition before = writer.current_position();
            writer.append(record_id, payload.data(), payload.size());
            // A record that began a new segment was written at its start, not where the last one ended.
            const WalPosition after = writer.current_position();
            wal->positions.push_back(after.segment == before.segment ? before : WalPosition{after.segment, 0});
            ++record_id;
        }
        writer.close();
    }
    // The last record began a segment beyond the ones wanted; a read there is not of a full segment.
    wal->positions.pop_back();
    // Written to the disk, so that dropping the pages from the page cache drops them, rather than leaving dirty pages.
    for (uint64_t segment = wal->first_segment; segment <= wal->first_segment + segments_written; ++segment) {
        const int fd = ::open(segment_path(wal->directory, segment).c_str(), O_RDONLY);
        if (fd >= 0) {
            ::fsync(fd);
            ::close(fd);
        }
    }
    written.push_back(std::move(wal));
    return *written.back();
}

// Drops the segment's pages from the page cache, and returns how many were still resident afterwards.
size_t drop_from_page_cache(const std::string& path) {
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return 0;
    }
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    struct stat status {};
    ::fstat(fd, &status);
    const auto size = static_cast<size_t>(status.st_size);
    size_t resident = 0;
    void* mapped = ::mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped != MAP_FAILED) {
        const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
        std::vector<unsigned char> pages((size + page - 1) / page);
        if (::mincore(mapped, size, pages.data()) == 0) {
            resident = static_cast<size_t>(std::count_if(pages.begin(), pages.end(), [](unsigned char flags) { return (flags & 1U) != 0; }));
        }
        ::munmap(mapped, size);
    }
    ::close(fd);
    return resident;
}

// Reads the one record at the position as the leader does, and returns a sum of its bytes so that they are read.
uint64_t read_record_at(const std::string& directory, WalPosition position, pubsub_itc_fw::WalCursor::ReadAhead read_ahead) {
    pubsub_itc_fw::WalCursor cursor;
    cursor.open(directory, position, read_ahead);
    int64_t record_id = 0;
    const uint8_t* payload = nullptr;
    size_t size = 0;
    if (!cursor.read_next(record_id, payload, size)) {
        return 0;
    }
    uint64_t sum = static_cast<uint64_t>(record_id);
    for (size_t index = 0; index < size; ++index) {
        sum += payload[index];
    }
    return sum;
}

void read_one_record(benchmark::State& state) {
    const bool cold = state.range(0) != 0;
    const auto read_ahead = state.range(2) != 0 ? pubsub_itc_fw::WalCursor::ReadAhead::pages_read_only : pubsub_itc_fw::WalCursor::ReadAhead::whole_segment;
    const auto segment_files = static_cast<size_t>(state.range(1));
    const WrittenWal& wal = wal_of(segment_files);
    uint64_t chain = 1;
    size_t resident_before_read = 0;
    for (auto _ : state) {
        const WalPosition position = wal.positions[mix(chain) % wal.positions.size()];
        if (cold) {
            resident_before_read += drop_from_page_cache(segment_path(wal.directory, position.segment));
        }
        const auto start = std::chrono::steady_clock::now();
        uint64_t sum = read_record_at(wal.directory, position, read_ahead);
        const auto finish = std::chrono::steady_clock::now();
        benchmark::DoNotOptimize(sum);
        state.SetIterationTime(std::chrono::duration<double>(finish - start).count());
        ++chain;
    }
    state.counters["pages_resident_before_read"] = static_cast<double>(resident_before_read);
    state.counters["records"] = static_cast<double>(wal.positions.size());
}

void find_segment_of_record(benchmark::State& state) {
    const WrittenWal& written = wal_of(static_cast<size_t>(state.range(0)));
    // A Wal opened on the directory, as the sequencer's is; opening it reads the records once to find the end.
    static std::vector<std::pair<std::string, std::unique_ptr<pubsub_itc_fw::Wal>>> opened;
    pubsub_itc_fw::Wal* wal = nullptr;
    for (auto& [directory, open_wal] : opened) {
        if (directory == written.directory) {
            wal = open_wal.get();
        }
    }
    if (wal == nullptr) {
        auto open_wal = std::make_unique<pubsub_itc_fw::Wal>();
        open_wal->open(written.directory, segment_size);
        wal = open_wal.get();
        opened.emplace_back(written.directory, std::move(open_wal));
    }
    // Records in the last three segments, which is where a recent command is.
    const int64_t last_record = written.first_full_segment_record + static_cast<int64_t>(written.positions.size()) - 1;
    const int64_t first_looked_for = last_record - 3 * 12'000;
    uint64_t chain = 1;
    WalPosition found{};
    for (auto _ : state) {
        found = wal->scan_start_for(first_looked_for + static_cast<int64_t>(mix(chain) % (3 * 12'000)));
        benchmark::DoNotOptimize(found);
        ++chain;
    }
    state.counters["last_segment_found"] = static_cast<double>(found.segment);
}

constexpr int repetitions = 5;

BENCHMARK(identity_to_number)->Repetitions(repetitions)->DisplayAggregatesOnly(true);

BENCHMARK(fenced_empty_loop)->Repetitions(repetitions)->DisplayAggregatesOnly(true);

BENCHMARK(find_absent)
    ->Args({10'000, 5'000})
    ->Args({static_cast<int64_t>(venue_capacity), 50'000'000})
    ->Args({static_cast<int64_t>(venue_capacity), 100'000'000})
    ->Repetitions(repetitions)
    ->DisplayAggregatesOnly(true);

// A fixed number of adds, so that the map's fill moves by no more than a few per cent over the repetitions.
BENCHMARK(add_new)
    ->Args({static_cast<int64_t>(venue_capacity), 50'000'000})
    ->Args({static_cast<int64_t>(venue_capacity), 100'000'000})
    ->Iterations(1'000'000)
    ->Repetitions(repetitions)
    ->DisplayAggregatesOnly(true);

BENCHMARK(read_one_record)
    ->ArgNames({"cold", "segment_files", "read_only_pages"})
    ->Args({0, 20, 1})
    ->Args({0, 4000, 1})
    ->Args({0, 16000, 1})
    ->UseManualTime()
    ->Iterations(2000)
    ->Repetitions(repetitions)
    ->DisplayAggregatesOnly(true);

BENCHMARK(read_one_record)
    ->ArgNames({"cold", "segment_files", "read_only_pages"})
    ->Args({1, 20, 0})
    ->Args({1, 20, 1})
    ->Args({1, 4000, 1})
    ->Args({1, 16000, 1})
    ->UseManualTime()
    ->Iterations(200)
    ->Repetitions(repetitions)
    ->DisplayAggregatesOnly(true);

BENCHMARK(find_segment_of_record)->Arg(20)->Arg(4000)->Arg(16000)->Repetitions(repetitions)->DisplayAggregatesOnly(true);

} // un-named namespace

BENCHMARK_MAIN();
