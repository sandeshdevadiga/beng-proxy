/*
 * Collect statistics of a beng-lb process.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_LB_STATS_H
#define BENG_LB_STATS_H

#include <stdint.h>

struct LbInstance;
struct beng_control_stats;

void
lb_get_stats(const LbInstance *instance,
             struct beng_control_stats *data);

#endif
