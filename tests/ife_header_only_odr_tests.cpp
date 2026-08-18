/**
 * @file ife_header_only_odr_tests.cpp
 * @brief IFE_HEADER_ONLY, across two translation units, linked together.
 *
 * The existing header-only test compiles ONE translation unit, which cannot
 * see the failure that matters: if the folded definitions stop being `inline`,
 * a single TU still compiles and links perfectly. It takes two TUs, both
 * folding the layer in and both linked into one binary, for the duplicate
 * symbols to appear.
 *
 * So this is mostly a LINK test. Its assertions are almost beside the point --
 * the failure it exists to catch happens before main() is reached, and shows
 * up as "duplicate symbol IFE::blocks::size_of(...)" from the linker.
 *
 * It covers three ways the header and its folded translation unit can meet:
 *
 *   1. two TUs each folding the layer in, linked together  -- needs `inline`
 *   2. the header included twice in one TU                 -- needs its guard
 *   3. the folded .cpp also included directly by the same TU -- needs the
 *      .cpp's own guard, since the header already pulled it in
 *
 * 2 and 3 are the circular-import cases: IFE_Blocks.hpp includes
 * IFE_Blocks.cpp at its foot, and IFE_Blocks.cpp opens by including
 * IFE_Blocks.hpp. Each guard makes the return trip a no-op; drop either and
 * this file stops compiling.
 *
 * Self-contained; non-zero exit on failure.
 */
#define IFE_HEADER_ONLY
#include "IFE_Blocks.hpp"

// (2) Again. The include guard must swallow this whole, the fold included.
#include "IFE_Blocks.hpp"

// (3) And the folded translation unit by its own name, the way a consumer
// with generated_source on its include path can reach it. IFE_Blocks.hpp has
// already pulled it in, so only the .cpp's guard prevents a second copy.
#include "IFE_Blocks.cpp"

#include <cstdio>
#include <string>
#include <vector>

namespace b = ::IFE::blocks;

// Defined in ife_header_only_odr_second.cpp, which folds the layer in too.
// (1) The link is the test: two TUs, one binary, one set of symbols.
::IFE::Size size_of_from_other_tu();
::IFE::Offset store_from_other_tu(::IFE::BYTE* base);

int main() {
    int failures = 0;
    auto check = [&failures](bool ok, const char* what) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
    };

    const std::vector<b::AttributeSizeEntry> attrs = {{.key = "SCANNER", .value = "TestCo"}};
    const b::AttributeSizesCreateInfo info{.entries = attrs};

    // Both translation units computed this through their own copy of the
    // folded definitions. One answer means one layer, however many copies of
    // the source the compiler saw.
    check(b::size_of(info) == size_of_from_other_tu(),
          "size_of disagrees across translation units");

    std::vector<::IFE::BYTE> f(512, 0);
    const ::IFE::Offset wrote = store_from_other_tu(f.data());
    check(wrote != 0, "the other translation unit could not store");

    // Read back through this TU's copy: the bytes one TU wrote are the bytes
    // the other reads, which is the property inline-across-TUs has to keep.
    const b::ATTRIBUTE_SIZES read{f.data(), wrote, f.size(), b::VERSION_WRITTEN};
    check(static_cast<bool>(read.validate()), "the stored block does not validate");
    check(read.count() == 1, "count did not survive the crossing");
    if (read.count() == 1)
        check(read.entry(0).kind() == ::IFE::constants::AttributeKinds::ATTRIBUTE_STRING,
              "KIND did not survive the crossing");

    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("ife_header_only_odr_tests: all checks passed\n");
    return 0;
}
