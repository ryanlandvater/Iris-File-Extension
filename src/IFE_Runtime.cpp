/**
 * @file IFE_Runtime.cpp
 * @brief The semantic layer, ported onto the generated block handles.
 * @copyright Iris Developers, 2025-2026
 *
 * What disappeared relative to the retired hand-written layer, and why the port is
 * worth the churn: every byte offset, every `LOAD_U*`, every hand-threaded
 * `if (__version > IRIS_EXTENSION_1_0); else goto ...`, and every
 * `#ifdef __EMSCRIPTEN__`. A reader body here is a sequence of accessor calls.
 *
 * What stays hand-written is the part that is genuinely semantic: the
 * traversal order, which blocks are optional, how a flat tile-offset array is
 * split across layers, the downsample computation, and what a recovery scan
 * looks for. None of that is derivable from a byte layout.
 */

#include "IFE_Runtime.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace IrisCodec {

namespace k  = ::IFE::constants;
namespace b  = ::IFE::blocks;

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

/// The root handle. Its own version is unknowable until it has been read, so
/// it is constructed with UINT32_MAX — every gate open for exactly one block —
/// exactly as v1's FILE_HEADER constructor did.
b::FILE_HEADER root_at(const BYTE* __base, size_t __size) noexcept {
    return b::FILE_HEADER{__base, 0, __size, UINT32_MAX};
}

/// Compose the file's version the way v1 does: major << 16 | minor, read once
/// at the root and propagated to every child by construction.
uint32_t version_of(const b::FILE_HEADER& __header) noexcept {
    return (static_cast<uint32_t>(__header.extension_major()) << 16) | __header.extension_minor();
}

b::FILE_HEADER versioned_root(const BYTE* __base, size_t __size) noexcept {
    const b::FILE_HEADER bootstrap = root_at(__base, __size);
    return b::FILE_HEADER{__base, 0, __size, version_of(bootstrap)};
}

// MARK: - Attribute slicing
//
// One attribute, as it sits on disk: a key, and a value that is either text or
// a run of offsets to nested structures. The slicing lives here, once, because
// the sizes array and the packed byte run have to agree exactly and a second
// copy of that agreement is a second place for it to drift -- the same reason
// the generated writer derives both from one payload.

struct AttributeSlice {
    const BYTE*       key        = nullptr;
    Size              key_size   = 0;
    const BYTE*       value      = nullptr;
    Size              value_size = 0;
    k::AttributeKinds kind       = k::AttributeKinds::ATTRIBUTE_STRING;

    /// How many nested structures the value locates. Meaningful only when the
    /// kind is nested; zero is a legal empty sequence.
    [[nodiscard]] Size item_count() const noexcept {
        return kind == k::AttributeKinds::ATTRIBUTE_NESTED
             ? b::nested_count(value_size) : 0;
    }
    /// The offset of nested structure `i`: an ordinary absolute file offset.
    [[nodiscard]] Offset item(Size __i) const noexcept {
        return b::nested_offset(value, __i);
    }
};

/// Why a slice failed. Three causes, reported apart rather than as one bool,
/// because the diagnostic a caller prints is the whole value of noticing.
enum class SliceError {
    NONE,
    UNREADABLE,       ///< the sizes or bytes block does not validate
    OVERRUN,          ///< an attribute extends past the end of the byte array
    NESTED_PARTIAL,   ///< a nested value is not a whole number of offsets
};

/// Cut one attributes structure into its slices.
///
/// Where the deserialization-side checks live. Both blocks can validate
/// structurally and still disagree with each other, because neither block
/// describes the other; this is where that is caught. `__at` receives the
/// index of the offending entry, so a message can name it.
SliceError slice_attributes(const b::ATTRIBUTES& __attrs,
                            std::vector<AttributeSlice>& __slices,
                            std::uint32_t* __at = nullptr) {
    __slices.clear();
    if (__at) *__at = 0;
    const auto sizes = __attrs.sizes_offset();
    const auto bytes = __attrs.bytes_offset();
    if (!sizes || !bytes) return SliceError::UNREADABLE;

    const ::IFE::ByteSpan blob = bytes.bytes();
    Size cursor = 0;
    for (uint32_t i = 0; i < sizes.count(); ++i) {
        if (__at) *__at = i;
        const auto entry = sizes.entry(i);
        AttributeSlice slice;
        slice.key_size   = entry.key_size();
        slice.value_size = entry.value_size();
        slice.kind       = entry.kind();

        if (slice.key_size > blob.size - cursor) return SliceError::OVERRUN;
        slice.key = blob.data + cursor;
        cursor += slice.key_size;
        if (slice.value_size > blob.size - cursor) return SliceError::OVERRUN;
        slice.value = blob.data + cursor;
        cursor += slice.value_size;

        // A nested value is a whole number of offsets or it is malformed: the
        // one rule about these bytes that the capped schema vocabulary cannot
        // state, so it is stated in the specification prose and enforced here.
        // Rejected outright rather than rounded down -- a partial offset is a
        // corrupted file, and reading the whole ones would be inventing a
        // structure the encoder never wrote.
        if (slice.kind == k::AttributeKinds::ATTRIBUTE_NESTED &&
            !b::nested_size_is_whole(slice.value_size))
            return SliceError::NESTED_PARTIAL;

        __slices.push_back(slice);
    }
    return SliceError::NONE;
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

}  // namespace

// MARK: - Entry points

bool IFE_EXPORT is_Iris_Codec_file(BYTE* const __base, size_t __size) {
    // MAGIC is a `constant` field, so the generated layer validates it rather
    // than handing it back: FILE_HEADER::validate() checks the magic number,
    // the recovery tag, and that the header fits -- exactly the two loads v1
    // performed, plus the bounds check it had to be given separately.
    return static_cast<bool>(root_at(__base, __size).validate());
}

Result IFE_EXPORT validate_file_structure(BYTE* const __base, size_t __size) noexcept {
    const b::FILE_HEADER header = versioned_root(__base, __size);

    // v1 validated the header, then the tile table, then the metadata, each
    // with validate_full, early-returning on failure. validate_deep does the
    // same walk plus cycle detection, and follows edges leaving array entries
    // as well as block headers -- which v1's chain did not.
    if (const b::Status status = header.validate_deep(); !status) return to_result(status);

    // Then the one part of the graph the generated walk cannot see: the
    // structures nested inside attribute values.
    try {
        const auto metadata = header.metadata_offset();
        if (!metadata) return to_result(metadata.validate());
        if (const auto attributes = metadata.attributes_offset()) {
            b::VisitPath   path;
            VisitedBlocks  seen;
            return to_result(validate_nested_attributes(attributes, path, seen, 0));
        }
    } catch (const std::bad_alloc&) {
        return {IRIS_FAILURE, "validation failed (out of memory walking nested attributes)"};
    }
    return to_result(b::Status{});
}

Abstraction::File IFE_EXPORT abstract_file_structure(BYTE* const __base, size_t __size) {
    using namespace Abstraction;
    File abstraction;

    const b::FILE_HEADER header = versioned_root(__base, __size);
    if (!header) fail(header.validate());

    abstraction.header = {.fileSize   = header.file_size(),
                          .extVersion = version_of(header),
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

    uint64_t total_tiles = 0;
    for (const auto& layer : abstraction.tileTable.extent.layers)
        total_tiles += static_cast<uint64_t>(layer.xTiles) * layer.yTiles;
    if (total_tiles != offsets.count())
        throw std::runtime_error(
            "Tile count from the layer extents (" + std::to_string(total_tiles) +
            ") does not match the tile offset array (" + std::to_string(offsets.count()) + ")");

    abstraction.tileTable.layers.resize(abstraction.tileTable.extent.layers.size());
    uint32_t tile = 0;
    for (size_t li = 0; li < abstraction.tileTable.extent.layers.size(); ++li) {
        const auto& extent = abstraction.tileTable.extent.layers[li];
        auto&       layer  = abstraction.tileTable.layers[li];
        layer.resize(static_cast<size_t>(extent.xTiles) * extent.yTiles);
        for (auto& entry : layer) {
            const auto stored = offsets.entry(tile++);
            entry.offset = stored.offset();
            entry.size   = stored.size_field();
        }
    }

    // ---- metadata, and the optional blocks it points at ------------------ //
    const auto metadata = header.metadata_offset();
    if (!metadata) fail(metadata.validate());

    auto& meta = abstraction.metadata;
    meta.codec = {static_cast<uint16_t>(metadata.codec_major()),
                  static_cast<uint16_t>(metadata.codec_minor()),
                  static_cast<uint16_t>(metadata.codec_build())};
    meta.micronsPerPixel = metadata.microns_pixel();
    meta.magnification   = metadata.magnification();

    if (const auto attributes = metadata.attributes_offset()) {
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

    if (const auto images = metadata.images_offset()) {
        for (uint32_t i = 0; i < images.count(); ++i) {
            const auto entry = images.entry(i);
            const auto bytes = entry.bytes_offset();
            if (!bytes) fail(bytes.validate());

            // The label is the first TITLE_SIZE bytes of the image block; the
            // encoded stream is the IMAGE_SIZE bytes that follow it.
            const Size  title_size = bytes.title_size();
            const auto  payload    = bytes.__offset + b::IMAGE_BYTES::header_size;
            std::string label(reinterpret_cast<const char*>(__base + payload), title_size);

            AssociatedImage image;
            image.offset            = payload + title_size;
            image.byteSize          = bytes.image_size();
            image.info.imageLabel   = label;
            image.info.width        = entry.width();
            image.info.height       = entry.height();
            image.info.encoding     = static_cast<ImageEncoding>(entry.encoding());
            image.info.sourceFormat = static_cast<Format>(entry.format());
            image.info.orientation  =
                static_cast<AssociatedImageInfo::Orientation>(entry.orientation());

            meta.associatedImages.insert(label);
            abstraction.images[std::move(label)] = std::move(image);
        }
    }

    if (const auto profile = metadata.icc_color_offset()) {
        const ::IFE::ByteSpan bytes = profile.bytes();
        meta.ICC_profile.assign(reinterpret_cast<const char*>(bytes.data), bytes.size);
    }

    if (const auto clinical = metadata.clinical_offset()) {
        abstraction.clinicalOffset = clinical.__offset + b::CLINICAL_METADATA::header_size;
        abstraction.clinicalSize   = clinical.count();
        abstraction.clinicalEncoding = static_cast<uint8_t>(
            clinical.encoding().value_or(k::ClinicalEncodings::CLINICAL_UNDEFINED));
    }

    if (const auto plane = metadata.microns_plane()) abstraction.micronsPerPlane = *plane;

    if (const auto annotations = metadata.annotations_offset()) {
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

        // Group titles are a byte run sliced by a parallel size array, as the
        // attributes are; each group's member identifiers follow its title.
        const auto sizes = annotations.group_sizes_offset();
        const auto blob  = annotations.group_bytes_offset();
        if (sizes && blob) {
            const ::IFE::ByteSpan titles = blob.bytes();
            Size cursor = 0;
            for (uint32_t i = 0; i < sizes.count(); ++i) {
                const auto entry      = sizes.entry(i);
                const Size title_size = entry.title_size();
                const Size members    = entry.member_count();
                if (cursor + title_size + members * 3 > titles.size)
                    throw std::runtime_error(
                        "Annotation group " + std::to_string(i) +
                        " extends past the annotation group byte array");

                std::string title(reinterpret_cast<const char*>(titles.data + cursor), title_size);
                cursor += title_size;

                Abstraction::AnnotationGroup group;
                group.offset = blob.__offset + b::ANNOTATION_GROUP_BYTES::header_size + cursor;
                group.number = static_cast<uint32_t>(members);
                cursor += members * 3;   // 24-bit identifiers

                meta.annotationGroups.insert(title);
                abstraction.annotations.groups[std::move(title)] = group;
            }
        }
    }

    return abstraction;
}

// MARK: - File mapping

namespace {

/// Record one block, keyed by offset. `size` is what the block occupies,
/// header and payload together.
void note(Abstraction::FileMap& __map, Abstraction::MapEntryType __type,
          Offset __offset, Size __size) {
    __map[__offset] = {.type = __type, .offset = __offset, .size = __size};
}

template <typename Block>
Size array_span(const Block& __b) noexcept {
    return Block::header_size + static_cast<Size>(__b.stride()) * __b.count();
}

template <typename Block>
Size blob_span(const Block& __b) noexcept {
    return Block::header_size + __b.count();
}

/// Record one attributes structure and every structure nested inside it.
///
/// The nested blocks have to appear in the map or the map is unsafe for what
/// it exists for: a caller looks up what lies after a write location so it
/// does not overwrite live data, and a block the map never mentions is a block
/// it will happily let you land on.
void note_attributes(Abstraction::FileMap& __map, const b::ATTRIBUTES& __attrs,
                     VisitedBlocks& __seen, Size __depth) {
    if (__depth > MAX_ATTRIBUTE_DEPTH) return;
    // The map is built over files that may already be damaged, so this is the
    // one walk with no validated graph behind it: a loop, or a value naming
    // one structure many times, has nothing but this to stop it. note() keys
    // by offset and is idempotent, so skipping a repeat changes no entry --
    // only how long the map takes to build.
    if (!__seen.insert(__attrs.__offset).second) return;
    note(__map, Abstraction::MAP_ENTRY_ATTRIBUTES, __attrs.__offset,
         b::ATTRIBUTES::header_size);

    const auto sizes = __attrs.sizes_offset();
    const auto bytes = __attrs.bytes_offset();
    if (sizes) note(__map, Abstraction::MAP_ENTRY_ATTRIBUTE_SIZES, sizes.__offset,
                    array_span(sizes));
    if (bytes) note(__map, Abstraction::MAP_ENTRY_ATTRIBUTES_BYTES, bytes.__offset,
                    blob_span(bytes));

    std::vector<AttributeSlice> slices;
    // Best-effort: the map is built over files that may already be damaged, so
    // an unreadable value stops the descent rather than failing the map.
    if (slice_attributes(__attrs, slices) != SliceError::NONE) return;
    for (const auto& slice : slices) {
        if (slice.kind != k::AttributeKinds::ATTRIBUTE_NESTED) continue;
        for (Size i = 0, n = slice.item_count(); i < n; ++i) {
            const b::ATTRIBUTES child{__attrs.__base, slice.item(i), __attrs.__size,
                                      __attrs.__version};
            if (child) note_attributes(__map, child, __seen, __depth + 1);
        }
    }
}

}  // namespace

Abstraction::FileMap IFE_EXPORT generate_file_map(BYTE* const __base, size_t __size) {
    using namespace Abstraction;
    FileMap map;
    map.file_size = __size;

    const b::FILE_HEADER header = versioned_root(__base, __size);
    if (!header) fail(header.validate());
    note(map, MAP_ENTRY_FILE_HEADER, header.__offset, b::FILE_HEADER::header_size);

    const auto table = header.tile_table_offset();
    if (!table) fail(table.validate());
    note(map, MAP_ENTRY_TILE_TABLE, table.__offset, b::TILE_TABLE::header_size);

    if (const auto cipher = table.cipher_offset())
        note(map, MAP_ENTRY_CIPHER, cipher.__offset, b::CIPHER::header_size);

    if (const auto extents = table.layer_extents_offset())
        note(map, MAP_ENTRY_LAYER_EXTENTS, extents.__offset, array_span(extents));

    if (const auto offsets = table.tile_offsets_offset()) {
        note(map, MAP_ENTRY_TILE_OFFSETS, offsets.__offset, array_span(offsets));
        // The tile data itself is unframed -- it carries no block header, so
        // it can only be located through the entries that address it.
        for (uint32_t i = 0; i < offsets.count(); ++i) {
            const auto entry = offsets.entry(i);
            if (entry.offset() != k::NULL_TILE && entry.size_field() != 0)
                note(map, MAP_ENTRY_TILE_DATA, entry.offset(), entry.size_field());
        }
    }

    const auto metadata = header.metadata_offset();
    if (!metadata) fail(metadata.validate());
    note(map, MAP_ENTRY_METADATA, metadata.__offset, b::METADATA::header_size);

    if (const auto attributes = metadata.attributes_offset()) {
        VisitedBlocks seen;
        note_attributes(map, attributes, seen, 0);
    }

    if (const auto images = metadata.images_offset()) {
        note(map, MAP_ENTRY_ASSOCIATED_IMAGES, images.__offset, array_span(images));
        for (uint32_t i = 0; i < images.count(); ++i)
            if (const auto bytes = images.entry(i).bytes_offset())
                note(map, MAP_ENTRY_ASSOCIATED_IMAGE_BYTES, bytes.__offset,
                     b::IMAGE_BYTES::header_size + bytes.title_size() + bytes.image_size());
    }

    if (const auto profile = metadata.icc_color_offset())
        note(map, MAP_ENTRY_ICC_PROFILE, profile.__offset, blob_span(profile));

    if (const auto clinical = metadata.clinical_offset())
        note(map, MAP_ENTRY_CLINICAL_METADATA, clinical.__offset, blob_span(clinical));

    if (const auto annotations = metadata.annotations_offset()) {
        note(map, MAP_ENTRY_ANNOTATIONS, annotations.__offset, array_span(annotations));
        for (uint32_t i = 0; i < annotations.count(); ++i)
            if (const auto bytes = annotations.entry(i).bytes_offset())
                note(map, MAP_ENTRY_ANNOTATION_BYTES, bytes.__offset, blob_span(bytes));
        if (const auto sizes = annotations.group_sizes_offset())
            note(map, MAP_ENTRY_ANNOTATION_GROUP_SIZES, sizes.__offset, array_span(sizes));
        if (const auto blob = annotations.group_bytes_offset())
            note(map, MAP_ENTRY_ANNOTATION_GROUP_BYTES, blob.__offset, blob_span(blob));
    }

    return map;
}

// MARK: - Recovery

namespace {

/// Every block type, by tag, with the map entry it becomes. Derived from the
/// generated enumeration rather than a second literal table: the tag values
/// live in IFE_Blocks.hpp and are stated once.
Abstraction::MapEntryType entry_for(k::RecoveryCodes __tag) noexcept {
    using namespace Abstraction;
    switch (__tag) {
        case k::RecoveryCodes::RECOVER_FILE_HEADER:            return MAP_ENTRY_FILE_HEADER;
        case k::RecoveryCodes::RECOVER_TILE_TABLE:             return MAP_ENTRY_TILE_TABLE;
        case k::RecoveryCodes::RECOVER_CIPHER:                 return MAP_ENTRY_CIPHER;
        case k::RecoveryCodes::RECOVER_METADATA:               return MAP_ENTRY_METADATA;
        case k::RecoveryCodes::RECOVER_ATTRIBUTES:             return MAP_ENTRY_ATTRIBUTES;
        case k::RecoveryCodes::RECOVER_LAYER_EXTENTS:          return MAP_ENTRY_LAYER_EXTENTS;
        case k::RecoveryCodes::RECOVER_TILE_OFFSETS:           return MAP_ENTRY_TILE_OFFSETS;
        case k::RecoveryCodes::RECOVER_ATTRIBUTE_SIZES:        return MAP_ENTRY_ATTRIBUTE_SIZES;
        case k::RecoveryCodes::RECOVER_ATTRIBUTE_BYTES:        return MAP_ENTRY_ATTRIBUTES_BYTES;
        case k::RecoveryCodes::RECOVER_IMAGES:                 return MAP_ENTRY_ASSOCIATED_IMAGES;
        case k::RecoveryCodes::RECOVER_IMAGE_BYTES:            return MAP_ENTRY_ASSOCIATED_IMAGE_BYTES;
        case k::RecoveryCodes::RECOVER_ICC_PROFILE:            return MAP_ENTRY_ICC_PROFILE;
        case k::RecoveryCodes::RECOVER_ANNOTATIONS:            return MAP_ENTRY_ANNOTATIONS;
        case k::RecoveryCodes::RECOVER_ANNOTATION_BYTES:       return MAP_ENTRY_ANNOTATION_BYTES;
        case k::RecoveryCodes::RECOVER_ANNOTATION_GROUP_SIZES: return MAP_ENTRY_ANNOTATION_GROUP_SIZES;
        case k::RecoveryCodes::RECOVER_ANNOTATION_GROUP_BYTES: return MAP_ENTRY_ANNOTATION_GROUP_BYTES;
        case k::RecoveryCodes::RECOVER_CLINICAL_METADATA:      return MAP_ENTRY_CLINICAL_METADATA;
        case k::RecoveryCodes::RECOVER_UNDEFINED:              break;
    }
    return MAP_ENTRY_UNDEFINED;
}

}  // namespace

Abstraction::FileMap IFE_EXPORT recover_file_structure(BYTE* const __base, size_t __size) {
    using namespace Abstraction;
    FileMap map;
    map.file_size = __size;

    // A block's signature is its VALIDATION field: a u64 holding the block's
    // own offset, immediately followed by a u16 recovery tag. Scanning for
    // "a u64 equal to where it was found" is what makes VALIDATION worth its
    // eight bytes, and the shared 0x55 high byte on every tag is what keeps
    // the false-positive rate of the second test negligible.
    constexpr Size SIGNATURE = 8 + 2;

    // A tile frame has no tag: what identifies it is that its VALIDATION is
    // forty bits rather than sixty-four, which no other structure in the
    // format does. So the same pass tests both widths at each position.
    constexpr Size FRAME_SIGNATURE = 5;
    constexpr Size SMALLEST = SIGNATURE < FRAME_SIGNATURE ? SIGNATURE : FRAME_SIGNATURE;
    if (__size < SMALLEST) return map;

    // The scan has no file header to read a version from -- that header may be
    // the very thing that was lost -- so frames are read at the version this
    // build writes. Reading a frame from a *later* file still works: its
    // fields are laid out backward from the stream, so the ones this build
    // knows sit where they have always sat and the rest lie further back.
    for (Offset at = 0; at < __size; ++at) {
        if (at + SIGNATURE <= __size && ::IFE::load<uint64_t>(__base + at) == at) {
            const auto tag = static_cast<k::RecoveryCodes>(::IFE::load<uint16_t>(__base + at + 8));
            if (const MapEntryType type = entry_for(tag); type != MAP_ENTRY_UNDEFINED) {
                // Size is unknown without trusting fields the corruption may
                // have reached, so record the header only. A caller builds the
                // handle for the type and asks it, having validated it first.
                note(map, type, at, 0);
                continue;
            }
        }

        if (at + FRAME_SIGNATURE > __size) continue;
        if (::IFE::load_u40(__base + at) != at) continue;

        // The frame must fit behind the stream it precedes; a match too near
        // the start of file is a coincidence, not a frame.
        const Offset stream_at = at + FRAME_SIGNATURE;
        const b::TILE_PIXEL_DATA frame{__base, stream_at, __size, b::VERSION_WRITTEN};
        if (!frame.validate()) continue;

        // Recorded with the stream's extent left at zero, as every other type
        // here is. The frame does not carry a length and deliberately does not
        // -- how far a compressed stream runs is a question its codec answers,
        // and this layer knows nothing about codecs. What the frame supplies
        // is TILE_INDEX, which no amount of reading the stream can recover,
        // because streams may be written in any order.
        note(map, MAP_ENTRY_TILE_FRAME, stream_at - b::TILE_PIXEL_DATA::header_size,
             b::TILE_PIXEL_DATA::header_size);
        note(map, MAP_ENTRY_TILE_DATA, stream_at, 0);
    }

    return map;
}

}  // namespace IrisCodec
