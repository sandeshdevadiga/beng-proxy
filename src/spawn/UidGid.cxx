/*
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "UidGid.hxx"

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

void
UidGid::LoadEffective()
{
    uid = geteuid();
    gid = getegid();
}

void
UidGid::Apply() const
{
    if (gid != 0 && setregid(gid, gid) < 0) {
        fprintf(stderr, "failed to setgid %d: %s\n",
                int(gid), strerror(errno));
        _exit(EXIT_FAILURE);
    }

    if (uid != 0 && setreuid(uid, uid) < 0) {
        fprintf(stderr, "failed to setuid %d: %s\n",
                int(uid), strerror(errno));
        _exit(EXIT_FAILURE);
    }
}
