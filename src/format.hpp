// Packed stream layout shared by the encoder and the decoder.
//
// Byte 0 holds header flags. When kHasAlphabet is set, bytes 1..4 hold a
// little-endian offset, relative to byte 1, of the literal alphabet that
// follows the bit stream. The bit stream starts after the header and is read
// most significant bit first.
//
// Every symbol starts with a flag bit: kFlagLiteral or kFlagMatch. A literal
// carries `literal_bits` bits; `literal_base` is added and, when an alphabet is
// present, the sum indexes the alphabet. A match-class symbol carries a code
// (see Code) followed, for most codes, by a length.
//
// Numbers are written as an exponent of fixed width followed by the mantissa
// without its leading one bit: the exponent is the bit width of the value
// (0 for the value 0). The exponent width depends on the largest value the
// field may hold (`exponent_width`).
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace flz::detail {

inline constexpr std::uint8_t kFlagMatch = 0;
inline constexpr std::uint8_t kFlagLiteral = 1;

// Header flag bits.
inline constexpr std::uint8_t kHasAlphabet = 1;     // literals index a frequency-sorted alphabet
inline constexpr std::uint8_t kWideOffsets2 = 2;    // context 2 offsets take 24 bits instead of 15
inline constexpr std::uint8_t kLongLengths1 = 4;    // context 1 lengths take 7 bits instead of 3
inline constexpr std::uint8_t kKnownHeaderFlags = kHasAlphabet | kWideOffsets2 | kLongLengths1;

// Codes of match-class symbols. Codes from kFirstOffset up are match offsets.
enum Code : std::uint32_t {
    kLiteralSwitch = 0,  // followed by a 15-bit value: base * 9 + bit count for literals
    kBaseOffset = 1,     // followed by a length and a 6-bit index into the previous match's 64-wide offset band
    kContext1 = 2,       // kContext1 + n - 1 switches to context n; no length follows
    kContext2 = 3,
    kContext3 = 4,
    kBlock = 5,          // followed by a length: the next length + kMinBlockLength symbols omit their flag bit
    kRun = 6,            // followed by a length: repeat the previous byte length + kMinRunLength times
    kFirstOffset = 7,
};

inline constexpr int kMinMatchLength = 3;
inline constexpr int kMinRunLength = 1;
inline constexpr int kMinBlockLength = 11;
inline constexpr int kBaseIndexBits = 6;       // width of the index in a kBaseOffset symbol
inline constexpr int kLiteralSwitchBits = 15;  // width of the value in a kLiteralSwitch symbol
inline constexpr std::size_t kNumContexts = 3;

// A match with distance d and length l is stored as offset d - l + kFirstOffset
// and length l - kMinMatchLength. Matches never overlap the bytes they produce,
// so d >= l and the offset never collides with the reserved codes.
constexpr std::uint32_t offset_code(std::uint32_t distance, std::uint32_t length)
{
    return distance - length + kFirstOffset;
}

// Largest value width each context allows for offsets and lengths.
struct ContextFields {
    int offset_bits;
    int length_bits;
};

constexpr int bit_width(std::uint32_t value) { return static_cast<int>(std::bit_width(value)); }

// Width of the exponent for a field holding values of up to `max_value_bits` bits.
constexpr int exponent_width(int max_value_bits) { return bit_width(static_cast<std::uint32_t>(max_value_bits)); }

// Bits used by `value` in a field of up to `max_value_bits` bits.
constexpr int value_cost(std::uint32_t value, int max_value_bits)
{
    const int mantissa = bit_width(value);
    return exponent_width(max_value_bits) + (mantissa > 0 ? mantissa - 1 : 0);
}

constexpr ContextFields context_fields(std::size_t context, std::uint8_t header_flags)
{
    switch (context) {
        case 1: return {15, (header_flags & kLongLengths1) ? 7 : 3};
        case 2: return {(header_flags & kWideOffsets2) ? 24 : 15, 7};
        default: return {24, 15};
    }
}

inline void store_le32(std::uint8_t* p, std::uint32_t v)
{
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

inline std::uint32_t load_le32(const std::uint8_t* p)
{
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

}  // namespace flz::detail
