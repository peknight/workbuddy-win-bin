/* 诊断用：向指定顶层窗口发 WM_IME_REQUEST/IMR_QUERYCHARPOSITION，
 * 打印应用返回的光标矩形（屏幕坐标）。用来确认 Chromium 是否真的响应
 * 这个请求，以及返回的位置是否与真实光标一致。 */
#include <windows.h>
#include <imm.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

int wmain( int argc, wchar_t **argv )
{
    HWND hwnd;
    IMECHARPOSITION pos;
    LRESULT ret;
    RECT rect;

    if (argc < 2)
    {
        wprintf( L"usage: %ls <hwnd-hex>\n", argv[0] );
        return 2;
    }
    hwnd = (HWND)(ULONG_PTR)wcstoull( argv[1], NULL, 16 );
    if (!IsWindow( hwnd ))
    {
        wprintf( L"hwnd %p is not a window\n", hwnd );
        return 1;
    }
    GetWindowRect( hwnd, &rect );
    wprintf( L"hwnd %p window_rect %ld,%ld-%ld,%ld\n", hwnd,
             rect.left, rect.top, rect.right, rect.bottom );

    memset( &pos, 0, sizeof(pos) );
    pos.dwSize = sizeof(pos);
    pos.dwCharPos = 0;
    ret = SendMessageW( hwnd, WM_IME_REQUEST, IMR_QUERYCHARPOSITION, (LPARAM)&pos );
    wprintf( L"IMR_QUERYCHARPOSITION ret %ld point %ld,%ld line_height %lu "
             L"char_pos %lu\n", ret, pos.pt.x, pos.pt.y,
             pos.cLineHeight, pos.dwCharPos );
    wprintf( L"screen %lux%lu\n", GetSystemMetrics( SM_CXSCREEN ),
             GetSystemMetrics( SM_CYSCREEN ));
    return ret ? 0 : 1;
}
