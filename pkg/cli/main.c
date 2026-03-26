/*
 * main.c — Casprix Package Manager (cpkg) entry point
 *
 * This file is intentionally thin: it initialises any process-level
 * state and delegates everything to cpkg_run(), which lives in
 * cli_complete.c.  Keeping main() separate makes the CLI logic
 * unit-testable without spawning a subprocess.
 *
 * Copyright (c) 2026 Casprix Project
 * SPDX-License-Identifier: MIT
 */

#include "cli.h"

int main(int argc, char **argv)
{
    return cpkg_run(argc, argv);
}
