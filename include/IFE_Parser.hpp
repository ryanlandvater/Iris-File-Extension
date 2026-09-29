/**
 * @file IFE_Parser.hpp
 * @brief Iris File Extension — the READ body behind `Iris::File::Parser`.
 *
 * TIERS. FastFHIR's shape, applied to IFE's read side:
 *
 *   - PUBLIC  — `Iris::File::Parser` (declared in "IrisFileExtension.hpp"): a
 *               `std::shared_ptr<Parser_t>` handle. Its `.` methods are the
 *               everyday read API, and include "IrisFileExtension.hpp" is all
 *               a reader needs.
 *   - BODY    — `Iris::File::Parser_t` (here): the object that holds the
 *               `FileAccessInfo` mapping and exposes the read entry points, plus
 *               the deeper surface (the file map / recovery producers). Reach it
 *               with `parser->…` by including THIS header.
 *
 * Mirrors FastFHIR's `Parser` / `Parser_t` and, beside it, IFE's write side
 * (`Builder` / `Builder_t`, "IFE_Builder.hpp").
 *
 * The mapping is borrowed: a `Parser_t` never owns the bytes it reads, exactly
 * as `FileAccessInfo` documents. Lifetime is the caller's, and the handle only
 * shares the *parser*, not the mapping.
 *
 * @copyright Iris Developers, 2025-2026
 */

#ifndef IFE_Parser_hpp
#define IFE_Parser_hpp

#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include "IrisFileExtension.hpp"   // the Parser handle, FileAccessInfo, the read entry points
#include "IFE_Advanced.hpp"        // the file-map surface (generate_file_map, ...)

namespace Iris::File {

/**
 * @brief The read body behind @ref Parser. Internal tier.
 *
 * Holds the mapping — borrowed, or owned when made by `Parser::open` — and
 * forwards to the read entry points. Nothing is lifted at construction:
 * binding a `Parser` to a huge slide is nearly free, and the structure is
 * lifted once, on first use of `abstraction()`.
 */
class Parser_t
{
    friend class Parser;

    FileAccessInfo m_info;   ///< the mapping this parser reads
    /// Keeps an owned mapping alive (an `Iris::Memory`, type-erased so this
    /// header needs no private Iris-Headers include). Empty when borrowed.
    std::shared_ptr<const void> m_owner;

    mutable std::once_flag    m_lifted;
    mutable Abstraction::File m_abstraction;

public:
    /// Preconditions: `info.file_ptr`/`info.file_size` describe a mapping that
    /// @p owner (if any) keeps alive.
    explicit Parser_t(const FileAccessInfo& info,
                      std::shared_ptr<const void> owner = {}) noexcept
        : m_info(info), m_owner(std::move(owner)) {}

    Parser_t(const Parser_t&)            = delete;
    Parser_t& operator=(const Parser_t&) = delete;

    /// The mapping this parser reads.
    [[nodiscard]] const FileAccessInfo& info() const noexcept { return m_info; }
    /// Whether this parser owns its mapping (made by `Parser::open`).
    [[nodiscard]] bool owns_mapping() const noexcept { return m_owner != nullptr; }
    /// Pointer to the first mapped byte.
    [[nodiscard]] const BYTE* data() const noexcept { return m_info.file_ptr; }
    /// Bytes available at @ref data.
    [[nodiscard]] Size size() const noexcept { return m_info.file_size; }

    // ---- the read entry points, as methods -------------------------------- //

    /// Quick header check (magic, recovery tag, header fits). Does not validate.
    [[nodiscard]] Result is_iris_codec_file() const noexcept {
        return ::Iris::File::is_iris_codec_file(m_info);
    }
    /// Deep structural validation of every offset in the graph.
    [[nodiscard]] Result validate_file_structure() const noexcept {
        return ::Iris::File::validate_file_structure(m_info);
    }
    /// Lift the file structure into memory for quick data access.
    [[nodiscard]] Abstraction::File abstract_file_structure() const {
        return ::Iris::File::abstract_file_structure(m_info);
    }

    // ---- the lifted structure and zero-copy bytes (see Parser) ------------ //

    [[nodiscard]] const Abstraction::File& abstraction() const;
    [[nodiscard]] std::span<const BYTE>    tile(uint32_t layer, uint32_t tile) const;
    [[nodiscard]] uint16_t                 tile_planes(uint32_t layer, uint32_t tile) const;
    [[nodiscard]] std::span<const BYTE>    image(const std::string& label) const;

    // ---- the deeper (modification) surface -------------------------------- //

    /// The offset map of every block, for a caller about to modify a file.
    [[nodiscard]] Abstraction::FileMap generate_file_map() const {
        return ::Iris::File::generate_file_map(m_info);
    }
};

}  // namespace Iris::File

#endif  // IFE_Parser_hpp
