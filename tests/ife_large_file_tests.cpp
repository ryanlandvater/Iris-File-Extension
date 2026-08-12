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
 * v1 writes and the generated handles read, exactly as the oracle test does --
 * two descriptions of the format agreeing proves nothing, and this is a case
 * where they could agree and both be wrong.
 *
 * POSIX only, and CMake builds it only there: ftruncate on NTFS writes zeros
 * unless the file is first marked sparse with FSCTL_SET_SPARSE, which would
 * turn a 32 KB test into a 4 GB one.
 *
 * Usage: ife_large_file_tests <writable-directory>
 */
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "IrisTypes.hpp"
#include "IrisCodecTypes.hpp"

#include "IrisCodecExtension.hpp"   // the oracle: v1 writers
#include "IFE_Blocks.hpp"           // under test: generated readers

namespace {

int g_failures = 0;

#define IFE_CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
        ++g_failures; \
    } \
} while (0)

namespace S = IrisCodec::Serialization;
namespace b = ::IFE::blocks;
namespace k = ::IFE::constants;

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
constexpr std::uint32_t X_EXTENT = 512;
constexpr std::uint32_t Y_EXTENT = 256;

/// A sparse file, mapped. Unlinks itself.
class SparseFile {
public:
    SparseFile(const std::string& __path, std::uint64_t __size)
        : _path(__path), _size(__size) {
        _fd = ::open(_path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
        if (_fd < 0) return;
        // Sets the length without writing: the file is all hole until stored to.
        if (::ftruncate(_fd, static_cast<off_t>(_size)) != 0) { close_fd(); return; }
        void* p = ::mmap(nullptr, static_cast<size_t>(_size),
                         PROT_READ | PROT_WRITE, MAP_SHARED, _fd, 0);
        if (p == MAP_FAILED) { close_fd(); return; }
        _base = static_cast<Iris::BYTE*>(p);
    }

    ~SparseFile() {
        if (_base) ::munmap(_base, static_cast<size_t>(_size));
        close_fd();
        if (!_path.empty()) ::unlink(_path.c_str());
    }

    SparseFile(const SparseFile&)            = delete;
    SparseFile& operator=(const SparseFile&) = delete;

    [[nodiscard]] bool          ok()   const noexcept { return _base != nullptr; }
    [[nodiscard]] Iris::BYTE*   base() const noexcept { return _base; }
    [[nodiscard]] std::uint64_t size() const noexcept { return _size; }

    /// Bytes the filesystem actually allocated, which is the claim that makes
    /// this test cheap enough to run anywhere.
    [[nodiscard]] std::uint64_t allocated_bytes() const {
        // Flush first: until the dirty pages reach the filesystem, st_blocks
        // reports zero and the measurement would be vacuously small rather
        // than evidence of anything.
        if (_base) ::msync(_base, static_cast<size_t>(_size), MS_SYNC);
        struct stat st {};
        if (_fd < 0 || ::fstat(_fd, &st) != 0) return 0;
        return static_cast<std::uint64_t>(st.st_blocks) * 512ull;
    }

private:
    void close_fd() { if (_fd >= 0) { ::close(_fd); _fd = -1; } }

    std::string   _path;
    std::uint64_t _size = 0;
    int           _fd   = -1;
    Iris::BYTE*   _base = nullptr;
};

void test_v1_slide_above_4GiB(const std::string& __dir) {
    using namespace IrisCodec;

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

    // ---- what v1 will encode --------------------------------------------- //
    Iris::LayerExtents extents = {
        {.xTiles = 2, .yTiles = 1, .scale = 1.0f, .downsample = 1.0f},
    };
    Abstraction::TileTable::Layers layers = {
        {{.offset = TILE_BASE, .size = TILE0_SIZE},
         {.offset = TILE_BASE + TILE0_SIZE, .size = TILE1_SIZE}},
    };

    // The structural blocks all live in the first few hundred bytes; only the
    // tile data is out past 4 GiB. Laying them out with v1's own SIZE_*, as
    // every other oracle fixture does.
    IrisCodec::Offset at = 0;
    auto place = [&at](IrisCodec::Size bytes) { const auto here = at; at += bytes; return here; };

    const IrisCodec::Offset header_at  = place(S::FILE_HEADER::HEADER_SIZE);
    const IrisCodec::Offset table_at   = place(S::TILE_TABLE::HEADER_SIZE);
    const IrisCodec::Offset extents_at = place(S::SIZE_EXTENTS(extents));
    const IrisCodec::Offset tiles_at   = place(S::SIZE_TILE_OFFSETS(layers));
    const IrisCodec::Offset meta_at    = place(S::METADATA::HEADER_SIZE);
    IFE_CHECK(at < TILE_BASE);   // the structure must not reach into the hole

    Iris::BYTE* p = file.base();

    S::STORE_EXTENTS(p, extents_at, extents);
    S::STORE_TILE_OFFSETS(p, tiles_at, layers);
    S::STORE_TILE_TABLE(p, S::TileTableCreateInfo{
        .tileTableOffset    = table_at,
        .encoding           = IrisCodec::TILE_ENCODING_JPEG,
        .format             = Iris::FORMAT_R8G8B8A8,
        .cipherOffset       = S::NULL_OFFSET,
        .tilesOffset        = tiles_at,
        .layerExtentsOffset = extents_at,
        .layers             = static_cast<std::uint32_t>(extents.size()),
        .widthPixels        = X_EXTENT,
        .heightPixels       = Y_EXTENT});

    S::STORE_METADATA(p, S::MetadataCreateInfo{
        .metadataOffset  = meta_at,
        .codecVersion    = {1, 0, 0},
        .attributes      = S::NULL_OFFSET,
        .images          = S::NULL_OFFSET,
        .ICC_profile     = S::NULL_OFFSET,
        .annotations     = S::NULL_OFFSET,
        .micronsPerPixel = 0.25f,
        .magnification   = 40.0f});

    // A byte at each end of the tile region, so the pages carrying the tile
    // data are really allocated and the offsets address something written
    // rather than hole. v1 validates that they are in bounds either way.
    p[TILE_BASE]                                     = 0xAB;
    p[TILE_BASE + TILE0_SIZE + TILE1_SIZE - 1]       = 0xCD;

    // Performs whole-file validation, so this call is itself a check that v1
    // accepts a file this size.
    S::STORE_FILE_HEADER(p, S::HeaderCreateInfo{
        .fileSize        = file_size,
        .revision        = REVISION,
        .tileTableOffset = table_at,
        .metadataOffset  = meta_at});

    // ---- the generated layer reads what v1 wrote -------------------------- //
    // The version comes from the file, not from VERSION_WRITTEN. This build
    // knows 1.1; v1 writes 1.0, and it never wrote METADATA's 1.1 CLINICAL
    // offset -- so a handle claiming 1.1 walks a pointer field that is still
    // zero and validate_deep follows it to byte 0. Reading major/minor first
    // and constructing with them is what IFE_Runtime's versioned_root does,
    // and is the whole bidirectional-compatibility idiom in three lines.
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
    IFE_CHECK(tt.x_extent() == X_EXTENT);

    // The assertions this file exists for. Each offset needs five bytes; a
    // reader that loaded four and zero-extended returns a value under 2^32,
    // and a reader that loaded eight picks up the neighbouring u24 SIZE.
    const auto to = tt.tile_offsets_offset();
    IFE_CHECK(to.count() == 2);

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
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <writable-directory>\n", argv[0]);
        return 2;
    }
    // The directory arrives as an argument rather than a compile definition:
    // as a definition it becomes a C string literal, which a Windows path does
    // not survive. Same reason ife_runtime_tests takes its writer path this way.
    test_v1_slide_above_4GiB(std::string(argv[1]));

    if (g_failures) {
        std::fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ife_large_file_tests: all checks passed\n");
    return 0;
}
