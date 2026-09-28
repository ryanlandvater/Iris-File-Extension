/**
 * @file readme_context.hpp
 * @brief The placeholder names the README's C++ examples use, declared once so
 *        each example compiles as written (tests/readme/readme_compiles.py).
 *
 * An example says `Parser::open(path)` or `generate_file_map({ptr, size})`
 * without saying where `path` or `ptr` came from — that is the reader's
 * program. These declarations stand in for it. They are declarations only:
 * the gate compiles syntax-only and never links, so nothing here is defined.
 *
 * Adding an example that names something new: declare it here, with the type
 * the example implies.
 */
#ifndef IFE_README_CONTEXT_HPP
#define IFE_README_CONTEXT_HPP

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "IrisFileExtension.hpp"

namespace readme {

// Reading: a mapped file, a path to one, and a tile to ask for.
extern const Iris::BYTE*   ptr;
extern std::size_t         size;
extern const Iris::BYTE*   file_ptr;
extern std::size_t         file_size;
extern std::filesystem::path path;
extern std::uint32_t       layer;
extern std::uint32_t       tile;
extern Iris::File::Offset  write_location;

// Writing: what an encoder has in hand.
extern std::filesystem::path           temp_path;
extern std::filesystem::path           final_path;
extern Iris::Extent                    extent;
extern std::uint32_t                   blank;
extern std::vector<Iris::BYTE>         jpeg;
extern std::vector<Iris::BYTE>         thumbnail;
extern Iris::File::AssociatedImageInfo thumbnail_info;
extern Iris::File::Metadata            metadata;

}  // namespace readme

#endif  // IFE_README_CONTEXT_HPP
