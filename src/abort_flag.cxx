/*
 * An async_operation implementation which sets a flag.  This can be
 * used by libraries which don't have their own implementation, but
 * need to know whether the operation has been aborted.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "abort_flag.hxx"
#include "util/Cast.hxx"

/*
 * async operation
 *
 */

static void
af_abort(struct async_operation *ao)
{
    abort_flag &af = ContainerCast2(*ao, &abort_flag::operation);

    assert(!af.aborted);

    af.aborted = true;
}

static const struct async_operation_class abort_flag_operation = {
    .abort = af_abort,
};


/*
 * constructor
 *
 */

void
abort_flag_init(struct abort_flag *af)
{
    af->operation.Init(abort_flag_operation);
    af->aborted = false;
}
