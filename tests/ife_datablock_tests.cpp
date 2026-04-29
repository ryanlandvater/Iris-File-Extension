/**
 * @file ife_datablock_tests.cpp
 * @brief Phase 4 tests: universal DATA_BLOCK header, Builder, amend_pointer.
 *
 * Covers:
 *  - Round-trip read/write of the universal 10-byte preamble.
 *  - `IFE_FILE_MAGIC` low 32 bits == legacy `MAGIC_BYTES` (`'Iris'`).
 *  - `Builder::claim_block` lays out a valid universal preamble at the
 *    arena offset returned by `claim_space`.
 *  - `Builder::claim_file_header` carries `IFE_FILE_MAGIC` and the
 *    `RESOURCE_HEADER` recovery tag.
 *  - `Builder::amend_pointer` is a release-ordered atomic store and is
 *    safe under concurrent multi-threaded amend-races.
 *  - Bounds and error paths.
 *
 * @copyright Copyright (c) 2026 Ryan Landvater. MIT licensed.
 */
#include "IFE_Builder.hpp"
#include "IFE_DataBlock.hpp"
#include "IFE_Memory.hpp"
#include "IFE_Types.hpp"
#include "IrisFileExtension.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#define IFE_REQUIRE(cond)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            return 1;                                                        \
        }                                                                    \
    } while (0)

namespace {

int test_header_roundtrip() {
    std::uint8_t buf[IFE::DATA_BLOCK_HEADER_SIZE]{};
    IFE::write_header(buf, IFE::RECOVERY_TAG::RESOURCE_TILE_TABLE,
                      0xAABBCCDD11223344ULL);
    const auto h = IFE::read_header(buf);
    IFE_REQUIRE(h.validation == 0xAABBCCDD11223344ULL);
    IFE_REQUIRE(h.tag() == IFE::RECOVERY_TAG::RESOURCE_TILE_TABLE);
    IFE_REQUIRE(h.recovery_tag ==
                static_cast<std::uint16_t>(
                    IFE::RECOVERY_TAG::RESOURCE_TILE_TABLE));
    return 0;
}

int test_file_magic_matches_legacy() {
    // Compile-time pin (also enforced inside IFE_DataBlock.hpp), repeated
    // here as a runtime check so the test binary fails loudly if a future
    // edit accidentally drops the static_assert.
    IFE_REQUIRE((IFE::IFE_FILE_MAGIC & 0xFFFFFFFFu) ==
                static_cast<std::uint32_t>(MAGIC_BYTES));
    // ASCII spelling of the magic in arena (little-endian) order. The 32-bit
    // hex literal `0x49726973` reads as 'I','r','i','s' (left-to-right hex
    // nibble pairs), but on little-endian targets memcpy stores the
    // *least-significant* byte first, producing the arena-order sequence
    // 's','i','r','I'. Building the buffer via memcpy keeps the test
    // independent of any internal helper.
    std::uint8_t buf[8]{};
    std::uint64_t magic = IFE::IFE_FILE_MAGIC;
    std::memcpy(buf, &magic, sizeof(magic));
    IFE_REQUIRE(buf[0] == 's');
    IFE_REQUIRE(buf[1] == 'i');
    IFE_REQUIRE(buf[2] == 'r');
    IFE_REQUIRE(buf[3] == 'I');
    // High half is zero (no extra distinguisher; the magic is just a
    // u64 zero-extension of the legacy 32-bit value).
    IFE_REQUIRE(buf[4] == 0);
    IFE_REQUIRE(buf[5] == 0);
    IFE_REQUIRE(buf[6] == 0);
    IFE_REQUIRE(buf[7] == 0);
    IFE_REQUIRE(IFE::is_file_magic(buf));
    return 0;
}

int test_builder_claim_block_layout() {
    auto         mem = IFE::Memory::create(64 * 1024);
    IFE::Builder b{mem};

    constexpr std::size_t body = 24;
    auto handle = b.claim_block(IFE::RECOVERY_TAG::RESOURCE_TILE_TABLE, body,
                                /*validation=*/0xDEADBEEFCAFEBABEULL);

    IFE_REQUIRE(handle.valid());
    IFE_REQUIRE(handle.body_size == body);
    IFE_REQUIRE(handle.tag == IFE::RECOVERY_TAG::RESOURCE_TILE_TABLE);
    IFE_REQUIRE(handle.body_offset ==
                handle.offset + IFE::DATA_BLOCK_HEADER_SIZE);
    // Body pointer matches the in-arena address.
    IFE_REQUIRE(handle.body == mem.data() + handle.body_offset);
    // Read back the universal preamble at the absolute offset.
    const auto h = IFE::read_header(mem.data() + handle.offset);
    IFE_REQUIRE(h.validation == 0xDEADBEEFCAFEBABEULL);
    IFE_REQUIRE(h.tag() == IFE::RECOVERY_TAG::RESOURCE_TILE_TABLE);
    // Preamble plus body sits inside the arena cursor.
    IFE_REQUIRE(mem.write_head() >=
                handle.offset + IFE::DATA_BLOCK_HEADER_SIZE + body);
    return 0;
}

int test_builder_file_header() {
    auto         mem = IFE::Memory::create(64 * 1024);
    IFE::Builder b{mem};

    auto fh = b.claim_file_header(/*body_bytes=*/32);
    IFE_REQUIRE(fh.valid());
    IFE_REQUIRE(fh.tag == IFE::RECOVERY_TAG::RESOURCE_HEADER);

    // Body at file_header.body should be zero-initialised by the arena.
    for (std::size_t i = 0; i < fh.body_size; ++i) {
        IFE_REQUIRE(fh.body[i] == 0);
    }
    // Universal preamble carries the file magic.
    const auto h = IFE::read_header(mem.data() + fh.offset);
    IFE_REQUIRE(h.validation == IFE::IFE_FILE_MAGIC);
    IFE_REQUIRE(h.tag() == IFE::RECOVERY_TAG::RESOURCE_HEADER);

    // is_file_magic recognises the FILE_HEADER's first 8 bytes.
    IFE_REQUIRE(IFE::is_file_magic(mem.data() + fh.offset));

    // validate_at via the const Memory::View reports a match.
    auto view = mem.view();
    IFE_REQUIRE(IFE::validate_at(view, fh.offset,
                                 IFE::RECOVERY_TAG::RESOURCE_HEADER));
    // and a mismatched tag is rejected.
    IFE_REQUIRE(!IFE::validate_at(view, fh.offset,
                                  IFE::RECOVERY_TAG::RESOURCE_TILE_TABLE));
    // Out-of-range offset is rejected without UB.
    IFE_REQUIRE(!IFE::validate_at(view, mem.capacity() + 1,
                                  IFE::RECOVERY_TAG::RESOURCE_HEADER));
    return 0;
}

int test_amend_pointer_basic() {
    auto         mem = IFE::Memory::create(64 * 1024);
    IFE::Builder b{mem};

    // Parent has a u64 slot at body offset 16 (within a 32-byte body).
    auto parent = b.claim_block(IFE::RECOVERY_TAG::RESOURCE_HEADER, 32);
    const std::uint64_t slot = parent.body_offset + 16;

    // Initially zero.
    IFE_REQUIRE(b.read_pointer(slot) == 0);

    // Claim a child and amend the slot.
    auto child = b.claim_block(IFE::RECOVERY_TAG::RESOURCE_TILE_TABLE, 8);
    b.amend_pointer(slot, child.offset);
    IFE_REQUIRE(b.read_pointer(slot) == child.offset);
    return 0;
}

int test_amend_pointer_oob() {
    auto         mem = IFE::Memory::create(64 * 1024);
    IFE::Builder b{mem};
    bool threw = false;
    try {
        b.amend_pointer(mem.capacity() - 4, 0xCAFE);  // straddles end
    } catch (const std::out_of_range&) {
        threw = true;
    }
    IFE_REQUIRE(threw);
    return 0;
}

int test_amend_pointer_concurrent() {
    // 32 threads race to amend distinct slots concurrently. Each thread also
    // claims its own child block, so this exercises lock-free claim_space
    // and amend_pointer in the same loop.
    constexpr int N_THREADS = 32;
    constexpr int N_PER_THR = 256;

    auto         mem = IFE::Memory::create(8 * 1024 * 1024);
    IFE::Builder b{mem};

    // One parent block holding N_THREADS*N_PER_THR slots of u64 each.
    constexpr std::size_t SLOTS = N_THREADS * N_PER_THR;
    auto parent = b.claim_block(IFE::RECOVERY_TAG::RESOURCE_HEADER,
                                SLOTS * sizeof(std::uint64_t));

    std::vector<std::thread>   threads;
    std::vector<std::uint64_t> child_offsets(SLOTS, 0);

    for (int t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < N_PER_THR; ++i) {
                const std::size_t k = static_cast<std::size_t>(t) * N_PER_THR
                                    + static_cast<std::size_t>(i);
                auto c = b.claim_block(IFE::RECOVERY_TAG::RESOURCE_TILE_TABLE,
                                       /*body_bytes=*/8);
                child_offsets[k] = c.offset;
                const std::uint64_t slot =
                    parent.body_offset + k * sizeof(std::uint64_t);
                b.amend_pointer(slot, c.offset);
            }
        });
    }
    for (auto& th : threads) th.join();

    // Every slot should now hold the child offset that thread k claimed.
    for (std::size_t k = 0; k < SLOTS; ++k) {
        const std::uint64_t slot =
            parent.body_offset + k * sizeof(std::uint64_t);
        IFE_REQUIRE(b.read_pointer(slot) == child_offsets[k]);
        IFE_REQUIRE(child_offsets[k] != 0);
    }
    // No two children landed at the same offset.
    {
        std::vector<std::uint64_t> sorted = child_offsets;
        std::sort(sorted.begin(), sorted.end());
        for (std::size_t i = 1; i < sorted.size(); ++i) {
            IFE_REQUIRE(sorted[i] != sorted[i - 1]);
        }
    }
    return 0;
}

int test_empty_builder_throws() {
    IFE::Builder b{IFE::Memory{}};
    bool         threw = false;
    try {
        (void)b.claim_block(IFE::RECOVERY_TAG::RESOURCE_HEADER, 8);
    } catch (const std::logic_error&) {
        threw = true;
    }
    IFE_REQUIRE(threw);
    return 0;
}

}  // namespace

int main() {
    int rc = 0;
    rc |= test_header_roundtrip();
    rc |= test_file_magic_matches_legacy();
    rc |= test_builder_claim_block_layout();
    rc |= test_builder_file_header();
    rc |= test_amend_pointer_basic();
    rc |= test_amend_pointer_oob();
    rc |= test_amend_pointer_concurrent();
    rc |= test_empty_builder_throws();
    if (rc == 0) {
        std::fprintf(stderr, "ife_datablock_tests: OK\n");
    }
    return rc;
}
