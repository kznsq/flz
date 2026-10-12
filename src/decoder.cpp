#include <algorithm>
#include <array>
#include <cstring>
#include <string>

#include "flz/flz.hpp"
#include "format.hpp"

namespace flz {
namespace {

using namespace detail;

[[noreturn]] void corrupt(const char* what)
{
    throw Error(std::string("corrupt FLZ stream: ") + what);
}

// Most-significant-bit-first reader that refuses to read past its input.
class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> data) : data_(data) {}

    std::uint32_t bit() { return bits(1); }

    // n <= 32
    std::uint32_t bits(int n)
    {
        if (n == 0)
            return 0;
        if (count_ < n) {
            refill();
            if (count_ < n)
                corrupt("unexpected end of data");
        }
        const auto value = static_cast<std::uint32_t>(buffer_ >> (64 - n));
        buffer_ <<= n;
        count_ -= n;
        return value;
    }

    // Exponent of `exponent_bits` bits, then the mantissa below its implicit leading one.
    std::uint32_t value(int exponent_bits)
    {
        const std::uint32_t width = bits(exponent_bits);
        if (width == 0)
            return 0;
        if (width > 32)
            corrupt("value wider than 32 bits");
        return (std::uint32_t{1} << (width - 1)) | bits(static_cast<int>(width) - 1);
    }

private:
    void refill()
    {
        while (count_ <= 56 && position_ < data_.size()) {
            buffer_ |= static_cast<std::uint64_t>(data_[position_++]) << (56 - count_);
            count_ += 8;
        }
    }

    std::span<const std::uint8_t> data_;
    std::size_t position_ = 0;
    std::uint64_t buffer_ = 0;
    int count_ = 0;
};

}  // namespace

void decode_stream(std::span<const std::uint8_t> packed, std::span<std::uint8_t> output)
{
    if (output.empty())
        return;
    if (packed.empty())
        corrupt("missing header");

    const std::uint8_t header_flags = packed[0];
    if (header_flags & ~kKnownHeaderFlags)
        corrupt("unknown header flags");

    std::span<const std::uint8_t> bit_stream = packed.subspan(1);
    std::span<const std::uint8_t> alphabet;
    if (header_flags & kHasAlphabet) {
        if (packed.size() < 5)
            corrupt("truncated header");
        // The stored offset is relative to byte 1.
        const std::size_t alphabet_at = std::size_t{1} + load_le32(packed.data() + 1);
        if (alphabet_at < 5 || alphabet_at >= packed.size())
            corrupt("alphabet offset out of range");
        const std::size_t alphabet_size = std::size_t{packed[alphabet_at]} + 1;
        if (alphabet_size > packed.size() - alphabet_at - 1)
            corrupt("truncated alphabet");
        alphabet = packed.subspan(alphabet_at + 1, alphabet_size);
        bit_stream = packed.subspan(5, alphabet_at - 5);
    }

    std::array<int, kNumContexts> offset_exponent{};
    std::array<int, kNumContexts> length_exponent{};
    for (std::size_t c = 0; c < kNumContexts; ++c) {
        const ContextFields fields = context_fields(c + 1, header_flags);
        offset_exponent[c] = exponent_width(fields.offset_bits);
        length_exponent[c] = exponent_width(fields.length_bits);
    }

    BitReader in(bit_stream);
    std::uint8_t* const out = output.data();
    const std::size_t size = output.size();
    std::size_t pos = 0;

    std::size_t context = 0;
    int literal_bits = 8;
    std::uint32_t literal_base = 0;
    std::uint64_t base_band = 0;     // offset of the previous match divided by 64
    std::uint32_t flagless_left = 0; // symbols left in the current block

    while (pos < size) {
        if (flagless_left > 0) {
            --flagless_left;
        } else if (in.bit() == kFlagLiteral) {
            const std::uint64_t index = std::uint64_t{in.bits(literal_bits)} + literal_base;
            if (!alphabet.empty()) {
                if (index >= alphabet.size())
                    corrupt("literal outside the alphabet");
                out[pos++] = alphabet[index];
            } else {
                if (index > 0xFF)
                    corrupt("literal out of range");
                out[pos++] = static_cast<std::uint8_t>(index);
            }
            continue;
        }

        std::uint64_t code = in.value(offset_exponent[context]);
        if (code >= kContext1 && code <= kContext3) {
            context = static_cast<std::size_t>(code - kContext1);
            continue;
        }
        if (code == kLiteralSwitch) {
            const std::uint32_t v = in.value(exponent_width(kLiteralSwitchBits));
            literal_base = v / 9;
            literal_bits = static_cast<int>(v % 9);
            continue;
        }

        const std::uint64_t length_field = in.value(length_exponent[context]);
        if (code == kRun) {
            const std::uint64_t length = length_field + kMinRunLength;
            if (pos == 0)
                corrupt("run before the first byte");
            if (length > size - pos)
                corrupt("run past the end of output");
            std::memset(out + pos, out[pos - 1], static_cast<std::size_t>(length));
            pos += static_cast<std::size_t>(length);
            continue;
        }
        if (code == kBlock) {
            flagless_left = static_cast<std::uint32_t>(length_field + kMinBlockLength);
            continue;
        }
        if (code == kBaseOffset)
            code = in.value(exponent_width(kBaseIndexBits)) + (base_band << kBaseIndexBits);

        base_band = code >> kBaseIndexBits;
        const std::uint64_t length = length_field + kMinMatchLength;
        if (code < kFirstOffset)
            corrupt("match offset collides with a reserved code");
        // distance >= length, so source and destination never overlap.
        const std::uint64_t distance = code - kFirstOffset + length;
        if (distance > pos)
            corrupt("match reaches before the start of output");
        if (length > size - pos)
            corrupt("match past the end of output");
        std::memcpy(out + pos, out + pos - distance, static_cast<std::size_t>(length));
        pos += static_cast<std::size_t>(length);
    }
}

}  // namespace flz
