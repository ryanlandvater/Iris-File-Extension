/**
 * @file ife_header_only_odr_second.cpp
 * @brief The second translation unit for the header-only ODR test.
 *
 * Exists only to fold the block layer in a second time. Linked with
 * ife_header_only_odr_tests.cpp, it is what turns a missing `inline` on the
 * folded definitions into a link error instead of a silent pass.
 *
 * Deliberately not a header-including-a-header: the point is a *separately
 * compiled* unit that has its own copy of every definition.
 */
#define IFE_HEADER_ONLY
#include "IFE_Blocks.hpp"

#include <string>
#include <vector>

namespace b = ::IFE::blocks;

::IFE::Size size_of_from_other_tu() {
    const std::vector<b::AttributeSizeEntry> attrs = {{.key = "SCANNER", .value = "TestCo"}};
    return b::size_of(b::AttributeSizesCreateInfo{.entries = attrs});
}

::IFE::Offset store_from_other_tu(::IFE::BYTE* __base) {
    const std::vector<b::AttributeSizeEntry> attrs = {{.key = "SCANNER", .value = "TestCo"}};
    constexpr ::IFE::Offset at = 64;
    return b::store(__base, at, b::AttributeSizesCreateInfo{.entries = attrs}) ? at : 0;
}
