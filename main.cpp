// Copyright (c) 2022 Leonard Hecker <leonard@hecker.io>
// Licensed under the MIT license.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <conio.h>
#include <fcntl.h>
#include <io.h>
#else
#include <termios.h>
#endif

#include <charconv>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>
#include <cstring>
#include <cmath>
#include <cstdio>

#define escape(s) "\x1b" s

static void write_console(const std::string_view& s) noexcept
{
#ifdef _WIN32
    WriteConsoleA(GetStdHandle(STD_OUTPUT_HANDLE), s.data(), s.size(), nullptr, nullptr);
#else
    fwrite(s.data(), 1, s.size(), stdout);
#endif
}

static std::string read_next_vt() noexcept
{
    std::string buffer;
    size_t state = 0;

    for (;;)
    {
#ifdef _WIN32
        const auto ch = _getch_nolock();
#else
        const auto ch = getchar();
        if (ch == EOF)
        {
            return "";
        }
#endif
        buffer.push_back(static_cast<char>(ch));

        switch (state)
        {
        case 0: // Fe Escape sequence
            if (ch == 0x1B) // ESC
            {
                state++;
            }
            break;
        case 1: // Control Sequence Introducer
            if (ch == 0x5B) // [
            {
                state++;
            }
            else
            {
                buffer.clear();
                state = 0;
            }
            break;
        case 2: // parameter bytes in the range 0x30–0x3F
            if (ch >= 0x30 && ch <= 0x3F)
            {
                break;
            }
            else
            {
                state++;
            }
        case 3: // intermediate bytes in the range 0x20–0x2F
            if (ch >= 0x20 && ch <= 0x2F)
            {
                break;
            }
            else
            {
                state++;
            }
        case 4: // final byte in the range 0x40–0x7E
            if (ch >= 0x40 && ch <= 0x7E)
            {
                return buffer;
            }
            else
            {
                buffer.clear();
                state = 0;
            }
        default:
            break;
        }
    }
}

static void get_window_size(size_t& dx, size_t& dy)
{
    write_console(
        "\x1b[9999;9999H" // Cursor Position (CUP)
        "\x1b[6n" // Report Cursor Position (CPR) using Device Status Report (DSR) 6
    );

    for (;;)
    {
        const auto seq = read_next_vt();
        if (seq.size() <= 3 || seq.back() != 'R')
        {
            continue;
        }

        const auto count = seq.size() - 3;
        const auto beg = seq.data() + 2;
        const auto end = beg + count;
        const auto mid = std::find(beg, end, ';');
        if (!mid)
        {
            abort();
        }

        static constexpr auto parse = [](const char* beg, const char* end, size_t& out) {
            if (std::from_chars(beg, end, out, 10).ec != std::errc())
            {
                abort();
            }
        };
        parse(beg, mid, dy);
        parse(mid + 1, end, dx);

        dx = std::clamp<size_t>(dx, 1, 1024);
        dy = std::clamp<size_t>(dy, 1, 1024);
        break;
    }
}

static void hue_to_rgb(double color_index, double num_colors, uint8_t& r, uint8_t& g, uint8_t& b)
{
    // https://en.wikipedia.org/wiki/HSL_and_HSV#HSV_to_RGB
    const auto h = color_index / num_colors * 360.0;
    const auto hh = static_cast<int>(h / 60.0);
    const auto v = static_cast<int>(256.0 / 60.0 * std::fmod(h, 60.0));

    switch (hh % 6)
    {
    case 0:
        r = 255;
        g = v;
        b = 0;
        break;
    case 1:
        r = 255 - v;
        g = 255;
        b = 0;
        break;
    case 2:
        r = 0;
        g = 255;
        b = v;
        break;
    case 3:
        r = 0;
        g = 255 - v;
        b = 255;
        break;
    case 4:
        r = v;
        g = 0;
        b = 255;
        break;
    case 5:
        r = 255;
        g = 0;
        b = 255 - v;
        break;
    }
}

int main(int argc, const char* argv[])
{
#ifdef _WIN32
    SetConsoleCP(CP_UTF8);
    SetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN);
#else
    // Disable line buffering and automatic echo for stdin.
    // This allows us to call getchar() without blocking until the next newline.
    termios term;
    tcgetattr(0, &term);
    term.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(0, TCSANOW, &term);
    // Disable stdout buffering, allowing us to drop the fflush().
    setvbuf(stdout, nullptr, _IONBF, 0);
#endif

    if (argc != 1 && argc != 2)
    {
        fprintf(stderr, "usage: rainbowbench <num_colors>\n");
        return 1;
    }

    // HSV offers at most 1530 distinct colors in 8-bit RGB
    static constexpr size_t total_rainbow_colors = 1530;
    size_t num_colors = total_rainbow_colors;
    if (argc == 2)
    {
        if (std::from_chars(argv[1], argv[1] + strlen(argv[1]), num_colors).ec != std::errc())
        {
            fprintf(stderr, "invalid num_colors\n");
            return 1;
        }
        num_colors = std::clamp<size_t>(num_colors, 1, total_rainbow_colors);
    }

    size_t dx = 0;
    size_t dy = 0;
    get_window_size(dx, dy);

    std::string rainbow;
    std::vector<size_t> rainbow_indices;
    {
        char buffer[64];
        // Colors of current iteration
        uint8_t r = 255;
        uint8_t g = 0;
        uint8_t b = 0;
        // Colors of previous iteration
        uint8_t rp;
        uint8_t gp;
        uint8_t bp;
        hue_to_rgb(num_colors - 1, num_colors, rp, gp, bp);

        for (size_t i = 0, count = num_colors + dx; i < count; ++i)
        {
            hue_to_rgb(i, num_colors, r, g, b);

            // Using ▀ would be graphically more pleasing, but in this benchmark we want to
            // test rendering performance and DirectWrite, as used in Windows Terminal,
            // has a very poor font-fallback performance. If we were to use ▀, we'd
            // primarily test how DirectWrite's instead of Terminal's performance.
            const auto length = snprintf(buffer, std::ssize(buffer), "\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm%c", rp, gp, bp, r, g, b, static_cast<char>('A' + i % ('Z' - 'A' + 1)));
            rainbow_indices.push_back(rainbow.size());
            rainbow.append(buffer, length);

            rp = r;
            gp = g;
            bp = b;
        }
    }

    // DECTCEM hide cursor
    write_console("\x1b[?25l");

    size_t kcgs = 0;
    size_t frame = 0;
    auto reference = std::chrono::steady_clock::now();
    std::string output;

    for (size_t i = 0;; ++i)
    {
        output.clear();
        output.append(
            "\033[?2026h" // begin synchronized update
            "\x1b[H" // Cursor Position (CUP)
        );

        for (size_t y = 0; y < dy; ++y)
        {
            const auto idx = (i + y * 2) % num_colors;
            const auto beg = rainbow_indices[idx];
            const auto end = rainbow_indices[idx + dx];
            const auto count = end - beg;
            output.append(rainbow.data() + beg, count);
        }

        output.append(
            "\x1b[39;49m" // Foreground/Background color reset (part of SGR)
            "\x1b[H" // Cursor Position (CUP)
        );
        output.append(std::to_string(kcgs));
        output.append(
            "kcg/s"
            "\033[?2026l" // end synchronized update
        );

        frame++;
        write_console(output);

        const auto now = std::chrono::steady_clock::now();
        const auto duration = now - reference;
        if (duration >= std::chrono::seconds(1))
        {
            kcgs = static_cast<size_t>(dx * dy * frame / 1000.0 / std::chrono::duration<double>(duration).count() + 0.5);
            reference = now;
            frame = 0;
        }
    }
}
