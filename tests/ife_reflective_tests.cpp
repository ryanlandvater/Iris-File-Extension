/**
 * @file ife_reflective_tests.cpp
 * @brief Phase 6a tests: Reflective::Node walker + Builder::finalize.
 *
 * Covers:
 *  - Anonymous-arena round-trip: build FILE_HEADER + TILE_TABLE + a
 *    TILE_OFFSETS resource carrying an IFE_ARRAY<uint64_t>, finalize, walk
 *    via Reflective::Node, assert every field.
 *  - File-backed end-to-end: createFromFile, finalize, verify the file size
 *    is trimmed to write_head() and the on-disk bytes still parse.
 *  - IFE_ARRAY introspection via a generic Node + array<T>() accessor.
 *  - Tag/size/OOB error paths.
 *  - Multi-reader stress: N threads walk a finished arena concurrently and
 *    must produce identical entry sequences.
 *
 * @copyright Copyright (c) 2026 Ryan Landvater. MIT licensed.
 */
#include "IFE_Array.hpp"
#include "IFE_Builder.hpp"
#include "IFE_DataBlock.hpp"
#include "IFE_Memory.hpp"
#include "IFE_Reflective.hpp"
#include "IFE_Types.hpp"
#include "IFE_VTables.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
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

namespace v = IFE::vtables;

// ---------------------------------------------------------------------------
// Build a representative arena: FILE_HEADER + TILE_TABLE + TILE_OFFSETS
// resource block + an IFE_ARRAY<uint64> child carrying tile offsets. Wires
// the forward pointers and finalizes.
//
// The body of the FILE_HEADER (and TILE_TABLE / TILE_OFFSETS) is the *fixed
// layout body size* declared by the codegen vtable, i.e. header_size minus
// the 10-byte universal preamble.
// ---------------------------------------------------------------------------
struct BuiltLayout {
    std::uint64_t file_header_offset = 0;
    std::uint64_t tile_table_offset  = 0;
    std::uint64_t tile_offsets_offset = 0;
    std::uint64_t tile_array_offset  = 0;
    std::uint32_t x_extent = 0;
    std::uint32_t y_extent = 0;
    std::vector<std::uint64_t> offsets_in;
};

template <class T>
void store_le(IFE::Builder& /*b*/, std::uint8_t* p, T v) {
    std::memcpy(p, &v, sizeof(T));
}

BuiltLayout populate(IFE::Memory mem) {
    IFE::Builder b{std::move(mem)};
    BuiltLayout L;

    // FILE_HEADER body size = header_size - preamble.
    auto fh = b.claim_file_header(v::FILE_HEADER::header_size -
                                  IFE::DATA_BLOCK_HEADER_SIZE);
    L.file_header_offset = fh.offset;

    // Stamp version + revision now (FILE_SIZE / pointers come later).
    {
        const std::uint16_t major = 9;
        const std::uint16_t minor = 7;
        const std::uint32_t rev   = 0xDEADBEEFu;
        // FILE_HEADER offsets are inside the *whole block*; subtract the
        // preamble to get an offset inside `fh.body`.
        std::memcpy(fh.body + (v::FILE_HEADER::offset::EXTENSION_MAJOR -
                               IFE::DATA_BLOCK_HEADER_SIZE),
                    &major, sizeof(major));
        std::memcpy(fh.body + (v::FILE_HEADER::offset::EXTENSION_MINOR -
                               IFE::DATA_BLOCK_HEADER_SIZE),
                    &minor, sizeof(minor));
        std::memcpy(fh.body + (v::FILE_HEADER::offset::FILE_REVISION -
                               IFE::DATA_BLOCK_HEADER_SIZE),
                    &rev, sizeof(rev));
    }

    // TILE_TABLE block.
    auto tt = b.claim_block(IFE::RECOVERY_TAG::RESOURCE_TILE_TABLE,
                            v::TILE_TABLE::header_size -
                                IFE::DATA_BLOCK_HEADER_SIZE);
    L.tile_table_offset = tt.offset;
    L.x_extent = 0x12345678u;
    L.y_extent = 0xCAFEBABEu;
    std::memcpy(tt.body + (v::TILE_TABLE::offset::X_EXTENT -
                           IFE::DATA_BLOCK_HEADER_SIZE),
                &L.x_extent, sizeof(L.x_extent));
    std::memcpy(tt.body + (v::TILE_TABLE::offset::Y_EXTENT -
                           IFE::DATA_BLOCK_HEADER_SIZE),
                &L.y_extent, sizeof(L.y_extent));

    // TILE_OFFSETS resource block (header that points at the array).
    auto to_block = b.claim_block(IFE::RECOVERY_TAG::RESOURCE_TILE_OFFSETS,
                                  v::TILE_OFFSETS::header_size -
                                      IFE::DATA_BLOCK_HEADER_SIZE);
    L.tile_offsets_offset = to_block.offset;
    {
        const std::uint16_t entry_size   = 8;
        const std::uint32_t entry_number = 5;
        std::memcpy(to_block.body + (v::TILE_OFFSETS::offset::ENTRY_SIZE -
                                     IFE::DATA_BLOCK_HEADER_SIZE),
                    &entry_size, sizeof(entry_size));
        std::memcpy(to_block.body + (v::TILE_OFFSETS::offset::ENTRY_NUMBER -
                                     IFE::DATA_BLOCK_HEADER_SIZE),
                    &entry_number, sizeof(entry_number));
    }

    // IFE_ARRAY<uint64> with the actual tile offsets.
    L.offsets_in = {0x100, 0x200, 0x300, 0x400, 0x500};
    auto arr = b.claim_array<std::uint64_t>(L.offsets_in.size());
    L.tile_array_offset = arr.offset;
    for (std::size_t i = 0; i < L.offsets_in.size(); ++i) {
        arr.write(i, L.offsets_in[i]);
    }

    // Wire forward pointers:
    //   FILE_HEADER::TILE_TABLE_OFFSET -> tt.offset
    //   TILE_TABLE on its own doesn't directly point to TILE_OFFSETS in v1
    //     (TILE_OFFSETS_OFFSET in TILE_TABLE points at the resource block).
    b.amend_pointer(L.file_header_offset +
                        v::FILE_HEADER::offset::TILE_TABLE_OFFSET,
                    L.tile_table_offset);
    b.amend_pointer(L.tile_table_offset +
                        v::TILE_TABLE::offset::TILE_OFFSETS_OFFSET,
                    L.tile_offsets_offset);
    // (The IFE_ARRAY child has no slot in TILE_OFFSETS' fixed layout —
    // tests reach it via tile_array_offset directly.)

    const std::uint64_t size = b.finalize();
    (void)size;
    return L;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

int test_anonymous_roundtrip() {
    auto mem = IFE::Memory::create(64 * 1024);
    auto view = mem.view();           // pin the arena before dropping Builder
    BuiltLayout L;
    {
        L = populate(mem);            // Builder is constructed/destroyed inside
    }
    // `mem` is still alive but we walk via the snapshotted View to mirror
    // the documented async-read contract.
    IFE::Reflective::Node fh{view, L.file_header_offset};
    IFE_REQUIRE(fh.recovery_tag() == IFE::RECOVERY_TAG::RESOURCE_HEADER);
    IFE_REQUIRE(fh.validation() == IFE::IFE_FILE_MAGIC);
    IFE_REQUIRE(fh.has_fixed_layout());
    IFE_REQUIRE(fh.entry_count() == 8);
    IFE_REQUIRE(fh.body_offset() ==
                L.file_header_offset + IFE::DATA_BLOCK_HEADER_SIZE);
    IFE_REQUIRE(fh.body_size_hint() ==
                v::FILE_HEADER::header_size - IFE::DATA_BLOCK_HEADER_SIZE);

    // Each entry must have an absolute arena offset == block_offset +
    // codegen-declared field offset.
    const auto entries = fh.entries();
    bool saw_validation = false;
    bool saw_file_size  = false;
    for (const auto& e : entries) {
        if (e.name == "VALIDATION") {
            IFE_REQUIRE(e.offset == L.file_header_offset + 0);
            IFE_REQUIRE(e.size == 8);
            saw_validation = true;
        }
        if (e.name == "FILE_SIZE") {
            IFE_REQUIRE(e.offset ==
                        L.file_header_offset + v::FILE_HEADER::offset::FILE_SIZE);
            IFE_REQUIRE(e.size == 8);
            saw_file_size = true;
        }
    }
    IFE_REQUIRE(saw_validation);
    IFE_REQUIRE(saw_file_size);

    // Typed field<>() round-trip.
    IFE_REQUIRE(fh.field<std::uint16_t>("EXTENSION_MAJOR") == 9);
    IFE_REQUIRE(fh.field<std::uint16_t>("EXTENSION_MINOR") == 7);
    IFE_REQUIRE(fh.field<std::uint32_t>("FILE_REVISION") == 0xDEADBEEFu);
    IFE_REQUIRE(fh.field<std::uint64_t>("VALIDATION") == IFE::IFE_FILE_MAGIC);

    // FILE_SIZE was stamped by finalize and equals the post-finalize write
    // head (the Memory handle is still alive so we can read it back).
    IFE_REQUIRE(fh.field<std::uint64_t>("FILE_SIZE") == mem.write_head());

    // Forward pointer to TILE_TABLE.
    IFE_REQUIRE(fh.pointer("TILE_TABLE_OFFSET") == L.tile_table_offset);

    // Walk to TILE_TABLE via Node::child and assert its scalars.
    IFE::Reflective::Node tt = fh.child("TILE_TABLE_OFFSET");
    IFE_REQUIRE(tt.recovery_tag() == IFE::RECOVERY_TAG::RESOURCE_TILE_TABLE);
    IFE_REQUIRE(tt.field<std::uint32_t>("X_EXTENT") == L.x_extent);
    IFE_REQUIRE(tt.field<std::uint32_t>("Y_EXTENT") == L.y_extent);

    // Walk to TILE_OFFSETS via TILE_TABLE.
    IFE::Reflective::Node to_node = tt.child("TILE_OFFSETS_OFFSET");
    IFE_REQUIRE(to_node.recovery_tag() ==
                IFE::RECOVERY_TAG::RESOURCE_TILE_OFFSETS);

    // Walk the IFE_ARRAY directly (no slot in TILE_OFFSETS' fixed layout).
    IFE::Reflective::Node arr_node{view, L.tile_array_offset};
    IFE_REQUIRE(arr_node.recovery_tag() == IFE::RECOVERY_TAG::ARRAY_UINT64);
    IFE_REQUIRE(!arr_node.has_fixed_layout());
    IFE_REQUIRE(arr_node.entries().empty());
    IFE_REQUIRE(arr_node.body_size_hint() ==
                L.offsets_in.size() * sizeof(std::uint64_t));

    // Read the array back via ArrayView (the same path the typed
    // accessor uses internally).
    IFE::ArrayView<std::uint64_t> av(view, L.tile_array_offset);
    IFE_REQUIRE(av.size() == L.offsets_in.size());
    for (std::size_t i = 0; i < av.size(); ++i) {
        IFE_REQUIRE(av.at(i) == L.offsets_in[i]);
    }
    return 0;
}

int test_finalize_no_file_header_throws() {
    auto mem = IFE::Memory::create(4096);
    IFE::Builder b{mem};
    bool threw = false;
    try { (void)b.finalize(); }
    catch (const std::logic_error&) { threw = true; }
    IFE_REQUIRE(threw);

    // Empty builder also throws.
    IFE::Builder empty{IFE::Memory{}};
    threw = false;
    try { (void)empty.finalize(); }
    catch (const std::logic_error&) { threw = true; }
    IFE_REQUIRE(threw);
    return 0;
}

int test_finalize_idempotent_and_stamps_file_size() {
    auto mem = IFE::Memory::create(64 * 1024);
    IFE::Builder b{mem};
    auto fh = b.claim_file_header(v::FILE_HEADER::header_size -
                                  IFE::DATA_BLOCK_HEADER_SIZE);
    (void)b.claim_block(IFE::RECOVERY_TAG::RESOURCE_TILE_TABLE,
                        v::TILE_TABLE::header_size -
                            IFE::DATA_BLOCK_HEADER_SIZE);
    const std::uint64_t s1 = b.finalize();
    IFE_REQUIRE(s1 == mem.write_head());

    // Re-stamp must observe the same write head; idempotence holds.
    const std::uint64_t s2 = b.finalize();
    IFE_REQUIRE(s2 == s1);

    // Verify the FILE_SIZE slot in the arena directly.
    std::uint64_t slot = 0;
    std::memcpy(&slot,
                mem.data() + fh.offset + v::FILE_HEADER::offset::FILE_SIZE,
                sizeof(slot));
    IFE_REQUIRE(slot == s1);

    // A subsequent claim + re-finalize advances the stamped size.
    (void)b.claim_block(IFE::RECOVERY_TAG::RESOURCE_METADATA,
                        v::METADATA::header_size - IFE::DATA_BLOCK_HEADER_SIZE);
    const std::uint64_t s3 = b.finalize();
    IFE_REQUIRE(s3 > s1);
    std::memcpy(&slot,
                mem.data() + fh.offset + v::FILE_HEADER::offset::FILE_SIZE,
                sizeof(slot));
    IFE_REQUIRE(slot == s3);
    return 0;
}

int test_file_backed_truncate_and_reopen() {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() /
                         "ife_reflective_test.iris";
    std::error_code ec;
    fs::remove(tmp, ec);  // tolerate stale leftover

    constexpr std::size_t cap = 1 * 1024 * 1024;  // 1 MiB sparse reservation
    BuiltLayout L;
    std::uint64_t finalized_size = 0;
    {
        auto mem = IFE::Memory::createFromFile(tmp, cap);
        IFE::Builder probe{mem};
        // Build using the populate() helper which itself constructs a
        // Builder; do it inline here so we keep `mem` alive for view().
        L = populate(mem);
        finalized_size = mem.write_head();
    }

    // After all handles drop, the file on disk must be exactly
    // `finalized_size` bytes (truncate ran during finalize) and still parse
    // as a valid IFE arena.
    IFE_REQUIRE(fs::exists(tmp));
    IFE_REQUIRE(fs::file_size(tmp) == finalized_size);

    // Read the file bytes back and verify the universal preamble + magic.
    // (The read-mode Memory factory is Phase 6b; this test goes via
    // std::ifstream on purpose to validate persistence end-to-end.)
    std::ifstream in(tmp, std::ios::binary);
    IFE_REQUIRE(in.good());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(finalized_size));
    in.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    IFE_REQUIRE(in.good());

    // First WRITE_HEAD_BYTES are the cursor (low 63 bits == finalized size).
    std::uint64_t cursor = 0;
    std::memcpy(&cursor, bytes.data(), sizeof(cursor));
    cursor &= IFE::STREAM_OFFSET_MASK;
    IFE_REQUIRE(cursor == finalized_size);

    // FILE_HEADER lands at WRITE_HEAD_BYTES and must carry IFE_FILE_MAGIC
    // followed by the FILE_HEADER recovery tag.
    IFE_REQUIRE(L.file_header_offset == IFE::WRITE_HEAD_BYTES);
    IFE_REQUIRE(IFE::is_file_magic(bytes.data() + L.file_header_offset));
    std::uint16_t tag = 0;
    std::memcpy(&tag,
                bytes.data() + L.file_header_offset +
                    IFE::DATA_BLOCK_RECOVERY_OFFSET,
                sizeof(tag));
    IFE_REQUIRE(tag == static_cast<std::uint16_t>(
                           IFE::RECOVERY_TAG::RESOURCE_HEADER));

    // FILE_SIZE slot inside the FILE_HEADER body matches the on-disk size.
    std::uint64_t persisted_size = 0;
    std::memcpy(&persisted_size,
                bytes.data() + L.file_header_offset +
                    v::FILE_HEADER::offset::FILE_SIZE,
                sizeof(persisted_size));
    IFE_REQUIRE(persisted_size == finalized_size);

    fs::remove(tmp, ec);
    return 0;
}

int test_node_construction_errors() {
    auto mem = IFE::Memory::create(64 * 1024);
    auto view = mem.view();

    // Empty view -> throws.
    bool threw = false;
    try { IFE::Reflective::Node n(IFE::Memory::View{}, 0); }
    catch (const std::invalid_argument&) { threw = true; }
    IFE_REQUIRE(threw);

    // OOB offset -> throws.
    threw = false;
    try { IFE::Reflective::Node n(view, view.capacity() + 100); }
    catch (const std::invalid_argument&) { threw = true; }
    IFE_REQUIRE(threw);

    // Offset past capacity-by-less-than-preamble -> throws.
    threw = false;
    try {
        IFE::Reflective::Node n(view, view.capacity() - 1);
    } catch (const std::invalid_argument&) { threw = true; }
    IFE_REQUIRE(threw);

    // A FILE_HEADER built into a too-small region (capacity smaller than
    // the FILE_HEADER body): the populate path would itself OOM, so we
    // instead place a forged FILE_HEADER preamble too close to the end
    // of the arena and assert the body bounds check fires.
    {
        IFE::Builder b{mem};
        // Push the cursor close to the end so a claimed FILE_HEADER would
        // not have room — but here we just hand-write a preamble at the
        // tail of the arena and probe via Node directly.
        const std::uint64_t off = view.capacity() - IFE::DATA_BLOCK_HEADER_SIZE - 4;
        IFE::write_header(const_cast<std::uint8_t*>(view.data()) + off,
                          IFE::RECOVERY_TAG::RESOURCE_HEADER,
                          IFE::IFE_FILE_MAGIC);
        threw = false;
        try { IFE::Reflective::Node n(view, off); }
        catch (const std::invalid_argument&) { threw = true; }
        IFE_REQUIRE(threw);
    }
    return 0;
}

int test_field_accessor_errors() {
    auto mem = IFE::Memory::create(64 * 1024);
    IFE::Builder b{mem};
    auto fh = b.claim_file_header(v::FILE_HEADER::header_size -
                                  IFE::DATA_BLOCK_HEADER_SIZE);
    (void)b.finalize();

    auto view = mem.view();
    IFE::Reflective::Node n(view, fh.offset);

    // Unknown field -> std::out_of_range.
    bool threw = false;
    try { (void)n.field<std::uint32_t>("DOES_NOT_EXIST"); }
    catch (const std::out_of_range&) { threw = true; }
    IFE_REQUIRE(threw);

    // Size mismatch -> std::invalid_argument (FILE_REVISION is 4 bytes,
    // we ask for 8).
    threw = false;
    try { (void)n.field<std::uint64_t>("FILE_REVISION"); }
    catch (const std::invalid_argument&) { threw = true; }
    IFE_REQUIRE(threw);

    // NULL pointer follow-through -> out_of_range. METADATA_OFFSET was
    // never amended, so it reads zero.
    threw = false;
    try { (void)n.child("METADATA_OFFSET"); }
    catch (const std::out_of_range&) { threw = true; }
    IFE_REQUIRE(threw);

    threw = false;
    try { (void)n.array<std::uint64_t>("METADATA_OFFSET"); }
    catch (const std::out_of_range&) { threw = true; }
    IFE_REQUIRE(threw);
    return 0;
}

int test_array_typed_accessor_tag_mismatch() {
    // Build an IFE_ARRAY<float> and try to read it as ArrayView<uint64> via
    // Reflective::Node::array<>. The mismatch must be caught by the
    // ArrayView constructor.
    auto mem = IFE::Memory::create(64 * 1024);
    IFE::Builder b{mem};
    auto fh = b.claim_file_header(v::FILE_HEADER::header_size -
                                  IFE::DATA_BLOCK_HEADER_SIZE);
    auto fa = b.claim_array<float>(4);
    fa.write(0, 1.0f); fa.write(1, 2.0f); fa.write(2, 3.0f); fa.write(3, 4.0f);
    // Park the float array offset in TILE_TABLE_OFFSET (same 8-byte slot
    // shape; we're only exercising the type-mismatch path).
    b.amend_pointer(fh.offset + v::FILE_HEADER::offset::TILE_TABLE_OFFSET,
                    fa.offset);
    (void)b.finalize();

    auto view = mem.view();
    IFE::Reflective::Node n(view, fh.offset);

    bool threw = false;
    try { (void)n.array<std::uint64_t>("TILE_TABLE_OFFSET"); }
    catch (const std::invalid_argument&) { threw = true; }
    IFE_REQUIRE(threw);

    // The matching type does succeed.
    auto av = n.array<float>("TILE_TABLE_OFFSET");
    IFE_REQUIRE(av.size() == 4);
    IFE_REQUIRE(av.at(0) == 1.0f);
    IFE_REQUIRE(av.at(3) == 4.0f);
    return 0;
}

int test_concurrent_readers() {
    auto mem = IFE::Memory::create(64 * 1024);
    auto view = mem.view();
    BuiltLayout L = populate(mem);

    constexpr int kThreads = 8;
    constexpr int kIters   = 256;
    std::atomic<int> failures{0};

    auto worker = [&](int /*tid*/) {
        for (int i = 0; i < kIters; ++i) {
            try {
                IFE::Reflective::Node fh(view, L.file_header_offset);
                if (fh.recovery_tag() != IFE::RECOVERY_TAG::RESOURCE_HEADER) {
                    failures.fetch_add(1);
                    return;
                }
                if (fh.entry_count() != 8) {
                    failures.fetch_add(1);
                    return;
                }
                if (fh.field<std::uint16_t>("EXTENSION_MAJOR") != 9) {
                    failures.fetch_add(1);
                    return;
                }
                IFE::Reflective::Node tt = fh.child("TILE_TABLE_OFFSET");
                if (tt.field<std::uint32_t>("X_EXTENT") != L.x_extent) {
                    failures.fetch_add(1);
                    return;
                }
                IFE::ArrayView<std::uint64_t> av(view, L.tile_array_offset);
                if (av.size() != L.offsets_in.size()) {
                    failures.fetch_add(1);
                    return;
                }
                for (std::size_t k = 0; k < av.size(); ++k) {
                    if (av.at(k) != L.offsets_in[k]) {
                        failures.fetch_add(1);
                        return;
                    }
                }
            } catch (...) {
                failures.fetch_add(1);
                return;
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) threads.emplace_back(worker, t);
    for (auto& th : threads) th.join();

    IFE_REQUIRE(failures.load() == 0);
    return 0;
}

}  // namespace

int main() {
    if (test_anonymous_roundtrip())                  return 1;
    if (test_finalize_no_file_header_throws())       return 1;
    if (test_finalize_idempotent_and_stamps_file_size()) return 1;
    if (test_file_backed_truncate_and_reopen())      return 1;
    if (test_node_construction_errors())             return 1;
    if (test_field_accessor_errors())                return 1;
    if (test_array_typed_accessor_tag_mismatch())    return 1;
    if (test_concurrent_readers())                   return 1;
    std::fprintf(stderr, "ife_reflective_tests: all passed\n");
    return 0;
}
