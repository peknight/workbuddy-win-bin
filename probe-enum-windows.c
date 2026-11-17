/* 诊断用：枚举当前 prefix 里的顶层/子窗口，打印 hwnd、类名、标题、
 * 位置和是否可见。用来确认 WorkBuddy 的 IME 目标是哪个 HWND。 */
#include <windows.h>
#include <stdio.h>
#include <wchar.h>

static BOOL CALLBACK enum_proc( HWND hwnd, LPARAM arg )
{
    WCHAR class_name[128], title[256];
    RECT rect;
    DWORD pid = 0;

    (void)arg;
    GetClassNameW( hwnd, class_name, ARRAYSIZE(class_name) );
    GetWindowTextW( hwnd, title, ARRAYSIZE(title) );
    GetWindowRect( hwnd, &rect );
    GetWindowThreadProcessId( hwnd, &pid );
    wprintf( L"%p pid %lu parent %p %s '%ls' [%ld,%ld-%ld,%ld] %s\n",
             hwnd, pid, GetParent(hwnd), class_name, title,
             rect.left, rect.top, rect.right, rect.bottom,
             IsWindowVisible(hwnd) ? L"visible" : L"hidden" );
    EnumChildWindows( hwnd, enum_proc, 0 );
    return TRUE;
}

int wmain( void )
{
    EnumWindows( enum_proc, 0 );
    return 0;
}
