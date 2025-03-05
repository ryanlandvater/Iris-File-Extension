# Iris File Extension

This is the official implementation of the Iris File Extension specification, part of the Iris Digital Pathology project. This provies the exact byte-offset vtables and enumerations referenced by the Iris Codec specification.
> [!WARNING]
> **This repository is primarily for Scanner Vendors and programmers wishing to write custom encoders and decoders**. If this does not describe your goals, you should instead should incorporate the [Iris Codec Community Module](https://github.com/IrisDigitalPathology/Iris-Codec.git) into your project. This repository allows for low-level manipulation of the Iris File Extension file structure in a very narrow scope: it just provides byte offsets and validation checks against the current IFE standard. Most programmers (particularly for research) attempting to access Iris files **should not use this repository**. 

> [!NOTE]
> The scope of this repository is only deserializing Iris slide files. Decompression is **NOT** a component of this repository. The WSI tile byte arrays will be referenced in their compressed forms and it is up to your implementation to decompress these tiles. If you would like a system that performs image decompression, you should instead incorporate the [Iris Codec Community Module](https://github.com/IrisDigitalPathology/Iris-Codec.git).

This repository builds tools to access Iris files as C++ headers/library or as modules with Python or JavaScript (32-bit) bindings. The repository uses the CMake build system. 

<p xmlns:cc="http://creativecommons.org/ns#" >This repository is licensed under the MIT software license. The Iris File Extension is licensed under <a href="https://creativecommons.org/licenses/by-nd/4.0/?ref=chooser-v1" target="_blank" rel="license noopener noreferrer" style="display:inline-block;">CC BY-ND 4.0 <img style="height:22px!important;margin-left:3px;vertical-align:text-bottom;" src="https://mirrors.creativecommons.org/presskit/icons/cc.svg?ref=chooser-v1" alt=""><img style="height:22px!important;margin-left:3px;vertical-align:text-bottom;" src="https://mirrors.creativecommons.org/presskit/icons/by.svg?ref=chooser-v1" alt=""><img style="height:22px!important;margin-left:3px;vertical-align:text-bottom;" src="https://mirrors.creativecommons.org/presskit/icons/nd.svg?ref=chooser-v1" alt=""></a></p>

# Implementation
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
    IrisFileExtension
)
```
### Always Validate a Slide
It is best practice to always validate an Iris slide file. The validation requires the operating system's returned file size as part of the validation process. Validation is simple but throws uncaught exceptions and thus must be wrapped in a try-catch block. Validation is performed by calling the [`IrisCodec::validate_file_structure`](https://github.com/IrisDigitalPathology/Iris-File-Extension/blob/2646ee4e986f90247e447000c035490d3114d98f/src/IrisCodecExtension.hpp#L69) method defined in [IrisCodecExtension.hpp](./src/IrisCodecExtension.hpp) and implemented in [IrisCodecExtension.cpp](https://github.com/IrisDigitalPathology/Iris-File-Extension/blob/main/src/IrisCodecExtension.cpp#L194). 
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
...
try {
    using namespace IrisCodec::Abstraction;
    File file = abstract_file_structure ((uint8_t*)ptr, size);
} catch (std::runtime_error &error) {
    ...handle the read error
}
```

## Python Interface

## JavaScript Interface

# Publications
