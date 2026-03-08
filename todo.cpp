/*
 * todo.cpp  –  TUI Todo with vertical sections (Kanban-style)
 *
 * Build:  g++ -std=c++17 -o todo todo.cpp && ./todo
 *
 * Keys:
 *   ←/→        switch active section
 *   ↑/↓        navigate tasks
 *   a           add task to current section
 *   d           delete selected task
 *   Space       toggle done
 *   p           cycle priority  (High→Med→Low)
 *   m           move task to another section
 *   N           add new section
 *   X           remove current section (must be empty)
 *   [ / ]       scroll section view left/right
 *   q           quit
 */

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

// ── constants ─────────────────────────────────────────────────────────
static constexpr int MAX_SECTIONS = 32;
static constexpr int MAX_TASKS    = 256;
static constexpr int MAX_VIS      = 3;
static constexpr const char* SAVE_FILE = "todos.dat";

// ── ANSI ──────────────────────────────────────────────────────────────
#define CLEAR      "\033[2J\033[H"
#define RESET      "\033[0m"
#define BOLD       "\033[1m"
#define DIM        "\033[2m"
#define ITALIC     "\033[3m"

// 256-color helpers
#define FG(n)      "\033[38;5;" #n "m"
#define BG(n)      "\033[48;5;" #n "m"

// Palette
#define C_BASE_BG       BG(235)          // near-black background
#define C_TITLE_BG      BG(237)          // slightly lighter for title bar
#define C_TITLE_FG      FG(252)          // soft white
#define C_HINT_FG       FG(242)          // muted grey
#define C_HINT_KEY_FG   FG(110)          // soft blue for key labels

#define C_COL_ACTIVE_BG BG(237)          // active column bg
#define C_COL_INACT_BG  BG(235)          // inactive column bg

#define C_SEL_BG        BG(238)          // selected task highlight
#define C_SEL_FG        FG(255)          // selected task text

#define C_DONE_FG       FG(240)          // struck-through done tasks
#define C_TASK_FG       FG(250)          // normal task text

#define C_PRI_HIGH      FG(203)          // soft red
#define C_PRI_MED       FG(179)          // warm amber
#define C_PRI_LOW       FG(243)          // muted grey

#define C_CHECK_DONE    FG(71)           // muted green
#define C_CHECK_OPEN    FG(245)          // grey

#define C_SCROLL_FG     FG(240)          // scroll indicators
#define C_ARROW_FG      FG(110)          // nav arrows

#define C_DIVIDER_ACT   FG(110)          // active col divider colour
#define C_DIVIDER_INACT FG(238)          // inactive col divider

// Section header accent colours (fg on dark bg)
static const char* const SEC_ACCENT_FG[] = {
    FG(110),   // steel blue
    FG(176),   // soft violet
    FG(73),    // teal
    FG(179),   // amber
    FG(174),   // dusty rose
    FG(108),   // sage green
    FG(146),   // lavender
    FG(138),   // mauve
};
static const char* const SEC_ACCENT_BG[] = {
    BG(17),    // dark blue
    BG(53),    // dark purple
    BG(23),    // dark teal
    BG(58),    // dark olive
    BG(52),    // dark red
    BG(22),    // dark green
    BG(54),    // dark violet
    BG(95),    // dark brown
};
static constexpr int N_HDR_COLORS = 8;

#define HIDE_CUR  (std::fputs("\033[?25l", stdout))
#define SHOW_CUR  (std::fputs("\033[?25h", stdout))
#define MOVE(r,c) (std::printf("\033[%d;%dH", (r), (c)))

// ── data ──────────────────────────────────────────────────────────────
struct Task {
    std::string text;
    bool        done     = false;
    int         priority = 2; // 1=High 2=Medium 3=Low
};

struct Section {
    std::string       name;
    std::vector<Task> tasks;
    int               cursor = 0;
    int               scroll = 0;

    Section() = default;
    explicit Section(std::string n) : name(std::move(n)) {}
};

static std::vector<Section> sections;
static int active   = 0;
static int view_off = 0;
static int T_ROWS   = 24;
static int T_COLS   = 80;
static volatile sig_atomic_t resize_flag = 0;

// ── terminal ──────────────────────────────────────────────────────────
static termios orig_term;

static bool orig_term_saved = false;

static void raw_on() {
    termios t;
    tcgetattr(STDIN_FILENO, &t);
    if (!orig_term_saved) {
        orig_term       = t;
        orig_term_saved = true;
    }
    t.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
    t.c_cc[VMIN]  = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);  // TCSAFLUSH discards pending input
}

static void raw_off() {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_term);
}

// 100 ms timeout; returns -1 on timeout
static int getch() {
    fd_set fds;
    timeval tv { 0, 100'000 };
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0)
        return -1;
    unsigned char c;
    return (read(STDIN_FILENO, &c, 1) == 1) ? static_cast<int>(c) : -1;
}

// 50 ms timeout for escape sequence bytes
static int getch_seq() {
    fd_set fds;
    timeval tv { 0, 50'000 };
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0)
        return -1;
    unsigned char c;
    return (read(STDIN_FILENO, &c, 1) == 1) ? static_cast<int>(c) : -1;
}

static void get_winsize() {
    winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
        T_ROWS = ws.ws_row;
        T_COLS = ws.ws_col;
    }
}

static void sigwinch_handler(int) { resize_flag = 1; }

// ── persistence ───────────────────────────────────────────────────────
static void save() {
    std::ofstream f(SAVE_FILE);
    if (!f) return;
    f << sections.size() << '\n';
    for (auto& S : sections) {
        f << S.name << '\n' << S.tasks.size() << '\n';
        for (auto& t : S.tasks)
            f << t.done << ' ' << t.priority << ' ' << t.text << '\n';
    }
}

static void load() {
    std::ifstream f(SAVE_FILE);
    if (!f) {
        sections.emplace_back("Section 1");
        sections.emplace_back("Section 2");
        sections.emplace_back("Section 3");
        return;
    }

    int nsec = 0;
    f >> nsec;
    f.ignore();
    nsec = std::clamp(nsec, 0, MAX_SECTIONS);

    for (int s = 0; s < nsec; s++) {
        Section S;
        std::getline(f, S.name);

        int count = 0;
        f >> count;
        f.ignore();
        count = std::clamp(count, 0, MAX_TASKS);

        for (int i = 0; i < count; i++) {
            Task t;
            int  done, pri;
            f >> done >> pri;
            f.ignore(1);
            std::getline(f, t.text);
            t.done     = (done != 0);
            t.priority = pri;
            S.tasks.push_back(std::move(t));
        }
        sections.push_back(std::move(S));
    }
    if (sections.empty()) {
        sections.emplace_back("Section 1");
        sections.emplace_back("Section 2");
        sections.emplace_back("Section 3");
    }
}

// ── layout helpers ────────────────────────────────────────────────────
static int nsec()     { return static_cast<int>(sections.size()); }
static int vis_count(){ int v = nsec() - view_off; if(v>MAX_VIS)v=MAX_VIS; return std::max(v,0); }

static void clamp_view() {
    if (active < view_off) view_off = active;
    if (active >= view_off + MAX_VIS) view_off = active - MAX_VIS + 1;
    view_off = std::clamp(view_off, 0, std::max(0, nsec() - 1));
}

// ── priority helpers ──────────────────────────────────────────────────
static const char* pri_sym(int p) { return p==1?"●":p==2?"●":"○"; }
static const char* pri_fg (int p) { return p==1?C_PRI_HIGH:p==2?C_PRI_MED:C_PRI_LOW; }

// ── draw helpers ──────────────────────────────────────────────────────
// Print exactly `width` visible chars, padding or truncating as needed
static void print_cell(const char* s, int width, bool trunc_marker = true) {
    int printed = 0;
    for (const char* p = s; *p && printed < width; ) {
        unsigned char uc = static_cast<unsigned char>(*p);
        // count UTF-8 sequence length (treat multi-byte as 1 visual cell)
        int seq = 1;
        if      ((uc & 0xE0) == 0xC0) seq = 2;
        else if ((uc & 0xF0) == 0xE0) seq = 3;
        else if ((uc & 0xF8) == 0xF0) seq = 4;
        if (printed == width - 1 && *(p + seq) != '\0' && trunc_marker) {
            std::fputs("…", stdout); printed++; break;
        }
        for (int i = 0; i < seq && *p; i++) std::putchar(*p++);
        printed++;
    }
    while (printed++ < width) std::putchar(' ');
}


// ── draw one column ───────────────────────────────────────────────────
static void draw_col(int col_x, int cw, int si, bool is_act, int hdr_row, int crows) {
    Section& S = sections[static_cast<size_t>(si)];
    const char* acc_fg = SEC_ACCENT_FG[si % N_HDR_COLORS];
    const char* acc_bg = SEC_ACCENT_BG[si % N_HDR_COLORS];
    const char* col_bg = is_act ? C_COL_ACTIVE_BG : C_COL_INACT_BG;

    // ── section header ──
    MOVE(hdr_row, col_x);
    std::printf("%s%s%s", acc_bg, acc_fg, BOLD);
    std::string title = " " + S.name + " ";
    std::string count_str = std::string("(") + std::to_string(S.tasks.size()) + ") ";
    // left-align name, right-align count
    int name_w = static_cast<int>(title.size());
    int cnt_w  = static_cast<int>(count_str.size());
    int gap    = cw - name_w - cnt_w;
    std::fputs(title.c_str(), stdout);
    for (int i = 0; i < gap; i++) std::putchar(' ');
    std::printf("%s%s%s%s", DIM, acc_fg, count_str.c_str(), RESET);

    // ── divider row ──
    MOVE(hdr_row + 1, col_x);
    if (is_act) {
        std::printf("%s%s", C_DIVIDER_ACT, BOLD);
        std::fputs("├", stdout);
        for (int i = 1; i < cw - 1; i++) std::fputs("─", stdout);
        std::fputs("┤", stdout);
    } else {
        std::fputs(C_DIVIDER_INACT, stdout);
        std::fputs("│", stdout);
        for (int i = 1; i < cw - 1; i++) std::putchar(' ');
        std::fputs("│", stdout);
    }
    std::fputs(RESET, stdout);

    // ── clamp scroll/cursor ──
    int tc = static_cast<int>(S.tasks.size());
    if (S.cursor < 0) S.cursor = 0;
    if (tc > 0 && S.cursor >= tc) S.cursor = tc - 1;
    if (S.scroll < 0) S.scroll = 0;
    if (tc > 0 && S.scroll > tc - 1) S.scroll = tc - 1;
    if (S.cursor < S.scroll) S.scroll = S.cursor;
    if (S.cursor >= S.scroll + crows) S.scroll = S.cursor - crows + 1;

    // ── task rows ──
    for (int row = 0; row < crows; row++) {
        int ti = S.scroll + row;
        MOVE(hdr_row + 2 + row, col_x);

        if (ti >= tc) {
            // empty row — just fill with column bg
            std::fputs(col_bg, stdout);
            for (int i = 0; i < cw; i++) std::putchar(' ');
            std::fputs(RESET, stdout);
            continue;
        }

        Task& t  = S.tasks[static_cast<size_t>(ti)];
        bool sel = is_act && (ti == S.cursor);

        // row background
        if      (sel)    std::printf("%s%s", C_SEL_BG, BOLD);
        else if (t.done) std::printf("%s%s", col_bg, DIM);
        else             std::fputs(col_bg, stdout);

        // left border accent for active col
        if (is_act && sel)
            std::printf("%s▌%s%s", acc_fg, sel ? C_SEL_BG : col_bg, BOLD);
        else
            std::fputs(" ", stdout);

        // priority dot
        std::printf("%s%s ", pri_fg(t.priority), pri_sym(t.priority));

        // reset bg after pri colour
        if      (sel)    std::printf("%s%s", C_SEL_BG, BOLD);
        else if (t.done) std::printf("%s%s", col_bg, DIM);
        else             std::fputs(col_bg, stdout);

        // checkbox
        if (t.done) std::printf("%s✓ %s", C_CHECK_DONE, RESET);
        else        std::printf("%s□ %s", C_CHECK_OPEN,  RESET);

        // restore bg again after RESET
        if      (sel)    std::printf("%s%s", C_SEL_BG, BOLD);
        else if (t.done) std::printf("%s%s%s", col_bg, DIM, C_DONE_FG);
        else             std::printf("%s%s", col_bg, C_TASK_FG);

        // task text (right-pad to fill column)
        int avail = cw - 5; // 1 border + 2 pri + 2 check
        if (avail < 1) avail = 1;
        if (sel) std::fputs(C_SEL_FG, stdout);
        print_cell(t.text.c_str(), avail);
        std::fputs(RESET, stdout);
    }

    // ── bottom border + scroll indicator ──
    MOVE(hdr_row + 2 + crows, col_x);
    if (is_act) {
        std::printf("%s%s", C_DIVIDER_ACT, BOLD);
        std::fputs("└", stdout);
        for (int i = 1; i < cw - 1; i++) std::fputs("─", stdout);
        std::fputs("┘", stdout);
        std::fputs(RESET, stdout);
    } else {
        std::printf("%s", C_DIVIDER_INACT);
        std::fputs("│", stdout);
        for (int i = 1; i < cw - 1; i++) std::putchar(' ');
        std::fputs("│", stdout);
        std::fputs(RESET, stdout);
    }

    // scroll counter overlaid on border
    if (tc > 0) {
        std::string ind = std::to_string(S.cursor + 1) + "/" + std::to_string(tc);
        if (tc > crows) {
            if (S.scroll > 0)               ind = "↑ " + ind;
            if (S.scroll + crows < tc)      ind = ind + " ↓";
        }
        int ind_col = col_x + cw - static_cast<int>(ind.size()) - 2;
        if (ind_col > col_x) {
            MOVE(hdr_row + 2 + crows, ind_col);
            std::printf("%s%s %s %s", is_act ? C_DIVIDER_ACT : C_DIVIDER_INACT,
                        is_act ? BOLD : DIM, ind.c_str(), RESET);
        }
    }
}

// ── full redraw ───────────────────────────────────────────────────────
static void draw() {
    get_winsize();
    clamp_view();
    int vc  = vis_count();
    int cw  = vc > 0 ? T_COLS / vc : T_COLS;
    int col_hdr = 4;
    int crows   = T_ROWS - col_hdr - 2 - 1 - 2;
    if (crows < 1) crows = 1;

    std::fputs(CLEAR, stdout);
    // fill entire background
    std::fputs(C_BASE_BG, stdout);

    // ── title bar ──
    MOVE(1, 1);
    std::printf("%s%s%s", C_TITLE_BG, C_TITLE_FG, BOLD);
    char tb[256];
    std::snprintf(tb, sizeof(tb),
        "  ✦ todo  ");
    std::fputs(tb, stdout);
    // section breadcrumb
    std::printf("%s  %d section%s  ", DIM, nsec(), nsec() == 1 ? "" : "s");
    if (view_off > 0 || view_off + vc < nsec())
        std::printf("cols %d–%d of %d  ", view_off+1, view_off+vc, nsec());
    // right-align size
    char sz[32]; std::snprintf(sz, sizeof(sz), "%dx%d  ", T_COLS, T_ROWS);
    int sz_col = T_COLS - static_cast<int>(std::strlen(sz));
    MOVE(1, sz_col);
    std::printf("%s%s%s%s", C_TITLE_BG, DIM, sz, RESET);

    // ── hint bar ──
    MOVE(2, 1);
    std::printf("%s  ", C_HINT_FG);
    auto hint = [](const char* key, const char* label) {
        std::printf("%s%s%s%s %s  ", BOLD, C_HINT_KEY_FG, key, C_HINT_FG, label);
    };
    hint("↑↓", "nav");
    hint("←→", "col");
    hint("a", "add");
    hint("d", "del");
    hint("spc", "done");
    hint("p", "pri");
    hint("m", "move");
    hint("N", "+col");
    hint("X", "-col");
    hint("[/]", "scroll");
    hint("q", "quit");
    std::fputs(RESET, stdout);

    if (sections.empty()) {
        MOVE(T_ROWS / 2, T_COLS / 2 - 15);
        std::printf("%s  No sections — press %s%sN%s%s to add one  %s",
                    C_HINT_FG, RESET, BOLD, RESET, C_HINT_FG, RESET);
    } else {
        for (int vi = 0; vi < vc; vi++)
            draw_col(vi * cw + 1, cw, view_off + vi,
                     (view_off + vi) == active,
                     col_hdr, crows);
    }

    // ── side-scroll arrows ──
    MOVE(T_ROWS, 1);
    std::printf("%s%s", C_BASE_BG, C_HINT_FG);
    if (view_off > 0)
        std::printf("%s%s  ◄ [  more  %s", BOLD, C_ARROW_FG, RESET);
    else
        std::fputs("            ", stdout);
    if (view_off + MAX_VIS < nsec())
        std::printf("%s%s  more  ]  ► %s", BOLD, C_ARROW_FG, RESET);
    std::fputs(RESET, stdout);

    std::fflush(stdout);
}

// ── input helpers ─────────────────────────────────────────────────────
// Read a line using raw read() so it works reliably across repeated calls.
// Enables canonical mode + echo just for the duration of the prompt.
static std::string prompt_line(const char* msg) {
    // Switch to canonical mode with echo so the user gets normal line editing
    termios cooked = orig_term;
    cooked.c_lflag |= (ICANON | ECHO);
    cooked.c_cc[VMIN]  = 1;
    cooked.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &cooked);
    SHOW_CUR;

    std::fputs(msg, stdout); std::fflush(stdout);

    std::string line;
    char c;
    while (true) {
        ssize_t n = ::read(STDIN_FILENO, &c, 1);
        if (n <= 0 || c == '\n') break;
        if (c == '\r') break;
        // handle backspace
        if (c == 127 || c == '\b') {
            if (!line.empty()) {
                line.pop_back();
                std::fputs("\b \b", stdout); std::fflush(stdout);
            }
            continue;
        }
        line += c;
    }

    HIDE_CUR;
    raw_on();   // restore raw + non-blocking mode
    return line;
}

// Read a single character immediately (no Enter needed), raw mode stays active.
// Valid chars are in the `valid` string; returns def if ESC/invalid pressed.
static int prompt_key(const char* msg, const char* valid, int def) {
    std::fputs(msg, stdout); std::fflush(stdout);
    SHOW_CUR;
    while (true) {
        fd_set fds; timeval tv{5, 0};  // 5s timeout fallback
        FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0)
            { HIDE_CUR; return def; }
        unsigned char c;
        if (::read(STDIN_FILENO, &c, 1) != 1) { HIDE_CUR; return def; }
        if (c == 27) { HIDE_CUR; return def; }  // ESC = cancel
        if (std::strchr(valid, (char)c)) {
            std::printf("%c\n", c); std::fflush(stdout);
            HIDE_CUR;
            return c - '0';
        }
    }
}

static int prompt_int(const char* msg, int lo, int hi, int def) {
    // Build valid-chars string from lo..hi
    char valid[16]; int vi = 0;
    for (int i = lo; i <= hi && vi < 15; i++) valid[vi++] = '0' + i;
    valid[vi] = '\0';
    return prompt_key(msg, valid, def);
}

// ── actions ───────────────────────────────────────────────────────────
static void act_add_task() {
    if (sections.empty()) return;
    Section& S = sections[static_cast<size_t>(active)];
    if (static_cast<int>(S.tasks.size()) >= MAX_TASKS) return;

    // draw the header in raw mode, then prompt_line will switch to cooked
    std::fputs(CLEAR, stdout);
    std::printf(C_TITLE_BG C_TITLE_FG BOLD "  ✓ ADD TASK to \"%s\"", S.name.c_str());
    int pad = T_COLS - 20 - static_cast<int>(S.name.size());
    for (int i = 0; i < pad; i++) std::putchar(' ');
    std::fputs(RESET "\n\n", stdout);
    std::fflush(stdout);

    std::string text = prompt_line("  Task: ");
    if (text.empty()) return;

    int pri = prompt_int("  Priority (1=High 2=Medium 3=Low) [2]: ", 1, 3, 2);
    S.tasks.push_back({ text, false, pri });
    S.cursor = static_cast<int>(S.tasks.size()) - 1;
    save();
}

static void act_del_task() {
    if (sections.empty()) return;
    Section& S = sections[static_cast<size_t>(active)];
    if (S.tasks.empty()) return;
    S.tasks.erase(S.tasks.begin() + S.cursor);
    if (S.cursor >= static_cast<int>(S.tasks.size()) && S.cursor > 0)
        S.cursor--;
    save();
}

static void act_toggle() {
    if (sections.empty()) return;
    Section& S = sections[static_cast<size_t>(active)];
    if (S.tasks.empty()) return;
    S.tasks[static_cast<size_t>(S.cursor)].done ^= true;
    save();
}

static void act_priority() {
    if (sections.empty()) return;
    Section& S = sections[static_cast<size_t>(active)];
    if (S.tasks.empty()) return;
    Task& t  = S.tasks[static_cast<size_t>(S.cursor)];
    t.priority = (t.priority % 3) + 1;
    save();
}

static void act_move() {
    if (nsec() < 2) return;
    Section& S = sections[static_cast<size_t>(active)];
    if (S.tasks.empty()) return;

    std::fputs(CLEAR C_TITLE_BG C_TITLE_FG BOLD "  ✓ MOVE TASK" RESET "\n\n", stdout);
    std::printf("  Moving: \"%s\"\n\n  Sections:\n", S.tasks[static_cast<size_t>(S.cursor)].text.c_str());
    for (int i = 0; i < nsec(); i++) {
        if (i == active) std::fputs(DIM, stdout);
        std::printf("    %d. %s%s\n", i+1, sections[static_cast<size_t>(i)].name.c_str(),
                    i == active ? " (current)" : "");
        std::fputs(RESET, stdout);
    }
    std::fflush(stdout);

    int dest = prompt_int("\n  Move to (0=cancel): ", 0, nsec(), 0) - 1;
    if (dest >= 0 && dest < nsec() && dest != active) {
        Section& D = sections[static_cast<size_t>(dest)];
        if (static_cast<int>(D.tasks.size()) < MAX_TASKS) {
            D.tasks.push_back(S.tasks[static_cast<size_t>(S.cursor)]);
            S.tasks.erase(S.tasks.begin() + S.cursor);
            if (S.cursor >= static_cast<int>(S.tasks.size()) && S.cursor > 0)
                S.cursor--;
            save();
        }
    }
}

static void act_add_section() {
    if (nsec() >= MAX_SECTIONS) return;
    std::fputs(CLEAR C_TITLE_BG C_TITLE_FG BOLD "  ✓ NEW SECTION" RESET "\n\n", stdout);
    std::fflush(stdout);
    std::string name = prompt_line("  Section name: ");
    if (name.empty()) return;
    sections.emplace_back(name);
    active   = nsec() - 1;
    view_off = std::max(0, nsec() - MAX_VIS);
    save();
}

static void act_rem_section() {
    if (sections.empty()) return;
    Section& S = sections[static_cast<size_t>(active)];
    if (!S.tasks.empty()) {
        std::fputs(CLEAR BG(88) FG(217) BOLD "  ✗ CANNOT REMOVE" RESET "\n\n", stdout);
        std::printf("  \"%s\" still has %zu task%s. Delete or move them first.\n\n",
                    S.name.c_str(), S.tasks.size(), S.tasks.size() == 1 ? "" : "s");
        std::fputs("  Press any key...", stdout); std::fflush(stdout);
        getch();
        return;
    }
    sections.erase(sections.begin() + active);
    if (active >= nsec() && active > 0) active--;
    view_off = std::clamp(view_off, 0, std::max(0, nsec() - MAX_VIS));
    save();
}

// ── main ──────────────────────────────────────────────────────────────
int main() {
    signal(SIGWINCH, sigwinch_handler);
    get_winsize();
    load();
    raw_on();
    HIDE_CUR;

    bool dirty = true;  // draw on first frame

    for (;;) {
        if (resize_flag) { resize_flag = 0; get_winsize(); dirty = true; }
        if (dirty) { draw(); dirty = false; }

        int ch = getch();
        if (ch == -1) continue;
        if (ch == 'q' || ch == 'Q') break;

        if (ch == '\033') {
            int c2 = getch_seq();
            if (c2 == '[') {
                int c3 = getch_seq();
                Section* S = !sections.empty()
                             ? &sections[static_cast<size_t>(active)]
                             : nullptr;
                if      (c3 == 'A') { if (S && S->cursor > 0) { S->cursor--; dirty = true; } }
                else if (c3 == 'B') { if (S && S->cursor < (int)S->tasks.size()-1) { S->cursor++; dirty = true; } }
                else if (c3 == 'C') { if (active < nsec()-1) { active++; clamp_view(); dirty = true; } }
                else if (c3 == 'D') { if (active > 0)        { active--; clamp_view(); dirty = true; } }
            }
        }
        else if (ch == 'a' || ch == 'A') { act_add_task();    dirty = true; }
        else if (ch == 'd' || ch == 'D') { act_del_task();    dirty = true; }
        else if (ch == ' ')              { act_toggle();       dirty = true; }
        else if (ch == 'p' || ch == 'P') { act_priority();    dirty = true; }
        else if (ch == 'm' || ch == 'M') { act_move();        dirty = true; }
        else if (ch == 'N')              { act_add_section();  dirty = true; }
        else if (ch == 'X')              { act_rem_section();  dirty = true; }
        else if (ch == '[') { if (view_off > 0)                { view_off--; dirty = true; } }
        else if (ch == ']') { if (view_off + MAX_VIS < nsec()) { view_off++; dirty = true; } }
    }

    raw_off();
    SHOW_CUR;
    std::fputs(CLEAR "Goodbye!\n", stdout);
    return 0;
}