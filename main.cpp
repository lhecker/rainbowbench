// Copyright (c) 2022 Leonard Hecker <leonard@hecker.io>
// Licensed under the MIT license.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <conio.h>
#include <fcntl.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

static void write_console(const std::string_view& s) noexcept {
#ifdef _WIN32
    WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), s.size(), nullptr, nullptr);
#else
    write(STDOUT_FILENO, s.data(), s.size());
#endif
}

static std::string read_next_vt() noexcept {
    std::string buffer;
    size_t state = 0;

    for (;;) {
#ifdef _WIN32
        const auto ch = _getch_nolock();
#else
        const auto ch = getchar();
        if (ch == EOF) {
            return {};
        }
#endif
        buffer.push_back(static_cast<char>(ch));

        switch (state) {
        case 0: // Fe Escape sequence
            if (ch == 0x1B) {
                state++; // ESC
            }
            break;
        case 1: // Control Sequence Introducer
            if (ch == 0x5B) {
                state++; // [
            } else {
                buffer.clear();
                state = 0;
            }
            break;
        case 2: // parameter bytes in the range 0x30–0x3F
            if (ch >= 0x30 && ch <= 0x3F) {
                break;
            } else {
                state++;
            }
        case 3: // intermediate bytes in the range 0x20–0x2F
            if (ch >= 0x20 && ch <= 0x2F) {
                break;
            } else {
                state++;
            }
        case 4: // final byte in the range 0x40–0x7E
            if (ch >= 0x40 && ch <= 0x7E) {
                return buffer;
            } else {
                buffer.clear();
                state = 0;
            }
        default:
            break;
        }
    }
}

static std::tuple<size_t, size_t> get_window_size() {
    write_console(
        "\x1b[9999;9999H" // Cursor Position (CUP)
        "\x1b[6n"         // Report Cursor Position (CPR) using Device Status Report (DSR) 6
    );

    for (;;) {
        const auto seq = read_next_vt();
        if (seq.size() <= 3 || seq.back() != 'R') {
            continue;
        }

        const auto count = seq.size() - 3;
        const auto beg = seq.data() + 2;
        const auto end = beg + count;
        const auto mid = std::find(beg, end, ';');
        if (!mid) {
            abort();
        }

        static constexpr auto parse = [](const char* beg, const char* end) -> size_t {
            auto out = size_t{};
            if (std::from_chars(beg, end, out, 10).ec != std::errc()) {
                abort();
            }
            return out;
        };

        const auto dx = std::clamp<size_t>(parse(mid + 1, end), 1, 1024);
        const auto dy = std::clamp<size_t>(parse(beg, mid), 1, 1024);
        return {dx, dy};
    }
}

using RGBColor = std::tuple<uint8_t, uint8_t, uint8_t>;

static RGBColor hue_to_rgb(double color_index, double num_colors) {
    // https://en.wikipedia.org/wiki/HSL_and_HSV#HSV_to_RGB
    const auto h = color_index / num_colors * 360.0;
    const auto hh = static_cast<int>(h / 60.0);
    const auto v = static_cast<int>(256.0 / 60.0 * std::fmod(h, 60.0));

    switch (hh % 6) {
    case 0:
        return {255, v, 0};
    case 1:
        return {255 - v, 255, 0};
    case 2:
        return {0, 255, v};
    case 3:
        return {0, 255 - v, 255};
    case 4:
        return {v, 0, 255};
    case 5:
        return {255, 0, 255 - v};
    }
    std::abort();
}

static bool exitFlag = false;

#ifdef _WIN32
BOOL WINAPI signalHandler(DWORD) {
    exitFlag = true;
    return TRUE;
}
#else
static void signalHandler(int) {
    exitFlag = true;
}
#endif

int main(int argc, const char* argv[]) {
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleMode(
        GetStdHandle(STD_OUTPUT_HANDLE),
        ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN
    );
    SetConsoleCtrlHandler(signalHandler, TRUE);
#else
    // Disable line buffering and automatic echo for stdin.
    // This allows us to call getchar() without blocking until the next newline.
    termios term;
    tcgetattr(0, &term);
    term.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(0, TCSANOW, &term);
    // Disable stdout buffering, allowing us to drop the fflush().
    setvbuf(stdout, nullptr, _IONBF, 0);
    signal(SIGINT, signalHandler);
#endif

    if (argc != 1 && argc != 2) {
        fprintf(stderr, "usage: rainbowbench <num_colors>\n");
        return 1;
    }

    // HSV offers at most 1530 distinct colors in 8-bit RGB
    static constexpr size_t total_rainbow_colors = 1530;
    size_t num_colors = total_rainbow_colors;
    if (argc == 2) {
        if (std::from_chars(argv[1], argv[1] + strlen(argv[1]), num_colors).ec != std::errc()) {
            fprintf(stderr, "invalid num_colors\n");
            return 1;
        }
        num_colors = std::clamp<size_t>(num_colors, 1, total_rainbow_colors);
    }

    const auto [dx, dy] = get_window_size();

    std::string rainbow;
    std::vector<size_t> rainbow_indices;
    {
        char buffer[64];
        auto [rp, gp, bp] = hue_to_rgb(num_colors - 1, num_colors);

        for (size_t i = 0, count = num_colors + dx; i < count; ++i) {
            const auto [r, g, b] = hue_to_rgb(i, num_colors);

            // Using ▀ would be graphically more pleasing, but in this benchmark
            // we want to test rendering performance and DirectWrite, as used
            // in Windows Terminal, has a very poor font-fallback performance.
            // If we were to use ▀, we'd primarily test how fast DirectWrite's is.
            const auto length = snprintf(
                buffer,
                std::ssize(buffer),
                "\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm%c",
                rp,
                gp,
                bp,
                r,
                g,
                b,
                static_cast<char>('A' + i % ('Z' - 'A' + 1))
            );
            rainbow_indices.push_back(rainbow.size());
            rainbow.append(buffer, length);

            rp = r;
            gp = g;
            bp = b;
        }
    }

    write_console(
        "\x1b[?1049h" // enable alternative screen buffer
        "\x1b[?25l"   // DECTCEM hide cursor
    );

    const size_t area = dx * dy;
    size_t kcgs = 0;
    size_t frames = 0;
    size_t glyphs = 0;
    size_t frame = 0;
    auto reference = std::chrono::steady_clock::now();
    std::string output;
    std::string stats;

    for (size_t i = 0; !exitFlag; ++i) {
        output.clear();
        stats.clear();

        stats.append(std::to_string(frames));
        stats.append(" fps | ");
        stats.append(std::to_string(kcgs));
        stats.append(" kcg/s");
        if (stats.size() > dx) {
            stats.resize(dx);
        }

        output.append(
            "\033[?2026h" // begin synchronized update
            "\x1b[H"      // Cursor Position (CUP)
            "\x1b[39;49m" // Foreground/Background color reset (part of SGR)
        );
        output.append(stats);

        {
            const auto idx = (i + stats.size()) % num_colors;
            const auto beg = rainbow_indices[idx];
            const auto end = rainbow_indices[idx + dx - stats.size()];
            const auto count = end - beg;
            output.append(rainbow.data() + beg, count);
        }

        for (size_t y = 1; y < dy; ++y) {
            const auto idx = (i + y * 2) % num_colors;
            const auto beg = rainbow_indices[idx];
            const auto end = rainbow_indices[idx + dx];
            const auto count = end - beg;
            output.append(rainbow.data() + beg, count);
        }

        output.append(
            "\033[?2026l" // end synchronized update
        );

        glyphs += area - stats.size();
        frame++;
        write_console(output);

        const auto now = std::chrono::steady_clock::now();
        const auto duration = now - reference;
        if (duration >= std::chrono::seconds(1)) {
            const auto durationCount = std::chrono::duration<double>(duration).count();
            kcgs = static_cast<size_t>(glyphs / 1000.0 / durationCount + 0.5);
            frames = static_cast<size_t>(frame / durationCount + 0.5);
            reference = now;
            glyphs = 0;
            frame = 0;
        }
    }

    // Start with a fresh line, show cursor again, disable Synchronized Output.
    write_console(
        "\x1b[?2026l" // end synchronized update
        "\x1b[?25h"   // DECTCEM show cursor
        "\x1b[?1049l" // disable alternative screen buffer
    );
}
