/**
 * @file term_editor.c
 * @brief Implementation of term_editor.h: a nano-like editor drawn with ANSI
 * codes, for a terminal on the USB port.
 *
 * The text is an array of lines (big arrays go to PSRAM where there is
 * some, through port_alloc). Only what changed is sent again: a line being
 * typed in, or the rows below an inserted or deleted line; scrolling and
 * Ctrl-L redraw it all. Lines longer than the terminal scroll sideways.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */

#include "term_editor.h"

#include "board_watchdog.h"

#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "basic_port.h"
#include "ff.h"
#include "pico/stdlib.h"

#define MAX_LINE   1024  ///< the longest line kept
#define BASIC_LINE 255   ///< the longest program line BASIC takes
#define TAB_WIDTH  4     ///< Tab inserts spaces to the next multiple of this

/// Keys beyond the bytes a terminal sends as they are.
enum {
    K_ESC = 0x100, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN,
    K_INS, K_DEL, K_TOP, K_BOTTOM, K_F1, K_F3, K_SUP, K_SDOWN,
};

/// A line of text (not 0-terminated).
typedef struct {
    char *s;  ///< its characters
    int len;  ///< how many
    int cap;  ///< room
} line_t;

static line_t *lines;  ///< the text
static int nlines, lines_cap;  ///< lines in it, and room
static line_t *clip;  ///< lines cut (Ctrl-X) or copied (Ctrl-C), for Ctrl-V
static int nclip, clip_cap;  ///< lines in the clipboard, and room
static bool cutting;  ///< the last key cut or copied a line (another adds to the clipboard)
static bool selecting;  ///< lines are marked (Shift+Up/Down), from sel_anchor to the cursor's
static int sel_anchor;  ///< where the marking started

static int cx, cy;  ///< the cursor: column in the line, line
static int want_x;  ///< the column Up and Down aim for
static int rowoff, coloff;  ///< the first line and column on the screen
static int rows = 24, cols = 80, textrows = 23;  ///< the terminal, and the rows of text
static bool modified, overwrite, is_program, out_of_memory;
static char msg[120];  ///< shown on the status line until the next key
static char find_text[64];  ///< the last thing searched for
static int dirty_lo, dirty_hi;  ///< lines to send again (dirty_lo > dirty_hi: none)
static bool full;  ///< send the whole screen again
static int pending = -1;  ///< a byte read too far while decoding a key

// ---- Undo: snapshots of the whole text --------------------------------------------------
// Programs and the files EDIT is for are small, so each step keeps the whole
// text (and the cursor) as it was. Typing in a row is one step, deleting in
// a row another.

#define UNDO_STEPS 100  ///< steps kept at most
#if PICO_RP2040
#define UNDO_BYTES (32 * 1024)  ///< and bytes (SRAM is short)
#else
#define UNDO_BYTES (256 * 1024)
#endif

/// The text as it was: the lines joined by '\n', and the cursor.
typedef struct {
    char *data;  ///< the text
    int size;    ///< its bytes
    int cx, cy;  ///< the cursor
} snap_t;

static snap_t undo_stack[UNDO_STEPS], redo_stack[UNDO_STEPS];  ///< oldest first
static int nundo, nredo;  ///< steps in each
/// What the last key did, so typing (or deleting) in a row is one step.
enum { E_NONE, E_TYPE, E_DELETE, E_OTHER };
static int last_edit, this_edit;

// ---- Output: a buffer sent in one go ------------------------------------------------

static char ob[2048];  ///< what goes to the terminal next
static int on;  ///< bytes in it

/// Sends the buffer.
static void flush(void) {
    fwrite(ob, 1, on, stdout);
    fflush(stdout);
    on = 0;
}

/// Adds bytes.
/// @param s the bytes
/// @param n how many
static void put(const char *s, int n) {
    if (on + n > (int)sizeof ob)
        flush();
    memcpy(ob + on, s, n);
    on += n;
}

/// Adds a string literal (its length counted by the compiler).
#define PUTS(lit) put(lit, sizeof lit - 1)

/// Adds text, printf style.
/// @param fmt the format
static void putf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void putf(const char *fmt, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    put(buf, n < (int)sizeof buf ? n : (int)sizeof buf - 1);
}

// ---- The text --------------------------------------------------------------------------

static void undo_mark(int kind);

/// Makes room for `n` characters in a line; false if there's no memory.
/// @param l the line
/// @param n the characters
/// @return there's room
static bool line_room(line_t *l, int n) {
    if (n <= l->cap)
        return true;
    int cap = n < 16 ? 16 : n + n / 2;
    char *s = port_realloc(l->s, cap);
    if (!s) {
        out_of_memory = true;
        return false;
    }
    l->s = s;
    l->cap = cap;
    return true;
}

/// Puts a new line at `at` holding `n` characters of `s`.
/// @param at where
/// @param s its text
/// @param n how many characters
/// @return done (false: out of memory)
static bool insert_line(int at, const char *s, int n) {
    if (nlines == lines_cap) {
        int cap = lines_cap ? lines_cap * 2 : 64;
        line_t *nl = port_realloc(lines, cap * sizeof *nl);
        if (!nl) {
            out_of_memory = true;
            return false;
        }
        lines = nl;
        lines_cap = cap;
    }
    line_t l = {0};
    if (!line_room(&l, n))
        return false;
    memcpy(l.s, s, n);
    l.len = n;
    memmove(&lines[at + 1], &lines[at], (nlines - at) * sizeof *lines);
    lines[at] = l;
    ++nlines;
    return true;
}

/// Removes line `at`.
/// @param at which
static void delete_line(int at) {
    port_release(lines[at].s);
    memmove(&lines[at], &lines[at + 1], (nlines - at - 1) * sizeof *lines);
    --nlines;
}

/// The bytes in a stack's snapshots.
/// @param st the stack
/// @param n its steps
/// @return the bytes
static int stack_bytes(const snap_t *st, int n) {
    int b = 0;
    for (int i = 0; i < n; ++i)
        b += st[i].size;
    return b;
}

/// Drops the oldest step of a stack.
/// @param st the stack
/// @param n its steps (one fewer after)
static void drop_oldest(snap_t *st, int *n) {
    port_release(st[0].data);
    memmove(&st[0], &st[1], (*n - 1) * sizeof *st);
    --*n;
}

/// Empties a stack.
/// @param st the stack
/// @param n its steps (0 after)
static void stack_clear(snap_t *st, int *n) {
    while (*n)
        drop_oldest(st, n);
}

/// The text and cursor as they are now; false if there's no memory.
/// @param sn filled in
/// @return taken
static bool take(snap_t *sn) {
    int size = 0;
    for (int i = 0; i < nlines; ++i)
        size += lines[i].len + 1;
    char *d = port_alloc(size ? size : 1);
    if (!d)
        return false;
    int k = 0;
    for (int i = 0; i < nlines; ++i) {
        memcpy(d + k, lines[i].s, lines[i].len);
        k += lines[i].len;
        d[k++] = '\n';
    }
    *sn = (snap_t){d, size, cx, cy};
    return true;
}

/// Pushes a snapshot onto a stack, dropping the oldest steps to stay within
/// the step and byte limits.
/// @param st the stack
/// @param n its steps
/// @param sn the snapshot (the stack keeps it)
static void push(snap_t *st, int *n, snap_t sn) {
    while (*n && (*n == UNDO_STEPS || stack_bytes(st, *n) + sn.size > UNDO_BYTES))
        drop_oldest(st, n);
    if (sn.size > UNDO_BYTES) {  // too big to keep at all
        port_release(sn.data);
        return;
    }
    st[(*n)++] = sn;
}

/// Puts the text and cursor back as a snapshot has them (and frees it).
/// @param sn the snapshot
static void restore(snap_t sn) {
    for (int i = 0; i < nlines; ++i)
        port_release(lines[i].s);
    nlines = 0;
    int start = 0;
    for (int k = 0; k < sn.size; ++k)
        if (sn.data[k] == '\n') {
            insert_line(nlines, sn.data + start, k - start);
            start = k + 1;
        }
    if (!nlines)
        insert_line(0, "", 0);
    port_release(sn.data);
    cy = sn.cy < nlines ? sn.cy : nlines - 1;
    cx = sn.cx <= lines[cy].len ? sn.cx : lines[cy].len;
}

/// Before a change: keeps the text as it is, for Ctrl-Z (once for a run of
/// typing, or of deleting). A new change forgets what Ctrl-R could redo.
/// @param kind E_TYPE, E_DELETE or E_OTHER
static void undo_mark(int kind) {
    this_edit = kind;
    if (kind == last_edit && kind != E_OTHER)
        return;
    snap_t sn;
    if (!take(&sn)) {
        out_of_memory = true;
        return;
    }
    push(undo_stack, &nundo, sn);
    stack_clear(redo_stack, &nredo);
}

/// Ctrl-Z (undo) or Ctrl-R (redo): back to the last step, keeping where we
/// were on the other stack.
/// @param undo undo (else redo)
static void undo_redo(bool undo) {
    snap_t *from = undo ? undo_stack : redo_stack, *to = undo ? redo_stack : undo_stack;
    int *nfrom = undo ? &nundo : &nredo, *nto = undo ? &nredo : &nundo;
    if (!*nfrom) {
        snprintf(msg, sizeof msg, undo ? "Nothing to undo" : "Nothing to redo");
        return;
    }
    snap_t now;
    if (!take(&now)) {
        out_of_memory = true;
        return;
    }
    push(to, nto, now);
    restore(from[--*nfrom]);
    modified = true;
    full = true;
}

/// Frees the text and the cut buffer.
static void free_all(void) {
    for (int i = 0; i < nlines; ++i)
        port_release(lines[i].s);
    for (int i = 0; i < nclip; ++i)
        port_release(clip[i].s);
    port_release(lines);
    port_release(clip);
    lines = clip = NULL;
    nlines = lines_cap = nclip = clip_cap = 0;
    stack_clear(undo_stack, &nundo);
    stack_clear(redo_stack, &nredo);
}

/// Reads the file (none yet: one empty line). CR LF or LF lines; a ^Z at the
/// end is left off.
/// @param path the file
/// @return read (false: out of memory or a read error)
static bool load(const char *path) {
    FIL f;
    char *cur = port_alloc(MAX_LINE);
    if (!cur)
        return false;
    int n = 0;
    bool ok = true;
    if (f_open(&f, path, FA_READ) == FR_OK) {
        char buf[512];
        UINT got;
        while (ok && f_read(&f, buf, sizeof buf, &got) == FR_OK && got) {
            for (UINT i = 0; i < got; ++i) {
                char c = buf[i];
                if (c == '\n') {
                    if (n && cur[n - 1] == '\r')
                        --n;
                    ok = insert_line(nlines, cur, n);
                    n = 0;
                } else if (c != 0x1a && n < MAX_LINE) {
                    cur[n++] = c;
                }
            }
        }
        f_close(&f);
    }
    if (ok && (n || !nlines))
        ok = insert_line(nlines, cur, n);
    port_release(cur);
    return ok;
}

/// Writes the text back, lines ending in CR LF.
/// @param path the file
/// @return written
static bool save(const char *path) {
    FIL f;
    if (f_open(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
        return false;
    bool ok = true;
    UINT w;
    int last = nlines;
    while (last > 0 && !lines[last - 1].len)  // blank lines at the end
        --last;
    for (int i = 0; ok && i < last; ++i)
        ok = f_write(&f, lines[i].s, lines[i].len, &w) == FR_OK && w == (UINT)lines[i].len &&
             f_write(&f, "\r\n", 2, &w) == FR_OK && w == 2;
    ok = f_close(&f) == FR_OK && ok;
    return ok;
}

/// Whether a program has line numbers: its first line that isn't blank
/// starts with one.
/// @return it has
static bool program_numbered(void) {
    for (int i = 0; i < nlines; ++i) {
        int k = 0;
        while (k < lines[i].len && (lines[i].s[k] == ' ' || lines[i].s[k] == '\t'))
            ++k;
        if (k < lines[i].len)
            return isdigit((unsigned char)lines[i].s[k]);
    }
    return false;
}

/// For a program: the first line BASIC wouldn't take (a line number in a
/// program without them or none in one with them, a number over 65529, or
/// longer than 255), with why; -1 if all are fine. Blank lines are fine.
/// @param why set to the reason
/// @return the bad line, or -1
static int check_program(const char **why) {
    bool numbered = program_numbered();
    for (int i = 0; i < nlines; ++i) {
        const char *s = lines[i].s;
        int n = lines[i].len, k = 0;
        while (k < n && (s[k] == ' ' || s[k] == '\t'))
            ++k;
        if (k == n)
            continue;
        if (n - k > BASIC_LINE) {  // (its indent doesn't count)
            *why = "a program line can be 255 characters at most";
            return i;
        }
        if (!numbered) {
            if (isdigit((unsigned char)s[k])) {
                *why = "a line number, but the first line has none (all or none)";
                return i;
            }
            continue;
        }
        if (!isdigit((unsigned char)s[k])) {
            *why = "no line number, but the first line has one (all or none)";
            return i;
        }
        long num = 0;
        while (k < n && isdigit((unsigned char)s[k]) && num <= 65529)
            num = num * 10 + (s[k++] - '0');
        if (num > 65529) {
            *why = "line numbers go up to 65529";
            return i;
        }
    }
    return -1;
}

// ---- Keys ---------------------------------------------------------------------------------

/// The next byte, waiting up to `us` (forever if 0); -1 if none came.
/// @param us how long
/// @return the byte, or -1
static int byte_in(uint32_t us) {
    if (pending >= 0) {
        int c = pending;
        pending = -1;
        return c;
    }
    if (!us) {
        int c;
        while ((c = port_key(100000)) < 0)
            board_watchdog_feed();  // someone is editing
        return c;
    }
    return port_key(us);
}

/// The next key, with the terminal's escape sequences decoded (xterm, VT100,
/// Linux console).
/// @return a byte, or a K_ code
static int key_in(void) {
    static bool after_cr;
    int c = byte_in(0);
    if (c == '\n' && after_cr) {  // a CR LF terminal: the LF is the same Enter
        after_cr = false;
        c = byte_in(0);
    }
    after_cr = c == '\r';
    if (c == '\n')
        return '\r';
    if (c != 27)
        return c;
    int c1 = byte_in(30000);
    if (c1 < 0)
        return K_ESC;
    if (c1 == 'O') {  // SS3: F1-F4, and keys in application mode
        int c2 = byte_in(30000);
        switch (c2) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        case 'P': return K_F1;
        case 'R': return K_F3;
        default: return 0;
        }
    }
    if (c1 != '[') {
        pending = c1;  // Esc, then a key of its own
        return K_ESC;
    }
    int p[2] = {0, 0}, np = 0, f;
    while ((f = byte_in(30000)) >= 0 && (isdigit(f) || f == ';')) {
        if (f == ';')
            np = np < 1 ? np + 1 : 1;
        else
            p[np] = p[np] * 10 + (f - '0');
    }
    bool ctrl = p[1] == 5;  // xterm's modifier 5: Ctrl
    bool shift = p[1] == 2;  // and 2: Shift
    switch (f) {
    case 'A': return shift ? K_SUP : K_UP;
    case 'B': return shift ? K_SDOWN : K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return ctrl ? K_TOP : K_HOME;
    case 'F': return ctrl ? K_BOTTOM : K_END;
    case '~':
        switch (p[0]) {
        case 1: case 7: return ctrl ? K_TOP : K_HOME;
        case 4: case 8: return ctrl ? K_BOTTOM : K_END;
        case 2: return K_INS;
        case 3: return K_DEL;
        case 5: return K_PGUP;
        case 6: return K_PGDN;
        case 11: return K_F1;
        case 13: return K_F3;
        }
        return 0;
    case '[':  // the Linux console's F1-F5: Esc [ [ A...
        f = byte_in(30000);
        return f == 'A' ? K_F1 : f == 'C' ? K_F3 : 0;
    }
    return 0;
}

/// Asks the terminal its size (by moving the cursor far away and asking
/// where it is); 80 x 24 if it doesn't say.
static void get_size(void) {
    PUTS("\x1b[999;999H\x1b[6n");
    flush();
    int r = 0, c = 0, ch, state = 0;
    absolute_time_t until = make_timeout_time_ms(300);
    while (state < 4 && !time_reached(until)) {
        if ((ch = port_key(20000)) < 0)
            continue;
        if (state == 0 && ch == 27)
            state = 1;
        else if (state == 1 && ch == '[')
            state = 2;
        else if (state == 2 && isdigit(ch))
            r = r * 10 + ch - '0';
        else if (state == 2 && ch == ';')
            state = 3;
        else if (state == 3 && isdigit(ch))
            c = c * 10 + ch - '0';
        else if (state == 3 && ch == 'R')
            state = 4;
    }
    rows = state == 4 && r >= 5 ? (r > 100 ? 100 : r) : 24;
    cols = state == 4 && c >= 20 ? (c > 250 ? 250 : c) : 80;
    textrows = rows - 1;
}

// ---- Drawing ----------------------------------------------------------------------------

/// Marks lines a..b to be sent again.
/// @param a the first
/// @param b the last
static void dirty(int a, int b) {
    if (a < dirty_lo)
        dirty_lo = a;
    if (b > dirty_hi)
        dirty_hi = b;
}

/// Sends screen row `r`.
/// @param r the row (0 the top)
static void draw_row(int r) {
    int i = rowoff + r;
    putf("\x1b[%d;1H", r + 1);
    bool marked = selecting && i < nlines &&
                  i >= (sel_anchor < cy ? sel_anchor : cy) && i <= (sel_anchor > cy ? sel_anchor : cy);
    if (marked)
        PUTS("\x1b[7m");
    if (i < nlines) {
        const line_t *l = &lines[i];
        for (int k = coloff; k < l->len && k < coloff + cols; ++k) {
            unsigned char c = (unsigned char)l->s[k];
            if (c < 32 || c >= 127) {
                char g[3];
                put(g, port_glyph_utf8(c, g));
            } else {
                put((const char *)&c, 1);
            }
        }
    } else {
        PUTS("\x1b[2m~\x1b[0m");
    }
    if (marked)
        PUTS(" \x1b[0m");  // (a marked empty line still shows)
    PUTS("\x1b[K");
}

/// Sends the status line: the message if there is one, else where you are
/// and the keys.
static void draw_status(const char *name) {
    char buf[260];
    int n;
    if (msg[0])
        n = snprintf(buf, sizeof buf, " %s", msg);
    else
        n = snprintf(buf, sizeof buf, " %s%s  Ln %d/%d Col %d  %s   ^S save  Esc quit  F1 help",
                     name, modified ? " *" : "", cy + 1, nlines, cx + 1, overwrite ? "OVR" : "INS");
    if (n > cols)
        n = cols;
    while (n < cols && n < (int)sizeof buf - 1)
        buf[n++] = ' ';
    putf("\x1b[%d;1H\x1b[7m", rows);
    put(buf, n);
    PUTS("\x1b[0m");
}

/// Brings the cursor into view, scrolling if need be.
static void scroll(void) {
    if (cy < rowoff) {
        rowoff = cy;
        full = true;
    } else if (cy >= rowoff + textrows) {
        rowoff = cy - textrows + 1;
        full = true;
    }
    if (cx < coloff) {
        coloff = cx;
        full = true;
    } else if (cx >= coloff + cols) {
        coloff = cx - cols + 1;
        full = true;
    }
}

/// Sends what changed, the status line and the cursor.
/// @param name the name on the status line
static void refresh(const char *name) {
    scroll();
    PUTS("\x1b[?25l");
    for (int r = 0; r < textrows; ++r)
        if (full || (rowoff + r >= dirty_lo && rowoff + r <= dirty_hi))
            draw_row(r);
    full = false;
    dirty_lo = INT_MAX;
    dirty_hi = -1;
    draw_status(name);
    putf("\x1b[%d;%dH\x1b[?25h", cy - rowoff + 1, cx - coloff + 1);
    flush();
}

/// Reads a reply on the status line (Backspace edits, Enter takes it, Esc
/// gives up).
/// @param label what is asked
/// @param buf the reply (it starts with what is there)
/// @param size its room
/// @return Enter (false: Esc)
static bool ask(const char *label, char *buf, int size) {
    int n = strlen(buf);
    for (;;) {
        putf("\x1b[%d;1H\x1b[7m %s%s\x1b[K\x1b[0m", rows, label, buf);
        flush();
        int k = key_in();
        if (k == '\r')
            return true;
        if (k == K_ESC || k == 3 || k == 7)
            return false;
        if ((k == 8 || k == 127) && n)
            buf[--n] = 0;
        else if (k >= 32 && k < 127 && n < size - 1) {
            buf[n++] = (char)k;
            buf[n] = 0;
        }
    }
}

/// F1: the keys, until a key is pressed.
static void help(void) {
    static const char *const text[] = {
        "EDIT: the full-screen editor",
        "",
        "  Arrows, Home, End, PgUp, PgDn     move",
        "  Ctrl-Home, Ctrl-End               the top, the bottom",
        "  Backspace, Delete                 delete (joining lines at their ends)",
        "  Enter                             split the line",
        "  Insert                            insert or overwrite",
        "  Tab                               spaces to the next column of 4",
        "  Shift+Up, Shift+Down              mark lines",
        "  Ctrl-X                            cut the marked lines, or the line",
        "  Ctrl-C                            copy them (again: add the next line)",
        "  Ctrl-V                            paste above the cursor's line",
        "  Ctrl-Z, Ctrl-R                    undo, redo",
        "  Ctrl-F, F3                        find, find the next",
        "  Ctrl-G                            go to a line (or a BASIC line number)",
        "  Ctrl-S                            save (a program: BASIC takes it on leaving)",
        "  Esc or Ctrl-Q                     leave (asks if there are changes)",
        "  Ctrl-L                            draw the screen again",
        "",
        "A program's lines all have line numbers, or none do (then use labels:",
        "a name and a colon starting a line, LOOP:, and GOTO LOOP).",
        "",
        "Any key goes back.",
    };
    PUTS("\x1b[2J");
    for (unsigned i = 0; i < sizeof text / sizeof *text && (int)i < rows; ++i)
        putf("\x1b[%u;1H%s", i + 1, text[i]);
    flush();
    key_in();
    full = true;
}

// ---- Editing ----------------------------------------------------------------------------

/// Puts the cursor on line `y`, column `x`, within the text.
/// @param x the column
/// @param y the line
static void go(int x, int y) {
    cy = y < 0 ? 0 : y >= nlines ? nlines - 1 : y;
    cx = x < 0 ? 0 : x > lines[cy].len ? lines[cy].len : x;
}

/// Types a character at the cursor (or over the one there).
/// @param c the character
static void type(char c) {
    undo_mark(E_TYPE);
    line_t *l = &lines[cy];
    if (overwrite && cx < l->len) {
        l->s[cx++] = c;
    } else {
        if (l->len >= MAX_LINE) {
            snprintf(msg, sizeof msg, "Lines can be %d characters at most", MAX_LINE);
            return;
        }
        if (!line_room(l, l->len + 1))
            return;
        memmove(l->s + cx + 1, l->s + cx, l->len - cx);
        l->s[cx++] = c;
        ++l->len;
    }
    modified = true;
    dirty(cy, cy);
}

/// Joins line `y + 1` onto the end of line `y`.
/// @param y the line
static void join(int y) {
    line_t *a = &lines[y], *b = &lines[y + 1];
    if (a->len + b->len > MAX_LINE) {
        snprintf(msg, sizeof msg, "The joined line would be too long");
        return;
    }
    if (!line_room(a, a->len + b->len))
        return;
    memcpy(a->s + a->len, b->s, b->len);
    a->len += b->len;
    delete_line(y + 1);
    modified = true;
    dirty(y, INT_MAX);
}

/// Enter: the rest of the line becomes a new line below.
static void split(void) {
    undo_mark(E_OTHER);
    line_t *l = &lines[cy];
    if (!insert_line(cy + 1, l->s + cx, l->len - cx))
        return;
    lines[cy].len = cx;  // (the array may have moved)
    dirty(cy, INT_MAX);
    go(0, cy + 1);
    modified = true;
}

/// Empties the clipboard.
static void clip_clear(void) {
    for (int i = 0; i < nclip; ++i)
        port_release(clip[i].s);
    nclip = 0;
}

/// Makes room for one more line in the clipboard.
/// @return there is room
static bool clip_room(void) {
    if (nclip < clip_cap)
        return true;
    int cap = clip_cap ? clip_cap * 2 : 16;
    line_t *nc = port_realloc(clip, cap * sizeof *nc);
    if (!nc) {
        out_of_memory = true;
        return false;
    }
    clip = nc;
    clip_cap = cap;
    return true;
}

/// Cuts (moves) line `y` to the end of the clipboard.
/// @param y the line
static void cut_one(int y) {
    if (!clip_room())
        return;
    clip[nclip++] = lines[y];
    memmove(&lines[y], &lines[y + 1], (nlines - y - 1) * sizeof *lines);
    if (--nlines == 0)
        insert_line(0, "", 0);
    modified = true;
}

/// Copies line `y` to the end of the clipboard.
/// @param y the line
static void copy_one(int y) {
    if (!clip_room())
        return;
    line_t c = {0};
    if (!line_room(&c, lines[y].len))
        return;
    memcpy(c.s, lines[y].s, lines[y].len);
    c.len = lines[y].len;
    clip[nclip++] = c;
}

/// Ctrl-X (cut) or Ctrl-C (copy): the marked lines, or the cursor's line
/// (added to the clipboard if the last key cut or copied too; a copy then
/// moves down, so Ctrl-C again takes the next line).
/// @param cut cut (else copy)
/// @param was the last key cut or copied
static void cut_or_copy(bool cut, bool was) {
    if (cut)
        undo_mark(E_OTHER);
    if (selecting) {
        int a = sel_anchor < cy ? sel_anchor : cy, b = sel_anchor > cy ? sel_anchor : cy;
        clip_clear();
        for (int y = a; y <= b; ++y)
            if (cut)
                cut_one(a);
            else
                copy_one(y);
        selecting = false;
        go(0, a);
        snprintf(msg, sizeof msg, "%d line%s %s", b - a + 1, b > a ? "s" : "", cut ? "cut" : "copied");
        dirty(a, INT_MAX);
        return;
    }
    if (!was)
        clip_clear();
    if (cut) {
        cut_one(cy);
        go(0, cy);
        dirty(cy, INT_MAX);
    } else {
        copy_one(cy);
        go(0, cy + 1);
    }
    cutting = true;
}

/// Ctrl-V: puts the clipboard's lines in above the cursor's line.
static void paste(void) {
    if (!nclip) {
        snprintf(msg, sizeof msg, "Nothing to paste: Ctrl-X cuts, Ctrl-C copies");
        return;
    }
    undo_mark(E_OTHER);
    for (int i = 0; i < nclip; ++i)
        if (!insert_line(cy + i, clip[i].s, clip[i].len))
            break;
    if (nclip) {
        modified = true;
        dirty(cy, INT_MAX);
        go(0, cy + nclip);
    }
}

/// Finds `find_text` after the cursor (round to the top at the end).
static void find_next(void) {
    int n = strlen(find_text);
    if (!n)
        return;
    for (int step = 0; step <= nlines; ++step) {
        int y = (cy + step) % nlines;
        const line_t *l = &lines[y];
        for (int x = step ? 0 : cx + 1; x + n <= l->len; ++x)
            if (!strncasecmp(l->s + x, find_text, n)) {
                go(x, y);
                return;
            }
    }
    snprintf(msg, sizeof msg, "\"%s\" isn't there", find_text);
}

/// Ctrl-G: a program's line number, else a line of the file.
static void go_to_line(void) {
    char buf[12] = "";
    bool numbered = is_program && program_numbered();
    if (!ask(numbered ? "Go to BASIC line: " : "Go to line: ", buf, sizeof buf) || !buf[0])
        return;
    long n = strtol(buf, NULL, 10);
    if (numbered) {
        for (int i = 0; i < nlines; ++i) {
            const char *s = lines[i].s;
            int k = 0;
            while (k < lines[i].len && s[k] == ' ')
                ++k;
            if (k < lines[i].len && isdigit((unsigned char)s[k]) && strtol(s + k, NULL, 10) >= n) {
                go(0, i);
                return;
            }
        }
        go(0, nlines - 1);
    } else {
        go(0, (int)n - 1);
    }
}

/// Ctrl-S: checks a program, then writes the file.
/// @param path the file
/// @return saved
static bool do_save(const char *path) {
    if (is_program) {
        const char *why;
        int bad = check_program(&why);
        if (bad >= 0) {
            go(0, bad);
            snprintf(msg, sizeof msg, "Not saved: line %d: %s", bad + 1, why);
            return false;
        }
    }
    if (!save(path)) {
        snprintf(msg, sizeof msg, "Couldn't write %s (disk full?)", path);
        return false;
    }
    modified = false;
    snprintf(msg, sizeof msg, "Saved%s", is_program ? ": BASIC takes it when you leave" : "");
    return true;
}

void term_edit(const char *path, bool program) {
    is_program = program;
    modified = overwrite = out_of_memory = cutting = selecting = false;
    last_edit = this_edit = E_NONE;
    pending = -1;
    cx = cy = want_x = rowoff = coloff = 0;
    msg[0] = 0;
    if (!load(path)) {
        free_all();
        printf("Not enough memory to edit %s\n", path);
        return;
    }
    const char *name = is_program ? "Program" : path;
    get_size();
    PUTS("\x1b[0m\x1b[2J");
    full = true;
    dirty_lo = INT_MAX;
    dirty_hi = -1;
    for (;;) {
        if (out_of_memory) {
            snprintf(msg, sizeof msg, "Out of memory: save what you have");
            out_of_memory = false;
        }
        refresh(name);
        int k = key_in();
        bool was_cutting = cutting;
        cutting = false;
        if (msg[0]) {
            msg[0] = 0;
            draw_status(name);
        }
        bool vertical = false, keep_selection = false;
        this_edit = E_NONE;
        switch (k) {
        case K_UP: go(want_x, cy - 1); vertical = true; break;
        case K_DOWN: go(want_x, cy + 1); vertical = true; break;
        case K_PGUP: go(want_x, cy - textrows); vertical = true; break;
        case K_PGDN: go(want_x, cy + textrows); vertical = true; break;
        case K_LEFT:
            if (cx)
                --cx;
            else if (cy)
                go(INT_MAX, cy - 1);
            break;
        case K_RIGHT:
            if (cx < lines[cy].len)
                ++cx;
            else if (cy < nlines - 1)
                go(0, cy + 1);
            break;
        case K_HOME: cx = 0; break;
        case K_END: cx = lines[cy].len; break;
        case K_TOP: go(0, 0); break;
        case K_BOTTOM: go(INT_MAX, nlines - 1); break;
        case K_INS: overwrite = !overwrite; break;
        case K_DEL:
            if (cx < lines[cy].len || cy < nlines - 1)
                undo_mark(E_DELETE);
            if (cx < lines[cy].len) {
                line_t *l = &lines[cy];
                memmove(l->s + cx, l->s + cx + 1, l->len - cx - 1);
                --l->len;
                modified = true;
                dirty(cy, cy);
            } else if (cy < nlines - 1) {
                join(cy);
            }
            break;
        case 8:
        case 127:
            if (cx || cy)
                undo_mark(E_DELETE);
            if (cx) {
                --cx;
                line_t *l = &lines[cy];
                memmove(l->s + cx, l->s + cx + 1, l->len - cx - 1);
                --l->len;
                modified = true;
                dirty(cy, cy);
            } else if (cy) {
                int x = lines[cy - 1].len;
                join(cy - 1);
                go(x, cy - 1);
            }
            break;
        case '\r': split(); break;
        case '\t':
            do
                type(' ');
            while (cx % TAB_WIDTH);
            break;
        case K_SUP:
        case K_SDOWN:
            if (!selecting) {
                selecting = true;
                sel_anchor = cy;
            }
            dirty(cy, cy);
            go(want_x, cy + (k == K_SUP ? -1 : 1));
            dirty(cy, cy);
            vertical = true;
            keep_selection = true;
            break;
        case 24:  // Ctrl-X
        case 25:  // Ctrl-Y, as in older editors
            cut_or_copy(true, was_cutting);
            break;
        case 3: cut_or_copy(false, was_cutting); break;  // Ctrl-C (never Break here)
        case 22: paste(); break;  // Ctrl-V
        case 26: undo_redo(true); break;  // Ctrl-Z
        case 18: undo_redo(false); break;  // Ctrl-R
        case 6: {  // Ctrl-F
            char buf[sizeof find_text];
            snprintf(buf, sizeof buf, "%s", find_text);
            if (ask("Find: ", buf, sizeof buf) && buf[0]) {
                snprintf(find_text, sizeof find_text, "%s", buf);
                find_next();
            }
            full = true;
            break;
        }
        case K_F3: find_next(); break;
        case 7: go_to_line(); full = true; break;  // Ctrl-G
        case 19: do_save(path); break;  // Ctrl-S
        case 12: get_size(); PUTS("\x1b[2J"); full = true; break;  // Ctrl-L
        case K_F1: help(); break;
        case K_ESC:
        case 17: {  // Ctrl-Q
            if (modified) {
                char buf[4] = "";
                if (!ask("Save the changes (Y/N)? ", buf, sizeof buf)) {
                    full = true;
                    break;  // Esc: back to editing
                }
                if ((buf[0] == 'Y' || buf[0] == 'y') && !do_save(path)) {
                    full = true;
                    break;  // not saved (a bad line): stay
                }
            }
            PUTS("\x1b[0m\x1b[2J\x1b[H\x1b[?25h");
            flush();
            free_all();
            pending = -1;
            return;
        }
        default:
            if (k >= 32 && k < 256 && k != 127)
                type((char)k);
        }
        last_edit = this_edit;  // a key that changed nothing ends a run of typing
        if (!vertical)
            want_x = cx;
        if (selecting && !keep_selection) {  // any other key ends the marking
            selecting = false;
            full = true;
        }
    }
}
