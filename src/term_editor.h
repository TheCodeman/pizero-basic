/**
 * @file term_editor.h
 * @brief A full-screen text editor on an ANSI/VT100 terminal (EDIT).
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>

/// Edits the text file at `path` (made if it isn't there) full screen until
/// Esc or Ctrl-Q. With `program`, every line that isn't blank must start with
/// a line number before Ctrl-S saves (BASIC reads the file back as the
/// program).
/// @param path the file
/// @param program the file is a BASIC program
void term_edit(const char *path, bool program);

/// A code page 437 character's picture as UTF-8 (basic_port_serial.c).
/// @param c the character, 0-255
/// @param out its bytes (3 at most)
/// @return how many
int port_glyph_utf8(int c, char *out);
