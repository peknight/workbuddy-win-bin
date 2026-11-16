/*
 * workbuddy:// 转发器。
 *
 * 企业微信 5.0.13 起在跳转 WorkBuddy 前会做两步检查：
 *   1. 读 HKCR\workbuddy\shell\open\command 里的 exe 路径；
 *   2. 用 GetFileVersionInfoW 读这个 exe 的 FileVersion，和
 *      workbuddy_aibot_min_version_pc 比较。
 *
 * 早先直接把协议指向 winebrowser.exe，而 winebrowser 没有任何 VERSIONINFO
 * 资源，企业微信于是认为“装了但版本读不出来”，弹“版本过低”。这里用一个
 * 带版本资源的薄壳代替它：版本号和真实的 WorkBuddy.exe 保持一致，收到 URL
 * 后仍交给 winebrowser，由它转给宿主 xdg-open。
 */

#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>

#define URL_MAX 8192

static void show_error(const wchar_t *what)
{
	wchar_t text[512];

	wsprintfW(text, L"%s\n\nGetLastError = %lu", what, GetLastError());
	MessageBoxW(NULL, text, L"WorkBuddy protocol forwarder",
		    MB_ICONERROR | MB_OK);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev, PWSTR cmdline, int show)
{
	wchar_t system_dir[MAX_PATH];
	wchar_t browser[MAX_PATH];
	wchar_t command[URL_MAX + MAX_PATH];
	STARTUPINFOW startup = { .cb = sizeof(startup) };
	PROCESS_INFORMATION process;

	(void)instance;
	(void)prev;
	(void)cmdline;
	(void)show;

	if (GetSystemDirectoryW(system_dir, MAX_PATH) == 0) {
		show_error(L"GetSystemDirectoryW failed");
		return 2;
	}
	wsprintfW(browser, L"%s\\winebrowser.exe", system_dir);
	if (GetFileAttributesW(browser) == INVALID_FILE_ATTRIBUTES) {
		show_error(L"winebrowser.exe not found");
		return 3;
	}

	/*
	 * 只取第一个参数：deep link 本身不带空格，CommandLineToArgvW 会把
	 * 引号处理掉，剩下的原样转给 winebrowser。
	 */
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (argv == NULL || argc < 2) {
		show_error(L"no workbuddy:// URL given");
		if (argv != NULL) {
			LocalFree(argv);
		}
		return 4;
	}

	wsprintfW(command, L"\"%s\" \"%s\"", browser, argv[1]);
	LocalFree(argv);

	if (!CreateProcessW(browser, command, NULL, NULL, FALSE, 0, NULL, NULL,
			    &startup, &process)) {
		show_error(L"failed to start winebrowser.exe");
		return 5;
	}

	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return 0;
}
