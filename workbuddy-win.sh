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
# wine-fix 的包装器：在独立的挂载命名空间里换上补丁版 winex11 / imm32 /
# explorer，系统 wine 和 prefix 都不动。见 wine-fix 仓库。
readonly WINE_BIN='/usr/bin/wine-fix-run'
readonly FONTS_REG="${SHARE}/workbuddy-win-fonts.reg"
readonly TRAY_DISMISS_EXE="${SHARE}/tray-dismiss.exe"
readonly TRAY_DISMISS_X11="${SHARE}/tray-dismiss-x11"
readonly BASH_BRIDGE="${SHARE}/wb-bash-bridge.py"
readonly BASH_PROXY="${SHARE}/wb-bash-proxy.exe"

# 实际用来跑 wine 的命令。wine-fix-run 版本对不上时会自己退回原版 wine。
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
  bridge     启动 Linux 工具链桥并打印端口
  help       显示本帮助

环境变量:
  WORKBUDDY_WIN_PREFIX   指定 prefix，默认 \$XDG_DATA_HOME/wineprefixes/${APP_ID}
  WORKBUDDY_NO_LINUX_BASH=1  关闭 Bash 工具的 Linux 工具链桥
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
	# explorer 由 wine-fix 挂上打补丁的 native 版本（没有 builtin 标记，按
	# native 加载即可），这里不用再写 explorer.exe=n。
	export WINEDLLOVERRIDES='mscoree,mshtml='

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
		"${WINE_LAUNCH[@]}" regedit /S "${FONTS_REG}"
		message "中文字体已应用（派生 Tahoma + 替换表 -> 文泉驿微米黑）"
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

# WorkBuddy 自带的 PortableGit 在 Wine 下 fork 不出外部进程，Bash 工具只能跑
# shell 内建。这里起一个宿主侧守护进程，把 wb-bash-proxy.exe 收到的脚本交给
# 宿主的登录 shell 执行。详见 PITFALLS.md 第 19 条。
read_bridge_state() {
	local key value

	BRIDGE_PID=''
	BRIDGE_PORT=''
	BRIDGE_TOKEN=''
	[[ -r ${1} ]] || return 0
	while IFS='=' read -r key value; do
		case "$key" in
		pid) BRIDGE_PID="$value" ;;
		port) BRIDGE_PORT="$value" ;;
		token) BRIDGE_TOKEN="$value" ;;
		esac
	done <"$1"
}

start_bash_bridge() {
	local runtime state cmdline i

	if [[ ${WORKBUDDY_NO_LINUX_BASH:-0} == 1 ]]; then
		return 0
	fi
	# 用户自己指定了 Git Bash 时尊重原设置，不偷偷换掉。
	if [[ -n ${CODEBUDDY_CODE_GIT_BASH_PATH:-} ]]; then
		return 0
	fi
	if [[ ! -r ${BASH_BRIDGE} || ! -f ${BASH_PROXY} ]]; then
		message "缺少 Linux 工具链桥文件，Bash 工具将使用自带 Git Bash"
		return 0
	fi
	if ! command -v python3 >/dev/null 2>&1; then
		message "缺少 python3，无法启动 Linux 工具链桥"
		return 0
	fi

	runtime="${XDG_RUNTIME_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}}/${PKGNAME}"
	mkdir -p -m 700 "$runtime" 2>/dev/null || true
	state="${runtime}/bash-bridge.state"

	read_bridge_state "$state"
	if [[ -n ${BRIDGE_PID} && -n ${BRIDGE_PORT} && -n ${BRIDGE_TOKEN} ]] &&
		kill -0 "${BRIDGE_PID}" 2>/dev/null; then
		cmdline=$(tr '\0' ' ' <"/proc/${BRIDGE_PID}/cmdline" 2>/dev/null || true)
		if [[ ${cmdline} == *wb-bash-bridge.py* ]]; then
			export WORKBUDDY_BASH_BRIDGE_PORT="${BRIDGE_PORT}"
			export WORKBUDDY_BASH_BRIDGE_TOKEN="${BRIDGE_TOKEN}"
			export CODEBUDDY_CODE_GIT_BASH_PATH="Z:${BASH_PROXY//\//\\}"
			return 0
		fi
	fi

	# 状态文件只记录本启动器自己的守护进程，旧文件直接作废。
	rm -f "$state"
	# bridge 不需要 wine 的 LD_PRELOAD / prefix 变量；它跑在宿主上。
	env -u LD_PRELOAD -u WINEPREFIX -u WINEDEBUG -u WINEDLLOVERRIDES -u XMODIFIERS \
		setsid python3 "$BASH_BRIDGE" --state-file "$state" --idle-timeout 1800 \
		>>"${runtime}/bash-bridge.log" 2>&1 &

	for i in {1..100}; do
		[[ -s ${state} ]] && break
		sleep 0.05
	done
	read_bridge_state "$state"
	if [[ -z ${BRIDGE_PID} || -z ${BRIDGE_PORT} || -z ${BRIDGE_TOKEN} ]] ||
		! kill -0 "${BRIDGE_PID}" 2>/dev/null; then
		message "Linux 工具链桥启动失败，详见 ${runtime}/bash-bridge.log"
		return 0
	fi

	export WORKBUDDY_BASH_BRIDGE_PORT="${BRIDGE_PORT}"
	export WORKBUDDY_BASH_BRIDGE_TOKEN="${BRIDGE_TOKEN}"
	export CODEBUDDY_CODE_GIT_BASH_PATH="Z:${BASH_PROXY//\//\\}"
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
}

# WorkBuddy 的 Bash 工具默认套一层 tsbx.dll 沙箱（LsSDK_CreateSandbox +
# LsInject 注入 x64 进程）。Wine 下注入必然失败，命令还没启动就报
# SandboxError 0x6，所以新 prefix 默认把安全中心里的这个开关关掉。
# 想保留沙箱设 WORKBUDDY_SANDBOX=1（当前 Wine 版本下不可用）。
ensure_sandbox_off() {
	local app_dir config

	if [[ ${WORKBUDDY_SANDBOX:-0} == 1 ]]; then
		return 0
	fi
	app_dir="${WINEPREFIX}/drive_c/users/$(id -un)/.workbuddy/app"
	config="${app_dir}/app-config.json"
	mkdir -p "$app_dir"
	python3 - "$config" <<'PY'
import json
import os
import sys

path = sys.argv[1]
try:
    with open(path, encoding="utf-8") as handle:
        data = json.load(handle)
except FileNotFoundError:
    data = {}
except (OSError, ValueError):
    # 文件损坏时交给应用自己重建，别把它覆盖成只剩一个键。
    sys.exit(0)

if not isinstance(data, dict) or "sandboxSafetyEnabled" in data:
    sys.exit(0)

data["sandboxSafetyEnabled"] = False
tmp = path + ".tmp"
with open(tmp, "w", encoding="utf-8") as handle:
    json.dump(data, handle, ensure_ascii=False, indent=2)
    handle.write("\n")
os.replace(tmp, path)
PY
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

	# bridge/prefix 是给脚本读输出的，stdout 被重定向时也要保持原样。
	case "${1:-}" in
	bridge | prefix) ;;
	*) normalize_stdio ;;
	esac

	case "${1:-}" in
	bridge)
		start_bash_bridge
		printf 'port=%s\n' "${WORKBUDDY_BASH_BRIDGE_PORT:-}"
		printf 'state=%s\n' "${XDG_RUNTIME_DIR:-${XDG_STATE_HOME:-$HOME/.local/state}}/${PKGNAME}/bash-bridge.state"
		;;
	prefix)
		setup_wine_env "$(resolve_prefix)"
		printf '%s\n' "${WINEPREFIX}"
		;;
	setup)
		setup_wine_env "$(resolve_prefix)"
		ensure_prefix
		ensure_sandbox_off
		;;
	fontfix)
		setup_wine_env "$(resolve_prefix)"
		apply_fonts
		;;
	*)
		# 其余参数原样透传，调试时要靠 --remote-debugging-port 之类
		start_bash_bridge
		setup_wine_env "$(resolve_prefix)"
		ensure_prefix
		ensure_sandbox_off
		launch "$@"
		;;
	esac
}

main "$@"
