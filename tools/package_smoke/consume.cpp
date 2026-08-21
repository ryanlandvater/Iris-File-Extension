/**
 * @file consume.cpp
 * @brief Use the installed package the way a downstream project would.
 *
 * Two things a package can still fail at after installing cleanly: its headers
 * may not be reachable at the paths the exported target advertises, and its
 * exported symbols may not resolve at link time. This does both — one include
 * of the umbrella header, one call into the shared library.
 *
 * The buffer is deliberately not a slide. `is_iris_codec_file` bounds-checks
 * its input and rejects it (the same rejection ife_runtime_tests pins over
 * random noise), so what is being checked here is that the call links and
 * runs, not what it decides. Reading real bytes is the test suite's job; this
 * one is about the package.
 */
#include <array>
#include <cstdio>

#include <IrisFileExtension.hpp>

int main() {
    // Qualified, with no `using namespace`: a consumer of the package gets
    // exactly what the installed headers declare, and the in-tree tests reach
    // this type through a using-directive they bring themselves.
    std::array<Iris::BYTE, 64> not_a_slide{};

    if (IrisCodec::is_iris_codec_file({not_a_slide.data(), not_a_slide.size()})) {
        std::fputs("package smoke: 64 zero bytes were accepted as an Iris file\n", stderr);
        return 1;
    }

    std::puts("package smoke: linked the installed library and called into it");
    return 0;
}
