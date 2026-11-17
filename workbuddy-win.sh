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
readonly TRAY_DISMISS_EXE="${SHARE}/tray-dismiss.exe"
readonly TRAY_DISMISS_X11="${SHARE}/tray-dismiss-x11"
readonly WINE_XIM_SO="${SHARE}/winex11-xim-spot.so"
readonly WINE_XIM_VERSION="${SHARE}/winex11-xim-spot.wine-version"

# 实际用来跑 wine 的命令。默认就是 wine 本身，setup_wine_xim_spot 判断能用
# 补丁版 winex11.so 时会换成 bwrap 包一层，见该函数和 PITFALLS.md 第 17 条。
WINE_LAUNCH=("${WINE_BIN}")

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

# 不要用 systemd-run 的默认输出接法（journal socket）启动：Wine 里 Electron 的
# main 进程会拿 stdout/stderr 的 fd 再 open 一次，socket 和目录都会在
# node:net 的 createWritableStdioStream 里抛 EISDIR，弹出「A JavaScript error
# occurred in the main process」。这类 fd 先换成普通日志文件，见 PITFALLS.md 第 14 条。
normalize_stdio() {
	local kind state_dir

	kind=$(stat -Lc %F /proc/self/fd/1 2>/dev/null || true)
	case "$kind" in
	socket | fifo | directory) ;;
	*) return 0 ;;
	esac

	state_dir="${XDG_STATE_HOME:-$HOME/.local/state}/${PKGNAME}"
	mkdir -p "$state_dir"
	exec >>"${state_dir}/launch.log" 2>&1
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
	# Wine 的 imm32 只有在设置了 XMODIFIERS 时才会去连宿主 XIM 服务器；
	# 不设的话进程里虽然 LANG 正常，输入法窗口根本不出现，只能打英文。
	# 这里只认 ibus 的 fcitx 类变量没用，XIM 变量是 Win32 输入链路的开关。
	# 用户自己带值时保留，方便换 fcitx5 的 @im=fcitx。见 PITFALLS.md 第 16 条。
	export XMODIFIERS="${XMODIFIERS:-@im=ibus}"
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

	setup_wine_xim_spot
}

# 输入法候选框贴在窗口左下角：wine 默认用 XIMPreeditCallbacks 风格建 XIC，而
# winex11.drv 的 SetIMECompositionRect 只在 XIMPreeditPosition 风格下才把光标
# 位置（XNSpotLocation）转发给输入法，callbacks 风格直接丢弃；ibus 拿不到位置就
# 退回「焦点窗口左下角」。包里这份 winex11.so 除了放宽这处判断，还会在组字时
# 主动向应用查 IMR_QUERYCHARPOSITION（Chromium 138 只走这条路报位置），其余与
# 发行版一致，见 PITFALLS.md 第 17 条。
#
# wine 的 unix 库按「发行版库目录 → WINEDLLPATH」的顺序找，WINEDLLPATH 覆盖不了
# 系统目录，所以用 bwrap 起一个只给自己用的挂载命名空间，把这份文件单点挂到系统
# 库路径上，/usr/lib 下的真文件不动。wine 版本对不上（unix 库 ABI 会变）时退回
# 原版，输入法本身仍然可用，只是候选框位置不对。
find_wine_lib_dir() {
	local dir

	for dir in /usr/lib/wine /usr/lib64/wine /usr/lib/x86_64-linux-gnu/wine \
		/opt/wine-stable/lib/wine /opt/wine-devel/lib/wine /opt/wine-staging/lib/wine; do
		if [[ -f "${dir}/x86_64-unix/ntdll.so" ]]; then
			printf '%s\n' "${dir}"
			return 0
		fi
	done
	return 1
}

setup_wine_xim_spot() {
	local lib_dir target wine_version want_version

	if [[ ! -f ${WINE_XIM_SO} ]]; then
		return 0
	fi
	if ! lib_dir=$(find_wine_lib_dir); then
		message "找不到 wine 的 unix 库目录，输入法候选框会贴在窗口左下角"
		return 0
	fi
	target="${lib_dir}/x86_64-unix/winex11.so"

	wine_version=$("${WINE_BIN}" --version 2>/dev/null || true)
	want_version=$(cat "${WINE_XIM_VERSION}" 2>/dev/null || true)
	if [[ -z ${want_version} || ${wine_version} != "${want_version}" ]]; then
		message "wine 是 ${wine_version:-未知}、补丁版 winex11.so 是 ${want_version:-未知}，候选框会贴在窗口左下角"
		return 0
	fi
	if ! command -v bwrap >/dev/null; then
		message "缺少 bubblewrap，输入法候选框会贴在窗口左下角"
		return 0
	fi
	# 用户命名空间被禁时 bwrap 会直接失败，先探一次，别等到启动才炸
	if ! bwrap --dev-bind / / true >/dev/null 2>&1; then
		message "bwrap 起不来（用户命名空间受限），输入法候选框会贴在窗口左下角"
		return 0
	fi

	WINE_LAUNCH=(bwrap --dev-bind / / --ro-bind "${WINE_XIM_SO}" "${target}" "${WINE_BIN}")
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
		"${WINE_LAUNCH[@]}" regedit /S "${FONTS_REG}"
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

# 托盘右键菜单弹出后，Electron 会把前台还给托盘宿主，菜单窗口失活；Wine 的
# mouse capture 又只覆盖同一个进程，点别的窗口时 Chromium 收不到取消事件。
# 这里拉起一对 helper：X11 侧记录根窗口上的按下位置，Wine 侧发现点击落在
# Chromium 菜单外就发 Esc 取消。两者都用单实例锁，应用重启时继续复用。
start_tray_dismiss() {
	local runtime events win_events

	if [[ ! -x ${TRAY_DISMISS_X11} || ! -f ${TRAY_DISMISS_EXE} ]]; then
		message "缺少托盘菜单辅助程序，菜单可能点在别处也不会消失"
		return 0
	fi

	runtime="${XDG_RUNTIME_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}}/${PKGNAME}"
	mkdir -p -m 700 "$runtime" 2>/dev/null || true
	events="${runtime}/tray-events.bin"
	# Wine 把 Z: 映射到宿主根目录，路径分隔符要换成反斜杠
	win_events="Z:${events//\//\\}"

	# helper 不需要托盘 shape 修复，别让它继承 LD_PRELOAD 里的 X11 拦截库
	env -u LD_PRELOAD setsid "${TRAY_DISMISS_X11}" "$events" >/dev/null 2>&1 &
	env -u LD_PRELOAD setsid "${WINE_LAUNCH[@]}" "${TRAY_DISMISS_EXE}" \
		"$win_events" >/dev/null 2>&1 &
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
			"${WINE_LAUNCH[@]}" wineboot -u >/dev/null 2>&1 || true
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
	start_tray_dismiss
	exec "${WINE_LAUNCH[@]}" WorkBuddy.exe --no-sandbox "$@"
}

main() {
	case "${1:-}" in
	help | -h | --help)
		usage
		exit 0
		;;
	esac

	normalize_stdio
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
