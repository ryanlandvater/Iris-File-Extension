/**
 * @file slide_info_abstraction.cpp
 * @author Ryan Landvater (ryanlandvater@gmail.com)
 * @brief Example Iris File Encoding API using IrisCodec::Abstraction to aid in file decoding
 * 
 * @version 0.1
 * @date 2025-03-04
 * 
 * @copyright Copyright (c) 2025 Ryan Landvater
 * 
 * This file gives an example of how to implement the Iris Codec File Extension 
 * using the IrisCodec::Abstraction::File higher-level structure. This method
 * removes the manual process of removing slide information by header and arrays
 * and delegates it to the abstraction structure. All significant data blocks
 * such as image byte arrays are not read from disk. The IrisCodec::Abstraction::File
 * maintains offsets to the byte locations and offsets of these data blocks so they can
 * be used in a zero-copy manner.
 * 
 */

#include <iostream>
#include <format>
#include <filesystem>
#include <sys/mman.h>
#include <Iris/IrisFileExtension.hpp>
constexpr char help_statement[] =
"This is an example implementation of the Iris File Extension \
official low-level headers using the file abstraction assistance. \
Please provide a valid slide file path as the ONLY ARGUMENT to test \
file decoding using slide abstraction.\n";
int inline INVALID_FILE_PATH (std::string& source_path)
{
    std::cerr   << "Provided file path \""
                << source_path
                << "\" is not a valid file path\n"
                << help_statement;
    return EXIT_FAILURE;
}
inline size_t GET_FILE_SIZE (FILE* const file_handle)
{
    auto result = fseek(file_handle, 0L, SEEK_END);
    if (result == -1) throw std::runtime_error
        ("Failed to fseek the end of the file");
    
    size_t size = ftell(file_handle);
    fseek(file_handle, 0L, SEEK_SET);
    return size;
}
#if _WIN32
inline void* FILE_MAP (FILE* const file_handle, size_t size, HANDLE& map)
{
    HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file->handle));
    if (handle == NULL) throw std::runtime_error
        ("Failed to get WIN32 system file handle");

    map = CreateFileMapping(handle, NULL, PAGE_READONLY, 0,0,NULL);
    if (!map) throw std::runtime_error
        ("Failed to map file into memory");
    void*  ptr = (BYTE*) MapViewOfFile(map,FILE_MAP_READ,0,0,0);
    if (!ptr) throw std::runtime_error
        ("Failed to create view of mapped file in memory");
    return ptr;
}
inline void FILE_UNMAP (FILE* const file_handle, size_t size, HANDLE& handle)
{
    FlushViewOfFile(ptr, 0);
    UnmapViewOfFile(ptr);
    if (map == INVALID_HANDLE_VALUE) return;
    CloseHandle(map);
}
#else
using HANDLE = void*;
inline void* FILE_MAP (FILE* const file_handle, size_t size, HANDLE& map)
{
    int file_number = fileno(file_handle);
    if (file_number == -1) throw std::runtime_error
        ("Failed to get posix system file number");
    
    void* ptr = mmap (NULL, size, PROT_READ, MAP_SHARED, file_number, 0);
    if (!ptr) throw std::runtime_error
        ("Failed to map file into memory");
    
    return ptr;
}
inline void FILE_UNMAP (void* const ptr, size_t size, HANDLE& map)
{
    munmap(ptr, size);
}
#endif
const char* PARSE_ECODING (IrisCodec::Encoding encoding);
const char* PARSE_FORMAT (Iris::Format format);
const char* PARSE_IMAGE_ENCODING (IrisCodec::ImageEncoding image_encoding);


int main(int argc, const char * argv[]) {
    
    if (argc < 2) {
        std::cerr << help_statement;
        return EXIT_FAILURE;
    }
    std::string source_path (argv[1]);
    if (!std::filesystem::exists(source_path.c_str()))
        return INVALID_FILE_PATH(source_path);
    #if _WIN32
    FILE* file_handle = fopen(source_path.c_str(), "rbR");
    #else
    FILE* file_handle = fopen(source_path.c_str(), "rb");
    #endif
    if (!file_handle)
        return INVALID_FILE_PATH(source_path);

    size_t size = 0ULL;
    void*  ptr  = NULL;
    HANDLE map  = NULL;
    try {
        size = GET_FILE_SIZE(file_handle);
        ptr  = FILE_MAP(file_handle, size, map);
        
        // ALWAYS VALIDATE the file structure before attempting to
        // read it. This will check the file against the IFE
        // Specfification to ensure adherence.
        IrisCodec::validate_file_structure((uint8_t*)ptr, size);
        std::cout   << "Iris Slide file \"" << source_path
                    << "\" successfully passed file validation.\n";
        
    } catch (std::runtime_error &error) {
        std::cerr   << "Failed to create slide file abstraction: "
                    << error.what() << "\n";
        if (ptr || map)  FILE_UNMAP(ptr, size, map);
        if (file_handle) fclose(file_handle);
        return EXIT_FAILURE;
    }
    
    try {
        using namespace IrisCodec::Abstraction;
        
        auto slide = abstract_file_structure ((uint8_t*)ptr, size);
        std::cout << "Slide File information:\n"
        << "\t Encoded using IFE Spec v"
            <<(slide.header.extVersion>>16)     << "."
            <<(slide.header.extVersion&0xFFFF)  << "\n"
        << "\t Encoding: " << PARSE_ECODING(slide.tileTable.encoding) << "\n"
        << "\t Format: " << PARSE_FORMAT(slide.tileTable.format) << "\n"
        << "\t Lowest resolution pixel dimensions: "
            << slide.tileTable.extent.width << "px by "
        << slide.tileTable.extent.height << "px\n"
        << "\t Layer Extents (256px tiles): \n";
        
        int layer_index = 0;
        for (auto&& layer : slide.tileTable.extent.layers) {
            std::cout << "\t\t Layer " << layer_index << ": "
                << layer.xTiles << " xTiles, "
                << layer.yTiles << " yTiles, "
                << std::round(layer.scale) << "x scale\n";
            ++layer_index;
        }
        
        if (slide.metadata.attributes.size() == 0) {
            std::cout << "\t No encoded metadata attributes present\n";
        } else {
            std::cout << "\t Metadata attributes:\n";
            for (auto&& attribute : slide.metadata.attributes) {
                std::cout   << "\t\t[" << attribute.first << "]: "
                            << reinterpret_cast<const char*>(attribute.second.data())
                            << std::endl;
            }
        }
        
        if (slide.metadata.associatedImages.size() == 0) {
            std::cout << "\t No encoded metadata associated image labels present\n";
        } else {
            std::cout << "\t Associated image labels:\n";
            for (auto&& image : slide.metadata.associatedImages)
                if (slide.images.contains(image)) {
                    auto info = slide.images[image];
                    std::cout << "\t\t" << image << ": \n"
                    << "\t\t\t" << info.width << "px x " << info.height << "px\n"
                    << "\t\t\tFormat:" << PARSE_IMAGE_ENCODING(info.encoding) << "\n";
                }
                
        }
        
    } catch (std::runtime_error &error) {
        std::cerr   << "Failed to read slide file information: "
                    << error.what() << "\n";
        if (ptr || map)  FILE_UNMAP(ptr, size, map);
        if (file_handle) fclose(file_handle);
        return EXIT_FAILURE;
    }
    
    if (ptr || map)  FILE_UNMAP(ptr, size, map);
    if (file_handle) fclose(file_handle);
    return EXIT_SUCCESS;
}

inline const char* PARSE_ECODING (IrisCodec::Encoding encoding)
{
    switch (encoding) {
        case IrisCodec::TILE_ENCODING_UNDEFINED: return "TILE_ENCODING_UNDEFINED";
        case IrisCodec::TILE_ENCODING_IRIS:return "TILE_ENCODING_IRIS";
        case IrisCodec::TILE_ENCODING_JPEG:return "TILE_ENCODING_JPEG";
        case IrisCodec::TILE_ENCODING_AVIF:return "TILE_ENCODING_AVIF";
    }
}
inline const char* PARSE_FORMAT (Iris::Format format)
{
    switch (format) {
        case Iris::FORMAT_UNDEFINED: return "FORMAT_UNDEFINED";
        case Iris::FORMAT_B8G8R8: return "FORMAT_B8G8R8";
        case Iris::FORMAT_R8G8B8: return "FORMAT_R8G8B8";
        case Iris::FORMAT_B8G8R8A8: return "FORMAT_B8G8R8A8";
        case Iris::FORMAT_R8G8B8A8: return "FORMAT_R8G8B8A8";
    } return "FORMAT_UNDEFINED";
}
inline const char* PARSE_IMAGE_ENCODING (IrisCodec::ImageEncoding image_encoding)
{
    switch (image_encoding) {
        case IrisCodec::IMAGE_ENCODING_UNDEFINED:return "IMAGE_ENCODING_UNDEFINED";
        case IrisCodec::IMAGE_ENCODING_PNG:return "IMAGE_ENCODING_PNG";
        case IrisCodec::IMAGE_ENCODING_JPEG:return "IMAGE_ENCODING_JPEG";
        case IrisCodec::IMAGE_ENCODING_AVIF:return "IMAGE_ENCODING_AVIF";
    } return "IMAGE_ENCODING_UNDEFINED";
}
