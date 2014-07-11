/*
 * Listener on a UDP port.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_UDP_LISTENER_HXX
#define BENG_PROXY_UDP_LISTENER_HXX

#include "glibfwd.hxx"

#include <stddef.h>

struct in_addr;
class SocketAddress;

struct udp_handler {
    /**
     * @param uid the peer process uid, or -1 if unknown
     */
    void (*datagram)(const void *data, size_t length,
                     SocketAddress address,
                     int uid,
                     void *ctx);

    void (*error)(GError *error, void *ctx);
};

struct udp_listener *
udp_listener_new(SocketAddress address,
                 const struct udp_handler *handler, void *ctx,
                 GError **error_r);

struct udp_listener *
udp_listener_port_new(const char *host_and_port, int default_port,
                      const struct udp_handler *handler, void *ctx,
                      GError **error_r);

void
udp_listener_free(struct udp_listener *udp);

/**
 * Enable the object after it has been disabled by
 * udp_listener_disable().  A new object is enabled by default.
 */
void
udp_listener_enable(struct udp_listener *udp);

/**
 * Disable the object temporarily.  To undo this, call
 * udp_listener_enable().
 */
void
udp_listener_disable(struct udp_listener *udp);

/**
 * Replaces the socket.  The old one is closed, and the new one is now
 * owned by this object.
 *
 * This may only be called on an object that is "enabled", see
 * udp_listener_enable().
 */
void
udp_listener_set_fd(struct udp_listener *udp, int fd);

/**
 * Joins the specified multicast group.
 *
 * @return true on success
 */
bool
udp_listener_join4(struct udp_listener *udp, const struct in_addr *group,
                   GError **error_r);

/**
 * Send a reply datagram to a client.
 */
bool
udp_listener_reply(struct udp_listener *udp, SocketAddress address,
                   const void *data, size_t data_length,
                   GError **error_r);

#endif
