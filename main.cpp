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
#include <array>
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
    ColorMode_None = 3,
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

struct RGB {
    uint8_t r, g, b;
};

int main(int argc, const char* argv[]) {
    if (argc < 1 || argc > 3) {
        fprintf(stderr, "usage: rainbowbench [-fg] [-bg] <num_colors>\n");
        return 1;
    }

    // HSV offers at most 1530 distinct colors in 8-bit RGB
    static constexpr size_t max_rainbow_colors = 1530;
    size_t num_colors = max_rainbow_colors;
    size_t argv_index = 1;
    ColorMode color_mode = ColorMode_All;
    char char_override[4];
    size_t char_override_length = 0;

    for (; argv_index < argc; ++argv_index) {
        const auto arg = argv[argv_index];
        if (strcmp(arg, "-fg") == 0) {
            color_mode = ColorMode_Foreground;
        } else if (strcmp(arg, "-bg") == 0) {
            color_mode = ColorMode_Background;
        } else if (strcmp(arg, "-ng") == 0) {
            color_mode = ColorMode_None;
        } else if (strncmp(arg, "-ch=", 4) == 0) {
            char* endptr;
            const auto codepoint = strtoul(arg + 4, &endptr, 16);
            if (codepoint < 0x80) {
                char_override[0] = static_cast<char>(codepoint);
                char_override_length = 1;
            } else if (codepoint < 0x800) {
                char_override[0] = static_cast<char>((0xc0 | (codepoint >> 6)));
                char_override[1] = static_cast<char>((0x80 | (codepoint & 0x3f)));
                char_override_length = 2;
            } else if (codepoint < 0x10000) {
                char_override[0] = static_cast<char>((0xe0 | (codepoint >> 12)));
                char_override[1] = static_cast<char>((0x80 | ((codepoint >> 6) & 0x3f)));
                char_override[2] = static_cast<char>((0x80 | (codepoint & 0x3f)));
                char_override_length = 3;
            } else if (codepoint <= 0x110000) {
                char_override[0] = static_cast<char>((0xf0 | (codepoint >> 18)));
                char_override[1] = static_cast<char>((0x80 | ((codepoint >> 12) & 0x3f)));
                char_override[2] = static_cast<char>((0x80 | ((codepoint >> 6) & 0x3f)));
                char_override[3] = static_cast<char>((0x80 | (codepoint & 0x3f)));
                char_override_length = 4;
            }
        } else {
            char* endptr;
            num_colors = strtoull(arg, &endptr, 10);
            num_colors = std::clamp<size_t>(num_colors, 1, max_rainbow_colors);
        }
    }

    std::array<RGB, max_rainbow_colors> colors{};
    for (size_t i = 0; i < num_colors; ++i) {
        // https://en.wikipedia.org/wiki/HSL_and_HSV#HSV_to_RGB
        const auto h = double(i) / double(num_colors) * 360.0;
        const auto hh = static_cast<int>(h / 60.0);
        const auto v = static_cast<int>(256.0 / 60.0 * std::fmod(h, 60.0));
        uint8_t r, g, b;

        switch (hh % 6) {
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
        default:
            std::terminate();
        }

        colors[i] = {r, g, b};
    }

    size_t screen_cols = 0;
    size_t screen_rows = 0;
    size_t screen_area = 0;
    std::string rainbow;
    std::vector<size_t> rainbow_indices;

    const auto rebuild_rainbow = [&]() {
        screen_area = screen_cols * screen_rows;

        const auto fg_offset = std::max<size_t>(1, (num_colors + 5) / 10);
        char buffer[64];

        for (size_t i = 0, count = num_colors + screen_cols; i < count; ++i) {
            // Using ▀ would be graphically more pleasing, but in this benchmark
            // we want to test rendering performance and DirectWrite, as used
            // in Windows Terminal, has a very poor font-fallback performance.
            // If we were to use ▀, we'd primarily test how fast DirectWrite is.
            int length = 0;
            switch (color_mode) {
            case ColorMode_All: {
                const auto bg = colors[i % num_colors];
                const auto fg = colors[(i + fg_offset) % num_colors];
                length = snprintf(buffer, std::ssize(buffer), "\x1b[48;2;%d;%d;%d;38;2;%d;%d;%dm", bg.r, bg.g, bg.b, fg.r, fg.g, fg.b);
                break;
            }
            case ColorMode_Foreground: {
                const auto fg = colors[i % num_colors];
                length = snprintf(buffer, std::ssize(buffer), "\x1b[38;2;%d;%d;%dm", fg.r, fg.g, fg.b);
                break;
            }
            case ColorMode_Background: {
                const auto bg = colors[i % num_colors];
                length = snprintf(buffer, std::ssize(buffer), "\x1b[48;2;%d;%d;%dm", bg.r, bg.g, bg.b);
                break;
            }
            default:
                break;
            }

            rainbow_indices.push_back(rainbow.size());
            rainbow.append(buffer, length);

            if (char_override_length) {
                rainbow.append(&char_override[0], char_override_length);
            } else {
                rainbow.push_back(static_cast<char>('!' + i % 94));
            }
        }
    };

#ifdef _WIN32
    const auto previousCP = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);

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

    float mbps = 0;
    float frames = 0;
    size_t written = 0;
    size_t frame = 0;
    auto reference = std::chrono::steady_clock::now();
    std::string output;
    char statsBuffer[128];
    size_t statsLength = 0;

    for (size_t i = 0;; ++i) {
        const auto state = signal_state.exchange(0, std::memory_order_relaxed);
        if (state & SignalState_Sigint) {
            break;
        }
        if (state & SignalState_Sigwinch) {
#ifdef _WIN32
            CONSOLE_SCREEN_BUFFER_INFO info{};
            GetConsoleScreenBufferInfo(consoleHandles[1], &info);
            screen_cols = info.dwSize.X;
            screen_rows = info.dwSize.Y;
            rebuild_rainbow();
#else
            winsize size{};
            ioctl(STDOUT_FILENO, TIOCGWINSZ, &size);
            screen_cols = size.ws_col;
            screen_rows = size.ws_row;
            rebuild_rainbow();
#endif
        }

        statsLength = sprintf(&statsBuffer[0], "%.1f fps | %.3f MB/s", frames, mbps);
        statsLength = std::min(statsLength, screen_cols);

        output.clear();
        output.append(
            "\033[?2026h" // begin synchronized update
            "\x1b[H"      // Cursor Position (CUP)
            "\x1b[39;49m" // Foreground/Background color reset (part of SGR)
        );
        output.append(&statsBuffer[0], statsLength);

        {
            const auto idx = (i + statsLength) % num_colors;
            const auto beg = rainbow_indices[idx];
            const auto end = rainbow_indices[idx + screen_cols - statsLength];
            const auto count = end - beg;
            output.append(rainbow.data() + beg, count);
        }

        for (size_t y = 1; y < screen_rows; ++y) {
            const auto idx = (i + y * 2) % num_colors;
            const auto beg = rainbow_indices[idx];
            const auto end = rainbow_indices[idx + screen_cols];
            const auto count = end - beg;
            output.append(rainbow.data() + beg, count);
        }

        output.append(
            "\033[?2026l" // end synchronized update
        );

        write_console(output);

        written += output.size();
        frame++;

        const auto now = std::chrono::steady_clock::now();
        const auto duration = now - reference;
        if (duration >= std::chrono::seconds(1)) {
            const auto durationCount = std::chrono::duration<float>(duration).count();
            mbps = written / durationCount / 1e6f;
            frames = frame / durationCount;
            reference = now;
            written = 0;
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
    SetConsoleOutputCP(previousCP);

    for (size_t i = 0; i < 2; ++i) {
        SetConsoleMode(consoleHandles[i], previousModes[i]);
    }
#endif
}
