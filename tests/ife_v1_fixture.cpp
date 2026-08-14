/**
 * @file ife_v1_fixture.cpp
 * @brief Loads the snapshot the SHIPPED encoder wrote.
 *
 * This file used to build its bytes with v1's STORE_* functions. Those bytes
 * are now permanent evidence: hosted on iris.exampleslides.org, pinned by
 * digest in tests/corpus/manifest.json, fetched into .deps/corpus/ at
 * configure time. The loader stays free of both layers'
 * types — nothing here includes IFE_Runtime or the retired hand-written
 * layer — so any test in any TU can consume the snapshot.
 */
#include "ife_v1_fixture.hpp"

#include <cstdio>
#include <vector>

namespace v1_fixture {

std::vector<unsigned char> load_snapshot(const std::string& __path, Expected& __expected) {
    __expected = expectations();

    std::FILE* in = std::fopen(__path.c_str(), "rb");
    if (!in) {
        std::fprintf(stderr, "ife_v1_fixture: could not open %s\n", __path.c_str());
        return {};
    }
    std::vector<unsigned char> bytes;
    std::fseek(in, 0, SEEK_END);
    const long n = std::ftell(in);
    std::fseek(in, 0, SEEK_SET);
    if (n > 0) bytes.resize(static_cast<std::size_t>(n));
    const auto read = std::fread(bytes.data(), 1, bytes.size(), in);
    std::fclose(in);

    if (read != bytes.size()) {
        std::fprintf(stderr, "ife_v1_fixture: short read from %s\n", __path.c_str());
        return {};
    }
    __expected.file_size = bytes.size();
    return bytes;
}

}  // namespace v1_fixture
