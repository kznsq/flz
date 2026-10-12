// flz: command-line front end for the FLZ compressor.
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "flz/flz.hpp"

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kExtension = ".flz";

void usage()
{
    std::fputs(
        "FLZ 1.0  Copyright (c) 2003-2026 Andrii Frolov aka kznsq\n"
        "\n"
        "Usage: flz [options] <input> [output]\n"
        "  Compresses <input> to <input>.flz, or decompresses <input>.flz to <input>.\n"
        "\n"
        "Options:\n"
        "  -m0 .. -m4  compression level: 0 fastest .. 4 best (default 3)\n"
        "  -d          decompress (the default when <input> ends with .flz)\n"
        "  -c          compress (the default otherwise)\n"
        "  -t          test: decompress <input> and report, without writing a file\n"
        "  -f          overwrite an existing output file\n"
        "  -v          print sizes and timings\n",
        stderr);
}

std::vector<std::uint8_t> read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw flz::Error("cannot open " + path.string());
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (in.bad())
        throw flz::Error("cannot read " + path.string());
    return data;
}

// Writes through a temporary file so a failure never leaves a partial output.
void write_file(const fs::path& path, const std::vector<std::uint8_t>& data)
{
    fs::path temp = path;
    temp += ".part";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw flz::Error("cannot create " + temp.string());
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        out.close();
        if (!out) {
            std::error_code ignored;
            fs::remove(temp, ignored);
            throw flz::Error("cannot write " + temp.string());
        }
    }
    fs::rename(temp, path);
}

double seconds_since(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

bool has_flz_extension(const fs::path& p)
{
    std::string ext = p.extension().string();
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == kExtension;
}

}  // namespace

int main(int argc, char* argv[])
{
    enum class Mode { Auto, Compress, Decompress, Test } mode = Mode::Auto;
    int level = flz::kDefaultLevel;
    bool force = false;
    bool verbose = false;
    std::optional<fs::path> input, output;

    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a.size() == 3 && a[0] == '-' && a[1] == 'm' && a[2] >= '0' && a[2] <= '4')
            level = a[2] - '0';
        else if (a == "-d")
            mode = Mode::Decompress;
        else if (a == "-c")
            mode = Mode::Compress;
        else if (a == "-t")
            mode = Mode::Test;
        else if (a == "-f")
            force = true;
        else if (a == "-v" || a == "-debug")
            verbose = true;
        else if (a == "-h" || a == "--help") {
            usage();
            return 0;
        } else if (a.size() > 1 && a[0] == '-') {
            std::fprintf(stderr, "flz: unknown option %s\n", argv[i]);
            return 2;
        } else if (!input)
            input = argv[i];
        else if (!output)
            output = argv[i];
        else {
            std::fprintf(stderr, "flz: unexpected argument %s\n", argv[i]);
            return 2;
        }
    }
    if (!input) {
        usage();
        return 2;
    }
    if (mode == Mode::Auto)
        mode = has_flz_extension(*input) ? Mode::Decompress : Mode::Compress;

    try {
        const auto start = std::chrono::steady_clock::now();
        const std::vector<std::uint8_t> in = read_file(*input);

        std::vector<std::uint8_t> result;
        if (mode == Mode::Compress) {
            flz::CompressOptions options;
            options.level = level;
            if (isatty(STDERR_FILENO))
                options.progress = [](double f) { std::fprintf(stderr, "\r%3d%%", static_cast<int>(f * 100)); };
            result = flz::compress(in, options);
            if (options.progress)
                std::fputs("\r    \r", stderr);
        } else {
            result = flz::decompress(in);
        }
        const double elapsed = seconds_since(start);

        if (mode == Mode::Test) {
            std::printf("%s: OK, %zu bytes\n", input->string().c_str(), result.size());
            return 0;
        }

        if (!output) {
            output = *input;
            if (mode == Mode::Compress)
                *output += kExtension;
            else if (has_flz_extension(*input))
                output->replace_extension();
            else
                *output += ".out";
        }
        if (!force && fs::exists(*output)) {
            std::fprintf(stderr, "flz: %s exists; use -f to overwrite\n", output->string().c_str());
            return 1;
        }
        write_file(*output, result);

        if (verbose) {
            const std::size_t original = mode == Mode::Compress ? in.size() : result.size();
            const std::size_t packed = mode == Mode::Compress ? result.size() : in.size();
            std::printf("%s -> %s\n", input->string().c_str(), output->string().c_str());
            std::printf("%zu -> %zu bytes (%.2f%%), %.3f s, %.1f MB/s\n", in.size(), result.size(),
                        original ? 100.0 * static_cast<double>(packed) / static_cast<double>(original) : 0.0, elapsed,
                        elapsed > 0 ? static_cast<double>(original) / elapsed / 1e6 : 0.0);
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "flz: %s\n", e.what());
        return 1;
    }
}
