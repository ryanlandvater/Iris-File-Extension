# Iris File Extension

This is the official implementation of the Iris File Extension specification, part of the Iris Digital Pathology project.


```CMake 
FetchContent_Declare (
    IrisHeaders
    GIT_REPOSITORY https://github.com/IrisDigitalPathology/Iris-Headers.git
    GIT_TAG "origin/main"
    GIT_SHALLOW ON
)
```
and linked as follows:
```CMake
target_link_libraries (
    YOUR_TARGET PUBLIC
    IrisHeaders # Use headers
    IrisBuffer  # (optional) IrisBuffers
)
```