#!/usr/bin/env bash
# WorkBuddy 国内版（Windows 客户端）启动器，跑在发行版自带的 wine 11 上。
# 程序本体是包里的只读文件（/usr/lib/workbuddy-win-bin/app），prefix 里只有用户数据。
set -euo pipefail

readonly PKGNAME='@PKGNAME@'
readonly PKGVER='@PKGVER@'
# 默认 prefix 名：$XDG_DATA_HOME/wineprefixes/<APP_ID>。
# WM_CLASS 不受它影响，上游 wine 固定取进程名 workbuddy.exe，见 PITFALLS.md。
readonly APP_ID='@APP_ID@'
readonly BIN='@BIN@'
readonly SHARE="/usr/lib/${PKGNAME}"
readonly APP_DIR="${SHARE}/app"
readonly WINE_BIN='/usr/bin/wine'
readonly FONTS_REG="${SHARE}/workbuddy-win-fonts.reg"
readonly EXPLORER_EXE="${SHARE}/explorer-systray-foreground.exe"

message() {
	printf '%s\n' "$*" >&2
}

usage() {
	cat <<EOF
用法: ${BIN} [子命令 | 启动参数...]

不带子命令即启动 WorkBuddy，首次运行会自动建 prefix 并打好字体补丁。
其余参数原样传给 WorkBuddy.exe，例如开远程调试端口:
  ${BIN} --remote-debugging-port=9222 --remote-allow-origins='*'

子命令:
  setup      建 prefix、补字体（prefix 已存在时只补缺失部分）
  fontfix    重新应用中文字体替换（界面出现方框时用）
  prefix     打印当前使用的 WINEPREFIX 路径
  help       显示本帮助

环境变量:
  WORKBUDDY_WIN_PREFIX   指定 prefix，默认 \$XDG_DATA_HOME/wineprefixes/${APP_ID}
EOF
}

resolve_prefix() {
	if [[ -n ${WORKBUDDY_WIN_PREFIX:-} ]]; then
		printf '%s\n' "$WORKBUDDY_WIN_PREFIX"
		return
	fi
	printf '%s\n' "${XDG_DATA_HOME:-$HOME/.local/share}/wineprefixes/${APP_ID}"
}

# 界面语言跟随系统 locale。Wine 的 Windows 用户区域设置（进而
# GetUserDefaultUILanguage）取自进程 locale，优先级 LC_ALL > LC_CTYPE > LANG；
# 纯 C / C.UTF-8 会被 Wine 当成 en-US，而工具会话常带这类值，所以遇到就跳过，
# 继续看下一个变量。三个都没给出明确区域时按英文处理。
# 注意 WorkBuddy 自己还会把显示语言记进 prefix 的 .workbuddy/app/app-config.json，
# 首次启动定下来的值不会因为换代终端而变；要换语言在应用设置里改。
detect_locale() {
	local candidate

	for candidate in "${LC_ALL:-}" "${LC_CTYPE:-}" "${LANG:-}"; do
		case "$candidate" in
		'' | C | C.UTF-8 | POSIX)
			continue
			;;
		esac
		printf '%s\n' "$candidate"
		return 0
	done
	printf '%s\n' 'en_US.UTF-8'
}

setup_wine_env() {
	export WINEPREFIX="$1"
	# 发行版 wine 自带全套 PE 库，别让环境里残留的 WINEDLLPATH 指到别处
	unset WINEDLLPATH
	# 界面语言跟随系统，三个变量写成同一个值，避免 LC_ALL 压过 LANG 造成
	# 从不同环境启动时语言来回变；具体取值见 detect_locale。
	local app_locale
	app_locale=$(detect_locale)
	export LANG="$app_locale"
	export LANGUAGE="${app_locale%%.*}"
	export LC_ALL="$app_locale"
	export WINEDEBUG='-all'
	# WorkBuddy 不碰 .NET。保持这条覆盖：新 prefix 的首个 wineboot 会弹
	# 「要不要装 wine-mono」的对话框，置空 mscoree/mshtml 直接跳过。
	# explorer.exe=n 让 wine 用包里打补丁的 explorer，托盘右键菜单才弹得出来，
	# 见 apply_explorer 和 PITFALLS.md 第 13 条。
	export WINEDLLOVERRIDES='mscoree,mshtml=;explorer.exe=n'

	# 托盘图标黑底：wine 会给托盘窗口设 bounding shape，Xfce panel 把它圈住的
	# 区域整块填黑。这个小库在 wine 进程里拦掉那次 shape 设置（只在有合成器时），
	# ShapeInput 不动，点击穿透照旧。见 PITFALLS.md。
	local noshape="/usr/lib/${PKGNAME}/no-shaped-systray.so"
	if [[ -f ${noshape} ]]; then
		export LD_PRELOAD="${noshape}${LD_PRELOAD:+:${LD_PRELOAD}}"
	fi
}

app_installed() {
	[[ -f "${APP_DIR}/WorkBuddy.exe" ]]
}

# GDI 那一路按家族名取 Tahoma，而 wine 自带的 tahoma.ttf 没有汉字；
# GDI 的 Replacements 又不覆盖已存在的家族，所以往 prefix 里放一份派生字体。
# 只在内容变了才写，避免每次启动都往 prefix 里拷 9M。
apply_fonts() {
	local win_fonts="${WINEPREFIX}/drive_c/windows/Fonts"
	local font changed=0

	for font in tahoma.ttf tahomabd.ttf; do
		if ! cmp -s "${SHARE}/fonts/${font}" "${win_fonts}/${font}"; then
			install -Dm644 "${SHARE}/fonts/${font}" "${win_fonts}/${font}"
			changed=1
		fi
	done

	if ((changed)); then
		"${WINE_BIN}" regedit /S "${FONTS_REG}"
		message "中文字体已应用（派生 Tahoma + 替换表 -> 文泉驿微米黑）"
	fi
}

# 托盘右键菜单弹不出来：wine 内置 explorer 把点击转发给应用时，这段输入落在
# explorer 自己的队列里，时间戳比应用的新；应用（Electron）随后调用的
# SetForegroundWindow 会被 wineserver 按 input 时间判为越权，直接放弃弹菜单。
# 包里这份 explorer.exe 在转发前先替应用抢一次前台，见 PITFALLS.md 第 13 条。
apply_explorer() {
	local win_explorer="${WINEPREFIX}/drive_c/windows/system32/explorer.exe"

	if [[ ! -f ${EXPLORER_EXE} ]]; then
		message "缺少 ${EXPLORER_EXE}，托盘右键菜单可能弹不出来"
		return 0
	fi
	if ! cmp -s "${EXPLORER_EXE}" "${win_explorer}"; then
		install -Dm755 "${EXPLORER_EXE}" "${win_explorer}"
		message "托盘菜单修复已应用（替换 prefix 里的 explorer.exe）"
	fi
}

ensure_prefix() {
	app_installed || {
		message "程序文件缺失：${APP_DIR}/WorkBuddy.exe，重装 ${PKGNAME} 后再试"
		exit 1
	}

	if [[ ! -d "${WINEPREFIX}/drive_c" ]]; then
		message "初始化 wine prefix: ${WINEPREFIX}"
		mkdir -p "${WINEPREFIX}"
		WINEDLLOVERRIDES='mscoree,mshtml=;explorer.exe=n' \
			"${WINE_BIN}" wineboot -u >/dev/null 2>&1 || true
	fi

	apply_fonts
	# wineboot / wine 升级可能把 explorer.exe 换回内置版，每次启动都对一遍
	apply_explorer
}

launch() {
	# Electron 的沙箱要 chrome-sandbox 带 setuid，而 /usr/lib 下这份是 root:root
	# 的普通文件，非 root 启动只能关掉沙箱（应用本身仍有 Win32 作业对象隔离）。
	# 用户显式传了 --no-sandbox 也照样透传，重复无所谓。
	cd "${APP_DIR}"
	exec "${WINE_BIN}" WorkBuddy.exe --no-sandbox "$@"
}

main() {
	case "${1:-}" in
	help | -h | --help)
		usage
		exit 0
		;;
	esac

	setup_wine_env "$(resolve_prefix)"

	case "${1:-}" in
	prefix)
		printf '%s\n' "${WINEPREFIX}"
		;;
	setup)
		ensure_prefix
		;;
	fontfix)
		apply_fonts
		;;
	*)
		# 其余参数原样透传，调试时要靠 --remote-debugging-port 之类
		ensure_prefix
		launch "$@"
		;;
	esac
}

main "$@"
