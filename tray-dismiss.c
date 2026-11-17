/*
 * WorkBuddy 托盘菜单的“点外面关掉”辅助进程，跑在同一个 wine prefix 里。
 *
 * Wine 下弹出菜单关不掉的原因见 PITFALLS.md 第 15 条：Electron 用 Chromium
 * 自绘菜单，弹出后应用自己又把前台还给托盘宿主窗口，菜单窗口于是失活；
 * 而 wine 的 mouse capture 只在目标进程自己的输入队列内生效（win32u 里没有
 * 对 XGrabPointer 的调用），点别的窗口根本进不到菜单。
 *
 * 点击检测不能放在 wine 里做：GetAsyncKeyState / WH_MOUSE_LL 都只看得到本
 * 进程自己的输入，收不到别的 X 客户端上的按键。所以由同包的 tray-dismiss-x11
 * 监听 X11 根指针，把按下位置写进一个小文件；本进程只负责枚举 Chromium 菜单
 * 窗口，判断点击是否落在菜单外，再向菜单窗口投递一个 Esc 结束菜单
 * 状态。外部点击本身已经由 X server 送到了目标窗口，这里不能再补鼠标点击：
 * 一是会把同一下点两遍，二是托盘面板这类区域不在 wine 桌面范围内，补的点击
 * 可能没有落点，菜单就会一直挂着。
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <tlhelp32.h>
#include <wchar.h>

#define EVENT_MAGIC 0x57425445u
#define POLL_INTERVAL_MS 30
#define IDLE_SCAN_INTERVAL_MS 500
#define PROCESS_CHECK_INTERVAL_MS 1000
#define MENU_SETTLE_MS 120
#define HEARTBEAT_INTERVAL_MS 1000
#define ORPHAN_TIMEOUT_MS 30000
#define STARTUP_TIMEOUT_MS 120000
#define MAX_MENUS 8

typedef struct {
    uint32_t magic;
    uint32_t seq;
    int32_t x;
    int32_t y;
    uint32_t flags;
    uint32_t checksum;
} tray_event;

static wchar_t event_path[MAX_PATH];
static wchar_t heartbeat_path[MAX_PATH];

static uint32_t event_checksum(const tray_event *event)
{
    const unsigned char *bytes = (const unsigned char *)event;
    uint32_t hash = 2166136261u;
    size_t i;

    for (i = 0; i < sizeof(*event) - sizeof(event->checksum); i++) {
        hash = (hash ^ bytes[i]) * 16777619u;
    }
    return hash;
}

static int read_event(tray_event *event)
{
    HANDLE file;
    DWORD got = 0;
    BOOL ok;

    file = CreateFileW(event_path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0;
    }
    ok = ReadFile(file, event, sizeof(*event), &got, NULL);
    CloseHandle(file);

    return ok && got == sizeof(*event) && event->magic == EVENT_MAGIC &&
           event->checksum == event_checksum(event);
}

static void write_heartbeat(void)
{
    HANDLE file;
    DWORD wrote = 0;
    uint32_t tick = GetTickCount();

    file = CreateFileW(heartbeat_path, GENERIC_WRITE,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    WriteFile(file, &tick, sizeof(tick), &wrote, NULL);
    CloseHandle(file);
}

static int is_menu_popup(HWND hwnd)
{
    char cls[64] = {0};
    RECT rect;
    DWORD style;

    if (!IsWindowVisible(hwnd)) {
        return 0;
    }
    style = (DWORD)GetWindowLongW(hwnd, GWL_STYLE);
    if (!(style & WS_POPUP) || (style & (WS_CAPTION | WS_THICKFRAME))) {
        return 0;
    }
    if (GetWindowTextLengthW(hwnd) != 0) {
        return 0;
    }
    GetClassNameA(hwnd, cls, sizeof(cls));
    if (strncmp(cls, "Chrome_WidgetWin_", 17) != 0) {
        return 0;
    }
    if (!GetWindowRect(hwnd, &rect)) {
        return 0;
    }
    if (rect.right - rect.left < 16 || rect.bottom - rect.top < 16) {
        return 0;
    }
    return (rect.right - rect.left) <= 1600 && (rect.bottom - rect.top) <= 1200;
}

struct menu_scan {
    HWND windows[MAX_MENUS];
    RECT rects[MAX_MENUS];
    int count;
};

static BOOL CALLBACK find_menu(HWND hwnd, LPARAM lp)
{
    struct menu_scan *scan = (struct menu_scan *)lp;

    if (scan->count >= MAX_MENUS || !is_menu_popup(hwnd)) {
        return TRUE;
    }
    if (!GetWindowRect(hwnd, &scan->rects[scan->count])) {
        return TRUE;
    }
    scan->windows[scan->count] = hwnd;
    scan->count++;
    return TRUE;
}

static int point_in_menu(const struct menu_scan *scan, int32_t x, int32_t y)
{
    int i;

    for (i = 0; i < scan->count; i++) {
        const RECT *rect = &scan->rects[i];
        if (x >= rect->left && x < rect->right &&
            y >= rect->top && y < rect->bottom) {
            return 1;
        }
    }
    return 0;
}

static int same_menu_set(const struct menu_scan *left,
                         const struct menu_scan *right)
{
    int i;
    int j;

    if (left->count != right->count) {
        return 0;
    }
    for (i = 0; i < left->count; i++) {
        int found = 0;
        for (j = 0; j < right->count; j++) {
            if (left->windows[i] == right->windows[j]) {
                found = 1;
                break;
            }
        }
        if (!found) {
            return 0;
        }
    }
    return 1;
}

/*
 * 菜单只能靠按键消息结束。WM_CANCELMODE 或鼠标消息能把原生窗口收掉，但
 * views::MenuController 还停在运行状态，托盘下一次 popUpContextMenu 会被
 * 忽略，表现就是“关掉以后右键没反应”。
 *
 * 这里直接往枚举到的菜单窗口投递 Esc：点击面板之类别的 X 客户端后 wine
 * 已经失去焦点，SendInput 补的键没有接收窗口，菜单要等很久才被别的路径收掉；
 * PostMessage 不依赖焦点，按下沿就能走 Chromium 自己的取消路径，窗口和状态
 * 一起清干净，紧接着右键就能重开。
 */
static int cancel_menu_by_escape(const struct menu_scan *scan)
{
    UINT scancode = MapVirtualKeyW(VK_ESCAPE, MAPVK_VK_TO_VSC);
    LPARAM key_down = 1 | ((LPARAM)scancode << 16);
    int i;
    int sent = 0;

    for (i = 0; i < scan->count; i++) {
        if (PostMessageW(scan->windows[i], WM_KEYDOWN, VK_ESCAPE, key_down)) {
            sent++;
        }
    }
    return sent > 0;
}

static int workbuddy_running(void)
{
    PROCESSENTRY32W entry;
    HANDLE snap;
    int found = 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return 1;
    }
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"WorkBuddy.exe") == 0) {
                found = 1;
                break;
            }
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
    return found;
}

int wmain(int argc, wchar_t **argv)
{
    struct menu_scan previous = {0};
    DWORD idle_since = 0;
    DWORD last_heartbeat = 0;
    DWORD last_process_check = 0;
    DWORD last_scan = 0;
    uint32_t last_seq = 0;
    int menu_seen = 0;
    int seen_workbuddy = 0;
    DWORD menu_started = 0;
    tray_event event;

    if (argc != 2) {
        fwprintf(stderr, L"usage: %ls <events-file>\n", argv[0]);
        return 2;
    }
    wcsncpy(event_path, argv[1], MAX_PATH - 1);
    _snwprintf(heartbeat_path, MAX_PATH, L"%ls.heartbeat", event_path);

    CreateMutexW(NULL, FALSE, L"Local\\WorkBuddyTrayDismiss");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        return 0;
    }

    for (;;) {
        struct menu_scan scan;
        DWORD now = GetTickCount();
        int have_event;

        have_event = read_event(&event) && event.seq != last_seq;
        /*
         * 有事件时每个循环都扫描，保证“点外面”尽快生效；没有事件时降到
         * 4Hz，只用来发现菜单出现/消失，避免常驻进程空转。
         */
        if (have_event || now - last_scan >= IDLE_SCAN_INTERVAL_MS) {
            last_scan = now;
            scan.count = 0;
            EnumWindows(find_menu, (LPARAM)&scan);

            if (scan.count > 0) {
                if (!menu_seen || !same_menu_set(&scan, &previous)) {
                    /*
                     * 弹出菜单的那一下右键也会被 X11 记录，菜单出现在它之后。
                     * 新菜单出现时以当前序号为基线，只处理后续点击。按窗口
                     * 句柄判断是不是同一轮菜单，避免快速重开时误用旧状态。
                     * 再留一个很短的静默期，兜住事件和菜单出现的先后竞态。
                     */
                    menu_seen = 1;
                    menu_started = now;
                } else if (have_event &&
                           now - menu_started >= MENU_SETTLE_MS &&
                           ((event.flags & 1u) ||
                            !point_in_menu(&scan, event.x, event.y))) {
                    cancel_menu_by_escape(&scan);
                }
            } else {
                menu_seen = 0;
            }
            previous = scan;
            if (have_event) {
                last_seq = event.seq;
            }
        }

        if (now - last_heartbeat >= HEARTBEAT_INTERVAL_MS) {
            write_heartbeat();
            last_heartbeat = now;
        }

        if (now - last_process_check >= PROCESS_CHECK_INTERVAL_MS) {
            last_process_check = now;
            if (workbuddy_running()) {
                seen_workbuddy = 1;
                idle_since = 0;
            } else if (!idle_since) {
                idle_since = now;
            } else if (now - idle_since >
                       (seen_workbuddy ? ORPHAN_TIMEOUT_MS :
                                         STARTUP_TIMEOUT_MS)) {
                return 0;
            }
        }
        Sleep(POLL_INTERVAL_MS);
    }
}
