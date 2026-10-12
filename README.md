# FLZ

A lossless compressor from the LZ77 family, by Andrii Frolov aka kznsq.

FLZ trades speed for density: it parses the input three times with different
field widths ("contexts"), picks the cheapest context for each stretch of input,
and codes literal bytes through a frequency-sorted alphabet in ranges of
similar values.

## Build

```bash
cmake -S . -B build -G Ninja
```

```bash
cmake --build build
```

```bash
ctest --test-dir build --output-on-failure
```

Needs CMake 3.20 and a C++20 compiler; there are no other dependencies. On
Apple silicon an Intel CMake running under Rosetta is detected and the build
still targets `arm64`.

## Command line

```
flz [options] <input> [output]
  -m0 .. -m4  compression level: 0 fastest .. 4 best (default 3)
  -d          decompress (the default when <input> ends with .flz)
  -c          compress (the default otherwise)
  -t          test: decompress <input> and report, without writing a file
  -f          overwrite an existing output file
  -v          print sizes and timings (-debug is accepted too)
```

`file` compresses to `file.flz`; `file.flz` decompresses to `file`.

## Library

```cpp
#include <flz/flz.hpp>

std::vector<std::uint8_t> packed = flz::compress(data, {.level = 3});
std::vector<std::uint8_t> back = flz::decompress(packed);  // throws flz::Error on damaged input
```

`encode_stream` / `decode_stream` work on the bare packed stream when the
unpacked size is stored elsewhere. All functions are reentrant and can be called
from several threads at once.

## File format

A `.flz` file is

| bytes | content |
|---|---|
| 0–3 | unpacked size, unsigned 32-bit little-endian |
| 4–7 | packed size, unsigned 32-bit little-endian (may be omitted when the unpacked size is 0) |
| 8– | packed stream |

The packed stream:

| bytes | content |
|---|---|
| 0 | header flags: 1 = literals go through an alphabet, 2 = context 2 offsets are 24 bits wide (else 15), 4 = context 1 lengths are 7 bits wide (else 3) |
| 1–4 | only with flag 1: offset of the alphabet, counted from byte 1, little-endian |
| … | bit stream, most significant bit of each byte first |
| … | only with flag 1: alphabet size minus 1 (one byte), then the alphabet |

**Numbers.** A field that holds values of up to *w* bits is written as an
exponent of `bit_width(w)` bits, equal to the bit width of the value (0 for the
value 0), followed by the value's bits below its leading one.

**Contexts.** Three sets of field widths; decoding starts in context 1.

| context | offset width | length width |
|---|---|---|
| 1 | 15 | 3 or 7 (header flag 4) |
| 2 | 24 or 15 (header flag 2) | 7 |
| 3 | 24 | 15 |

**Symbols.** Each symbol starts with a flag bit, except inside a block (below).

* Flag 1, literal: `literal_bits` bits (initially 8), plus `literal_base`
  (initially 0). With an alphabet, the sum indexes it; otherwise it is the byte.
* Flag 0: a code in the current offset width, then:

| code | meaning | followed by |
|---|---|---|
| 0 | literal switch | a 15-bit-wide number *v*: `literal_base = v / 9`, `literal_bits = v % 9` |
| 1 | match relative to the previous match | length, then a 6-bit-wide index *i*: offset = *i* + 64 × (previous offset / 64) |
| 2, 3, 4 | switch to context 1, 2, 3 | nothing |
| 5 | block | length *n*: the next *n* + 11 symbols other than literals carry no flag bit |
| 6 | run | length *n*: repeat the previous byte *n* + 1 times |
| ≥ 7 | match | length *n*: copy *n* + 3 bytes from distance offset − 7 + *n* + 3 |

Every match offset (direct or relative) becomes the "previous offset". Lengths
use the current context's length width. A match never overlaps the bytes it
produces (distance ≥ length).

## Notes

* The three context parses run in parallel, so peak memory holds three match
  finders at once.
* Inputs larger than 4 GiB − 1 bytes are refused: the container stores 32-bit sizes.
* Damaged input raises `flz::Error`; the decoder checks every read and copy.

## License

MIT, see [LICENSE](LICENSE). Copyright (c) 2003-2026 Andrii Frolov aka kznsq.
