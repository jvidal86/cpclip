/* SPDX-License-Identifier: GPL-2.0-or-later */
/* proc_util.c — see proc_util.h. */
#include "proc_util.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

void detach_from_terminal(void)
{
    int devnull = open("/dev/null", O_RDWR);
    if (devnull < 0)
        return;
    dup2(devnull, STDIN_FILENO);
    dup2(devnull, STDOUT_FILENO);
    dup2(devnull, STDERR_FILENO);
    if (devnull > STDERR_FILENO)
        close(devnull);
}

int signal_owner_ready(int write_fd)
{
    ssize_t n;
    do {
        n = write(write_fd, "1", 1);
    } while (n < 0 && errno == EINTR);
    return n == 1 ? 0 : -1;
}

int wait_for_owner_ready(int read_fd)
{
    char ready;
    ssize_t n;
    do {
        n = read(read_fd, &ready, 1);
    } while (n < 0 && errno == EINTR);
    close(read_fd);
    return n == 1 ? 0 : -1;
}
