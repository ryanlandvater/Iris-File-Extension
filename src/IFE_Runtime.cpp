/**
 * @file IFE_Runtime.cpp
 * @brief The semantic layer on the generated block handles.
 * @copyright Iris Developers, 2025-2026
 *
 * A reader body here is a sequence of accessor calls over generated handles:
 * no byte offsets, no `LOAD_U*`, no hand-threaded version branches, no
 * `#ifdef __EMSCRIPTEN__`.
 *
 * The part that is genuinely semantic stays hand-written: the traversal
 * order, which blocks are optional, how a flat tile-offset array is split
 * across layers, the downsample computation, and what a recovery scan looks
 * for. None of that is derivable from a byte layout.
 */

#include "IrisFileExtension.hpp"
#include "IFE_Primitives.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace Iris::File {

namespace k  = ::Iris::File::constants;
namespace b  = ::Iris::File::blocks;

namespace {

/// The single place a generated Status becomes an Iris::Result.
///
/// Every std::string in this layer is built here. Generated validators report
/// a code plus operands and never format anything (they are noexcept and
/// allocation-free); turning that into prose is the runtime's job, and doing
/// it in one function is what keeps the wording consistent across sixteen
/// blocks without sixteen message templates.
Result to_result(const b::Status& __status) noexcept try {
    if (__status) return IRIS_SUCCESS;

    const std::string where = std::string(__status.block) +
                              (*__status.field ? std::string(".") + __status.field : "") +
                              " at byte " + std::to_string(__status.at);
    switch (__status.code) {
        case b::Check::NOT_CONSTRUCTED:
            return {IRIS_FAILURE, where + " could not be read: the block does not fit within the file"};
        case b::Check::OUT_OF_BOUNDS:
            return {IRIS_FAILURE, where + " points outside the file"};
        case b::Check::BAD_VALIDATION:
            return {IRIS_FAILURE, where + " stores " + std::to_string(__status.found) +
                                  " as its own offset but sits at " + std::to_string(__status.expected) +
                                  "; the pointer that led here is wrong, or the block was moved"};
        case b::Check::BAD_RECOVERY:
            return {IRIS_FAILURE, where + " carries recovery tag " + std::to_string(__status.found) +
                                  " where " + std::to_string(__status.expected) + " was expected"};
        case b::Check::BAD_CONSTANT:
            return {IRIS_FAILURE, where + " holds " + std::to_string(__status.found) +
                                  " instead of the constant " + std::to_string(__status.expected)};
        case b::Check::BAD_STRIDE:
            return {IRIS_FAILURE, where + " declares a stride of " + std::to_string(__status.found) +
                                  ", narrower than the " + std::to_string(__status.expected) +
                                  " bytes an entry requires"};
        case b::Check::ARRAY_OVERRUN:
            return {IRIS_FAILURE, where + " declares " + std::to_string(__status.found) +
                                  " bytes of entries but only " + std::to_string(__status.expected) +
                                  " remain in the file"};
        case b::Check::CYCLE:
            return {IRIS_FAILURE, where + " is reached by an offset chain that returns to a block "
                                  "already on the path"};
        case b::Check::TOO_DEEP:
            return {IRIS_FAILURE, where + " is nested " + std::to_string(__status.found) +
                                  " deep, past the limit of " + std::to_string(__status.expected) +
                                  " this reader will follow"};
        case b::Check::BAD_NESTED_VALUE:
            return {IRIS_FAILURE, where + " is a nested attribute value of " +
                                  std::to_string(__status.found) + " bytes, which is not a whole "
                                  "number of " + std::to_string(__status.expected) +
                                  "-byte offsets"};
        case b::Check::CONFORMANCE:
            return {IRIS_FAILURE, where + " violates a normative requirement of the specification"};
        case b::Check::PAYLOAD_OVERRUN:
            return {IRIS_FAILURE, where + " declares " + std::to_string(__status.found) +
                                  " bytes of header and payload but only " +
                                  std::to_string(__status.expected) + " remain in the file"};
        case b::Check::VALUE_TOO_WIDE:
            return {IRIS_FAILURE, where + " was given " + std::to_string(__status.found) +
                                  ", wider than the field holds (at most " +
                                  std::to_string(__status.expected) + "); nothing was written"};
        case b::Check::OK: break;
    }
    return {IRIS_FAILURE, where + " failed validation"};
} catch (const std::bad_alloc&) {
    // noexcept, and the only thing above that can throw is the string building.
    return {IRIS_FAILURE, "validation failed (out of memory formatting the diagnostic)"};
}

[[noreturn]] void fail(const b::Status& __status) {
    throw std::runtime_error(to_result(__status).message);
}

/// An OPTIONAL slot on the read path. NULL_OFFSET is absence and reads as
/// absence; anything else must be the block it claims to be, or the read
/// fails -- exactly as a required slot does.
///
/// The `if (const auto x = slot())` form this replaces tests the handle's
/// bool, which demands validate() since RC-10.1. On its own that would turn a
/// present block that fails into an ABSENT one, silently: the caller gets a
/// slide with no ICC profile and no sign anything was lost. A missing block
/// that is reported is honest; one that quietly vanishes is not.
template <class Handle>
bool present(const Handle& __h) {
    if (__h.__offset == k::NULL_OFFSET) return false;
    if (!__h) fail(__h.validate());
    return true;
}


/// How deep a chain of nested attribute structures may go.
///
/// The generated block graph is bounded by MAX_BLOCK_DEPTH; nesting is the one
/// place a file chooses its own depth, so it needs its own bound. Reaching the
/// root's attributes already spends three levels (file header, metadata,
/// attributes), and real DICOM nesting is two or three deep -- a sequence of
/// items, occasionally holding a sequence of its own.
///
/// One constant, used by all three walks. Validation has to reject exactly
/// what the other two cannot handle: a bound enforced only where the tree is
/// lifted would let a file validate and then throw on abstraction, which is
/// the one thing validating first is supposed to prevent.
constexpr Size MAX_ATTRIBUTE_DEPTH = b::MAX_BLOCK_DEPTH - 3;

/// Attribute-block offsets a walk has already finished with.
///
/// The depth bound alone does not bound the *work*. VisitPath carries the
/// ancestry, deliberately, so a structure reached twice by different keys is
/// two legitimate visits -- and nesting makes the fan-out attacker-controlled,
/// since a single value names as many structures as its length allows. Thirteen
/// blocks each naming forty offsets into the next is five kilobytes on disk,
/// contains no cycle, exceeds no depth, and takes 40^13 visits.
///
/// A block's verdict does not depend on how it was reached, so finishing one
/// and remembering it makes every later arrival free and the walk linear in
/// the number of distinct blocks. Recorded on *completion*, never on entry:
/// marking early would let a structure that reaches itself find its own entry
/// and report success, which is the cycle the path exists to catch.
using VisitedBlocks = std::unordered_set<Offset>;

/// Deep-validate the nested attribute structures the generated walk cannot
/// reach.
///
/// `points_to` describes a *field*, and these edges live inside an opaque byte
/// run whose length is data — so no schema linkage can express them and the
/// generated validator does not follow them. This is the escape valve the
/// specification names: a rule that will not fit the capped vocabulary is
/// prose in the document and hand-written here.
///
/// The path carries the chain of attributes blocks rather than every block
/// visited, so a structure nested twice from different keys is two legitimate
/// visits while one that reaches itself is a cycle.
b::Status validate_nested_attributes(const b::ATTRIBUTES& __attrs, b::VisitPath& __path,
                                     VisitedBlocks& __seen, Size __depth) {
    // Three distinct failures, reported apart. They were once one code, and a
    // file that was merely too deep reported a cycle -- sending a reader to
    // hunt for a loop that was not there, and making a test unable to say
    // which guard had fired.
    if (__depth > MAX_ATTRIBUTE_DEPTH)
        return {b::Check::TOO_DEEP, b::ATTRIBUTES::type, "", __depth,
                MAX_ATTRIBUTE_DEPTH, __attrs.__offset};
    if (__path.contains(__attrs.__offset))
        return {b::Check::CYCLE, b::ATTRIBUTES::type, "", __attrs.__offset, 0,
                __attrs.__offset};
    // Unreachable while MAX_ATTRIBUTE_DEPTH stays below MAX_BLOCK_DEPTH, which
    // it is by construction -- kept because the path is shared machinery and
    // the bound above it is not this function's to guarantee.
    // Already finished with, by another path. Not a cycle -- the path check
    // above has ruled that out -- just the same structure named twice, which
    // the format allows and which is what makes the fan-out worth bounding.
    if (__seen.count(__attrs.__offset)) return {};
    if (!__path.push(__attrs.__offset))
        return {b::Check::TOO_DEEP, b::ATTRIBUTES::type, "", __path.depth,
                b::MAX_BLOCK_DEPTH, __attrs.__offset};
    // Popped on every exit, not only the successful one. Today each early
    // return abandons the whole walk, so a dirty path is invisible -- but the
    // SliceError vocabulary exists so a caller can report more than the first
    // fault, and the day one resumes, stale ancestors would make an unrelated
    // sibling report a cycle it does not have.
    struct PathScope {
        b::VisitPath& path;
        ~PathScope() { path.pop(); }
    } __scope{__path};

    std::vector<AttributeSlice> slices;
    std::uint32_t at = 0;
    switch (slice_attributes(__attrs, slices, &at)) {
        case SliceError::NONE: break;
        case SliceError::UNREADABLE:
            return {b::Check::NOT_CONSTRUCTED, b::ATTRIBUTES::type, "SIZES_OFFSET",
                    __attrs.__offset, 0, __attrs.__offset};
        case SliceError::OVERRUN:
            return {b::Check::ARRAY_OVERRUN, b::ATTRIBUTE_BYTES::type, "COUNT", at,
                    __attrs.bytes_offset().bytes().size,
                    __attrs.bytes_offset().__offset};
        case SliceError::NESTED_PARTIAL:
            // Reported against the entry that is wrong, with the size it
            // carries: a message naming "entry 3, 20 bytes" is actionable
            // where "the attributes are malformed" is not.
            return {b::Check::BAD_NESTED_VALUE, b::ATTRIBUTE_SIZES::type, "VALUE_SIZE",
                    __attrs.sizes_offset().entry(at).value_size(),
                    b::NESTED_OFFSET_SIZE, __attrs.sizes_offset().__offset};
    }

    for (const auto& slice : slices) {
        if (slice.kind != k::AttributeKinds::ATTRIBUTE_NESTED) continue;
        for (Size i = 0, n = slice.item_count(); i < n; ++i) {
            const b::ATTRIBUTES child{__attrs.__base, slice.item(i), __attrs.__size,
                                      __attrs.__version};
            // The child's own subtree -- its sizes and byte arrays -- is
            // ordinary generated territory, and cannot recurse.
            if (const b::Status s = child.validate_deep(); !s) return s;
            if (const b::Status s =
                    validate_nested_attributes(child, __path, __seen, __depth + 1);
                !s) return s;
        }
    }
    // On completion, never on entry: see VisitedBlocks.
    __seen.insert(__attrs.__offset);
    return {};
}

/// The most attribute nodes one file may lift into the abstraction.
///
/// Not a duplicate of the validator's bound, and it cannot be: validation
/// memoises a structure reached twice, because it only has to answer a
/// question about it. The abstraction has to *materialise* it, once per parent
/// that names it -- so a file the validator accepts in linear time can still
/// expand to more nodes than memory holds. A tree of forty-way sharing, twelve
/// deep, is a few kilobytes on disk and 40^12 nodes in RAM.
///
/// A million nodes is far past any real slide's laboratory metadata and far
/// short of exhausting a machine, so the file that trips this is malformed or
/// hostile, and it gets an error naming the reason rather than the OOM killer.
constexpr Size MAX_ATTRIBUTE_NODES = 1u << 20;

/// Lift one attributes structure, and everything it nests, into the
/// abstraction. Throws, as the rest of abstract_file_structure does.
///
/// Carries its own cycle check rather than relying on the caller having
/// validated: abstract_file_structure is documented to require validation
/// first, but a walk that recurses on file-supplied offsets should not turn a
/// skipped precondition into an unbounded one.
Abstraction::AttributeSet lift_attributes(const b::ATTRIBUTES& __attrs, b::VisitPath& __path,
                                          Size& __budget, Size __depth) {
    if (__depth > MAX_ATTRIBUTE_DEPTH)
        throw std::runtime_error(
            "Attribute nesting exceeds the maximum depth of " +
            std::to_string(MAX_ATTRIBUTE_DEPTH));
    if (__path.contains(__attrs.__offset))
        throw std::runtime_error(
            "The attributes block at " + std::to_string(__attrs.__offset) +
            " is reached from itself; the nesting contains a cycle");
    if (!__path.push(__attrs.__offset))
        throw std::runtime_error("Attribute nesting exceeds the block-graph depth");
    // Every failure below leaves by exception, so the pop has to survive
    // unwinding rather than sit at the end of the body.
    struct PathScope {
        b::VisitPath& path;
        ~PathScope() { path.pop(); }
    } __scope{__path};

    std::vector<AttributeSlice> slices;
    std::uint32_t at = 0;
    switch (slice_attributes(__attrs, slices, &at)) {
        case SliceError::NONE: break;
        case SliceError::UNREADABLE:
            throw std::runtime_error(
                "The attributes block at " + std::to_string(__attrs.__offset) +
                " does not have a readable sizes or byte array");
        case SliceError::OVERRUN:
            throw std::runtime_error(
                "Attribute " + std::to_string(at) + " of the attributes block at " +
                std::to_string(__attrs.__offset) +
                " extends past the attribute byte array");
        case SliceError::NESTED_PARTIAL:
            throw std::runtime_error(
                "Attribute " + std::to_string(at) + " of the attributes block at " +
                std::to_string(__attrs.__offset) + " is a nested value of " +
                std::to_string(__attrs.sizes_offset().entry(at).value_size()) +
                " bytes, which is not a whole number of " +
                std::to_string(b::NESTED_OFFSET_SIZE) + "-byte offsets");
    }

    // Charged per node rather than per block, because the cost this bounds is
    // the materialised tree, not the file.
    if (slices.size() > __budget)
        throw std::runtime_error(
            "The attribute structure expands to more than " +
            std::to_string(MAX_ATTRIBUTE_NODES) +
            " nodes; a structure shared by many parents is materialised once per parent");
    __budget -= slices.size();

    Abstraction::AttributeSet set;
    set.reserve(slices.size());
    for (const auto& slice : slices) {
        Abstraction::AttributeNode node;
        node.key.assign(reinterpret_cast<const char*>(slice.key), slice.key_size);
        node.nested = slice.kind == k::AttributeKinds::ATTRIBUTE_NESTED;
        if (!node.nested) {
            node.value.assign(reinterpret_cast<const char8_t*>(slice.value),
                              slice.value_size);
        } else {
            const Size items = slice.item_count();
            node.items.reserve(items);
            for (Size i = 0; i < items; ++i) {
                const b::ATTRIBUTES child{__attrs.__base, slice.item(i),
                                          __attrs.__size, __attrs.__version};
                if (!child) fail(child.validate());
                node.items.push_back(lift_attributes(child, __path, __budget, __depth + 1));
            }
        }
        set.push_back(std::move(node));
    }
    return set;
}

// MARK: - Tile entries
//
// A tile entry is (OFFSET, SIZE): a claim about where a compressed stream lies.
// It is the one edge the generated walk does not follow — a stream has no
// header of its own to validate — so nothing checked it, and a consumer doing
// `base + OFFSET` for SIZE bytes read wherever the entry said (2026-09-28: a
// 294-byte file with a 16 MiB tile validated). Like any length on the wire it
// is a claim, checked before anyone reads through it.

/// Why a tile entry cannot be read through, or nullptr when it can.
///
/// NULL_TILE is "no tile at this grid position": always legal, whatever SIZE
/// holds, since the spec does not constrain SIZE for it. A stream may not start
/// inside the FILE_HEADER (`__header_end` is the header's version-aware extent),
/// and may not run past the end of the file.
const char* tile_entry_error(Offset __offset, std::uint32_t __size, Size __file_size,
                             Size __header_end) noexcept {
    if (__offset == k::NULL_TILE) return nullptr;
    if (__offset < __header_end) return "starts inside the file header";
    // Never `__offset + __size <= __file_size`: that sum can wrap.
    if (__offset > __file_size || __file_size - __offset < __size)
        return "runs past the end of the file";
    return nullptr;
}

/// The one walk over the tile offsets array, split into layers by the layer
/// extents. Both read paths use it — validate_file_structure with a visitor
/// that keeps nothing, abstract_file_structure with one that records every
/// entry — so what one accepts the other accepts. Each entry is checked before
/// `__visit(layer, tile, offset, size)` sees it: it lies inside the file
/// (tile_entry_error), and on a Z-stacked layer it is framed (tile_frame_error).
/// Returns why the first bad entry cannot be read through, naming it, or an
/// empty string when every entry can.
///
/// Preconditions: both arrays have validated, so each COUNT fits the file.
template <class Visit>
std::string walk_tile_entries(const b::FILE_HEADER& __header, Size __file_size, Visit&& __visit) {
    const auto table   = __header.tile_table_offset();
    const auto offsets = table.tile_offsets_offset();
    const auto extents = table.layer_extents_offset();

    // X_TILES × Y_TILES is a claim. Summing stops once it passes the array's
    // u32 COUNT, so the total cannot wrap back onto a matching value.
    std::uint64_t total = 0;
    for (std::uint32_t l = 0; l < extents.count() && total <= offsets.count(); ++l)
        total += static_cast<std::uint64_t>(extents.entry(l).x_tiles()) * extents.entry(l).y_tiles();
    if (total != offsets.count())
        return "The layer extents disagree with the tile offset array (" +
               std::to_string(offsets.count()) + " entries) on the number of tiles";

    const Size    header_end = __header.extent();
    std::uint32_t global     = 0;
    for (std::uint32_t l = 0; l < extents.count(); ++l) {
        const auto          layer  = extents.entry(l);
        const std::uint16_t planes = layer.z_planes().value_or(0);
        const std::uint64_t tiles  = static_cast<std::uint64_t>(layer.x_tiles()) * layer.y_tiles();
        for (std::uint64_t t = 0; t < tiles; ++t, ++global) {
            const auto  entry = offsets.entry(global);
            const char* why   = tile_entry_error(entry.offset(), entry.size_field(),
                                                 __file_size, header_end);
            // A Z-stacked layer frames every stream (the spec, Focal Planes).
            if (!why && planes > 1 && entry.offset() != k::NULL_TILE)
                why = tile_frame_error(__header.__base, __file_size, __header.__version,
                                       entry.offset(), global, planes);
            if (why)
                return "Layer " + std::to_string(l) + ", tile " + std::to_string(t) +
                       " (OFFSET " + std::to_string(entry.offset()) + ", SIZE " +
                       std::to_string(entry.size_field()) + ") " + why;
            __visit(l, t, entry.offset(), entry.size_field());
        }
    }
    return {};
}

/// The one walk over the annotation groups. Group titles are a byte run sliced
/// by a parallel size array, as the attributes are, and each title is followed
/// by its members' 24-bit identifiers. Both read paths use it, as they share
/// walk_tile_entries. `__visit(title, at, members)` sees each group, where
/// `at` is the offset of its identifiers within the byte run. Returns why the
/// first group does not fit the byte array, or an empty string.
///
/// Preconditions: both blocks have validated, so COUNT and the byte run fit
/// the file.
template <class Visit>
std::string walk_annotation_groups(const b::ANNOTATION_GROUP_SIZES& __sizes,
                                   const b::ANNOTATION_GROUP_BYTES& __blob, Visit&& __visit) {
    const ::Iris::File::ByteSpan titles = __blob.bytes();
    Size cursor = 0;
    for (std::uint32_t i = 0; i < __sizes.count(); ++i) {
        const auto entry        = __sizes.entry(i);
        const Size title_size   = entry.title_size();
        const Size member_bytes = Size{entry.member_count()} * 3;
        // Never `cursor + title_size + member_bytes > size`: subtract instead.
        if (titles.size - cursor < title_size || titles.size - cursor - title_size < member_bytes)
            return "Annotation group " + std::to_string(i) +
                   " extends past the annotation group byte array";
        __visit(std::string_view(reinterpret_cast<const char*>(titles.data + cursor), title_size),
                cursor + title_size, entry.member_count());
        cursor += title_size + member_bytes;
    }
    return {};
}

/// validate_file_structure's view of the annotation groups. validate_deep has
/// already vouched for every block it reaches, so a slot that does not validate
/// here is an absent one.
std::string annotation_groups_error(const b::METADATA& __metadata) {
    const auto annotations = __metadata.annotations_offset();
    if (!annotations) return {};
    const auto sizes = annotations.group_sizes_offset();
    const auto blob  = annotations.group_bytes_offset();
    if (!sizes || !blob) return {};
    return walk_annotation_groups(sizes, blob, [](std::string_view, Size, std::uint32_t) {});
}

}  // namespace

// MARK: - Entry points

Result IFE_EXPORT is_iris_codec_file(const FileAccessInfo& __info) noexcept {
    // MAGIC is a `constant` field, so the generated layer validates it rather
    // than handing it back: FILE_HEADER::validate() checks the magic number,
    // the recovery tag, and that the header fits within the file.
    return to_result(root_at(__info.file_ptr, __info.file_size).validate());
}

Result IFE_EXPORT validate_file_structure(const FileAccessInfo& __info) noexcept {
    const b::FILE_HEADER header = versioned_root(__info.file_ptr, __info.file_size);

    // validate_deep walks the header, the tile table, and the metadata with
    // cycle detection, following edges that leave array entries as well as
    // block headers.
    if (const b::Status status = header.validate_deep(); !status) return to_result(status);

    // Then the parts of the graph the generated walk cannot see: the tile
    // streams, which have no header, and the structures nested inside
    // attribute values.
    try {
        const std::string tiles = walk_tile_entries(header, __info.file_size,
                                                    [](std::uint32_t, std::uint64_t, Offset, std::uint32_t) {});
        if (!tiles.empty()) return {IRIS_FAILURE, tiles};

        const auto metadata = header.metadata_offset();
        if (!metadata) return to_result(metadata.validate());
        if (const std::string groups = annotation_groups_error(metadata); !groups.empty())
            return {IRIS_FAILURE, groups};
        if (const auto attributes = metadata.attributes_offset()) {
            b::VisitPath   path;
            VisitedBlocks  seen;
            return to_result(validate_nested_attributes(attributes, path, seen, 0));
        }
    } catch (const std::bad_alloc&) {
        return {IRIS_FAILURE, "validation failed (out of memory walking tiles, annotation "
                              "groups or nested attributes)"};
    }
    return to_result(b::Status{});
}

Abstraction::File IFE_EXPORT abstract_file_structure(const FileAccessInfo& __info) {
    using namespace Abstraction;
    File abstraction;

    const b::FILE_HEADER header = versioned_root(__info.file_ptr, __info.file_size);
    if (!header) fail(header.validate());

    abstraction.header = {.fileSize   = header.file_size(),
                          .extVersion = ::Iris::File::primitives::compose_version(
                              header.extension_major(), header.extension_minor()),
                          .revision   = header.file_revision()};

    // ---- tile table ------------------------------------------------------ //
    const auto table = header.tile_table_offset();
    if (!table) fail(table.validate());

    abstraction.tileTable.encoding      = static_cast<Encoding>(table.encoding());
    abstraction.tileTable.format        = static_cast<Format>(table.format());
    abstraction.tileTable.extent.width  = table.x_extent();
    abstraction.tileTable.extent.height = table.y_extent();
    // Absent before 1.1, and zero means the same thing as absent, so both
    // normalise to the default here. The abstraction states the tile length
    // there is; it does not make the caller decode the two ways the file can
    // say "256".
    if (const auto length = table.tile_length(); length && *length != 0)
        abstraction.tileTable.tileLength = *length;

    const auto extents = table.layer_extents_offset();
    if (!extents) fail(extents.validate());
    abstraction.tileTable.extent.layers.resize(extents.count());
    abstraction.tileTable.planes.resize(extents.count());
    for (uint32_t i = 0; i < extents.count(); ++i) {
        auto& layer      = abstraction.tileTable.extent.layers[i];
        const auto entry = extents.entry(i);
        layer.xTiles     = entry.x_tiles();
        layer.yTiles     = entry.y_tiles();
        layer.scale      = entry.scale();
        // Same normalisation, same reason: one plane unless the file says more.
        abstraction.tileTable.planes[i] =
            std::max<uint16_t>(entry.z_planes().value_or(0), 1);
    }
    // Downsample is derived, not stored: the reciprocal of the scale relative
    // to the most magnified layer. Semantic, so it stays hand-written.
    if (!abstraction.tileTable.extent.layers.empty()) {
        const float max_scale = abstraction.tileTable.extent.layers.back().scale;
        for (auto& layer : abstraction.tileTable.extent.layers)
            layer.downsample = layer.scale != 0.f ? max_scale / layer.scale : 0.f;
    }

    // The tile offset array is flat; the layer extents say how to split it.
    const auto offsets = table.tile_offsets_offset();
    if (!offsets) fail(offsets.validate());

    // Checked before it is handed out: the abstraction's promise is that every
    // offset in it can be read through. Entries are visited only after the
    // tile count is known to match the array, so the vectors grow to what the
    // file actually holds.
    auto& layers = abstraction.tileTable.layers;
    layers.resize(abstraction.tileTable.extent.layers.size());
    const std::string tiles = walk_tile_entries(header, __info.file_size,
        [&](std::uint32_t __layer, std::uint64_t, Offset __offset, std::uint32_t __size) {
            layers[__layer].push_back({.offset = __offset, .size = __size});
        });
    if (!tiles.empty()) throw std::runtime_error(tiles);

    // ---- metadata, and the optional blocks it points at ------------------ //
    const auto metadata = header.metadata_offset();
    if (!metadata) fail(metadata.validate());

    auto& meta = abstraction.metadata;
    meta.codec = {static_cast<uint16_t>(metadata.codec_major()),
                  static_cast<uint16_t>(metadata.codec_minor()),
                  static_cast<uint16_t>(metadata.codec_build())};
    meta.micronsPerPixel = metadata.microns_pixel();
    meta.magnification   = metadata.magnification();

    if (const auto attributes = metadata.attributes_offset(); present(attributes)) {
        meta.attributes.type    = static_cast<MetadataType>(attributes.format());
        meta.attributes.version = attributes.version();

        // Checked here rather than left to slice_attributes, which reports one
        // failure for two causes: an unreadable block and a readable pair that
        // disagree. Naming the block that is actually broken is worth two lines.
        const auto sizes = attributes.sizes_offset();
        const auto bytes = attributes.bytes_offset();
        if (!sizes) fail(sizes.validate());
        if (!bytes) fail(bytes.validate());

        // Keys and values are one byte run sliced by a parallel size array --
        // there is no string type in IFE, by design.
        b::VisitPath tree_path;
        Size         budget = MAX_ATTRIBUTE_NODES;
        abstraction.attributeTree = lift_attributes(attributes, tree_path, budget, 0);

        // The flat map keeps carrying the top-level text values, unchanged: a
        // caller that never encodes a sequence sees exactly what it always saw.
        for (const auto& node : abstraction.attributeTree)
            if (!node.nested) meta.attributes[node.key] = node.value;
    }

    if (const auto images = metadata.images_offset(); present(images)) {
        for (uint32_t i = 0; i < images.count(); ++i) {
            const auto entry = images.entry(i);
            const auto bytes = entry.bytes_offset();
            if (!bytes) fail(bytes.validate());

            // The label is the first TITLE_SIZE bytes of the image block; the
            // encoded stream is the IMAGE_SIZE bytes that follow it.
            const Size  title_size = bytes.title_size();
            const auto  payload    = bytes.__offset + b::IMAGE_BYTES::header_size;
            std::string label(reinterpret_cast<const char*>(__info.file_ptr + payload), title_size);

            AssociatedImage image;
            image.offset            = payload + title_size;
            image.byteSize          = bytes.image_size();
            image.info.imageLabel   = label;
            image.info.width        = entry.width();
            image.info.height       = entry.height();
            image.info.encoding     = static_cast<ImageEncoding>(entry.encoding());
            image.info.sourceFormat = static_cast<Format>(entry.format());
            // The RAW sixteen bits, never orientation(): that accessor decodes
            // the half to degrees, while this enum's values ARE the half's bit
            // patterns (ORIENTATION_90 is 0x55A0, not 90). Casting the float
            // made a clean 90-degree image read back as enum 90 -- no named
            // orientation -- and a damaged negative angle undefined behaviour.
            // Iris-Codec's encoder decodes bits to degrees going in; this is
            // the matching step coming out. Any 16 bits are a defined value
            // of a uint16_t-backed enum, so no damage can make this UB.
            image.info.orientation  = static_cast<AssociatedImageInfo::Orientation>(
                ::Iris::File::load<std::uint16_t>(entry.__base + entry.__offset +
                                           b::IMAGES::IMAGE_ENTRY::offset::ORIENTATION));

            meta.associatedImages.insert(label);
            abstraction.images[std::move(label)] = std::move(image);
        }
    }

    if (const auto profile = metadata.icc_color_offset(); present(profile)) {
        const ::Iris::File::ByteSpan bytes = profile.bytes();
        meta.ICC_profile.assign(reinterpret_cast<const char*>(bytes.data), bytes.size);
    }

    if (const auto clinical = metadata.clinical_offset(); present(clinical)) {
        abstraction.clinicalOffset = clinical.__offset + b::CLINICAL_METADATA::header_size;
        abstraction.clinicalSize   = clinical.count();
        abstraction.clinicalEncoding = static_cast<uint8_t>(
            clinical.encoding().value_or(k::ClinicalEncodings::CLINICAL_UNDEFINED));
    }

    if (const auto plane = metadata.microns_plane()) abstraction.micronsPerPlane = *plane;

    if (const auto annotations = metadata.annotations_offset(); present(annotations)) {
        for (uint32_t i = 0; i < annotations.count(); ++i) {
            const auto entry = annotations.entry(i);
            const auto bytes = entry.bytes_offset();
            if (!bytes) fail(bytes.validate());

            Abstraction::Annotation note;
            note.offset    = bytes.__offset + b::ANNOTATION_BYTES::header_size;
            note.byteSize  = bytes.count();
            note.type      = static_cast<AnnotationTypes>(entry.format());
            note.xLocation = entry.x_location();
            note.yLocation = entry.y_location();
            note.xSize     = entry.x_size();
            note.ySize     = entry.y_size();
            note.width     = entry.pixel_width();
            note.height    = entry.pixel_height();
            note.parent    = entry.parent_id();

            const auto identifier = entry.identifier();
            abstraction.annotations[identifier] = note;
            meta.annotations.insert(identifier);
        }

        const auto sizes = annotations.group_sizes_offset();
        const auto blob  = annotations.group_bytes_offset();
        if (present(sizes) && present(blob)) {
            const std::string groups = walk_annotation_groups(sizes, blob,
                [&](std::string_view __title, Size __at, std::uint32_t __members) {
                    meta.annotationGroups.emplace(__title);
                    abstraction.annotations.groups[std::string(__title)] = {
                        .offset = blob.__offset + b::ANNOTATION_GROUP_BYTES::header_size + __at,
                        .number = __members};
                });
            if (!groups.empty()) throw std::runtime_error(groups);
        }
    }

    return abstraction;
}

// MARK: - File mapping

namespace {

/// A block the map walk has still to record: where a slot said it is, and
/// the type that slot declares.
struct Visit {
    Offset           at;
    k::RecoveryCodes tag;
    bool             required;   ///< one of the header's slots
};

/// Queue the children `__slots` name, in reverse, so they come off the stack
/// in slot order: each subtree is recorded before its next sibling. Tile
/// entries are recorded here. The tile data is unframed -- it carries no block
/// header, so it can only be located through the entries that address it.
void queue_children(Abstraction::FileMap& __map, const std::vector<Recovery::Slot>& __slots,
                    std::vector<Visit>& __todo) {
    for (const Recovery::Slot& s : __slots)
        if (s.repr == Recovery::SlotRepr::TileEntry && s.stored != k::NULL_TILE && s.claim != 0)
            note(__map, Abstraction::MAP_ENTRY_TILE_PIXEL_DATA, s.stored, s.claim);
    for (auto it = __slots.rbegin(); it != __slots.rend(); ++it)
        if (it->repr != Recovery::SlotRepr::TileEntry)
            __todo.push_back({it->stored, it->expect, it->parent == 0});
}

}  // namespace

Abstraction::FileMap IFE_EXPORT generate_file_map(const FileAccessInfo& __info) {
    using namespace Abstraction;
    FileMap map;
    map.file_size = __info.file_size;
    // Every test here is in_bounds(), not the bool. The map records what the
    // offset graph CLAIMS, over files that may already be damaged, so a block
    // whose witnesses fail still belongs in it -- the bool would drop it, and
    // the map exists to warn a writer off live bytes.
    const b::FILE_HEADER header = versioned_root(__info.file_ptr, __info.file_size);
    if (!header.in_bounds()) fail(header.validate());
    note(map, MAP_ENTRY_FILE_HEADER, header.__offset, header.extent());

    // One walk over the slots each block holds: slots_of(), which the recovery
    // census reads with too. It has no validated graph behind it, so the
    // (offset, type) seen set is what stops a loop, or a nested value naming
    // one structure many times, while a damaged slot that names a block under
    // another type still has that type's subtree walked.
    std::set<std::pair<Offset, k::RecoveryCodes>> seen;
    std::vector<Recovery::Slot> slots;
    std::vector<Visit>          todo;
    slots_of(__info.file_ptr, __info.file_size, header.__version, 0,
             k::RecoveryCodes::RECOVER_FILE_HEADER, slots);
    queue_children(map, slots, todo);
    while (!todo.empty()) {
        const Visit v = todo.back();
        todo.pop_back();
        const std::optional<Size> extent = b::with_block(
            v.tag, __info.file_ptr, v.at, __info.file_size, header.__version,
            std::optional<Size>{}, [](const auto& __h) -> std::optional<Size> {
                if (!__h.in_bounds()) return std::nullopt;
                return __h.extent();
            });
        // The header's slots name the tile table and the metadata: a file
        // without them is no slide to map.
        if (!extent && v.required)
            b::with_block(v.tag, __info.file_ptr, v.at, __info.file_size, header.__version, 0,
                          [](const auto& __h) -> int { fail(__h.validate()); });
        if (!extent) continue;
        note(map, entry_for(v.tag), v.at, *extent);
        if (!seen.emplace(v.at, v.tag).second) continue;
        slots.clear();
        slots_of(__info.file_ptr, __info.file_size, header.__version, v.at, v.tag, slots);
        queue_children(map, slots, todo);
    }
    return map;
}

}  // namespace Iris::File
