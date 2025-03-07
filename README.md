# Iris File Extension

This is the official implementation of the Iris File Extension specification, part of the Iris Digital Pathology project. This repository has a very limited scope; it provies the byte-offset vtables and enumerations referenced by the Iris Codec specification and validates files against the published IFE specification. You are permitted to build alternatives to this repository if desired, but you should validate them against this before releasing into production. 

Example Iris slide files are hosted to test decoding are hosted at [the Iris-Example-Files repository](https://github.com/IrisDigitalPathology/Iris-Example-Files). 

> [!CAUTION]
> **This repository is primarily for scanner device manufacturers and programmers wishing to write custom encoders and decoders. If this does not describe your goals, you should instead incorporate the [Iris Codec Community Module](https://github.com/IrisDigitalPathology/Iris-Codec.git) into your project**. This repository allows for low-level manipulation of the Iris File Extension file structure in a very narrow scope: it just provides byte offsets and validation checks against the current IFE standard. Most programmers (particularly for research) attempting to access Iris files **should not use this repository** and use Iris Codec instead. 

> [!NOTE]
> The scope of this repository is only serializing or deserializing Iris slide files. Compression and decompression are **NOT** components of this repository. The WSI tile byte arrays will be referenced in their on-disk compressed forms and it is up to your implementation to compress or decompress tiles. If you would like a system that performs image compression and decompression, you should instead incorporate the [Iris Codec Community Module](https://github.com/IrisDigitalPathology/Iris-Codec.git), which incorporates this repository for Iris slide file serialization.

This repository builds tools to access Iris files as C++ headers/library or as modules with Python or JavaScript (32-bit) bindings. The repository uses the CMake build system.

<p xmlns:cc="http://creativecommons.org/ns#" >This repository is licensed under the MIT software license. The Iris File Extension is licensed under <a href="https://creativecommons.org/licenses/by-nd/4.0/?ref=chooser-v1" target="_blank" rel="license noopener noreferrer" style="display:inline-block;">CC BY-ND 4.0 <img style="height:22px!important;margin-left:3px;vertical-align:text-bottom;" src="https://mirrors.creativecommons.org/presskit/icons/cc.svg?ref=chooser-v1" alt=""><img style="height:22px!important;margin-left:3px;vertical-align:text-bottom;" src="https://mirrors.creativecommons.org/presskit/icons/by.svg?ref=chooser-v1" alt=""><img style="height:22px!important;margin-left:3px;vertical-align:text-bottom;" src="https://mirrors.creativecommons.org/presskit/icons/nd.svg?ref=chooser-v1" alt=""></a></p>

# Implementation
> [!CAUTION]
> **This API is not still early in development and liable to change. As we apply new updates some of the exposed calls may change.** If dynamically linked, always check new headers against your code base when updating your version of the Iris File Extension API.

## C++ Interface
Incorporating the Iris File Extension into your code base **is simple** but requires additional headers. The [Iris Headers repository](https://github.com/IrisDigitalPathology/Iris-Headers) is automatically included when using this repository with CMake *(recommended)*; however it can be optionally configured and installed separately. 

### Non-CMake Project
If you are **NOT** using CMake to build your project, you should still use CMake to generate the Iris File Extension library.
```shell
git clone --depth 1 https://github.com/IrisDigitalPathology/Iris-File-Extension.git
# Optional cmake flags to consider: 
#   -DCMAKE_INSTALL_PREFIX='' for custom install directory
#   -DBUILD_EXAMPLES=ON to test build the included examples
cmake -B ./Iris-File-Extension/build ./Iris-File-Extension 
cmake --build ./Iris-File-Extension/build --config Release
cmake --install ./Iris-File-Extension/build
```

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
### Always Validate a Slide
It is best practice to always validate an Iris slide file. The validation requires the operating system's returned file size as part of the validation process. Validation is simple but throws uncaught exceptions and thus must be wrapped in a try-catch block. We let you handle how to respond to validation errors. Validation *can be* performed by calling the [`IrisCodec::validate_file_structure`](https://github.com/IrisDigitalPathology/Iris-File-Extension/blob/2646ee4e986f90247e447000c035490d3114d98f/src/IrisCodecExtension.hpp#L69) method defined in [IrisCodecExtension.hpp](./src/IrisCodecExtension.hpp) and implemented in [IrisCodecExtension.cpp](./src/IrisCodecExtension.cpp#L203). Alternatively you may break up `validate_file_structure` into it's component steps
```cpp
try {
    size = GET_FILE_SIZE(file_handle);
    ptr  = FILE_MAP(file_handle, size);

    IrisCodec::validate_file_structure((uint8_t*)ptr, size);
    
} catch (std::runtime_error &error) {
    ...handle the validation error
}
```
### Using Slide Abstraction
The easiest way to access slide information is via the [`IrisCodec::Abstraction::File`](https://github.com/IrisDigitalPathology/Iris-File-Extension/blob/2646ee4e986f90247e447000c035490d3114d98f/src/IrisCodecExtension.hpp#L206-L212), which abstracts representations of the data elements still residing on disk (and providing byte-offset locations within the mapped WSI file to access these elements in an optionally **zero-copy manner**). [An example implementation reading using file abstraction is available](./examples/slide_info_abstraction.cpp). 
```cpp
struct IrisCodec::Abstraction::File {
    Header          header;      // File Header information
    TileTable       tileTable;   // Table of slide extent and WSI 256 pixel tiles
    Images          images;      // Set of ancillary images (label, thumbnail, etc...)
    Annotations     annotations; // Set of on-slide annotation objects
    Metadata        metadata;    // Slide metadata (patient info, acquisition. etc...)
};
```
```cpp
try {
    using namespace IrisCodec::Abstraction;
    File file = abstract_file_structure ((uint8_t*)ptr, size);
    std::cout   << "Encoded using IFE Spec v"
                << (file.header.extVersion >> 16) << "."
                << (file.header.extVersion & 0xFFFF) << std::endl;
    
    // We can retrieve data easily from the slide
    // Get the encoding type (IrisCodec::Encoding)
    auto compression_fmt = file.tileTable.encoding;
    // Get the location and offset of the tile (layer, tile_index)
    auto& layer_0_1_bytes = file.tileTable.layers[0][1];
    // And 'decompress' based upon whatever JPEG, AVIF, etc... library you use
    char* some_buffer = decompress (ptr + layer_0_1_bytes.offset,layer_0_1_bytes.size);

    // Don't worry about clean up. You're just referencing on-disk locations.
} catch (std::runtime_error &error) {
    ...handle the read error
}
```

### Manually without file mapping
Insteand of using the file mapping routine, you may manually access data block elements. *Abstracted* objects live in the `Abstraction:: namespace` and use CammelCase; whereas *serialized data block* objects live in the `Serialization:: namespace` and are CAPITALIZED. *Serialized* objects are polymorphic classes derived from the DATA_BLOCK structure (*shown below*) and will return other DATA_BLOCKs using the **get_xxx(ptr)** methods; alteratively, they return *Abstraction* objects when using the **read_xxx(ptr)** methods. Creation of DATA_BLOCK objects is controlled in a layered manner by protecting constructors such that only valid precursor objects may derive objects to which they point (FILE_HEADER --> TILE_TABLE --> TILE_OFFSETS).

```cpp
// DATA_BLOCKs are very light-weight representations.
struct DATA_BLOCK {
    Offset      __offset            = NULL_OFFSET;
    Size        __size              = 0;
    uint32_t    __version           = 0;
};
```
```cpp
try {
    
    Size     size = GET_FILE_SIZE(file_handle);
    uint8_t* ptr  = FILE_MAP(file_handle, size);

    using namespace Serialization;
    FILE_HEADER __HEADER = FILE_HEADER (size);
    // You must separately validate the header as no ptr was provided
    // during creation. This step may change in the future.
    __HEADER.validate_header (ptr);

    // Get the tile table.
    TILE_TABLE  __TABLE     = __HEADER.get_tile_table (ptr);
    // The tile table header is validated as a part of this routine.
    // However you can still fully validate if not done already.
    __TABLE.validate_full (ptr); 

    // We can get derived DATA_BLOCKS
    LAYER_EXTENTS __EXTENTS = __TABLE.get_layer_extents (ptr);
    TILE_OFFSETS __OFFSETS  = __TABLE.get_tile_offsets (ptr);

    // Or read abstracted data
    Abstraction::header     = FILE_HEADER.read_header (ptr);
    Abstrction::tileTable   = TILE_TABLE.read_tile_table (ptr);

    // This method should be used if you only need/want some aspects
    // of the file without needing to abstract the entire slide
} catch (...) {
    ...handle validation / read errors
}

```

### File Data Mapping
**File data mapping is more advanced functionality**. The IFE provides a powerful tool to assess the location of data blocks within a serialized slide file. This is critical when performing file updates as you may overwrite already used regions (eg.when expanding arrays). A file map allows for finding all data-blocks before or after a byte offset location (using std::map binary search tree internally). A file map entry, shown below, describes the location, size, and type of data block within the file at the given location. You may recast the datablock as it's internally defined type and use it per the API, though we recommend validating it first before attempting to do so. 
```cpp
// File Map Entry contains information about the datablock (what type)
// and the offset location
struct FileMapEntry {
    using Datablock                 = Serialization::DATA_BLOCK;
    MapEntryType        type        = MAP_ENTRY_UNDEFINED;
    Datablock           datablock;
    Size                size        = 0;
};
```
```cpp
try {
    // Always validate the slide file first
    IrisCodec::validate_file_structure(ptr, size);
    // Then generate the slide map. 
    // See IrisCodecExtension.cpp for implementation of its construction
    auto file_map = IrisCodec::generate_file_map((uint8_t*)ptr, size);

    Offset write_location = //...some location you will write at;
    auto data_blocks_after = file_map.upper_bound(write_location);
    for (;data_blocks_after!=file_map.cend();++data_blocks_after) {

        //...do something (copy into memory for writing later, etc...)

        Offset offset_location = data_block->first;
        Size   block_byte_size = data_block->second.size;
        switch (data_block->second.type) {
            ...
            case MAP_ENTRY_TILE_TABLE: 
            static_cast<TILE_TABLE&>(data_block->second.datablock).validate_offset(ptr);
            static_cast<TILE_TABLE&>(data_block->second.datablock).read_tile_table(ptr);
            ...
        }
        
    }    
} catch (std::runtime_error &error) {
    ...handle the validation error
}
```

## Python Interface

## JavaScript Interface

# Publications
