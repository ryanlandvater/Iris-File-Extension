# Iris File Extension

This is the official implementation of the Iris File Extension specification, part of the Iris Digital Pathology project. This repository has a very limited scope; it provides the byte-offset vtables and enumerations referenced by the Iris Codec specification and validates files against the published IFE specification. This is an advanced repository. **If this is your first foray into Iris, the [Iris Codec Community Module](https://github.com/IrisDigitalPathology/Iris-Codec.git) is a much better choice**. 

> [!IMPORTANT]
> **Schema-driven.** The byte-offset vtables and enumerations are expressed as
> a JSON specification that generates both the C++ serialization layer and the
> specification document itself (LaTeX/HTML).

Example Iris slide files are hosted to test decoding are hosted at [the Iris-Example-Files repository](https://github.com/IrisDigitalPathology/Iris-Example-Files). 

> [!CAUTION]
> **This repository is primarily for scanner device manufacturers and programmers wishing to write custom encoders and decoders. If this does not describe your goals, you should instead incorporate the [Iris Codec Community Module](https://github.com/IrisDigitalPathology/Iris-Codec.git) into your project**. This repository allows for low-level manipulation of the Iris File Extension file structure in a very narrow scope: it just provides byte offsets and validation checks against the current IFE standard. Most programmers (particularly for research) attempting to access Iris files **should not use this repository** and use Iris Codec instead.

> [!NOTE]
> The scope of this repository is only serializing or deserializing Iris slide files. Compression and decompression are **NOT** components of this repository. The WSI tile byte arrays will be referenced in their on-disk compressed forms and it is up to your implementation to compress or decompress tiles. If you would like a system that performs image compression and decompression, you should instead incorporate the [Iris Codec Community Module](https://github.com/IrisDigitalPathology/Iris-Codec.git), which incorporates this repository for Iris slide file serialization.

This repository builds C++ access to Iris files — header-only, static, shared, or as a WebAssembly module. It exposes the *byte structure*; the [Iris Codec Community Module](https://github.com/IrisDigitalPathology/Iris-Codec.git) builds on it and exposes *compression and the high-level slide API*. Iris Codec publishes Python and JavaScript bindings for its layer; bindings for this layer are planned but not yet published. The repository builds with CMake
and has an equivalent Bazel build (`BUILD.bazel`); both are exercised in CI.

<p xmlns:cc="http://creativecommons.org/ns#" >This repository is licensed under the MIT software license. The Iris File Extension is licensed under <a href="https://creativecommons.org/licenses/by-nd/4.0/?ref=chooser-v1" target="_blank" rel="license noopener noreferrer" style="display:inline-block;">CC BY-ND 4.0 <img style="height:22px!important;margin-left:3px;vertical-align:text-bottom;" src="https://mirrors.creativecommons.org/presskit/icons/cc.svg?ref=chooser-v1" alt=""><img style="height:22px!important;margin-left:3px;vertical-align:text-bottom;" src="https://mirrors.creativecommons.org/presskit/icons/by.svg?ref=chooser-v1" alt=""><img style="height:22px!important;margin-left:3px;vertical-align:text-bottom;" src="https://mirrors.creativecommons.org/presskit/icons/nd.svg?ref=chooser-v1" alt=""></a></p>

# Installation
Incorporating the Iris File Extension into your code base is simple; Additional [Iris headers](https://github.com/IrisDigitalPathology/Iris-Headers) are required but are automatically included when this repository is built or included in a CMake project.

No release has been tagged yet, so build from source for now. Once one is, pre-compiled binaries will be published under the **releases tab** for Linux (x86-64 and arm64), macOS (universal) and Windows (x64): each archive carries the shared and static libraries, the public headers, and the CMake package configuration that `find_package(IrisFileExtension)` resolves, pinned by SHA-256 in the release's `SHA256SUMS`, with the specification as PDF and HTML. Python and JavaScript access to slides is provided today by the [Iris Codec Community Module](https://github.com/IrisDigitalPathology/Iris-Codec.git), which consumes this repository for slide file serialization.

### Non-CMake Project
If you are **NOT** using CMake to build your project, you should still use CMake to generate the Iris File Extension library.
```shell
git clone --depth 1 https://github.com/IrisDigitalPathology/Iris-File-Extension.git
# Optional cmake flags to consider: 
#   -DCMAKE_INSTALL_PREFIX='' for custom install directory
#   -DIFE_BUILD_EXAMPLES=ON to test build the included examples
cmake -B ./Iris-File-Extension/build ./Iris-File-Extension 
cmake --build ./Iris-File-Extension/build --config Release
cmake --install ./Iris-File-Extension/build
```

#### Bazel
An equivalent Bazel build covers the library, the examples and the full test
suite (`BUILD.bazel`). The test rules live in `tests/tests.bzl`, declared from
the `ife_tests()` macro that `BUILD.bazel` calls — the fixture genrules, the
header-only input library and every `cc_test` target. `generated_source/`
is regenerated at build time from `spec/` (never committed), and Iris-Headers
is resolved from the sibling checkout next to this repository (see
`MODULE.bazel`).

```shell
git clone https://github.com/IrisDigitalPathology/Iris-File-Extension.git
git clone https://github.com/IrisDigitalPathology/Iris-Headers.git
cd Iris-File-Extension
bazel test //... --test_output=errors
```

#### Xcode
On macOS, generate a native Xcode project (tests enabled, `generated_source/`
regenerated at configure) and open it directly:

```shell
cmake --preset xcode
open build-xcode/IrisFileExtension.xcodeproj
```

Headers from `src/` and `generated_source/` are part of the project navigator.
Of the schemes CMake generates, the `ife_*` test schemes are the ones to
use (`ALL_BUILD`, `RUN_TESTS`, `ZERO_CHECK` and `install` are CMake plumbing).

Pick a scheme (the `ife_*` targets are the tests) and Run — no extra signing
or configuration is needed for the command-line targets.

### CMake Project
If you **are** using CMake for your build, You may directly incorporate this repository into your code base using the following about **10 lines of code** in your project's CMakeLists.txt:
```CMake
FetchContent_Declare (
    IrisFileExtension
    GIT_REPOSITORY https://github.com/IrisDigitalPathology/Iris-File-Extension.git
    GIT_TAG "origin/main"
    GIT_SHALLOW ON
)
FetchContent_MakeAvailable(IrisFileExtension)
```
and then link against this repository as you would any other library
```CMake
target_link_libraries (
    YOUR_TARGET PRIVATE
    IrisFileExtension #Use 'IrisFileExtensionStatic' for static linkage
)
```
# Implementation
> [!CAUTION]
> **This API is still early in development and liable to change. As we apply new updates some of the exposed calls may change.** If dynamically linked, always check new headers against your code base when updating your version of the Iris File Extension API.

## C++ Interface

The library is namespaced by layer: `Iris::` is the shared core (Iris-Headers — `Result`, `Buffer`, `Memory`), `Iris::File::` is this repository. It exposes two handles: a **`Parser`** to read a slide and a **`Builder`** to write one. An application *owns* a handle and keeps its own job: a reader decodes the streams a Parser hands it; an encoder compresses, runs its threads, and names and moves its files, while its Builder claims space and writes every byte of the file's structure. Neither handle decodes or encodes an image — compression belongs to your codec (or to [Iris Codec](https://github.com/IrisDigitalPathology/Iris-Codec.git)).

Every C++ example on this page is compiled against the real headers by the `ife_readme_compiles` test, as published. Editing one? The HTML comment above each block (`<!-- ife-compile: fragment | program | skip -->`, invisible when rendered) tells the gate how to compile it; placeholder names such as `path` and `ptr` are declared in `tests/readme/readme_context.hpp`.

### Reading a Slide
> [!WARNING]
> When reading Iris slide files, you should **always validate** a slide before attempting to read data from it.

`Parser::open` maps the file read-only and keeps it mapped for as long as any copy of the handle lives, so a reader that keeps the Parser keeps its bytes. Validation returns an `Iris::Result`; on failure its `message` says why.
<!-- ife-compile: fragment -->
```cpp
#include "IrisFileExtension.hpp"
#include <cstdio>

using namespace Iris;
using namespace Iris::File;

Parser parser = Parser::open(path);        // throws if the file cannot be mapped
if (Result valid = parser.validate_file_structure(); valid != IRIS_SUCCESS) {
    std::printf("%s\n", valid.message.c_str());
    return;
}
const Abstraction::File& slide = parser.abstraction();    // lifted once, then kept
std::span<const BYTE> stream   = parser.tile(layer, tile); // zero-copy; empty = no tile there
// ...decompress stream.data(), stream.size() with the JPEG/AVIF library you use
```
`validate_file_structure` deep-validates the offset graph and bounds-checks every tile entry. `abstraction()` and `abstract_file_structure` check every block they read — both witnesses, and every length the block declares — and throw `std::runtime_error` on a structurally damaged file rather than reading it. They never return values they could not verify, and never repair; to reopen a damaged file, see [Recovering a Damaged File](#recovering-a-damaged-file). A caller that maps the file itself can bind a Parser to that mapping instead — `Parser({ptr, size})` borrows it — or call the free functions `validate_file_structure` / `abstract_file_structure` directly.

### Writing a Slide
The Builder controls how the stream is mutated — it claims space at the head and fills it — and your encoder decides everything else, the layout included: every `append_*` lands where the head is when you call it, so the order of your calls is the order on disk. Declare the tile pyramid, append every tile (from any number of threads, in any order) and the associated images, then compose the structure — tile table, images array, attributes, METADATA — from the grid you declared and the Builder's report of what it placed (`tile_offsets`, `image_entries`), and `finalize` with the two roots. The Builder frames each tile stream by default (and always on a Z-stacked layer), refuses to report placements while any tile is unaccounted for, and leaves a complete, closed file. A block you want early but can only fill later (METADATA names the blocks after it) is `claim`ed early and `fill`ed once they exist.
<!-- ife-compile: fragment -->
```cpp
#include "IrisFileExtension.hpp"
#include "IFE_Builder.hpp"   // the block tier: builder->claim / fill / append
#include <filesystem>

using namespace Iris;
using namespace Iris::File;
namespace b = Iris::File::blocks;

// The encoder picks the file name; the builder writes where it is told.
Builder builder = Builder::create({.filepath = temp_path});

BuilderTileTableInfo table;
table.encoding = TILE_ENCODING_JPEG;
table.format   = FORMAT_R8G8B8A8;
table.extent   = extent;                 // width, height, per-layer tile grid and scale
builder.set_tile_table(table);

// On your worker threads, one call per grid position:
builder.append_tile(layer, tile, jpeg.data(), jpeg.size());
builder.append_null_tile(layer, blank);  // no tile at this position (NULL_TILE)

builder.append_image(thumbnail_info, thumbnail.data(), thumbnail.size());

// The layout is yours. The grid is the `table` you declared above; the
// Builder reports where it placed the tiles (`tile_offsets`). Name each block
// and its order. Here the fixed-size METADATA is reserved ahead of the blocks
// a later edit replaces, and filled once their offsets exist.
const auto tiles  = builder.tile_offsets();
const auto planes = table.planes.empty()
    ? std::vector<uint16_t>(table.extent.layers.size(), 0) : table.planes;
std::vector<b::LayerExtentEntry> extents;
for (std::size_t l = 0; l < table.extent.layers.size(); ++l)
    extents.push_back({.X_TILES  = table.extent.layers[l].xTiles,
                       .Y_TILES  = table.extent.layers[l].yTiles,
                       .SCALE    = table.extent.layers[l].scale,
                       .Z_PLANES = planes[l]});
const Offset offsets_at = builder->append(b::TileOffsetsCreateInfo{.entries = tiles});
const Offset extents_at = builder->append(b::LayerExtentsCreateInfo{.entries = extents});
const Offset table_at   = builder->append(b::TileTableCreateInfo{
    .ENCODING             = static_cast<constants::TileEncodings>(table.encoding),
    .FORMAT               = static_cast<constants::PixelFormats>(table.format),
    .TILE_OFFSETS_OFFSET  = offsets_at,
    .LAYER_EXTENTS_OFFSET = extents_at,
    .X_EXTENT             = table.extent.width,
    .Y_EXTENT             = table.extent.height,
    .TILE_LENGTH          = table.tileLength});
const Offset metadata_at = builder->claim(b::METADATA::header_size);
const auto   images      = builder.image_entries();
const Offset images_at   = builder->append(b::ImagesCreateInfo{.entries = images});
builder->fill(metadata_at, b::MetadataCreateInfo{
    .ATTRIBUTES_OFFSET = constants::NULL_OFFSET,   // attributes omitted here
    .IMAGES_OFFSET     = images_at,
    .MICRONS_PIXEL     = metadata.micronsPerPixel});
builder.finalize({.tileTable = table_at, .metadata = metadata_at});

// Complete and closed: moving it into place is the encoder's job.
std::filesystem::rename(temp_path, final_path);
```
The Builder reserves a large sparse range (8 GiB by default; pass a larger `capacity` for a larger file) and never remaps it: only written pages cost memory or disk, and exhausting the reservation throws.

### The Slide Abstraction
[`Iris::File::Abstraction::File`](./include/IrisFileExtension.hpp) is the lifted structure: light-weight representations of what is on disk, with byte offsets into the mapped file for the payloads, which stay where they are (zero-copy). [An example reading through it is available](./examples/slide_info_abstraction.cpp).
<!-- ife-compile: skip -->
```cpp
struct Iris::File::Abstraction::File {
    Header           header;         // file size, IFE version, revision
    TileTable        tileTable;      // encoding, format, extent, and every tile's offset + size
    AssociatedImages images;         // label, thumbnail, macro... by label
    Annotations      annotations;    // on-slide annotation objects
    Metadata         metadata;       // codec version, attributes, ICC profile, mpp, magnification
    AttributeSet     attributeTree;  // the attributes with their nesting preserved
    // ...clinical metadata range, microns per focal plane
};
```

### Reading Blocks Directly
Below the abstraction, every block is a generated handle: construct it from an offset, test it (a handle's `bool` is `validate()`), and read its fields through named accessors. Each offset field returns the block it points to. The layouts are generated from `spec/ife_fields.json` — there is no hand-written layout table.
<!-- ife-compile: fragment -->
```cpp
#include "IFE_Primitives.hpp"   // versioned_root: the root handle, at the file's own version

namespace b = Iris::File::blocks;

const b::FILE_HEADER header = Iris::File::versioned_root(ptr, size);
if (!header) { /* header.validate() says why */ }
const auto table = header.tile_table_offset();   // the TILE_TABLE the header points at
if (!table)  { /* table.validate() says why */ }
const uint32_t width = table.x_extent();
const auto offsets = table.tile_offsets_offset();
for (uint32_t i = 0; offsets && i < offsets.count(); ++i) {
    const auto entry = offsets.entry(i);         // entry.offset(), entry.size_field()
}
```

### File Data Mapping
**File data mapping is more advanced functionality.** A file map records the location, size and type of every block, keyed by offset (a `std::map`), so you can find every block at or after a byte you are about to write — critical when modifying or recovering a file. An entry carries the type and offset; build the handle you want from them.
<!-- ife-compile: fragment -->
```cpp
#include "IFE_Recovery.hpp"
#include "IFE_Primitives.hpp"

using namespace Iris::File;

const Abstraction::FileMap map = generate_file_map({ptr, size});
const uint32_t version = versioned_root(ptr, size).__version;

for (auto it = map.lower_bound(write_location); it != map.end(); ++it) {
    const Abstraction::FileMapEntry& entry = it->second;   // type, offset, size
    if (entry.type == Abstraction::MAP_ENTRY_TILE_TABLE) {
        // Handles are constructed from an offset, not downcast from a base.
        const blocks::TILE_TABLE table{ptr, entry.offset, size, version};
        if (table) { /* read through it */ }
    }
}
```

### Recovering a Damaged File

The recovery engine — the two-witness reconciliation that repairs a bit-flipped
file — is **currently being rebuilt** against FastFHIR's census design (see
`MIGRATION.md`). It is not present in this revision. `generate_file_map` above
is the remaining advanced entry point; until the engine returns, a file
`abstract_file_structure` refuses cannot be reopened in-process.

## Language Bindings

This repository presently builds C++ — header-only, static, shared, or as a
WebAssembly module — and publishes no language bindings yet.

Bindings divide by layer rather than by language. The
[Iris Codec Community Module](https://github.com/IrisDigitalPathology/Iris-Codec.git)
publishes Python bindings over the high-performance codec: opening slides,
decoding tiles, compression. Bindings for this layer — validating, abstracting,
mapping and recovering the file structure, for callers who need to work at that
level, including anyone writing an encoder or decoder outside C++ — are planned
(Python first).

# Publications

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) — the append-only wire contract, the
two guards (the wire witness and the conformance corpus), and how to build and
test are spelled out there.
