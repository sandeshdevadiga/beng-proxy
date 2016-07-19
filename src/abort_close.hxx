/*
 * A wrapper for an async_operation which closes an istream on abort.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#ifndef BENG_PROXY_ABORT_CLOSE_HXX
#define BENG_PROXY_ABORT_CLOSE_HXX

struct pool;
class Istream;
class CancellablePointer;
struct async_operation_ref;

/**
 * @param istream the istream to be closed on abort; it should be
 * allocated from the specified pool, and must not have a handler
 */
struct async_operation_ref &
async_close_on_abort(struct pool &pool, Istream &istream,
                     CancellablePointer &cancel_ptr);

/**
 * Same as async_close_on_abort(), but allows #istream to be NULL.
 */
struct async_operation_ref &
async_optional_close_on_abort(struct pool &pool, Istream *istream,
                              struct async_operation_ref &async_ref);

#endif
