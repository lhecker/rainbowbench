// Copyright (c) 2022 Leonard Hecker <leonard@hecker.io>
// Licensed under the MIT license.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <csignal>
#include <cstring>
#include <string_view>
#include <tuple>
#include <vector>

enum SignalState : uint8_t {
    SignalState_Sigint = 0x1,
    SignalState_Sigwinch = 0x2,
};

enum ColorMode : uint8_t {
    ColorMode_All = 0,
    ColorMode_Foreground = 1,
    ColorMode_Background = 2,
};

#ifdef _WIN32
static const HANDLE consoleHandles[2]{
    GetStdHandle(STD_INPUT_HANDLE),
    GetStdHandle(STD_OUTPUT_HANDLE),
};
#endif

static std::atomic<uint8_t> signal_state{SignalState_Sigwinch};

static void write_console(const std::string_view& s) noexcept {
#ifdef _WIN32
    WriteConsoleA(consoleHandles[1], s.data(), s.size(), nullptr, nullptr);
#else
    write(STDOUT_FILENO, s.data(), s.size());
#endif
}

static std::tuple<uint8_t, uint8_t, uint8_t> hue_to_rgb(float color_index, float num_colors) {
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
    default:
        std::terminate();
    }
}

#ifdef _WIN32
BOOL WINAPI signalHandler(DWORD) {
    signal_state.fetch_or(SignalState_Sigint, std::memory_order_relaxed);
    return TRUE;
}
#else
static void signalHandler(int sig) {
    switch (sig) {
    case SIGWINCH:
        signal_state.fetch_or(SignalState_Sigwinch, std::memory_order_relaxed);
        break;
    default:
        signal_state.fetch_or(SignalState_Sigint, std::memory_order_relaxed);
        break;
    }
}
#endif

int main(int argc, const char* argv[]) {
    if (argc < 1 || argc > 3) {
        fprintf(stderr, "usage: rainbowbench [-fg] [-bg] <num_colors>\n");
        return 1;
    }

    // HSV offers at most 1530 distinct colors in 8-bit RGB
    static constexpr size_t total_rainbow_colors = 1530;
    size_t num_colors = total_rainbow_colors;
    size_t argv_index = 1;
    ColorMode color_mode = ColorMode_All;

    if (argc > argv_index) {
        if (strcmp(argv[argv_index], "-fg") == 0) {
            color_mode = ColorMode_Foreground;
            argv_index++;
        } else if (strcmp(argv[argv_index], "-bg") == 0) {
            color_mode = ColorMode_Background;
            argv_index++;
        }
    }

    if (argc > argv_index) {
        char* endptr;
        num_colors = strtoumax(argv[argv_index], &endptr, 10);
        num_colors = std::clamp<size_t>(num_colors, 1, total_rainbow_colors);
        argv_index++;
    }

    size_t dx = 0;
    size_t dy = 0;
    size_t area = 0;
    std::string rainbow;
    std::vector<size_t> rainbow_indices;

    const auto rebuild_rainbow = [&](int x, int y) {
        dx = x;
        dy = y;
        area = x * y;

        char buffer[64];
        auto [rp, gp, bp] = hue_to_rgb(num_colors - 1, num_colors);

        for (size_t i = 0, count = num_colors + dx; i < count; ++i) {
            const auto [r, g, b] = hue_to_rgb(i, num_colors);
            const auto ch = static_cast<char>('A' + i % ('Z' - 'A' + 1));

            // Using ▀ would be graphically more pleasing, but in this benchmark
            // we want to test rendering performance and DirectWrite, as used
            // in Windows Terminal, has a very poor font-fallback performance.
            // If we were to use ▀, we'd primarily test how fast DirectWrite is.
            int length = 0;
            switch (color_mode) {
            case ColorMode_All:
                length = snprintf(buffer, std::ssize(buffer), "\x1b[38;2;%d;%d;%d;48;2;%d;%d;%dm%c", rp, gp, bp, r, g, b, ch);
                break;
            case ColorMode_Foreground:
                length = snprintf(buffer, std::ssize(buffer), "\x1b[38;2;%d;%d;%dm%c", rp, gp, bp, ch);
                break;
            case ColorMode_Background:
                length = snprintf(buffer, std::ssize(buffer), "\x1b[48;2;%d;%d;%dm%c", r, g, b, ch);
                break;
            default:
                break;
            }

            rainbow_indices.push_back(rainbow.size());
            rainbow.append(buffer, length);

            rp = r;
            gp = g;
            bp = b;
        }
    };

#ifdef _WIN32
    const auto previousCP = GetConsoleCP();
    SetConsoleCP(CP_UTF8);

    DWORD previousModes[2]{};
    for (size_t i = 0; i < 2; ++i) {
        GetConsoleMode(consoleHandles[i], &previousModes[i]);
        SetConsoleMode(consoleHandles[i], previousModes[i] | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    SetConsoleCtrlHandler(signalHandler, TRUE);

    CreateThread(
        nullptr,
        0,
        [](LPVOID lpParameter) noexcept -> DWORD {
            INPUT_RECORD records[16];
            for (;;) {
                DWORD read = 0;
                if (!ReadConsoleInputW(consoleHandles[0], &records[0], 16, &read)) {
                    break;
                }

                for (DWORD i = 0; i < read; ++i) {
                    if (records[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
                        signal_state.fetch_or(SignalState_Sigwinch, std::memory_order_relaxed);
                    }
                }
            }
            return 0;
        },
        nullptr,
        0,
        nullptr
    );
#else
    signal(SIGINT, signalHandler);
    signal(SIGWINCH, signalHandler);
#endif

    write_console(
        "\x1b[?1049h" // enable alternative screen buffer
        "\x1b[?25l"   // DECTCEM hide cursor
    );

    size_t kcgs = 0;
    size_t frames = 0;
    size_t glyphs = 0;
    size_t frame = 0;
    auto reference = std::chrono::steady_clock::now();
    std::string output;
    std::string stats;

    for (size_t i = 0;; ++i) {
        const auto state = signal_state.exchange(0, std::memory_order_relaxed);
        if (state & SignalState_Sigint) {
            break;
        }
        if (state & SignalState_Sigwinch) {
#ifdef _WIN32
            CONSOLE_SCREEN_BUFFER_INFO info{};
            GetConsoleScreenBufferInfo(consoleHandles[1], &info);
            rebuild_rainbow(info.dwSize.X, info.dwSize.Y);
#else
            winsize size{};
            ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
            rebuild_rainbow(size.ws_col, size.ws_row);
#endif
        }

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
            const auto durationCount = std::chrono::duration<float>(duration).count();
            kcgs = static_cast<size_t>(glyphs / 1000.0f / durationCount + 0.5f);
            frames = static_cast<size_t>(frame / durationCount + 0.5f);
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

#ifdef _WIN32
    SetConsoleCP(previousCP);

    for (size_t i = 0; i < 2; ++i) {
        SetConsoleMode(consoleHandles[i], previousModes[i]);
    }
#endif
}
