/*
 * Launch WAS child processes.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "was_launch.hxx"
#include "system/fd_util.h"
#include "system/fd-util.h"
#include "spawn/Local.hxx"
#include "spawn/Prepared.hxx"
#include "spawn/ChildOptions.hxx"
#include "gerrno.h"
#include "util/ConstBuffer.hxx"

#include <daemon/log.h>
#include <inline/compiler.h>

#include <sys/socket.h>
#include <unistd.h>

void
WasProcess::Close()
{
    if (control_fd >= 0) {
        close(control_fd);
        control_fd = -1;
    }

    if (input_fd >= 0) {
        close(input_fd);
        input_fd = -1;
    }

    if (output_fd >= 0) {
        close(output_fd);
        output_fd = -1;
    }
}

bool
was_launch(WasProcess *process,
           const char *executable_path,
           ConstBuffer<const char *> args,
           const ChildOptions &options,
           GError **error_r)
{
    int control_fds[2], input_fds[2], output_fds[2];

    if (socketpair_cloexec(AF_UNIX, SOCK_STREAM, 0, control_fds) < 0) {
        set_error_errno_msg(error_r, "failed to create socket pair");
        return false;
    }

    if (pipe_cloexec(input_fds) < 0) {
        set_error_errno_msg(error_r, "failed to create first pipe");
        close(control_fds[0]);
        close(control_fds[1]);
        return false;
    }

    if (pipe_cloexec(output_fds) < 0) {
        set_error_errno_msg(error_r, "failed to create second pipe");
        close(control_fds[0]);
        close(control_fds[1]);
        close(input_fds[0]);
        close(input_fds[1]);
        return false;
    }

    PreparedChildProcess p;

    p.stdin_fd = output_fds[0];
    p.stdout_fd = input_fds[1];
    /* fd2 is retained */
    p.control_fd = control_fds[1];

    p.Append(executable_path);
    for (auto i : args)
        p.Append(i);

    if (!options.CopyTo(p, true, nullptr, error_r)) {
        close(control_fds[0]);
        close(input_fds[0]);
        close(output_fds[1]);
        return false;
    }

    pid_t pid = SpawnChildProcess(std::move(p));
    if (pid < 0) {
        set_error_errno_msg2(error_r, -pid, "clone() failed");
        close(control_fds[0]);
        close(input_fds[0]);
        close(output_fds[1]);
        return false;
    }

    fd_set_nonblock(input_fds[0], true);
    fd_set_nonblock(output_fds[1], true);

    process->pid = pid;
    process->control_fd = control_fds[0];
    process->input_fd = input_fds[0];
    process->output_fd = output_fds[1];
    return true;
}
