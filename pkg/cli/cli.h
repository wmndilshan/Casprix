/*
 * cli.h — Casprix Package Manager (cpkg) CLI public interface
 *
 * Copyright (c) 2026 Casprix Project
 * SPDX-License-Identifier: MIT
 */

#ifndef CPKG_CLI_H
#define CPKG_CLI_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * cpkg_run - main command dispatcher.
 *
 * Parses argv, dispatches to the appropriate command handler, and
 * returns an exit code suitable for passing to exit()/return from main().
 *
 *   0  — success
 *   1  — user/usage error
 *   2  — internal / unexpected error
 */
int cpkg_run(int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif /* CPKG_CLI_H */
