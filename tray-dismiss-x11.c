/*
 * 在 X11 侧记录鼠标按下位置和 Esc，供 tray-dismiss.exe 判断是否点在菜单外。
 *
 * wine 进程看不到其他 X 客户端上的按键状态。这里优先用 XInput2 的 raw 事件：
 * 它不抢输入、不需要 grab，按下沿由 X server 直接送出，不存在轮询漏掉短按的
 * 问题。事件里只带按键/键码，坐标在收到事件时用 XQueryPointer 现取（按下瞬间
 * 指针还没移动，足够准）。
 *
 * 没有 XInput2 的极端环境退回轮询 XQueryPointer / XQueryKeymap，功能不变，
 * 只是极快的点击（低于一个轮询周期）可能漏掉。
 */
#define _DEFAULT_SOURCE
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>
#include <X11/keysym.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define EVENT_MAGIC 0x57425445u
#define POLL_INTERVAL_US 10000
#define HEARTBEAT_TIMEOUT_SEC 5
#define STARTUP_GRACE_SEC 30
#define ESC_REPEAT_GUARD_MS 500

struct tray_event {
    uint32_t magic;
    uint32_t seq;
    int32_t x;
    int32_t y;
    uint32_t flags;
    uint32_t checksum;
};

struct recorder {
    Display *display;
    Window root;
    KeyCode escape_key;
    int events_fd;
    uint32_t seq;
    unsigned int previous_buttons;
    int previous_escape;
    Time last_button_time;
    Time last_escape_time;
};

static uint32_t event_checksum(const struct tray_event *event)
{
    const unsigned char *bytes = (const unsigned char *)event;
    uint32_t hash = 2166136261u;
    size_t i;

    for (i = 0; i < sizeof(*event) - sizeof(event->checksum); i++) {
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    return hash;
}

static int write_event(int fd, uint32_t seq, int32_t x, int32_t y,
                       uint32_t flags)
{
    struct tray_event event;
    ssize_t wrote;

    event.magic = EVENT_MAGIC;
    event.seq = seq;
    event.x = x;
    event.y = y;
    event.flags = flags;
    event.checksum = event_checksum(&event);

    wrote = pwrite(fd, &event, sizeof(event), 0);
    return wrote == (ssize_t)sizeof(event);
}

static int heartbeat_fresh(const char *path)
{
    struct stat st;

    if (stat(path, &st) != 0) {
        return 0;
    }
    return time(NULL) - st.st_mtime <= HEARTBEAT_TIMEOUT_SEC;
}

static void pointer_position(struct recorder *recorder, int32_t *x, int32_t *y)
{
    Window root_ret;
    Window child_ret;
    int win_x;
    int win_y;
    unsigned int mask;

    if (!XQueryPointer(recorder->display, recorder->root, &root_ret,
                       &child_ret, x, y, &win_x, &win_y, &mask)) {
        *x = 0;
        *y = 0;
    }
}

static void record_press(struct recorder *recorder)
{
    int32_t x;
    int32_t y;

    pointer_position(recorder, &x, &y);
    recorder->seq++;
    if (!write_event(recorder->events_fd, recorder->seq, x, y, 0)) {
        perror("write tray event");
    }
}

static void record_escape(struct recorder *recorder, Time event_time)
{
    int32_t x;
    int32_t y;

    /*
     * 按住 Esc 会触发自动重复。菜单只需要一次；多余的 Esc 可能在菜单关掉
     * 之后落到应用自己的控件上，所以同一段时间内只记一次。
     */
    if (recorder->last_escape_time != 0 &&
        event_time - recorder->last_escape_time < ESC_REPEAT_GUARD_MS) {
        return;
    }
    recorder->last_escape_time = event_time;
    pointer_position(recorder, &x, &y);
    recorder->seq++;
    if (!write_event(recorder->events_fd, recorder->seq, x, y, 1)) {
        perror("write tray event");
    }
}

static int setup_xi2(struct recorder *recorder)
{
    unsigned char mask[XIMaskLen(XI_LASTEVENT)];
    XIEventMask event_mask;
    int opcode;
    int event;
    int error;
    int major = 2;
    int minor = 0;

    if (!XQueryExtension(recorder->display, "XInputExtension", &opcode, &event,
                         &error)) {
        return 0;
    }
    if (XIQueryVersion(recorder->display, &major, &minor) != Success ||
        major < 2) {
        return 0;
    }

    memset(mask, 0, sizeof(mask));
    XISetMask(mask, XI_RawButtonPress);
    XISetMask(mask, XI_RawKeyPress);
    event_mask.deviceid = XIAllDevices;
    event_mask.mask_len = sizeof(mask);
    event_mask.mask = mask;
    XISelectEvents(recorder->display, recorder->root, &event_mask, 1);
    XSync(recorder->display, False);
    return 1;
}

static void handle_xi_event(struct recorder *recorder, XEvent *event,
                            int xi_opcode)
{
    XGenericEventCookie *cookie;
    XIRawEvent *raw;

    if (event->type != GenericEvent || event->xcookie.extension != xi_opcode) {
        return;
    }
    cookie = &event->xcookie;
    if (!XGetEventData(recorder->display, cookie)) {
        return;
    }
    raw = (XIRawEvent *)cookie->data;
    if (raw->evtype == XI_RawButtonPress) {
        /*
         * XIAllDevices 会把同一次物理按下按 master/slave 设备各报一遍，
         * 时间戳相同。只认第一次，否则 wine 侧会补两次点击。
         */
        if (recorder->last_button_time != 0 &&
            raw->time - recorder->last_button_time < 10) {
            XFreeEventData(recorder->display, cookie);
            return;
        }
        recorder->last_button_time = raw->time;
        record_press(recorder);
    } else if (raw->evtype == XI_RawKeyPress &&
               raw->detail == recorder->escape_key) {
        record_escape(recorder, raw->time);
    }
    XFreeEventData(recorder->display, cookie);
}

static void poll_fallback(struct recorder *recorder)
{
    int32_t x;
    int32_t y;
    Window root_ret;
    Window child_ret;
    int win_x;
    int win_y;
    unsigned int mask;
    unsigned int buttons;
    char keymap[32];
    int escape_down;

    if (!XQueryPointer(recorder->display, recorder->root, &root_ret,
                       &child_ret, &x, &y, &win_x, &win_y, &mask)) {
        return;
    }
    buttons = mask & (Button1Mask | Button2Mask | Button3Mask | Button4Mask |
                      Button5Mask);
    if (buttons && !recorder->previous_buttons) {
        recorder->seq++;
        if (!write_event(recorder->events_fd, recorder->seq, x, y, 0)) {
            perror("write tray event");
        }
    }
    recorder->previous_buttons = buttons;

    XQueryKeymap(recorder->display, keymap);
    escape_down =
        recorder->escape_key != 0 &&
        (((unsigned char)keymap[recorder->escape_key / 8]) &
         (1u << (recorder->escape_key % 8))) != 0;
    if (escape_down && !recorder->previous_escape) {
        recorder->seq++;
        if (!write_event(recorder->events_fd, recorder->seq, x, y, 1)) {
            perror("write tray event");
        }
    }
    recorder->previous_escape = escape_down;
}

int main(int argc, char **argv)
{
    const char *events_path;
    char *heartbeat_path;
    char *lock_path;
    struct tray_event event;
    struct recorder recorder;
    struct stat st;
    struct pollfd pfd;
    int xi_opcode = -1;
    int use_xi2;
    int lock_fd;
    time_t started;
    time_t last_heartbeat_check;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <events-file>\n", argv[0]);
        return 2;
    }
    memset(&recorder, 0, sizeof(recorder));
    events_path = argv[1];
    if (asprintf(&heartbeat_path, "%s.heartbeat", events_path) < 0 ||
        asprintf(&lock_path, "%s.lock", events_path) < 0) {
        perror("asprintf");
        return 1;
    }

    lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0) {
        perror(lock_path);
        return 1;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        return 0;
    }

    recorder.events_fd = open(events_path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (recorder.events_fd < 0) {
        perror(events_path);
        return 1;
    }
    if (fstat(recorder.events_fd, &st) == 0 &&
        st.st_size >= (off_t)sizeof(event) &&
        pread(recorder.events_fd, &event, sizeof(event), 0) ==
            (ssize_t)sizeof(event) &&
        event.magic == EVENT_MAGIC &&
        event.checksum == event_checksum(&event)) {
        recorder.seq = event.seq;
    } else {
        if (ftruncate(recorder.events_fd, sizeof(event)) != 0 ||
            !write_event(recorder.events_fd, 0, 0, 0, 0)) {
            perror("initialize tray event file");
            return 1;
        }
    }

    recorder.display = XOpenDisplay(NULL);
    if (!recorder.display) {
        fprintf(stderr, "cannot open X display\n");
        return 1;
    }
    recorder.root = DefaultRootWindow(recorder.display);
    recorder.escape_key = XKeysymToKeycode(recorder.display, XK_Escape);
    use_xi2 = setup_xi2(&recorder);
    if (use_xi2) {
        int event_base;
        int error_base;

        XQueryExtension(recorder.display, "XInputExtension", &xi_opcode,
                        &event_base, &error_base);
    }

    started = time(NULL);
    last_heartbeat_check = started;
    pfd.fd = ConnectionNumber(recorder.display);
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (;;) {
        time_t now;

        if (use_xi2) {
            while (XPending(recorder.display) > 0) {
                XEvent event;

                XNextEvent(recorder.display, &event);
                handle_xi_event(&recorder, &event, xi_opcode);
            }
            poll(&pfd, 1, 1000);
        } else {
            poll_fallback(&recorder);
            usleep(POLL_INTERVAL_US);
        }

        now = time(NULL);
        if (now != last_heartbeat_check) {
            last_heartbeat_check = now;
            /*
             * wine 侧的 helper 每秒写一次心跳。启动阶段先给 30 秒等它
             * 把 wineserver 拉起来，之后心跳缺失或过期就认为对方已经退出。
             */
            if (now - started > STARTUP_GRACE_SEC &&
                !heartbeat_fresh(heartbeat_path)) {
                break;
            }
        }
    }

    XCloseDisplay(recorder.display);
    close(recorder.events_fd);
    return 0;
}
