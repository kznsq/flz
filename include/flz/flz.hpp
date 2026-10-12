// FLZ - a lossless LZ77-family compressor.
// Copyright (c) 2003-2026 Andrii Frolov aka kznsq. MIT license, see LICENSE.
//
// Two layers are exposed:
//   * the packed stream (`encode_stream` / `decode_stream`): the compressed bit
//     stream alone; the caller stores the unpacked size elsewhere;
//   * the .flz container (`compress` / `decompress`): the packed stream behind
//     an 8-byte header holding the unpacked and packed sizes. This is the format
//     the command-line tool reads and writes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <vector>

namespace flz {

inline constexpr int kMinLevel = 0;      // fastest, single context, no literal alphabet
inline constexpr int kMaxLevel = 4;      // slowest, widest match search
inline constexpr int kDefaultLevel = 3;

// Inputs larger than this cannot be described by the container's 32-bit sizes.
inline constexpr std::size_t kMaxInputSize = 0xFFFFFFFFu;

struct CompressOptions {
    int level = kDefaultLevel;  // kMinLevel..kMaxLevel
    // Called from the calling thread with the completed fraction in [0, 1].
    std::function<void(double)> progress;
};

// Thrown for unusable arguments and for packed data that is truncated or corrupt.
class Error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Packed stream. An empty input produces an empty stream.
std::vector<std::uint8_t> encode_stream(std::span<const std::uint8_t> input,
                                        const CompressOptions& options = {});

// Fills `output` completely; `output.size()` must be the unpacked size.
void decode_stream(std::span<const std::uint8_t> packed, std::span<std::uint8_t> output);

// .flz container: [unpacked size: u32 LE][packed size: u32 LE][packed stream].
std::vector<std::uint8_t> compress(std::span<const std::uint8_t> input,
                                   const CompressOptions& options = {});
std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> container);

}  // namespace flz
