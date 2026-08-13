/**
 * @file ife_large_file_tests.cpp
 * @brief A slide larger than 4 GiB, so a u40 tile offset carries a fifth byte.
 *
 * `ife_v1_oracle_tests` says of its complete-file test that it "cannot reach
 * them: v1 requires every tile entry to address bytes inside the file, so a
 * 5-byte offset would need a file over 4 GB", and covers the packed widths by
 * driving STORE_TILE_OFFSETS on its own instead. That leaves one case
 * uncovered by anything: a *whole, validated* file in which the offsets a
 * reader follows genuinely exceed 32 bits. A `u40` truncated to a `u32`
 * survives every other gate in the project, because everywhere else the top
 * byte happens to be zero.
 *
 * The file is **sparse**. It is over 4 GiB long and occupies tens of kilobytes:
 * ftruncate sets the length, and only the bytes actually written are allocated.
 * The hole between the structural blocks and the tile data is never stored, so
 * this costs about what any other fixture costs.
 *
 * The structural bytes are the v1 snapshot's, copied verbatim and patched in
 * two places (FILE_SIZE, the first two tile-offset entries) -- the generated
 * handles read what the shipped encoder wrote, exactly as the oracle test
 * does; two descriptions of the format agreeing proves nothing, and this is a
 * case where they could agree and both be wrong.
 *
 * 64-bit hosts only (a 32-bit address space cannot map 4 GiB). Sparse on both
 * platforms: the mapping goes through Iris::MemoryArena (priv/IrisMemory.hpp),
 * which marks NTFS files sparse (FSCTL_SET_SPARSE) before mapping — matching
 * what ftruncate gives POSIX for free — so Windows no longer needs its own
 * gate.
 *
 * Usage: ife_large_file_tests <writable-directory>
 */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "IrisTypes.hpp"
#include "IrisCodecTypes.hpp"
#include "IrisMemory.hpp"           // priv/: the cross-platform sparse arena

#include "IFE_Blocks.hpp"           // under test: generated readers
#include "ife_corpus_path.hpp"       // corpus arg: directory (CTest) or file (Bazel)

namespace {

int g_failures = 0;

#define IFE_CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures; \
    } \
} while (0)

namespace b = ::IFE::blocks;
namespace k = ::IFE::constants;
namespace vt = ::IFE::vtables;

/// Report a failed Status in full. Without this a validation failure here says
/// only "false", and the block and offset are the whole diagnosis.
void report(const char* __what, const b::Status& __s) {
    if (__s) return;
    std::fprintf(stderr,
                 "FAIL: %s -- code %d on %s%s%s at byte %llu (found %llu, expected %llu)\n",
                 __what, static_cast<int>(__s.code), __s.block,
                 (*__s.field ? "." : ""), __s.field,
                 static_cast<unsigned long long>(__s.at),
                 static_cast<unsigned long long>(__s.found),
                 static_cast<unsigned long long>(__s.expected));
    ++g_failures;
}

constexpr std::uint64_t GiB = 1024ull * 1024ull * 1024ull;

/// Where the tile data sits. Comfortably past 4 GiB so that every tile offset
/// needs its fifth byte -- the whole point of the fixture. 64 KiB of headroom
/// keeps the first tile clear of the boundary itself, so a reader that is
/// merely off by a little still lands above 2^32 and the assertion stays about
/// the width rather than about the arithmetic.
constexpr std::uint64_t TILE_BASE = 4 * GiB + 64 * 1024;

constexpr std::uint32_t TILE0_SIZE = 4096;
constexpr std::uint32_t TILE1_SIZE = 8192;

constexpr std::uint32_t REVISION = 0x00C0FFEEu;

// The structural bytes are the snapshot's, copied at their own offsets (see
// ife_v1_oracle_tests.cpp); only FILE_SIZE and the first two tile-offset
// entries are rewritten, so the >4 GiB offsets a reader follows are the one
// thing no committed file ever carried.

/// Portable file lifecycle + allocation measurement. The arena owns the
/// mapping; truncating, unlinking and sizing the backing file are consumer
/// concerns this test exercises directly.

/// Create (or truncate to empty) the backing file, so a crashed earlier run
/// cannot leave stale bytes inside the mapped range.
bool create_empty_file(const std::string& __path) {
#ifdef _WIN32
    HANDLE h = CreateFileA(__path.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
#else
    int fd = ::open(__path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0) return false;
    ::close(fd);
    return true;
#endif
}

void remove_file(const std::string& __path) {
#ifdef _WIN32
    DeleteFileA(__path.c_str());
#else
    ::unlink(__path.c_str());
#endif
}

/// Bytes the filesystem actually allocated for the file. Flushes the mapping
/// first: until the dirty pages reach the filesystem, the size query reports
/// the pre-write state and the sparseness claim would be vacuously small.
std::uint64_t file_allocated_bytes(const std::string& __path,
                                   void* __base, std::size_t __len) {
#ifdef _WIN32
    if (__base) FlushViewOfFile(__base, __len);
    DWORD high = 0;
    const DWORD low = GetCompressedFileSizeA(__path.c_str(), &high);
    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) return 0;
    return (static_cast<std::uint64_t>(high) << 32) | low;
#else
    if (__base) ::msync(__base, __len, MS_SYNC);
    struct stat st {};
    if (::stat(__path.c_str(), &st) != 0) return 0;
    return static_cast<std::uint64_t>(st.st_blocks) * 512ull;
#endif
}

/// A sparse file, mapped through Iris::MemoryArena. Unlinks itself.
///
/// The arena (priv/IrisMemory.hpp) is what makes this portable: it marks NTFS
/// files sparse (FSCTL_SET_SPARSE) before mapping, so the file stays sparse on
/// Windows exactly as ftruncate makes it on POSIX. 64-bit only, like the
/// mapping itself.
class SparseFile {
public:
    SparseFile(const std::string& __path, std::uint64_t __size)
        : _path(__path) {
        if (!create_empty_file(__path)) return;
        _arena = Iris::MemoryArena::create_from_file(__path, __size);
    }

    ~SparseFile() {
        // Unmap and close before unlinking: DeleteFileA refuses to remove a
        // file that is still mapped, while POSIX tolerates it.
        //
        // close() rather than dropping the handle. Assigning an empty arena
        // only unmaps if this happens to hold the last reference; close()
        // releases regardless, which is the guarantee the unlink below needs.
        _arena.close();
        if (!_path.empty()) remove_file(_path);
    }

    SparseFile(const SparseFile&)            = delete;
    SparseFile& operator=(const SparseFile&) = delete;

    [[nodiscard]] bool          ok()   const noexcept { return static_cast<bool>(_arena); }
    [[nodiscard]] Iris::BYTE*   base() const noexcept { return _arena.base(); }
    [[nodiscard]] std::uint64_t size() const noexcept { return _arena.capacity(); }

    /// Bytes the filesystem actually allocated, which is the claim that makes
    /// this test cheap enough to run anywhere.
    [[nodiscard]] std::uint64_t allocated_bytes() const {
        return file_allocated_bytes(_path, _arena.base(), _arena.capacity());
    }

private:
    std::string       _path;
    Iris::MemoryArena _arena;
};

void test_v1_slide_above_4GiB(const std::string& __dir, const std::string& __corpus_dir) {
    const std::uint64_t file_size = TILE_BASE + TILE0_SIZE + TILE1_SIZE;
    IFE_CHECK(file_size > 0xFFFFFFFFull);

    const std::string path = __dir + "/ife_large_file.test_slide";
    SparseFile file(path, file_size);
    if (!file.ok()) {
        std::fprintf(stderr, "FAIL: could not create a %llu-byte sparse file at %s\n",
                     static_cast<unsigned long long>(file_size), path.c_str());
        ++g_failures;
        return;
    }

    // The structural bytes are the snapshot's, copied at their own offsets --
    // v1's layout is the file's layout, and the manifest digest pins it. Only
    // FILE_SIZE and the first two tile-offset entries are rewritten: the
    // whole point of this test is that everything else stays exactly as the
    // shipped encoder wrote it.
    std::vector<Iris::BYTE> snapshot;
    {
        std::FILE* in = std::fopen((__corpus_dir + "/v1_snapshot.test_slide").c_str(), "rb");
        if (!in) {
            std::fprintf(stderr, "FAIL: no snapshot in %s\n", __corpus_dir.c_str());
            ++g_failures;
            return;
        }
        std::fseek(in, 0, SEEK_END);
        const long n = std::ftell(in);
        std::fseek(in, 0, SEEK_SET);
        if (n > 0) snapshot.resize(static_cast<std::size_t>(n));
        const auto read = std::fread(snapshot.data(), 1, snapshot.size(), in);
        std::fclose(in);
        if (read != snapshot.size()) {
            std::fprintf(stderr, "FAIL: short read of the snapshot\n");
            ++g_failures;
            return;
        }
    }

    Iris::BYTE* p = file.base();
    std::memcpy(p, snapshot.data(), snapshot.size());

    // Patch FILE_SIZE: whole-file validation compares it against the size the
    // OS reports, which is now TILE_BASE plus the two relocated tiles.
    ::IFE::store<std::uint64_t>(p + vt::FILE_HEADER::offset::FILE_SIZE, file_size);

    // Patch the first two tile-offset entries to address the relocated tile
    // region. Their position follows from the 1.0 sizes alone: header, tile
    // table, then the three-entry extents array -- the same arithmetic v1's
    // place() did, without v1.
    constexpr ::IFE::Offset TILES_AT =
        vt::FILE_HEADER::header_size_v1_0 + vt::TILE_TABLE::header_size_v1_0
        + vt::LAYER_EXTENTS::header_size_v1_0
        + 3 * vt::LAYER_EXTENTS::entry_size_v1_0;
    Iris::BYTE* entry0 = p + TILES_AT + vt::TILE_OFFSETS::header_size
                      + 0 * vt::TILE_OFFSETS::entry_size_v1_0;
    Iris::BYTE* entry1 = p + TILES_AT + vt::TILE_OFFSETS::header_size
                      + 1 * vt::TILE_OFFSETS::entry_size_v1_0;
    ::IFE::store_u40(entry0 + vt::TILE_OFFSETS::entry::offset::OFFSET, TILE_BASE);
    ::IFE::store_u24(entry0 + vt::TILE_OFFSETS::entry::offset::SIZE, TILE0_SIZE);
    ::IFE::store_u40(entry1 + vt::TILE_OFFSETS::entry::offset::OFFSET, TILE_BASE + TILE0_SIZE);
    ::IFE::store_u24(entry1 + vt::TILE_OFFSETS::entry::offset::SIZE, TILE1_SIZE);

    // A byte at each end of the tile region, so the pages carrying the tile
    // data are really allocated and the offsets address something written
    // rather than hole.
    p[TILE_BASE]                               = 0xAB;
    p[TILE_BASE + TILE0_SIZE + TILE1_SIZE - 1] = 0xCD;

    // ---- the generated layer reads what v1 wrote -------------------------- //
    // The version comes from the file, not from VERSION_WRITTEN. This build
    // knows 1.1; v1 writes 1.0, and it never wrote METADATA's 1.1 CLINICAL
    // offset -- so a handle claiming 1.1 walks a pointer field that is still
    // zero and validate_deep follows it to byte 0. Reading major/minor first
    // and constructing with them is what IFE_Runtime's versioned_root does,
    // and is the whole bidirectional-compatibility idiom in three lines.
    constexpr ::IFE::Offset header_at = 0;   // the snapshot's FILE_HEADER is at SOF
    const b::FILE_HEADER bootstrap{p, header_at, file_size, UINT32_MAX};
    const std::uint32_t  version =
        (static_cast<std::uint32_t>(bootstrap.extension_major()) << 16) |
        bootstrap.extension_minor();
    IFE_CHECK(version == (1u << 16 | 0u));   // v1 writes 1.0

    const b::FILE_HEADER root{p, header_at, file_size, version};
    report("FILE_HEADER::validate", root.validate());
    report("FILE_HEADER::validate_deep", root.validate_deep());
    IFE_CHECK(root.file_size() == file_size);
    IFE_CHECK(root.file_revision() == REVISION);

    const auto tt = root.tile_table_offset();
    IFE_CHECK(tt.encoding() == k::TileEncodings::TILE_ENCODING_JPEG);
    IFE_CHECK(tt.x_extent() == 2048);   // the snapshot's width, as v1 wrote it

    // The assertions this file exists for. Each offset needs five bytes; a
    // reader that loaded four and zero-extended returns a value under 2^32,
    // and a reader that loaded eight picks up the neighbouring u24 SIZE.
    const auto to = tt.tile_offsets_offset();
    IFE_CHECK(to.count() == 84);   // the snapshot's tile count, all still valid

    IFE_CHECK(to.entry(0).offset() == TILE_BASE);
    IFE_CHECK(to.entry(0).offset() > 0xFFFFFFFFull);
    IFE_CHECK((to.entry(0).offset() >> 32) != 0);          // the fifth byte is set
    IFE_CHECK(to.entry(0).size_field() == TILE0_SIZE);

    IFE_CHECK(to.entry(1).offset() == TILE_BASE + TILE0_SIZE);
    IFE_CHECK(to.entry(1).offset() > 0xFFFFFFFFull);
    IFE_CHECK(to.entry(1).size_field() == TILE1_SIZE);

    // Truncation to 32 bits would leave these equal, since the two tiles are
    // 4096 bytes apart within the same 4 GiB block. Stating it separately
    // because it fails for a different reason than the equalities above.
    IFE_CHECK(to.entry(1).offset() - to.entry(0).offset() == TILE0_SIZE);

    // And the file really was sparse, which is the property that makes this
    // runnable in CI. Generous bound: the structural blocks, two touched tile
    // pages, and filesystem overhead -- not 4 GiB.
    const std::uint64_t allocated = file.allocated_bytes();
    IFE_CHECK(allocated < 16ull * 1024ull * 1024ull);
    std::printf("  %.2f GiB file, %llu KiB actually allocated\n",
                static_cast<double>(file_size) / static_cast<double>(GiB),
                static_cast<unsigned long long>(allocated / 1024));
}

}  // namespace

int main(int argc, const char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <writable-directory> <corpus-dir>\n", argv[0]);
        return 2;
    }
    // Both directories arrive as arguments rather than compile definitions:
    // as definitions they become C string literals, which a Windows path does
    // not survive.
    // Bazel passes "-" for the writable directory (resolved to $TEST_TMPDIR
    // here, since runfiles are not writable) and the runfiles path of a
    // corpus file; CTest passes both directories through unchanged.
    const char* const tmpdir = std::getenv("TEST_TMPDIR");
    const std::string writable =
        std::string(argv[1]) == "-" && tmpdir ? tmpdir : argv[1];
    test_v1_slide_above_4GiB(writable, ife_corpus_dir(argv[2]));

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ife_large_file_tests: all checks passed\n");
    return 0;
}
