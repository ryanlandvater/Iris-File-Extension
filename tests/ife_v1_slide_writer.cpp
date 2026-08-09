/**
 * @file ife_v1_slide_writer.cpp
 * @brief Writes a slide file with the SHIPPED encoder. Test fixture, and the
 *        input for the 4.4 acceptance comparison.
 *
 * A separate executable rather than a function the tests call, because the
 * hand-written layer and IFE_Runtime define the same four IrisCodec entry
 * points — deliberately, so a consumer switches by changing one include — and
 * two definitions cannot occupy one binary. The file therefore passes between
 * them on disk, which is how a real consumer would hand one over anyway.
 *
 * Usage: ife_v1_slide_writer <path>
 */
#include "ife_v1_fixture.hpp"

#include <cstdio>
#include <string>

int main(int argc, const char* argv[]) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <output-path>\n", argv[0]);
        return 2;
    }

    v1_fixture::Expected expected;
    const auto bytes = v1_fixture::build_slide(expected);

    std::FILE* out = std::fopen(argv[1], "wb");
    if (!out) {
        std::fprintf(stderr, "could not open %s for writing\n", argv[1]);
        return 1;
    }
    const auto written = std::fwrite(bytes.data(), 1, bytes.size(), out);
    std::fclose(out);

    if (written != bytes.size()) {
        std::fprintf(stderr, "short write: %zu of %zu bytes\n", written, bytes.size());
        return 1;
    }
    return 0;
}
