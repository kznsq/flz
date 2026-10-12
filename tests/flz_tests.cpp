// FLZ tests. Usage: flz_tests <directory with the golden files>
//
// The .flz files in tests/data are fixed reference archives of the samples
// there; they must keep decoding, whatever the encoder produces today.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "flz/flz.hpp"

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                              \
        }                                                                              \
    } while (0)

Bytes read_file(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "cannot open %s\n", p.string().c_str());
        std::exit(2);
    }
    return Bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

Bytes round_trip(const Bytes& data, int level)
{
    flz::CompressOptions options;
    options.level = level;
    return flz::decompress(flz::compress(data, options));
}

// Inputs that stress different parts of the encoder.
std::vector<std::pair<std::string, Bytes>> generated_inputs(std::size_t size, std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::vector<std::pair<std::string, Bytes>> out;
    Bytes b(size);

    for (auto& x : b)
        x = static_cast<std::uint8_t>(rng());
    out.emplace_back("random", b);

    std::fill(b.begin(), b.end(), std::uint8_t{0});
    out.emplace_back("zeros", b);

    for (std::size_t i = 0; i < size; ++i)
        b[i] = static_cast<std::uint8_t>(i);
    out.emplace_back("ramp", b);

    for (std::size_t i = 0; i < size; ++i)
        b[i] = static_cast<std::uint8_t>("abcab"[i % 5]);
    out.emplace_back("period5", b);

    for (auto& x : b)
        x = static_cast<std::uint8_t>('a' + rng() % 3);
    out.emplace_back("three_letters", b);

    // Runs, copies and noise mixed together.
    std::size_t i = 0;
    while (i < size) {
        const std::size_t n = std::min<std::size_t>(size - i, 1 + rng() % 300);
        switch (rng() % 3) {
            case 0: std::fill_n(b.begin() + static_cast<std::ptrdiff_t>(i), n, static_cast<std::uint8_t>(rng())); break;
            case 1:
                for (std::size_t k = 0; k < n; ++k)
                    b[i + k] = i > 0 ? b[(i + k) % i] : std::uint8_t{7};
                break;
            default:
                for (std::size_t k = 0; k < n; ++k)
                    b[i + k] = static_cast<std::uint8_t>(rng() % 8);
        }
        i += n;
    }
    out.emplace_back("mixed", b);
    return out;
}

void test_small_sizes()
{
    for (std::size_t size = 0; size <= 130; ++size)
        for (const auto& [name, data] : generated_inputs(size, static_cast<std::uint32_t>(size)))
            for (int level = flz::kMinLevel; level <= flz::kMaxLevel; ++level)
                if (round_trip(data, level) != data) {
                    std::fprintf(stderr, "round trip failed: %s size %zu level %d\n", name.c_str(), size, level);
                    ++g_failures;
                }
}

void test_large_inputs()
{
    for (const auto& [name, data] : generated_inputs(300000, 42))
        for (int level = flz::kMinLevel; level <= flz::kMaxLevel; ++level)
            if (round_trip(data, level) != data) {
                std::fprintf(stderr, "round trip failed: %s level %d\n", name.c_str(), level);
                ++g_failures;
            }
}

void test_compression_works()
{
    const Bytes zeros(1 << 20, 0);
    CHECK(flz::compress(zeros).size() < 1000);
    const Bytes empty;
    CHECK(flz::compress(empty).size() == 8);
    CHECK(flz::decompress(flz::compress(empty)).empty());
}

void test_deterministic()
{
    const auto inputs = generated_inputs(100000, 7);
    const Bytes& data = inputs.back().second;
    CHECK(flz::compress(data) == flz::compress(data));
}

void test_golden(const fs::path& dir)
{
    for (const char* name : {"sample.txt", "sample.bin"}) {
        const Bytes original = read_file(dir / name);
        std::string stem = name;
        stem[stem.find('.')] = '_';
        for (int level : {0, 3, 4}) {
            const Bytes packed = read_file(dir / (stem + "_m" + std::to_string(level) + ".flz"));
            CHECK(flz::decompress(packed) == original);
        }
        for (int level = flz::kMinLevel; level <= flz::kMaxLevel; ++level)
            CHECK(round_trip(original, level) == original);
    }
}

void test_arguments()
{
    flz::CompressOptions bad;
    bad.level = 5;
    bool threw = false;
    try {
        flz::compress(Bytes{1, 2, 3}, bad);
    } catch (const flz::Error&) {
        threw = true;
    }
    CHECK(threw);

    Bytes out(4);
    threw = false;
    try {
        flz::decode_stream({}, out);
    } catch (const flz::Error&) {
        threw = true;
    }
    CHECK(threw);
}

// Damaged input must be rejected with flz::Error or decode to some bytes;
// it must never crash or touch memory outside the buffers (run under the
// address sanitizer to check the latter).
void test_corrupt_input(const fs::path& dir)
{
    std::mt19937 rng(1);
    std::size_t rejected = 0, decoded = 0;
    for (const char* name : {"sample_txt_m3.flz", "sample_bin_m3.flz", "sample_bin_m0.flz"}) {
        const Bytes good = read_file(dir / name);
        for (int trial = 0; trial < 3000; ++trial) {
            Bytes bad = good;
            switch (trial % 4) {
                case 0:  // a few flipped bits
                    for (int k = 0; k < 1 + trial % 3; ++k)
                        bad[8 + rng() % (bad.size() - 8)] ^= static_cast<std::uint8_t>(1u << (rng() % 8));
                    break;
                case 1:  // truncated
                    bad.resize(rng() % bad.size());
                    break;
                case 2:  // random bytes after the header
                    for (std::size_t k = 9; k < bad.size(); ++k)
                        bad[k] = static_cast<std::uint8_t>(rng());
                    break;
                default:  // wrong sizes in the container header
                    bad[rng() % 8] = static_cast<std::uint8_t>(rng());
                    break;
            }
            try {
                (void)flz::decompress(bad);
                ++decoded;
            } catch (const flz::Error&) {
                ++rejected;
            } catch (const std::bad_alloc&) {
                ++rejected;  // a damaged size field may ask for more memory than exists
            }
        }
    }
    CHECK(rejected > 0);
    std::printf("  corrupt input: %zu rejected, %zu decoded to garbage\n", rejected, decoded);
}

}  // namespace

int main(int argc, char* argv[])
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: flz_tests <test data directory>\n");
        return 2;
    }
    const fs::path dir = argv[1];

    struct {
        const char* name;
        void (*run)(const fs::path&);
    } const tests[] = {
        {"small sizes", [](const fs::path&) { test_small_sizes(); }},
        {"large inputs", [](const fs::path&) { test_large_inputs(); }},
        {"compression works", [](const fs::path&) { test_compression_works(); }},
        {"deterministic", [](const fs::path&) { test_deterministic(); }},
        {"golden files", test_golden},
        {"arguments", [](const fs::path&) { test_arguments(); }},
        {"corrupt input", test_corrupt_input},
    };
    for (const auto& t : tests) {
        const int before = g_failures;
        t.run(dir);
        std::printf("%s: %s\n", g_failures == before ? "PASS" : "FAIL", t.name);
    }
    return g_failures == 0 ? 0 : 1;
}
