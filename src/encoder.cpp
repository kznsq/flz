// Encoder.
//
// 1. Parse: for each enabled context, a greedy parse with a short lazy
//    lookahead turns the input into literals, matches and runs, using that
//    context's offset and length limits. The parses are independent and run in
//    parallel.
// 2. Choose: the parses are walked in step and the input is cut into segments,
//    each coded with the context that spent the fewest bits on it.
// 3. Literals: literals are replaced by their rank in a frequency-sorted
//    alphabet, then grouped into ranges of similar ranks so each range is
//    coded with only as many bits as it needs.
// 4. Emit: the chosen symbols are written as a bit stream.
//
// Before the real pass, levels above 0 compress a small window three times to
// pick the context 1 length width and the context 2 offset width.
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <climits>
#include <cstring>
#include <future>

#include "flz/flz.hpp"
#include "format.hpp"
#include "match_finder.hpp"

namespace flz {
namespace {

using namespace detail;

constexpr std::int64_t kTreeResetInterval = std::int64_t{8} << 20;  // the match finder restarts every 8 MiB
constexpr std::int64_t kInsertAhead = 48;     // positions indexed ahead of the parse point, for the lookahead
constexpr int kMaxEntriesPerKey = 16 * 1024;  // positions kept per three-byte key
constexpr int kPruneInterval = 1024;          // insertions between removals of out-of-reach positions
constexpr std::size_t kRecentLiterals = 48;   // literals used to estimate the cost of one literal
constexpr std::size_t kRecentLengths = 256;   // match lengths averaged into the lookahead distance
constexpr int kInitialLookahead = 256;
constexpr int kGainShift = 13;                // gains are bytes per bit, fixed point
constexpr int kEstimatedLiteralCost = 9;      // flag bit + 8 bits, used when choosing contexts
constexpr int kLiteralRangeCost = 19;         // estimated cost of switching literal ranges
constexpr std::size_t kMaxRangedLiterals = std::size_t{32} << 20;
constexpr int kTrialWindow = 8 * 1024;

struct Settings {
    int level = kDefaultLevel;
    std::array<ContextFields, kNumContexts> fields{{{15, 3}, {24, 7}, {24, 15}}};
    std::array<bool, kNumContexts> enabled{true, true, true};
    bool literal_ranges = true;

    std::uint8_t header_flags(bool alphabet) const
    {
        return static_cast<std::uint8_t>((fields[0].length_bits > 3 ? kLongLengths1 : 0) |
                                         (fields[1].offset_bits > 15 ? kWideOffsets2 : 0) |
                                         (alphabet ? kHasAlphabet : 0));
    }
};

// Output of one context's parse.
struct Parse {
    std::vector<std::uint8_t> flags;        // kFlagLiteral or kFlagMatch per symbol
    std::vector<std::uint32_t> distances;   // per match-class symbol; 0 marks a run
    std::vector<std::uint32_t> lengths;     // per match-class symbol, in bytes
};

// Symbols in their coded form.
struct Symbols {
    std::vector<std::uint8_t> flags;
    std::vector<std::uint32_t> codes;    // offset code, or kRun
    std::vector<std::uint32_t> lengths;  // length field (bytes minus the minimum)
    std::vector<std::uint8_t> literals;

    void append(const Symbols& o)
    {
        flags.insert(flags.end(), o.flags.begin(), o.flags.end());
        codes.insert(codes.end(), o.codes.begin(), o.codes.end());
        lengths.insert(lengths.end(), o.lengths.begin(), o.lengths.end());
        literals.insert(literals.end(), o.literals.begin(), o.literals.end());
    }
    void clear()
    {
        flags.clear();
        codes.clear();
        lengths.clear();
        literals.clear();
    }
};

struct Segment {
    std::int64_t length;
    std::size_t context;  // 1..3
};

// Number of candidate positions examined per search before older ones are dropped.
int max_candidates(int level, int context, int length_bits, std::int64_t input_length)
{
    if (level <= 1)
        return 4;
    const std::int64_t budget = std::int64_t{1} << 31;
    std::int64_t n = (level == 4 ? budget * 16 : budget) / (input_length + 1) / context;
    if (level == 2)
        n /= 4;
    n = (n * length_bits) >> 4;
    return static_cast<int>(std::clamp<std::int64_t>(n, 4, INT_MAX));
}

class BitWriter {
public:
    void bit(std::uint32_t b) { bits(b, 1); }

    void bits(std::uint32_t value, int n)
    {
        assert(n <= 32 && (n == 32 || value >> n == 0));
        buffer_ = (buffer_ << n) | value;
        count_ += n;
        while (count_ >= 8) {
            count_ -= 8;
            bytes_.push_back(static_cast<std::uint8_t>(buffer_ >> count_));
        }
    }

    void value(std::uint32_t v, int max_value_bits)
    {
        assert(bit_width(v) <= max_value_bits);
        const int width = bit_width(v);
        bits(static_cast<std::uint32_t>(width), exponent_width(max_value_bits));
        if (width > 1)
            bits(v & ((std::uint32_t{1} << (width - 1)) - 1), width - 1);
    }

    std::vector<std::uint8_t> finish()
    {
        if (count_ > 0)
            bits(0, 8 - count_);
        return std::move(bytes_);
    }

private:
    std::vector<std::uint8_t> bytes_;
    std::uint64_t buffer_ = 0;
    int count_ = 0;
};

class Parser {
public:
    Parser(const std::uint8_t* data, std::int64_t end, int context, ContextFields fields, int level)
        : in_(data), end_(end), fields_(fields),
          max_distance_((std::int64_t{1} << fields.offset_bits) - 1 + kMinMatchLength - kFirstOffset),
          max_length_((std::int64_t{1} << fields.length_bits) - 1 + kMinMatchLength),
          max_run_((std::int64_t{1} << fields.length_bits) - 1 + kMinRunLength),
          max_candidates_(max_candidates(level, context, fields.length_bits, end)),
          retry_lookahead_(level != 0)
    {
    }

    Parse run(std::int64_t start, std::atomic<std::int64_t>& done)
    {
        Parse out;
        std::array<int, kRecentLiterals> recent_literals{};
        std::size_t recent_literal_at = 0;
        std::array<int, kRecentLengths> recent_lengths{};
        std::size_t recent_length_at = 0;
        int recent_length_count = 0;
        std::int64_t recent_length_sum = 0;

        const auto literal = [&](std::uint8_t c) {
            out.flags.push_back(kFlagLiteral);
            recent_literal_at %= kRecentLiterals;
            recent_literals[recent_literal_at++] = c;
        };

        // The finder restarts every kTreeResetInterval bytes and indexes a little ahead.
        finder_.reserve(static_cast<std::size_t>(std::min(end_ - start, kTreeResetInterval) + kInsertAhead));
        std::int64_t last_reset = 0;
        cur_ = start;
        while (cur_ < end_) {
            if (cur_ - last_reset >= kTreeResetInterval) {
                finder_.clear();
                last_reset = cur_;
            }
            index_pending();

            const auto [lo, hi] = std::minmax_element(recent_literals.begin(), recent_literals.end());
            const int literal_bits = bit_width(static_cast<std::uint32_t>(*hi - *lo));

            int best_match_gain = 0, best_run_gain = 0;
            std::int64_t best_match_pos = 0, best_match_len = 0, best_match_delay = 0;
            std::int64_t best_run_len = 0, best_run_delay = 0;
            std::int64_t lookahead = kInitialLookahead;
            bool retry = retry_lookahead_;
            for (std::int64_t i = 0; cur_ + i < end_ - kMinMatchLength && i < lookahead; ++i) {
                const std::int64_t at = cur_ + i;
                // Gains count the `i` literals coded before the candidate. A
                // literal costs at least its flag bit, even when recent literals
                // were all equal.
                const int delay_bits = static_cast<int>(i) * std::max(literal_bits, 1);
                if (at > 0) {
                    const std::uint8_t previous = in_[at - 1];
                    std::int64_t run = 0;
                    while (run < max_run_ && at + run < end_ && in_[at + run] == previous)
                        ++run;
                    if (run >= kMinRunLength) {
                        const int bits = 1 + value_cost(kRun, fields_.offset_bits) +
                                         value_cost(static_cast<std::uint32_t>(run - kMinRunLength), fields_.length_bits);
                        const int gain = static_cast<int>(((run + i) << kGainShift) / (bits + delay_bits));
                        if (gain > best_run_gain) {
                            best_run_gain = gain;
                            best_run_len = run;
                            best_run_delay = i;
                        }
                    }
                }

                const int bits = find_match(at);
                const int gain = match_len_ ? static_cast<int>(((match_len_ + i) << kGainShift) / (bits + delay_bits)) : 0;
                if (gain >= best_match_gain) {
                    best_match_gain = gain;
                    best_match_pos = match_pos_;
                    best_match_len = match_len_;
                    best_match_delay = i;
                } else {
                    // One worse position is tolerated before the lookahead stops.
                    if (!retry)
                        break;
                    retry = false;
                }
                if (recent_length_count)
                    lookahead = recent_length_sum / recent_length_count;
            }

            if (best_run_gain > best_match_gain) {
                for (std::int64_t i = 0; i < best_run_delay; ++i)
                    literal(in_[cur_++]);
                out.flags.push_back(kFlagMatch);
                out.distances.push_back(0);
                out.lengths.push_back(static_cast<std::uint32_t>(best_run_len));
                cur_ += best_run_len;
            } else if (best_match_len) {
                for (std::int64_t i = 0; i < best_match_delay; ++i)
                    literal(in_[cur_++]);
                const std::int64_t distance = cur_ - best_match_pos;
                assert(distance >= best_match_len);
                out.flags.push_back(kFlagMatch);
                out.distances.push_back(static_cast<std::uint32_t>(distance));
                out.lengths.push_back(static_cast<std::uint32_t>(best_match_len));
                recent_length_at %= kRecentLengths;
                recent_length_sum += best_match_len - recent_lengths[recent_length_at];
                recent_lengths[recent_length_at++] = static_cast<int>(best_match_len);
                recent_length_count = std::min(recent_length_count + 1, static_cast<int>(kRecentLengths));
                base_band_ = offset_code(static_cast<std::uint32_t>(distance),
                                         static_cast<std::uint32_t>(best_match_len)) >> kBaseIndexBits;
                cur_ += best_match_len;
            } else {
                literal(in_[cur_++]);
            }
            done.store(cur_ - start, std::memory_order_relaxed);
        }
        return out;
    }

private:
    // Indexes every position up to three bytes before the parse point, plus a
    // stretch ahead of it for the lookahead.
    void index_pending()
    {
        std::int64_t count = cur_ - indexed_ - kMinMatchLength;
        if (cur_ + kInsertAhead < end_ - kMinMatchLength)
            count += kInsertAhead;
        for (std::int64_t i = 0; i < count; ++i) {
            const std::int64_t position = indexed_ + i;
            finder_.insert(in_ + position, static_cast<std::uint32_t>(position), kMaxEntriesPerKey);
            // Now and then drop positions that fell out of reach.
            if (position - max_distance_ >= 0 && prune_counter_++ >= kPruneInterval) {
                if (std::uint32_t* slot = finder_.find(in_ + position - max_distance_))
                    finder_.prune(slot, cur_ - max_distance_);
                prune_counter_ = 0;
            }
        }
        if (count > 0)
            indexed_ += count;
    }

    // Best match for position `pos`: sets match_len_ (0 when none) and
    // match_pos_, returns its cost in bits.
    int find_match(std::int64_t pos)
    {
        match_len_ = 0;
        match_pos_ = 0;
        std::uint32_t* slot = finder_.find(in_ + pos);
        if (!slot)
            return 0;

        int best_bits = 0, best_gain = 0;
        finder_.scan(slot, [&](std::int64_t position, int visited) {
            const std::int64_t distance = pos - position;
            if (distance >= kMinMatchLength && distance < max_distance_) {
                // A candidate only matters if it is at least as long as the best so
                // far, so the known prefix is compared first.
                const std::int64_t known = match_len_ & ~std::int64_t{3};
                assert(position + known <= pos);
                if (match_len_ == 0 || std::memcmp(in_ + position, in_ + pos, static_cast<std::size_t>(known)) == 0) {
                    std::int64_t len = known;
                    for (std::int64_t a = position + known, b = pos + known;
                         a < pos && b < end_ && len < max_length_ && in_[a] == in_[b]; ++a, ++b)
                        ++len;
                    if (len >= kMinMatchLength && len >= match_len_) {
                        const auto code = offset_code(static_cast<std::uint32_t>(distance), static_cast<std::uint32_t>(len));
                        const int bits = 1 + value_cost(code, fields_.offset_bits) +
                                         value_cost(static_cast<std::uint32_t>(len - kMinMatchLength), fields_.length_bits);
                        int gain = static_cast<int>((len << kGainShift) / bits);
                        if (base_band_ == (code >> kBaseIndexBits))
                            gain += gain >> 4;  // the offset can likely be coded relative to the previous one
                        if (gain >= best_gain) {
                            best_gain = gain;
                            best_bits = bits;
                            match_len_ = len;
                            match_pos_ = position;
                        }
                    }
                }
            }
            return !(visited > max_candidates_ || position < pos - max_distance_);
        });
        return best_bits;
    }

    const std::uint8_t* in_;
    std::int64_t end_;
    ContextFields fields_;
    std::int64_t max_distance_;  // exclusive
    std::int64_t max_length_;
    std::int64_t max_run_;
    int max_candidates_;
    bool retry_lookahead_;

    MatchFinder finder_;
    std::int64_t cur_ = 0;
    std::int64_t indexed_ = 0;   // positions below this are in the finder
    int prune_counter_ = 0;
    std::uint32_t base_band_ = 0;
    std::int64_t match_len_ = 0;
    std::int64_t match_pos_ = 0;
};

// Runs the enabled contexts' parses in parallel, reporting progress from this thread.
std::array<Parse, kNumContexts> parse_all(std::span<const std::uint8_t> input, const Settings& s,
                                          std::int64_t start, std::int64_t end,
                                          const std::function<void(double)>& progress)
{
    std::array<Parse, kNumContexts> parses;
    std::array<std::atomic<std::int64_t>, kNumContexts> done{};
    std::array<std::future<Parse>, kNumContexts> jobs;
    int enabled = 0;
    for (std::size_t c = 0; c < kNumContexts; ++c) {
        if (!s.enabled[c])
            continue;
        ++enabled;
        jobs[c] = std::async(std::launch::async, [&, c] {
            Parser parser(input.data(), end, static_cast<int>(c + 1), s.fields[c], s.level);
            return parser.run(start, done[c]);
        });
    }
    for (std::size_t c = 0; c < kNumContexts; ++c) {
        if (!jobs[c].valid())
            continue;
        while (progress && jobs[c].wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
            std::int64_t sum = 0;
            for (auto& d : done)
                sum += d.load(std::memory_order_relaxed);
            progress(static_cast<double>(sum) / static_cast<double>(enabled * (end - start)));
        }
        parses[c] = jobs[c].get();
    }
    return parses;
}

// Walks the parses in step and assigns each stretch of input to one context.
void choose_contexts(std::span<const std::uint8_t> input, const Settings& s,
                     const std::array<Parse, kNumContexts>& parses, std::int64_t start, std::int64_t end,
                     Symbols& chosen, std::vector<Segment>& segments)
{
    struct Cursor {
        std::size_t flag = 0, match = 0;
        std::int64_t pos = 0;
    };
    std::array<Cursor, kNumContexts> cursor;
    for (auto& c : cursor)
        c.pos = start;

    std::array<Symbols, kNumContexts> pending, step;
    std::array<std::int64_t, kNumContexts> pending_bits{}, pending_length{};
    std::array<std::int64_t, kNumContexts> step_bits{}, step_length{};

    // Moves context `c` (0-based) forward by one symbol.
    const auto advance = [&](std::size_t c) {
        const Parse& p = parses[c];
        Cursor& at = cursor[c];
        Symbols& out = step[c];
        const std::uint8_t flag = p.flags[at.flag++];
        out.flags.push_back(flag);
        if (flag == kFlagMatch) {
            const std::uint32_t distance = p.distances[at.match];
            const std::uint32_t length = p.lengths[at.match++];
            const std::uint32_t code = distance ? offset_code(distance, length) : std::uint32_t{kRun};
            const std::uint32_t field = length - (distance ? kMinMatchLength : kMinRunLength);
            out.codes.push_back(code);
            out.lengths.push_back(field);
            step_bits[c] += 1 + value_cost(code, s.fields[c].offset_bits) + value_cost(field, s.fields[c].length_bits);
            step_length[c] += length;
            at.pos += length;
        } else {
            out.literals.push_back(input[static_cast<std::size_t>(at.pos)]);
            step_bits[c] += kEstimatedLiteralCost;
            step_length[c] += 1;
            at.pos += 1;
        }
    };
    const auto behind = [&](std::size_t c) {
        return s.enabled[c] && std::any_of(cursor.begin(), cursor.end(), [&](const Cursor& o) { return cursor[c].pos < o.pos; });
    };
    // Context `c` beats every other enabled context strictly.
    const auto cheapest = [&](std::size_t c) {
        if (!s.enabled[c])
            return false;
        for (std::size_t o = 0; o < kNumContexts; ++o)
            if (o != c && s.enabled[o] && step_bits[c] >= step_bits[o])
                return false;
        return true;
    };

    std::size_t best_last_step = 1;
    std::size_t context = 1;

    // Ends the pending segment, moving it to another context only if that
    // saves more than the switch costs.
    const auto close_pending = [&] {
        const auto& b = pending_bits;
        if (context == 1) {
            if (s.enabled[1] && b[1] + 14 <= b[0])
                context = 2;
            else if (s.enabled[2] && b[2] + 14 <= b[0])
                context = 3;
        } else if (context == 2) {
            if (s.enabled[0] && b[0] + 14 <= b[1])
                context = 1;
            else if (s.enabled[2] && b[2] + 15 <= b[1])
                context = 3;
        } else {
            if (s.enabled[0] && b[0] + 14 <= b[2])
                context = 1;
            else if (s.enabled[1] && b[1] + 16 <= b[2])
                context = 2;
        }
        chosen.append(pending[context - 1]);
        segments.push_back({pending_length[0], context});
        for (std::size_t c = 0; c < kNumContexts; ++c) {
            pending[c].clear();
            pending_bits[c] = 0;
            pending_length[c] = 0;
        }
    };
    const auto all_before_end = [&] {
        return std::all_of(cursor.begin(), cursor.end(), [&](const Cursor& c) { return c.pos < end; });
    };
    while (all_before_end()) {
        for (std::size_t c = 0; c < kNumContexts; ++c) {
            step[c].clear();
            step_bits[c] = 0;
            step_length[c] = 0;
        }
        for (std::size_t c = 0; c < kNumContexts; ++c)
            if (s.enabled[c])
                advance(c);
        // Bring every context to the same input position.
        for (;;) {
            std::size_t c = 0;
            while (c < kNumContexts && !behind(c))
                ++c;
            if (c == kNumContexts)
                break;
            advance(c);
        }

        std::size_t best = best_last_step;
        for (std::size_t c = 0; c < kNumContexts; ++c)
            if (cheapest(c))
                best = c + 1;

        if (best != best_last_step) {
            close_pending();
            best_last_step = best;
        }
        for (std::size_t c = 0; c < kNumContexts; ++c) {
            pending[c].append(step[c]);
            pending_bits[c] += step_bits[c];
            pending_length[c] += step_length[c];
        }
    }
    if (std::any_of(pending_length.begin(), pending_length.end(), [](std::int64_t n) { return n != 0; }))
        close_pending();
}

// Bytes sorted by falling frequency (ties by byte value); absent bytes are left out.
std::vector<std::uint8_t> make_alphabet(std::span<const std::uint8_t> data)
{
    std::array<std::size_t, 256> count{};
    for (std::uint8_t b : data)
        ++count[b];
    std::vector<std::uint8_t> alphabet;
    for (std::size_t b = 0; b < count.size(); ++b)
        if (count[b])
            alphabet.push_back(static_cast<std::uint8_t>(b));
    std::stable_sort(alphabet.begin(), alphabet.end(), [&](std::uint8_t a, std::uint8_t b) { return count[a] > count[b]; });
    return alphabet;
}

// A run of consecutive literals coded with `bit_width(max - min)` bits each.
struct LiteralRange {
    std::uint32_t start;  // index of the first literal
    std::uint32_t next;   // index of the next range in `ranges`, or kNoRange
    std::uint8_t min, max;
};
constexpr std::uint32_t kNoRange = 0xFFFFFFFFu;

// Greedily merges neighbouring literals into ranges while that lowers the
// estimated cost. Pass t merges neighbours whose minimum or maximum differ by
// less than t. Returns the ranges as a list starting at index 0.
std::vector<LiteralRange> make_literal_ranges(std::span<const std::uint8_t> literals)
{
    const auto n = static_cast<std::uint32_t>(literals.size());
    std::vector<LiteralRange> r(n);
    for (std::uint32_t i = 0; i < n; ++i)
        r[i] = {i, i + 1 < n ? i + 1 : kNoRange, literals[i], literals[i]};
    if (n == 0)
        return r;

    const auto chars = [&](std::uint32_t i) -> std::int64_t {
        return (r[i].next != kNoRange ? r[r[i].next].start : n) - r[i].start;
    };
    const auto price = [&](std::int64_t count, int lo, int hi) {
        return kLiteralRangeCost + count * bit_width(static_cast<std::uint32_t>(hi - lo));
    };

    for (int t = 0; t <= 256; ++t) {
        std::uint32_t i = 0;
        while (r[i].next != kNoRange) {
            LiteralRange& a = r[i];
            const LiteralRange& b = r[a.next];
            const int dmax = a.max - b.max, dmin = a.min - b.min;
            if (dmax * dmax < t * t || dmin * dmin < t * t) {
                const std::int64_t ca = chars(i), cb = chars(a.next);
                const int lo = std::min(a.min, b.min), hi = std::max(a.max, b.max);
                if (price(ca + cb, lo, hi) < price(ca, a.min, a.max) + price(cb, b.min, b.max)) {
                    a.min = static_cast<std::uint8_t>(lo);
                    a.max = static_cast<std::uint8_t>(hi);
                    a.next = b.next;
                    continue;
                }
            }
            i = a.next;
        }
    }
    return r;
}

std::int64_t literal_ranges_cost(const std::vector<LiteralRange>& r, std::size_t literal_count)
{
    std::int64_t total = 0;
    for (std::uint32_t i = 0; i != kNoRange; i = r[i].next) {
        const auto count = static_cast<std::int64_t>((r[i].next != kNoRange ? r[r[i].next].start : literal_count) - r[i].start);
        total += kLiteralRangeCost + count * bit_width(static_cast<std::uint32_t>(r[i].max - r[i].min));
    }
    return total;
}

std::vector<std::uint8_t> emit(const Settings& s, Symbols& chosen, const std::vector<Segment>& segments)
{
    // Literals become alphabet ranks.
    const std::vector<std::uint8_t> alphabet = make_alphabet(chosen.literals);
    bool use_alphabet = s.level != 0 && !alphabet.empty();
    std::vector<LiteralRange> ranges;
    if (use_alphabet) {
        std::array<std::uint8_t, 256> rank{};
        for (std::size_t i = 0; i < alphabet.size(); ++i)
            rank[alphabet[i]] = static_cast<std::uint8_t>(i);
        std::vector<std::uint8_t> ranked(chosen.literals.size());
        std::transform(chosen.literals.begin(), chosen.literals.end(), ranked.begin(), [&](std::uint8_t b) { return rank[b]; });

        if (s.literal_ranges && ranked.size() < kMaxRangedLiterals) {
            ranges = make_literal_ranges(ranked);
            const auto plain_bits = static_cast<std::int64_t>(ranked.size()) * 8;
            if (literal_ranges_cost(ranges, ranked.size()) + 8 + static_cast<std::int64_t>(alphabet.size()) * 8 >= plain_bits) {
                ranges.clear();
                use_alphabet = false;  // ranking alone does not pay for the alphabet
            }
        }
        if (use_alphabet)
            chosen.literals = std::move(ranked);
    }

    BitWriter w;
    std::size_t context = 1;
    std::uint32_t base_band = 0;
    std::uint32_t flagless_left = 0;
    int literal_bits = 8;
    int literal_base = 0;
    std::uint32_t range = ranges.empty() ? kNoRange : 0;
    std::size_t fi = 0, mi = 0, li = 0;

    // Inside a block, match-class symbols carry no flag bit.
    const auto match_flag = [&] {
        if (flagless_left == 0)
            w.bit(kFlagMatch);
        else
            --flagless_left;
    };

    for (const Segment& seg : segments) {
        if (seg.context != context) {
            match_flag();
            w.value(static_cast<std::uint32_t>(kContext1 + seg.context - 1), s.fields[context - 1].offset_bits);
            context = seg.context;
        }
        const ContextFields f = s.fields[context - 1];
        for (std::int64_t done = 0; done < seg.length;) {
            if (flagless_left == 0) {
                // Start a block when enough match-class symbols follow in a row.
                std::size_t run = 0;
                const auto max_run = static_cast<std::size_t>((std::int64_t{1} << f.length_bits) - 1 + kMinBlockLength);
                while (fi + run < chosen.flags.size() && run < max_run && chosen.flags[fi + run] == kFlagMatch)
                    ++run;
                const auto price = static_cast<std::size_t>(value_cost(kBlock, f.offset_bits) +
                                                            value_cost(static_cast<std::uint32_t>(run), f.length_bits));
                if (run >= price && run >= kMinBlockLength) {
                    w.bit(kFlagMatch);
                    w.value(kBlock, f.offset_bits);
                    w.value(static_cast<std::uint32_t>(run - kMinBlockLength), f.length_bits);
                    flagless_left = static_cast<std::uint32_t>(run);
                }
            }

            if (chosen.flags[fi++] == kFlagMatch) {
                const std::uint32_t code = chosen.codes[mi];
                const std::uint32_t length = chosen.lengths[mi++];
                match_flag();
                if (code >= kFirstOffset) {
                    const std::uint32_t band = code >> kBaseIndexBits;
                    const std::uint32_t index = code & ((1u << kBaseIndexBits) - 1);
                    const int relative_cost = std::max(bit_width(index) - 1, 0) + exponent_width(kBaseIndexBits) +
                                              exponent_width(f.offset_bits);
                    if (band == base_band && relative_cost < value_cost(code, f.offset_bits)) {
                        w.value(kBaseOffset, f.offset_bits);
                        w.value(length, f.length_bits);
                        w.value(index, kBaseIndexBits);
                    } else {
                        w.value(code, f.offset_bits);
                        w.value(length, f.length_bits);
                    }
                    base_band = band;
                    done += length + kMinMatchLength;
                } else {
                    w.value(code, f.offset_bits);
                    w.value(length, f.length_bits);
                    done += length + kMinRunLength;
                }
            } else {
                if (range != kNoRange && ranges[range].start == li) {
                    literal_bits = bit_width(static_cast<std::uint32_t>(ranges[range].max - ranges[range].min));
                    literal_base = literal_bits == 8 ? 0 : ranges[range].min;
                    match_flag();
                    w.value(kLiteralSwitch, f.offset_bits);
                    w.value(static_cast<std::uint32_t>(literal_base * 9 + literal_bits), kLiteralSwitchBits);
                    range = ranges[range].next;
                }
                w.bit(kFlagLiteral);
                const int v = chosen.literals[li++] - literal_base;
                assert(v >= 0 && bit_width(static_cast<std::uint32_t>(v)) <= literal_bits);
                w.bits(static_cast<std::uint32_t>(v), literal_bits);
                done += 1;
            }
        }
    }

    const std::vector<std::uint8_t> bits = w.finish();
    std::vector<std::uint8_t> out;
    out.reserve(1 + 4 + bits.size() + 1 + alphabet.size());
    out.push_back(s.header_flags(use_alphabet));
    if (use_alphabet) {
        out.resize(5);
        // Offset relative to byte 1 of the alphabet that follows the bit stream.
        store_le32(out.data() + 1, static_cast<std::uint32_t>(4 + bits.size()));
    }
    out.insert(out.end(), bits.begin(), bits.end());
    if (use_alphabet) {
        out.push_back(static_cast<std::uint8_t>(alphabet.size() - 1));
        out.insert(out.end(), alphabet.begin(), alphabet.end());
    }
    return out;
}

std::vector<std::uint8_t> encode_range(std::span<const std::uint8_t> input, const Settings& s, std::int64_t start,
                                       std::int64_t end, const std::function<void(double)>& progress)
{
    const auto parses = parse_all(input, s, start, end, progress);
    Symbols chosen;
    std::vector<Segment> segments;
    choose_contexts(input, s, parses, start, end, chosen, segments);
    return emit(s, chosen, segments);
}

}  // namespace

std::vector<std::uint8_t> encode_stream(std::span<const std::uint8_t> input, const CompressOptions& options)
{
    if (options.level < kMinLevel || options.level > kMaxLevel)
        throw Error("compression level must be between 0 and 4");
    if (input.size() > kMaxInputSize)
        throw Error("input is larger than 4 GiB - 1 bytes");
    if (input.empty())
        return {};

    const auto size = static_cast<std::int64_t>(input.size());
    Settings s;
    s.level = options.level;

    if (s.level != 0) {
        // Compress a small window with each width combination and keep the smallest.
        Settings trial = s;
        trial.level = 0;
        trial.literal_ranges = false;
        const std::int64_t window = std::min<std::int64_t>(kTrialWindow, size);
        std::int64_t start = (size >> 2) % (1 << 20) - window;
        if (start + window > size)
            start = size - window;
        start = std::max<std::int64_t>(start, 0);

        constexpr std::array<std::array<int, 2>, 3> kTrials{{{24, 3}, {15, 3}, {24, 7}}};  // context 2 offset, context 1 length
        std::array<std::size_t, 3> sizes{};
        for (std::size_t t = 0; t < kTrials.size(); ++t) {
            trial.fields[1].offset_bits = kTrials[t][0];
            trial.fields[0].length_bits = kTrials[t][1];
            sizes[t] = encode_range(input, trial, start, start + window, {}).size();
        }
        std::size_t pick = 2;
        if (sizes[0] && sizes[0] < sizes[1] && sizes[0] < sizes[2])
            pick = 0;
        else if (sizes[1] && sizes[1] < sizes[0] && sizes[1] < sizes[2])
            pick = 1;
        s.fields[1].offset_bits = kTrials[pick][0];
        s.fields[0].length_bits = kTrials[pick][1];
    }

    s.enabled = {true, s.level >= 1, s.level >= 2};
    s.literal_ranges = true;
    auto out = encode_range(input, s, 0, size, options.progress);
    if (options.progress)
        options.progress(1.0);
    return out;
}

}  // namespace flz
