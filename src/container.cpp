// .flz container: [unpacked size: u32 LE][packed size: u32 LE][packed stream].
#include <algorithm>

#include "flz/flz.hpp"
#include "format.hpp"

namespace flz {

namespace {

// The densest symbol, a 32770-byte copy, takes 25 bits, so a packed stream
// never expands by more than about 10500 times. Larger claims are corrupt and
// are refused before allocating the output.
constexpr std::uint64_t kMaxExpansion = 16384;

}  // namespace

std::vector<std::uint8_t> compress(std::span<const std::uint8_t> input, const CompressOptions& options)
{
    const std::vector<std::uint8_t> packed = encode_stream(input, options);
    std::vector<std::uint8_t> out(8 + packed.size());
    detail::store_le32(out.data(), static_cast<std::uint32_t>(input.size()));
    detail::store_le32(out.data() + 4, static_cast<std::uint32_t>(packed.size()));
    std::copy(packed.begin(), packed.end(), out.begin() + 8);
    return out;
}

std::vector<std::uint8_t> decompress(std::span<const std::uint8_t> container)
{
    if (container.size() < 4)
        throw Error("not an FLZ file: too short");
    const std::uint32_t size = detail::load_le32(container.data());
    if (size == 0)
        return {};
    if (container.size() < 8)
        throw Error("not an FLZ file: too short");
    const std::uint32_t packed = detail::load_le32(container.data() + 4);
    if (packed > container.size() - 8)
        throw Error("FLZ file is truncated");
    if (size > std::uint64_t{packed} * kMaxExpansion)
        throw Error("corrupt FLZ file: unpacked size does not fit the packed size");
    std::vector<std::uint8_t> out(size);
    decode_stream(container.subspan(8, packed), out);
    return out;
}

}  // namespace flz
