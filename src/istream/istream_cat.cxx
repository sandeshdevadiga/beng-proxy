/*
 * Concatenate several istreams.
 *
 * author: Max Kellermann <mk@cm4all.com>
 */

#include "istream_cat.hxx"
#include "istream_oo.hxx"
#include "istream_pointer.hxx"
#include "Bucket.hxx"
#include "util/ConstBuffer.hxx"

#include <boost/intrusive/slist.hpp>

#include <iterator>

#include <assert.h>
#include <stdarg.h>

struct CatIstream final : public Istream {
    struct Input final
        : IstreamHandler,
          boost::intrusive::slist_base_hook<boost::intrusive::link_mode<boost::intrusive::normal_link>> {

        CatIstream &cat;
        IstreamPointer istream;

        Input(CatIstream &_cat, Istream &_istream)
            :cat(_cat), istream(_istream, *this) {}

        void Read(FdTypeMask direct) {
            istream.SetDirect(direct);
            istream.Read();
        }

        bool FillBucketList(IstreamBucketList &list, GError **error_r) {
            return istream.FillBucketList(list, error_r);
        }

        size_t ConsumeBucketList(size_t nbytes) {
            return istream.ConsumeBucketList(nbytes);
        }

        /* virtual methods from class IstreamHandler */

        size_t OnData(const void *data, size_t length) override {
            return cat.OnInputData(*this, data, length);
        }

        ssize_t OnDirect(FdType type, int fd, size_t max_length) override {
            return cat.OnInputDirect(*this, type, fd, max_length);
        }

        void OnEof() override {
            assert(istream.IsDefined());
            istream.Clear();

            cat.OnInputEof(*this);
        }

        void OnError(GError *error) override {
            assert(istream.IsDefined());
            istream.Clear();

            cat.OnInputError(*this, error);
        }

        struct Disposer {
            void operator()(Input *input) {
                input->istream.Close();
            }
        };
    };

    bool reading = false;

    typedef boost::intrusive::slist<Input,
                                    boost::intrusive::constant_time_size<false>> InputList;
    InputList inputs;

    CatIstream(struct pool &p, ConstBuffer<Istream *> _inputs);

    Input &GetCurrent() {
        return inputs.front();
    }

    const Input &GetCurrent() const {
        return inputs.front();
    }

    bool IsCurrent(const Input &input) const {
        return &GetCurrent() == &input;
    }

    bool IsEOF() const {
        return inputs.empty();
    }

    void CloseAllInputs() {
        inputs.clear_and_dispose(Input::Disposer());
    }

    size_t OnInputData(Input &i, const void *data, size_t length) {
        return IsCurrent(i)
            ? InvokeData(data, length)
            : 0;
    }

    ssize_t OnInputDirect(gcc_unused Input &i, FdType type, int fd,
                          size_t max_length) {
        assert(IsCurrent(i));

        return InvokeDirect(type, fd, max_length);
    }

    void OnInputEof(Input &i) {
        const bool current = IsCurrent(i);
        inputs.erase(inputs.iterator_to(i));

        if (IsEOF()) {
            assert(current);
            DestroyEof();
        } else if (current && !reading) {
            /* only call Input::_Read() if this function was not called
               from CatIstream:Read() - in this case,
               istream_cat_read() would provide the loop.  This is
               advantageous because we avoid unnecessary recursing. */
            GetCurrent().Read(GetHandlerDirect());
        }
    }

    void OnInputError(gcc_unused Input &i, GError *error) {
        inputs.erase(inputs.iterator_to(i));
        CloseAllInputs();
        DestroyError(error);
    }

    /* virtual methods from class Istream */

    off_t _GetAvailable(bool partial) override;
    off_t _Skip(gcc_unused off_t length) override;
    void _Read() override;
    bool _FillBucketList(IstreamBucketList &list, GError **error_r) override;
    size_t _ConsumeBucketList(size_t nbytes) override;
    int _AsFd() override;
    void _Close() override;
};

/*
 * istream implementation
 *
 */

off_t
CatIstream::_GetAvailable(bool partial)
{
    off_t available = 0;

    for (const auto &input : inputs) {
        const off_t a = input.istream.GetAvailable(partial);
        if (a != (off_t)-1)
            available += a;
        else if (!partial)
            /* if the caller wants the exact number of bytes, and
               one input cannot provide it, we cannot provide it
               either */
            return (off_t)-1;
    }

    return available;
}

off_t
CatIstream::_Skip(off_t length)
{
    if (inputs.empty())
        return 0;

    off_t nbytes = inputs.front().istream.Skip(length);
    if (nbytes > 0)
        Consumed(nbytes);

    return nbytes;
}

void
CatIstream::_Read()
{
    if (IsEOF()) {
        DestroyEof();
        return;
    }

    const ScopePoolRef ref(GetPool() TRACE_ARGS);

    reading = true;

    CatIstream::InputList::const_iterator prev;
    do {
        prev = inputs.begin();
        GetCurrent().Read(GetHandlerDirect());
    } while (!IsEOF() && inputs.begin() != prev);

    reading = false;
}

bool
CatIstream::_FillBucketList(IstreamBucketList &list, GError **error_r)
{
    assert(!list.HasMore());

    for (auto &input : inputs) {
        if (!input.FillBucketList(list, error_r)) {
            inputs.erase(inputs.iterator_to(input));
            CloseAllInputs();
            Destroy();
            return false;
        }

        if (list.HasMore())
            break;
    }

    return true;
}

size_t
CatIstream::_ConsumeBucketList(size_t nbytes)
{
    size_t total = 0;

    for (auto &input : inputs) {
        size_t consumed = input.ConsumeBucketList(nbytes);
        Consumed(consumed);
        total += consumed;
        nbytes -= consumed;
        if (nbytes == 0)
            break;
    }

    return total;
}

int
CatIstream::_AsFd()
{
    /* we can safely forward the as_fd() call to our input if it's the
       last one */

    if (std::next(inputs.begin()) != inputs.end())
        /* not on last input */
        return -1;

    auto &i = GetCurrent();
    int fd = i.istream.AsFd();
    if (fd >= 0)
        Destroy();

    return fd;
}

void
CatIstream::_Close()
{
    CloseAllInputs();
    Destroy();
}

/*
 * constructor
 *
 */

inline CatIstream::CatIstream(struct pool &p, ConstBuffer<Istream *> _inputs)
    :Istream(p)
{
    auto i = inputs.before_begin();

    for (Istream *_input : _inputs) {
        if (_input == nullptr)
            continue;

        auto *input = NewFromPool<Input>(p, *this, *_input);
        i = inputs.insert_after(i, *input);
    }
}

Istream *
_istream_cat_new(struct pool &pool, Istream *const* inputs, unsigned n_inputs)
{
    return NewFromPool<CatIstream>(pool, pool,
                                   ConstBuffer<Istream *>(inputs, n_inputs));
}
