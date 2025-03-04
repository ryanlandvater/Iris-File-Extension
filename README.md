# Iris File Extension

This is the official implementation of the Iris File Extension specification, part of the Iris Digital Pathology project. This provies the exact byte-offset vtables and enumerations referenced by the Iris Codec specification.

> [!TIP]
> This may not be the repository you wish to use. A higher level implemenation that allows for reading and encoding Iris type files (*.iris extension*) is available as part of the [Iris Codec Community Module](https://github.com/IrisDigitalPathology/Iris-Codec.git). 

> [!WARNING]
> This implementation allows for low-level manipulation of the Iris File Extension file structure. Most programmers attempting to access Iris files should not use this repository and instead should incorporate the [Iris Codec Community Module](https://github.com/IrisDigitalPathology/Iris-Codec.git) into their projects. 

This repository builds tools to access Iris files as headers or as modules with Python or JavaScript bindings. The repository uses the CMake build system. 

<p xmlns:cc="http://creativecommons.org/ns#" >This repository is licensed under the MIT software license. The Iris File Extension is licensed under <a href="https://creativecommons.org/licenses/by-nd/4.0/?ref=chooser-v1" target="_blank" rel="license noopener noreferrer" style="display:inline-block;">CC BY-ND 4.0<img style="height:22px!important;margin-left:3px;vertical-align:text-bottom;" src="https://mirrors.creativecommons.org/presskit/icons/cc.svg?ref=chooser-v1" alt=""><img style="height:22px!important;margin-left:3px;vertical-align:text-bottom;" src="https://mirrors.creativecommons.org/presskit/icons/by.svg?ref=chooser-v1" alt=""><img style="height:22px!important;margin-left:3px;vertical-align:text-bottom;" src="https://mirrors.creativecommons.org/presskit/icons/nd.svg?ref=chooser-v1" alt=""></a></p>

# Implementation
## C++ Interface
Incorporating the Iris File Extension into your code base **is simple** but requires additional headers. The [Iris Headers repository]() is automatically included when using this repository with CMake *(recommended)*; however it can be optionally configured and installed separately. 

### Non-CMake Project
If you are **NOT** using CMake to build your project, you should still use CMake to generate the Iris File Extension library.
```shell
git clone --depth 1 https://github.com/IrisDigitalPathology/IrisFileExtension.git
cmake -B ./IrisFileExtension/build ./IrisFileExtension #(optional)-DCMAKE_INSTALL_PREFIX=''
cmake --build ./IrisFileExtension/build --config Release
cmake --install ./IrisFileExtension/build
```

### CMake Project
If you **are** using CMake for your build, You may directly incorporate this repository into your code base using the following about **10 lines of code** in your project's CMakeLists.txt:
```CMake
FetchContent_Declare (
    IrisFileExtension
    GIT_REPOSITORY https://github.com/IrisDigitalPathology/IrisFileExtension.git
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
### Using Slide Abstraction
The easiest way to access slide information is via the `IrisCodec::Abstraction::File` object, which abstracts representations of the data elements still residing on disk (and providing byte-offset locations within the mapped WSI file to access these elements in an optionally **zero-copy manner**).



## Python Interface

## JavaScript Interface

# Publications
