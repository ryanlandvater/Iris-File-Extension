/**
 * @file ife_memory_tests.cpp
 * @brief Phase 1 unit + stress tests for the IFE_Memory lock-free VMA.
 *
 * Self-contained (no external test framework) so CI doesn't need extra deps.
 * Run with `ctest` or directly; non-zero exit on failure.
 */
#include "IFE_Memory.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

#define IFE_CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures; \
    } \
} while (0)

void test_basic_claim() {
    auto mem = IFE::Memory::create(1024);
    IFE_CHECK(mem.capacity() == 1024);
    // First claim should land immediately after the 16-byte reserved header.
    auto a = mem.claim_space(64);
    IFE_CHECK(a.valid());
    IFE_CHECK(a.offset == IFE::WRITE_HEAD_BYTES);
    IFE_CHECK(a.size == 64);
    IFE_CHECK(a.ptr == mem.data() + IFE::WRITE_HEAD_BYTES);

    auto b = mem.claim_space(32);
    IFE_CHECK(b.offset == a.offset + 64);

    IFE_CHECK(mem.write_head() == a.offset + 64 + 32);
    IFE_CHECK(!mem.stream_locked());
}

void test_exhaustion() {
    auto mem = IFE::Memory::create(128);
    // Header takes 16; 112 free.
    auto a = mem.claim_space(100);
    IFE_CHECK(a.valid());
    bool threw = false;
    try { (void)mem.claim_space(64); } catch (const std::bad_alloc&) { threw = true; }
    IFE_CHECK(threw);
    // Cursor should have rolled back so a follow-up small claim still works.
    auto c = mem.claim_space(8);
    IFE_CHECK(c.valid());
    IFE_CHECK(c.offset == a.offset + 100);
}

void test_zero_and_empty() {
    IFE::Memory empty;
    bool threw = false;
    try { (void)empty.claim_space(8); } catch (const std::logic_error&) { threw = true; }
    IFE_CHECK(threw);

    auto mem = IFE::Memory::create(64);
    threw = false;
    try { (void)mem.claim_space(0); } catch (const std::logic_error&) { threw = true; }
    IFE_CHECK(threw);
}

void test_concurrent_claim_space() {
    constexpr int kThreads = 8;
    constexpr int kClaimsPerThread = 2000;
    constexpr std::size_t kClaimBytes = 32;
    const std::size_t cap =
        IFE::WRITE_HEAD_BYTES + kThreads * kClaimsPerThread * kClaimBytes + 64;

    auto mem = IFE::Memory::create(cap);
    std::vector<std::thread> threads;
    std::vector<std::vector<std::uint64_t>> all_offsets(kThreads);

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            auto& mine = all_offsets[t];
            mine.reserve(kClaimsPerThread);
            for (int i = 0; i < kClaimsPerThread; ++i) {
                auto s = mem.claim_space(kClaimBytes);
                mine.push_back(s.offset);
                // Write a sentinel so we can verify regions are disjoint.
                std::memset(s.ptr, (t * 31 + i) & 0xFF, s.size);
            }
        });
    }
    for (auto& th : threads) th.join();

    // All offsets must be unique and aligned to the claim size.
    std::set<std::uint64_t> seen;
    for (auto& v : all_offsets) {
        for (auto off : v) {
            IFE_CHECK(seen.insert(off).second); // unique
            IFE_CHECK((off - IFE::WRITE_HEAD_BYTES) % kClaimBytes == 0);
            IFE_CHECK(off + kClaimBytes <= mem.capacity());
        }
    }
    IFE_CHECK(seen.size() == static_cast<std::size_t>(kThreads * kClaimsPerThread));
    IFE_CHECK(mem.write_head() == IFE::WRITE_HEAD_BYTES + kThreads * kClaimsPerThread * kClaimBytes);
}

void test_stream_lock_exclusion() {
    auto mem = IFE::Memory::create(4096);
    // Pre-fill so we have a known offset.
    auto pre = mem.claim_space(128);
    (void)pre;

    std::atomic<bool> writer_started{false};
    std::atomic<bool> writer_finished{false};
    std::atomic<int>  claims_during_lock{0};
    const std::uint64_t head_before_lock = mem.write_head();

    auto guard = mem.acquire_stream();
    IFE_CHECK(guard.held());
    IFE_CHECK(mem.stream_locked());
    IFE_CHECK(guard.offset() == head_before_lock);
    IFE_CHECK(guard.data() == mem.data() + head_before_lock);

    std::thread writer([&] {
        writer_started.store(true, std::memory_order_release);
        // This call must block until guard is released.
        auto s = mem.claim_space(16);
        (void)s;
        writer_finished.store(true, std::memory_order_release);
        claims_during_lock.fetch_add(1);
    });

    while (!writer_started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Writer must NOT have completed while we hold the lock.
    IFE_CHECK(!writer_finished.load(std::memory_order_acquire));
    IFE_CHECK(mem.write_head() == head_before_lock);

    // Exercise move construction/assignment, then release.
    auto moved = std::move(guard);
    IFE_CHECK(moved.held());
    IFE_CHECK(!guard.held()); // NOLINT(bugprone-use-after-move)
    moved.release();
    IFE_CHECK(!moved.held());
    IFE_CHECK(!mem.stream_locked());

    writer.join();
    IFE_CHECK(writer_finished.load());
    IFE_CHECK(claims_during_lock.load() == 1);
    IFE_CHECK(mem.write_head() == head_before_lock + 16);
}

void test_view_lifetime() {
    IFE::Memory::View view;
    IFE_CHECK(!view);
    {
        auto mem = IFE::Memory::create(256);
        auto s = mem.claim_space(8);
        std::memcpy(s.ptr, "ABCDEFGH", 8);
        view = mem.view();
        IFE_CHECK(static_cast<bool>(view));
        // Drop all Memory handles; the View must keep the arena alive.
    }
    IFE_CHECK(view.capacity() == 256);
    IFE_CHECK(std::memcmp(view.data() + IFE::WRITE_HEAD_BYTES, "ABCDEFGH", 8) == 0);
}

void test_invalid_capacity() {
    bool threw = false;
    try { (void)IFE::Memory::create(8); } catch (const std::invalid_argument&) { threw = true; }
    IFE_CHECK(threw);
}

void test_file_backed_persistence() {
    // Pick a temp path under the build/test working directory. Using
    // `temp_directory_path()` keeps this portable; the file is removed at
    // the end (on success and on the error paths via the RAII guard below).
    namespace fs = std::filesystem;
    const fs::path tmp =
        fs::temp_directory_path() /
        ("ife_memory_filebacked_" +
         std::to_string(static_cast<unsigned long long>(
             std::chrono::steady_clock::now().time_since_epoch().count())) +
         ".bin");

    struct Cleanup {
        fs::path p;
        ~Cleanup() { std::error_code ec; fs::remove(p, ec); }
    } cleanup{tmp};

    constexpr std::size_t kCapacity = 64 * 1024; // 64 KiB sparse reservation
    constexpr char        kSentinel[] = "IFE-FILEBACKED-OK";
    constexpr std::size_t kSentinelLen = sizeof(kSentinel) - 1;

    std::uint64_t expected_offset = 0;
    {
        auto mem = IFE::Memory::createFromFile(tmp, kCapacity);
        IFE_CHECK(mem.capacity() == kCapacity);

        auto s = mem.claim_space(kSentinelLen);
        IFE_CHECK(s.valid());
        IFE_CHECK(s.offset == IFE::WRITE_HEAD_BYTES);
        std::memcpy(s.ptr, kSentinel, kSentinelLen);
        expected_offset = s.offset;

        // Trim the sparse reservation back to the actually-written size.
        mem.truncate_file(mem.write_head());
        // Drop the Memory handle: unmap + close fd; the page cache flushes
        // on close, so the file content is observable via std::ifstream.
    }

    std::error_code ec;
    const auto sz = fs::file_size(tmp, ec);
    IFE_CHECK(!ec);
    // After truncate_file(write_head()), the file should be exactly the
    // written extent (sentinel offset + sentinel length).
    IFE_CHECK(sz == expected_offset + kSentinelLen);

    std::ifstream in(tmp, std::ios::binary);
    IFE_CHECK(in.good());
    in.seekg(static_cast<std::streamoff>(expected_offset));
    char buf[kSentinelLen + 1] = {};
    in.read(buf, static_cast<std::streamsize>(kSentinelLen));
    IFE_CHECK(static_cast<std::size_t>(in.gcount()) == kSentinelLen);
    IFE_CHECK(std::memcmp(buf, kSentinel, kSentinelLen) == 0);
}

} // namespace

int main() {
    test_basic_claim();
    test_exhaustion();
    test_zero_and_empty();
    test_concurrent_claim_space();
    test_stream_lock_exclusion();
    test_view_lifetime();
    test_invalid_capacity();
    test_file_backed_persistence();

    if (g_failures == 0) {
        std::printf("ife_memory_tests: ALL PASS\n");
        return 0;
    }
    std::fprintf(stderr, "ife_memory_tests: %d FAILURE(S)\n", g_failures);
    return 1;
}
