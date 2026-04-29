/**
 * @file ife_array_tests.cpp
 * @brief Phase 5 tests: IFE_ARRAY block, KIND_AND_STEP, Span<T>, ArrayView<T>.
 *
 * @copyright Copyright (c) 2026 Ryan Landvater. MIT licensed.
 */
#include "IFE_Array.hpp"
#include "IFE_Builder.hpp"
#include "IFE_DataBlock.hpp"
#include "IFE_Memory.hpp"
#include "IFE_Types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

#define IFE_REQUIRE(cond)                                                    \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            return 1;                                                        \
        }                                                                    \
    } while (0)

namespace {

int test_span_basics() {
    int data[] = {1, 2, 3, 4, 5};
    IFE::Span<int> s(data, 5);
    IFE_REQUIRE(s.size() == 5);
    IFE_REQUIRE(!s.empty());
    IFE_REQUIRE(s.data() == data);
    IFE_REQUIRE(s[0] == 1 && s[4] == 5);
    IFE_REQUIRE(s.front() == 1 && s.back() == 5);
    int sum = 0;
    for (int v : s) sum += v;
    IFE_REQUIRE(sum == 15);
    IFE_REQUIRE(s.at(2) == 3);
    bool threw = false;
    try { (void)s.at(5); } catch (const std::out_of_range&) { threw = true; }
    IFE_REQUIRE(threw);

    IFE::Span<int> empty;
    IFE_REQUIRE(empty.empty());
    IFE_REQUIRE(empty.size() == 0);
    IFE_REQUIRE(empty.data() == nullptr);
    IFE_REQUIRE(empty.begin() == empty.end());
    return 0;
}

int test_kind_and_step_roundtrip() {
    const auto v1 = IFE::pack_kind_and_step(IFE::IFE_FieldKind::SCALAR, 4);
    const auto u1 = IFE::unpack_kind_and_step(v1);
    IFE_REQUIRE(u1.kind == IFE::IFE_FieldKind::SCALAR);
    IFE_REQUIRE(u1.step_bytes == 4);

    const auto v2 = IFE::pack_kind_and_step(IFE::IFE_FieldKind::BLOCK, 12);
    const auto u2 = IFE::unpack_kind_and_step(v2);
    IFE_REQUIRE(u2.kind == IFE::IFE_FieldKind::BLOCK);
    IFE_REQUIRE(u2.step_bytes == 12);

    // Layout pin: kind in high byte, step in low byte.
    IFE_REQUIRE((v2 >> 8) == static_cast<std::uint16_t>(IFE::IFE_FieldKind::BLOCK));
    IFE_REQUIRE((v2 & 0xFF) == 12);
    return 0;
}

int test_claim_array_uint64() {
    auto         mem = IFE::Memory::create(64 * 1024);
    IFE::Builder b{mem};
    auto h = b.claim_array<std::uint64_t>(8);
    IFE_REQUIRE(h.valid());
    IFE_REQUIRE(h.count == 8);
    IFE_REQUIRE(h.step_bytes == 8);
    IFE_REQUIRE(h.body_offset == h.offset + IFE::IFE_ARRAY_HEADER_SIZE);
    for (std::size_t i = 0; i < h.count; ++i) {
        h.write(i, static_cast<std::uint64_t>(i * 1000ull + 7ull));
    }

    // Read back via ArrayView.
    auto view = mem.view();
    IFE::ArrayView<std::uint64_t> av{view, h.offset};
    IFE_REQUIRE(av.size() == 8);
    IFE_REQUIRE(av.step_bytes() == 8);
    for (std::size_t i = 0; i < av.size(); ++i) {
        IFE_REQUIRE(av.at(i) == i * 1000ull + 7ull);
    }
    // Bounds check.
    bool threw = false;
    try { (void)av.at(8); } catch (const std::out_of_range&) { threw = true; }
    IFE_REQUIRE(threw);

    // Zero-copy span path: arena is 16-byte aligned and step==sizeof(T)==8,
    // so as_span() must return a non-empty Span.
    auto s = av.as_span();
    IFE_REQUIRE(s.size() == 8);
    for (std::size_t i = 0; i < s.size(); ++i) {
        IFE_REQUIRE(s[i] == i * 1000ull + 7ull);
    }
    return 0;
}

int test_claim_array_float() {
    auto         mem = IFE::Memory::create(64 * 1024);
    IFE::Builder b{mem};
    auto h = b.claim_array<float>(4);
    h.write(0, 1.5f);
    h.write(1, -2.5f);
    h.write(2, 3.25f);
    h.write(3, 0.0f);

    IFE::ArrayView<float> av{mem.view(), h.offset};
    IFE_REQUIRE(av.size() == 4);
    IFE_REQUIRE(av.at(0) == 1.5f);
    IFE_REQUIRE(av.at(1) == -2.5f);
    IFE_REQUIRE(av.at(2) == 3.25f);
    IFE_REQUIRE(av.at(3) == 0.0f);
    auto s = av.as_span();
    IFE_REQUIRE(s.size() == 4 && s[2] == 3.25f);
    return 0;
}

int test_claim_array_layer_extent() {
    auto         mem = IFE::Memory::create(64 * 1024);
    IFE::Builder b{mem};

    auto h = b.claim_array<IFE::LayerExtentBlock>(3);
    IFE_REQUIRE(h.step_bytes == 12);  // u32 + u32 + f32

    h.write(0, IFE::LayerExtentBlock{ 10u, 20u, 1.0f });
    h.write(1, IFE::LayerExtentBlock{ 30u, 40u, 2.0f });
    h.write(2, IFE::LayerExtentBlock{ 50u, 60u, 4.0f });

    IFE::ArrayView<IFE::LayerExtentBlock> av{mem.view(), h.offset};
    IFE_REQUIRE(av.size() == 3);
    IFE_REQUIRE(av.at(0).xTiles == 10u && av.at(0).yTiles == 20u && av.at(0).scale == 1.0f);
    IFE_REQUIRE(av.at(2).xTiles == 50u && av.at(2).yTiles == 60u && av.at(2).scale == 4.0f);
    return 0;
}

int test_empty_array_roundtrip() {
    auto         mem = IFE::Memory::create(64 * 1024);
    IFE::Builder b{mem};
    auto h = b.claim_array<std::uint32_t>(0);
    IFE_REQUIRE(h.valid());
    IFE_REQUIRE(h.count == 0);

    IFE::ArrayView<std::uint32_t> av{mem.view(), h.offset};
    IFE_REQUIRE(av.size() == 0);
    IFE_REQUIRE(av.empty());
    auto s = av.as_span();
    IFE_REQUIRE(s.empty());
    return 0;
}

int test_array_view_tag_mismatch() {
    auto         mem = IFE::Memory::create(64 * 1024);
    IFE::Builder b{mem};
    auto h = b.claim_array<std::uint64_t>(2);
    h.write(0, 1ull);
    h.write(1, 2ull);

    // Constructing ArrayView<float> over a uint64 array must throw on tag.
    bool threw = false;
    try {
        IFE::ArrayView<float> av{mem.view(), h.offset};
        (void)av;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    IFE_REQUIRE(threw);

    // OOB offset must throw.
    threw = false;
    try {
        IFE::ArrayView<std::uint64_t> av{mem.view(), mem.capacity() - 4};
        (void)av;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    IFE_REQUIRE(threw);
    return 0;
}

int test_array_view_step_mismatch() {
    // Hand-build an array preamble with the correct tag but a wrong step
    // byte, and confirm ArrayView<T> rejects it.
    auto         mem = IFE::Memory::create(64 * 1024);
    IFE::Builder b{mem};
    auto h = b.claim_array<std::uint64_t>(2);
    h.write(0, 1ull);
    h.write(1, 2ull);

    // Corrupt the KIND_AND_STEP step byte to claim 4-byte elements.
    std::uint16_t corrupt =
        IFE::pack_kind_and_step(IFE::IFE_FieldKind::SCALAR, 4);
    std::memcpy(mem.data() + h.offset + IFE::IFE_ARRAY_KIND_AND_STEP_OFFSET,
                &corrupt, sizeof(corrupt));

    bool threw = false;
    try {
        IFE::ArrayView<std::uint64_t> av{mem.view(), h.offset};
        (void)av;
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    IFE_REQUIRE(threw);
    return 0;
}

int test_concurrent_array_claims() {
    constexpr int N_THREADS = 16;
    constexpr int N_PER_THR = 64;

    auto         mem = IFE::Memory::create(8 * 1024 * 1024);
    IFE::Builder b{mem};

    std::vector<std::thread>   threads;
    std::vector<std::uint64_t> array_offsets(N_THREADS * N_PER_THR, 0);

    for (int t = 0; t < N_THREADS; ++t) {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < N_PER_THR; ++i) {
                const std::size_t k = static_cast<std::size_t>(t) * N_PER_THR
                                    + static_cast<std::size_t>(i);
                auto h = b.claim_array<std::uint32_t>(4);
                for (std::uint32_t j = 0; j < 4; ++j) {
                    h.write(j, static_cast<std::uint32_t>(k * 4 + j));
                }
                array_offsets[k] = h.offset;
            }
        });
    }
    for (auto& th : threads) th.join();

    auto view = mem.view();
    for (std::size_t k = 0; k < array_offsets.size(); ++k) {
        IFE::ArrayView<std::uint32_t> av{view, array_offsets[k]};
        IFE_REQUIRE(av.size() == 4);
        for (std::uint32_t j = 0; j < 4; ++j) {
            IFE_REQUIRE(av.at(j) == static_cast<std::uint32_t>(k * 4 + j));
        }
    }
    // No two arrays landed at the same offset.
    {
        std::vector<std::uint64_t> sorted = array_offsets;
        std::sort(sorted.begin(), sorted.end());
        for (std::size_t i = 1; i < sorted.size(); ++i) {
            IFE_REQUIRE(sorted[i] != sorted[i - 1]);
        }
    }
    return 0;
}

int test_overflow_count_throws() {
    auto         mem = IFE::Memory::create(64 * 1024);
    IFE::Builder b{mem};
    bool threw = false;
    try {
        // (size_t::max - header) / 8 + 1 → overflow.
        const std::size_t bad = (static_cast<std::size_t>(-1) -
                                 IFE::IFE_ARRAY_HEADER_SIZE) / 8 + 1;
        (void)b.claim_array<std::uint64_t>(bad);
    } catch (const std::overflow_error&) {
        threw = true;
    }
    IFE_REQUIRE(threw);
    return 0;
}

}  // namespace

int main() {
    int rc = 0;
    rc |= test_span_basics();
    rc |= test_kind_and_step_roundtrip();
    rc |= test_claim_array_uint64();
    rc |= test_claim_array_float();
    rc |= test_claim_array_layer_extent();
    rc |= test_empty_array_roundtrip();
    rc |= test_array_view_tag_mismatch();
    rc |= test_array_view_step_mismatch();
    rc |= test_concurrent_array_claims();
    rc |= test_overflow_count_throws();
    if (rc == 0) std::fprintf(stderr, "ife_array_tests: OK\n");
    return rc;
}
