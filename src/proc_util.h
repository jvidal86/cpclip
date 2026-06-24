/* SPDX-License-Identifier: GPL-2.0-or-later */
/* proc_util.h — process plumbing shared by the forking clipboard owners.
 *
 * Both the X11 and Wayland backends become a persistent background owner the
 * same way: fork, have the child confirm it holds the selection over a pipe
 * (the readiness handshake), then detach from the terminal. These helpers
 * capture that shared mechanism. See DESIGN.md §6 (fork readiness handshake).
 */
#ifndef CLIP_PROC_UTIL_H
#define CLIP_PROC_UTIL_H

/* Redirect stdin/stdout/stderr to /dev/null — a backgrounded owner has no
 * controlling terminal to speak to. */
void detach_from_terminal(void);

/* Child side: tell the parent "I own the selection now" (one byte).
 * Returns 0 on success, -1 on write error. */
int signal_owner_ready(int write_fd);

/* Parent side: block until the child signals readiness. Returns 0 when ready,
 * -1 if the child died first (pipe EOF). Closes `read_fd`. */
int wait_for_owner_ready(int read_fd);

#endif /* CLIP_PROC_UTIL_H */
