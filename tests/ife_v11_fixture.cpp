/**
 * @file ife_v11_fixture.cpp
 * @brief Loads the 1.1 witness.
 *
 * The bytes are written by tests/ife_corpus_writer_11.cpp, hosted on
 * iris.exampleslides.org, pinned by digest in tests/corpus/manifest.json and
 * fetched into .deps/corpus/ at configure time — the same delivery as the
 * 1.0 snapshot. The loader is plain data movement; it knows nothing of
 * either the generated or the retired layer.
 */
#include "ife_v11_fixture.hpp"

#include <cstdio>
#include <vector>

namespace v11_fixture {

std::vector<unsigned char> load_snapshot(const std::string& __path, Expected& __expected) {
    __expected = expectations();

    std::FILE* in = std::fopen(__path.c_str(), "rb");
    if (!in) {
        std::fprintf(stderr, "ife_v11_fixture: could not open %s\n", __path.c_str());
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
        std::fprintf(stderr, "ife_v11_fixture: short read from %s\n", __path.c_str());
        return {};
    }
    __expected.file_size = bytes.size();
    return bytes;
}

}  // namespace v11_fixture
