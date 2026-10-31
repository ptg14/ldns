#include "ui/term/terminal.hpp"

#include <chrono>
#include <iostream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>
#else
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace ldns::ui {

Terminal& Terminal::instance() {
    static Terminal inst;
    return inst;
}

Terminal::Terminal() = default;

Terminal::~Terminal() {
    disable_raw_mode();
}

bool Terminal::enable_raw_mode() {
    if (raw_mode_active_) {
        return true;
    }

#if defined(_WIN32)
    HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h_in == INVALID_HANDLE_VALUE || h_out == INVALID_HANDLE_VALUE) {
        return false;
    }

    orig_in_cp_ = GetConsoleCP();
    orig_out_cp_ = GetConsoleOutputCP();
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    DWORD in_mode = 0;
    DWORD out_mode = 0;
    GetConsoleMode(h_in, &in_mode);
    GetConsoleMode(h_out, &out_mode);
    orig_in_mode_ = in_mode;
    orig_out_mode_ = out_mode;

    // Enable Virtual Terminal Processing for VT escape sequences
    out_mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(h_out, out_mode);

    // Raw mode: disable line input, echo, processed input
    in_mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    in_mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    SetConsoleMode(h_in, in_mode);

    raw_mode_active_ = true;
    return true;
#else
    termios orig;
    if (tcgetattr(STDIN_FILENO, &orig) == -1) {
        return false;
    }
    orig_termios_ = new termios(orig);

    termios raw = orig;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
        delete static_cast<termios*>(orig_termios_);
        orig_termios_ = nullptr;
        return false;
    }

    raw_mode_active_ = true;
    return true;
#endif
}

void Terminal::disable_raw_mode() {
    if (!raw_mode_active_) {
        return;
    }

    show_cursor(true);

#if defined(_WIN32)
    HANDLE h_in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h_in != INVALID_HANDLE_VALUE) {
        SetConsoleMode(h_in, orig_in_mode_);
    }
    if (h_out != INVALID_HANDLE_VALUE) {
        SetConsoleMode(h_out, orig_out_mode_);
    }
    if (orig_in_cp_ != 0) {
        SetConsoleCP(orig_in_cp_);
    }
    if (orig_out_cp_ != 0) {
        SetConsoleOutputCP(orig_out_cp_);
    }
#else
    if (orig_termios_) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, static_cast<termios*>(orig_termios_));
        delete static_cast<termios*>(orig_termios_);
        orig_termios_ = nullptr;
    }
#endif

    raw_mode_active_ = false;
}

TerminalSize Terminal::size() const {
    TerminalSize sz{80, 24};
#if defined(_WIN32)
    HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(h_out, &csbi)) {
        sz.cols = static_cast<uint16_t>(csbi.srWindow.Right - csbi.srWindow.Left + 1);
        sz.rows = static_cast<uint16_t>(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
    }
#else
    winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        sz.cols = ws.ws_col;
        sz.rows = ws.ws_row;
    }
#endif
    if (sz.cols == 0) sz.cols = 80;
    if (sz.rows == 0) sz.rows = 24;
    return sz;
}

void Terminal::show_cursor(bool show) {
    if (show) {
        std::cout << "\033[?25h" << std::flush;
    } else {
        std::cout << "\033[?25l" << std::flush;
    }
}

void Terminal::clear_below() {
    std::cout << "\033[J" << std::flush;
}

void Terminal::clear_line() {
    std::cout << "\033[2K\r" << std::flush;
}

void Terminal::move_up(int n) {
    if (n > 0) {
        std::cout << "\033[" << n << "A" << std::flush;
    }
}

void Terminal::move_down(int n) {
    if (n > 0) {
        std::cout << "\033[" << n << "B" << std::flush;
    }
}

void Terminal::move_to_col(int col) {
    std::cout << "\033[" << col << "G" << std::flush;
}

int Terminal::read_byte() {
#if defined(_WIN32)
    return _getch();
#else
    char c = 0;
    int nread = static_cast<int>(read(STDIN_FILENO, &c, 1));
    if (nread <= 0) return -1;
    return static_cast<unsigned char>(c);
#endif
}

bool Terminal::has_byte_pending(int timeout_ms) {
#if defined(_WIN32)
    if (_kbhit()) return true;
    if (timeout_ms <= 0) return false;
    auto start = std::chrono::steady_clock::now();
    while (true) {
        if (_kbhit()) return true;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count() >= timeout_ms) {
            break;
        }
        Sleep(2);
    }
    return _kbhit() != 0;
#else
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    int ret = poll(&pfd, 1, timeout_ms);
    return ret > 0;
#endif
}

}  // namespace ldns::ui
