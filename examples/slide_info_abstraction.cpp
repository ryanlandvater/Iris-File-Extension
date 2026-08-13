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
 * The slide is mapped read-only through Iris::MemoryArena (priv/IrisMemory.hpp),
 * so this example also serves as the reference for the arena-based read path.
 *
 */

 #include <iostream>
 #include <format>
 #include <filesystem>
 #include <cmath> 
 #ifdef IFE_USE_RUNTIME
 // The generated layer (Phase 4). This is the whole of the cutover for a
 // consumer: one include line. Everything below is unchanged.
 #include "IFE_Runtime.hpp"
 #elif defined BUILD_EXAMPLES_TEST
 // if CMake is building this to test the installation
 #include "IrisFileExtension.hpp"
 #else
 #include <Iris/IrisFileExtension.hpp>
 #endif

 // priv/IrisMemory.hpp: the cross-platform read-only mapping used below. The
 // arena replaces the per-OS CreateFileMapping/mmap code this example used to
 // carry; it is linked through IrisFileExtensionLib (CMake) / :ife (Bazel).
 #include "IrisMemory.hpp"
 constexpr char help_statement[] =
 "This is an example implementation of the Iris File Extension \
 official low-level headers using the file abstraction assistance. \
 Please provide a valid slide file path as the ONLY ARGUMENT to test \
 file decoding using slide abstraction.\n";
 int inline INVALID_FILE_PATH(std::string& source_path)
 {
     std::cerr << "Provided file path \""
         << source_path
         << "\" is not a valid file path\n"
         << help_statement;
     return EXIT_FAILURE;
 }
 const char* PARSE_ECODING(IrisCodec::Encoding encoding);
 const char* PARSE_FORMAT(Iris::Format format);
 const char* PARSE_IMAGE_ENCODING(IrisCodec::ImageEncoding image_encoding);
 const char* PARSE_ANNOTATION_TYPE(Iris::AnnotationTypes annotation_type);
 
 
 int main(int argc, const char* argv[]) {
 
     if (argc < 2) {
         std::cerr << help_statement;
         return EXIT_FAILURE;
     }
     std::string source_path(argv[1]);
     if (!std::filesystem::exists(source_path.c_str()))
         return INVALID_FILE_PATH(source_path);
     // Map the slide read-only through Iris::MemoryArena (priv/IrisMemory.hpp):
     // one cross-platform mapping implementation, replacing the per-OS
     // CreateFileMapping/mmap branches this example used to carry. The slide
     // is never opened for writing, so a read-only file on disk or a slide
     // still being written by a scanner maps just as well. The arena unmaps
     // and closes its handles on scope exit.
     Iris::MemoryArena arena;
     std::uint8_t* ptr = nullptr;
     std::size_t size = 0;
     try {
         arena = Iris::MemoryArena::create_from_file_read_only(source_path);
         ptr = arena.base();
         size = arena.capacity();
 
         // ALWAYS VALIDATE the file structure before attempting to
         // read it. This will check the file against the IFE
         // Specfification to ensure adherence.
         IrisCodec::validate_file_structure(ptr, size);
         std::cout << "Iris Slide file \"" << source_path
             << "\" successfully passed file validation.\n";
 
     }
     catch (std::runtime_error& error) {
         std::cerr << "Failed to create slide file abstraction: "
             << error.what() << "\n";
         return EXIT_FAILURE;
     }
 
     try {
         using namespace IrisCodec::Abstraction;
 
         auto slide = IrisCodec::abstract_file_structure(ptr, size);
         std::cout << "Slide File information:\n"
             << "\t Encoded using IFE Spec v"
             << (slide.header.extVersion >> 16) << "."
             << (slide.header.extVersion & 0xFFFF) << "\n"
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
         }
         else {
             std::cout << "\t Metadata attributes:\n";
             for (auto&& attribute : slide.metadata.attributes) {
                 std::cout << "\t\t[" << attribute.first << "]: "
                     << reinterpret_cast<const char*>(attribute.second.data())
                     << std::endl;
             }
         }
 
         if (slide.metadata.associatedImages.size() == 0) {
             std::cout << "\t No encoded metadata associated image labels present\n";
         }
         else {
             std::cout << "\t Associated image labels:\n";
             for (auto&& image : slide.metadata.associatedImages)
                 if (slide.images.contains(image)) {
                     auto info = slide.images[image].info;
                     std::cout << "\t\t" << image << ": \n"
                         << "\t\t\t" << info.width << "px x " << info.height << "px\n"
                         << "\t\t\tFormat:" << PARSE_IMAGE_ENCODING(info.encoding) << "\n";
                 }

         }

         // On-slide annotations. Distinct from the associated images above:
         // those are ancillary pictures of the slide (label, thumbnail), these
         // are objects positioned in slide space, whose byte streams happen to
         // be image formats. Iterated through metadata.annotations, which is a
         // std::set of identifiers, so the order is the identifier order rather
         // than whatever the annotation map happens to hash to.
         if (slide.metadata.annotations.size() == 0) {
             std::cout << "\t No encoded on-slide annotations present\n";
         }
         else {
             std::cout << "\t On-slide annotations:\n";
             for (auto&& identifier : slide.metadata.annotations)
                 if (slide.annotations.contains(identifier)) {
                     auto&& note = slide.annotations[identifier];
                     std::cout << "\t\t[" << identifier << "]: "
                         << PARSE_ANNOTATION_TYPE(note.type) << "\n"
                         << "\t\t\t" << note.byteSize << " bytes, "
                         << note.width << "px x " << note.height << "px\n"
                         << "\t\t\tat (" << note.xLocation << ", " << note.yLocation
                         << ") sized " << note.xSize << " x " << note.ySize << "\n";
                     if (note.parent != note.NULL_ID)
                         std::cout << "\t\t\tchild of [" << note.parent << "]\n";
                 }
         }

     }
     catch (std::runtime_error& error) {
         std::cerr << "Failed to read slide file information: "
             << error.what() << "\n";
         return EXIT_FAILURE;
     }
 
     return EXIT_SUCCESS;
 }
 
 inline const char* PARSE_ECODING(IrisCodec::Encoding encoding)
 {
     switch (encoding) {
     case IrisCodec::TILE_ENCODING_UNDEFINED: return "TILE_ENCODING_UNDEFINED";
     case IrisCodec::TILE_ENCODING_IRIS:return "TILE_ENCODING_IRIS";
     case IrisCodec::TILE_ENCODING_JPEG:return "TILE_ENCODING_JPEG";
     case IrisCodec::TILE_ENCODING_AVIF:return "TILE_ENCODING_AVIF";
     } return "TILE_ENCODING_UNDEFINED";
 }
 inline const char* PARSE_FORMAT(Iris::Format format)
 {
     switch (format) {
     case Iris::FORMAT_UNDEFINED: return "FORMAT_UNDEFINED";
     case Iris::FORMAT_B8G8R8: return "FORMAT_B8G8R8";
     case Iris::FORMAT_R8G8B8: return "FORMAT_R8G8B8";
     case Iris::FORMAT_B8G8R8A8: return "FORMAT_B8G8R8A8";
     case Iris::FORMAT_R8G8B8A8: return "FORMAT_R8G8B8A8";
     } return "FORMAT_UNDEFINED";
 }
 inline const char* PARSE_IMAGE_ENCODING(IrisCodec::ImageEncoding image_encoding)
 {
     switch (image_encoding) {
     case IrisCodec::IMAGE_ENCODING_UNDEFINED:return "IMAGE_ENCODING_UNDEFINED";
     case IrisCodec::IMAGE_ENCODING_PNG:return "IMAGE_ENCODING_PNG";
     case IrisCodec::IMAGE_ENCODING_JPEG:return "IMAGE_ENCODING_JPEG";
     case IrisCodec::IMAGE_ENCODING_AVIF:return "IMAGE_ENCODING_AVIF";
     } return "IMAGE_ENCODING_UNDEFINED";
 }
 inline const char* PARSE_ANNOTATION_TYPE(Iris::AnnotationTypes annotation_type)
 {
     switch (annotation_type) {
     case Iris::ANNOTATION_UNDEFINED:return "ANNOTATION_UNDEFINED";
     case Iris::ANNOTATION_PNG:return "ANNOTATION_PNG";
     case Iris::ANNOTATION_JPEG:return "ANNOTATION_JPEG";
     case Iris::ANNOTATION_SVG:return "ANNOTATION_SVG";
     case Iris::ANNOTATION_TEXT:return "ANNOTATION_TEXT";
     } return "ANNOTATION_UNDEFINED";
 }
 
