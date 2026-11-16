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
 *
 * 版本检查之后企业微信还会调 WinVerifyTrust 校验这个 exe 的 Authenticode
 * 签名，未签名同样只弹一句“打开失败”（日志里的 invalid signature）。签字
 * 不在源码里做，由 PKGBUILD 在编完之后用一次性自签名证书补上。
 *
 * 每步都往 %TEMP%\workbuddy-protocol.log 追加一行，企业微信那边只弹一个
 * 笼统的“打开失败”，靠这个文件能区分是没被调用、没拿到参数，还是
 * winebrowser 起不来。
 */

#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <stdarg.h>
#include <string.h>

#define URL_MAX 8192

static void log_line(const wchar_t *format, ...)
{
	wchar_t temp_dir[MAX_PATH];
	wchar_t path[MAX_PATH];
	wchar_t line[1024];
	char utf8[2048];
	SYSTEMTIME now;
	HANDLE file;
	DWORD written;
	va_list args;

	if (GetTempPathW(MAX_PATH, temp_dir) == 0) {
		return;
	}
	wsprintfW(path, L"%s\\workbuddy-protocol.log", temp_dir);

	va_start(args, format);
	wvsprintfW(line, format, args);
	va_end(args);

	GetLocalTime(&now);

	/* 转换失败就整行丢掉，不写半截内容 */
	wchar_t stamped[1200];
	wsprintfW(stamped, L"%04u-%02u-%02u %02u:%02u:%02u [%lu] %s\r\n",
		  now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
		  now.wSecond, GetCurrentProcessId(), line);
	if (WideCharToMultiByte(CP_UTF8, 0, stamped, -1, utf8, sizeof(utf8),
				NULL, NULL) == 0) {
		return;
	}

	file = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
			   OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		return;
	}
	WriteFile(file, utf8, (DWORD)strlen(utf8), &written, NULL);
	CloseHandle(file);
}

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

	log_line(L"start, cmdline=%s", GetCommandLineW());

	if (GetSystemDirectoryW(system_dir, MAX_PATH) == 0) {
		log_line(L"GetSystemDirectoryW failed, err=%lu", GetLastError());
		show_error(L"GetSystemDirectoryW failed");
		return 2;
	}
	wsprintfW(browser, L"%s\\winebrowser.exe", system_dir);
	if (GetFileAttributesW(browser) == INVALID_FILE_ATTRIBUTES) {
		log_line(L"winebrowser not found at %s", browser);
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
		log_line(L"no URL to forward, argc=%d", argc);
		show_error(L"no workbuddy:// URL given");
		if (argv != NULL) {
			LocalFree(argv);
		}
		return 4;
	}

	wsprintfW(command, L"\"%s\" \"%s\"", browser, argv[1]);
	log_line(L"forwarding url=%s via %s", argv[1], browser);
	LocalFree(argv);

	if (!CreateProcessW(browser, command, NULL, NULL, FALSE, 0, NULL, NULL,
			    &startup, &process)) {
		log_line(L"CreateProcess failed, err=%lu", GetLastError());
		show_error(L"failed to start winebrowser.exe");
		return 5;
	}

	log_line(L"winebrowser started, pid=%lu", process.dwProcessId);
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return 0;
}
