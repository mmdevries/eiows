#ifndef LIBUV_EIOWS_H
#define LIBUV_EIOWS_H

#include <uv.h>
static_assert (UV_VERSION_MINOR >= 3, "EIOWS requires libuv >=1.3.0");

namespace uS {
    struct Loop : uv_loop_t {
        static Loop *createLoop() {
            return static_cast<Loop *>(uv_default_loop());
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
            uv_close(reinterpret_cast<uv_handle_t *>(&uv_timer), [](uv_handle_t *t) {
                delete reinterpret_cast<Timer *>(t);
            });
        }
    private:
        ~Timer() {}
    };

    struct Poll {
        uv_poll_t *uv_poll;
        void (*cb)(Poll *p, int status, int events) = nullptr;
        void (*closeCb)(Poll *p) = nullptr;

        Poll(Loop *loop, uv_os_sock_t fd) {
            uv_poll = new uv_poll_t;
            uv_poll_init_socket(loop, uv_poll, fd);
        }

        Poll(Poll &&other) {
            uv_poll = other.uv_poll;
            cb = other.cb;
            closeCb = other.closeCb;
            other.uv_poll = nullptr;
            other.cb = nullptr;
            other.closeCb = nullptr;
        }

        Poll(const Poll &other) = delete;

        ~Poll() {
            delete uv_poll;
        }

        bool isClosed() {
            return uv_is_closing(reinterpret_cast<uv_handle_t *>(uv_poll));
        }

        uv_os_sock_t getFd() const {
            return uv_poll->io_watcher.fd;
        }

        void setCb(void (*cb)(Poll *p, int status, int events)) {
            this->cb = cb;
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

        void close(Poll *self, void (*cb)(Poll *)) {
            closeCb = cb;
            uv_poll->data = self;
            uv_close(reinterpret_cast<uv_handle_t *>(uv_poll), [](uv_handle_t *p) {
                Poll *poll = static_cast<Poll *>(p->data);
                poll->closeCb(poll);
            });
        }
    };
}

#endif // LIBUV_EIOWS_H
