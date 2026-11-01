/**
 * @file basic.h
 * @brief The BASIC interpreter.
 *
 * Programs are kept as text lines in SRAM and interpreted directly; LOAD
 * reads plain-text and tokenised (binary) BASIC files, SAVE writes plain text.
 *
 * @author Ken Segler
 * @copyright Copyright (c) 2026 Ken Segler
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdbool.h>

/// Runs the BASIC prompt until SYSTEM (or BYE). If `file` is given it is
/// loaded and run first.
/// @param file a program to load and run first, or NULL
void basic_main(const char *file);

/// Prints the "NNNN Bytes free" line (as the banner does).
void basic_print_free(void);

/// Whether a program is running (not just a line at the Ok prompt).
/// @return it is
bool basic_program_running(void);

