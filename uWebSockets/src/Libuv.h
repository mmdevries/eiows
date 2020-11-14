#ifndef LIBUV_H
#define LIBUV_H

#include <uv.h>
static_assert (UV_VERSION_MINOR >= 3, "µWebSockets requires libuv >=1.3.0");

namespace uS {
    struct Loop : uv_loop_t {
        static Loop *createLoop() {
            return static_cast<Loop *>(uv_default_loop());
        }
    };

    struct Async {
        uv_async_t uv_async;

        Async(Loop *loop) {
            uv_async.loop = loop;
        }

        void start(void (*cb)(Async *)) {
            uv_async_init(uv_async.loop, &uv_async, (uv_async_cb) cb);
        }

        void send() {
            uv_async_send(&uv_async);
        }

        void close() {
            uv_close((uv_handle_t *) &uv_async, [](uv_handle_t *a) {
                delete reinterpret_cast<Async *>(a);
            });
        }

        void setData(void *data) {
            uv_async.data = data;
        }

        void *getData() {
            return uv_async.data;
        }
    };

    struct Timer {
        uv_timer_t uv_timer;

        Timer(Loop *loop) {
            uv_timer_init(loop, &uv_timer);
        }

        void start(void (*cb)(Timer *), int first, int repeat) {
            uv_timer_start(&uv_timer, (uv_timer_cb) cb, first, repeat);
        }

        void setData(void *data) {
            uv_timer.data = data;
        }

        void *getData() {
            return uv_timer.data;
        }

        void stop() {
            uv_timer_stop(&uv_timer);
        }

        void close() {
            uv_close((uv_handle_t *) &uv_timer, [](uv_handle_t *t) {
                delete reinterpret_cast<Timer *>(t);
            });
        }
    private:
        ~Timer() {}
    };

    struct Poll {
        uv_poll_t *uv_poll;
        void (*cb)(Poll *p, int status, int events);

        Poll(Loop *loop, uv_os_sock_t fd) {
            uv_poll = new uv_poll_t;
            uv_poll_init_socket(loop, uv_poll, fd);
            cb = nullptr;
        }

        Poll(Poll &&other) {
            uv_poll = other.uv_poll;
            cb = other.cb;
            other.uv_poll = nullptr;
        }

        Poll(const Poll &other) = delete;

        ~Poll() {
            delete uv_poll;
        }

        bool isClosed() {
            return uv_is_closing(reinterpret_cast<uv_handle_t *>(uv_poll));
        }

        uv_os_sock_t getFd() const {
#ifdef _WIN32
            uv_os_sock_t fd;
            uv_fileno((uv_handle_t *) uv_poll, (uv_os_fd_t *) &fd);
            return fd;
#else
            return uv_poll->io_watcher.fd;
#endif
        }

        void setCb(void (*cb)(Poll *p, int status, int events)) {
            this->cb = cb;
        }

        void (*getCb())(Poll *, int, int) {
            return cb;
        }

        void start(Poll *self, int events) {
            uv_poll->data = self;
            uv_poll_start(uv_poll, events, [](uv_poll_t *p, int status, int events) {
                Poll *self = static_cast<Poll *>(p->data);
                self->cb(self, status, events);
            });
        }

        void change(Poll *self, int events) {
            start(self, events);
        }

        void stop() {
            uv_poll_stop(uv_poll);
        }

        void close(void (*cb)(Poll *)) {
            this->cb = (void(*)(Poll *, int, int)) cb;
            uv_close((uv_handle_t *) uv_poll, [](uv_handle_t *p) {
                Poll *poll = static_cast<Poll *>(p->data);
                void (*cb)(Poll *) = (void(*)(Poll *)) poll->cb;
                cb(poll);
            });
        }
    };
}

#endif // LIBUV_H
