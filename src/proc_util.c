/* SPDX-License-Identifier: GPL-2.0-or-later */
/* proc_util.c — see proc_util.h. */
#include "proc_util.h"

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
    return write(write_fd, "1", 1) == 1 ? 0 : -1;
}

int wait_for_owner_ready(int read_fd)
{
    char ready;
    ssize_t n = read(read_fd, &ready, 1);
    close(read_fd);
    return n == 1 ? 0 : -1;
}
