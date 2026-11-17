# WorkBuddy on Wine 11 踩坑记录

## 1. 国内版和国际版不是同一个产品

- 国内版：`WorkBuddy.exe`，数据目录 `.workbuddy`，域名
  `copilot.tencent.com` / `www.workbuddy.cn`；
- 国际版：`WorkBuddyAI.exe`，数据目录 `.workbuddy-ai`，域名
  `workbuddy.ai`。

企业账号和企业微信入口只在国内版上通。下载时认准
`WorkBuddy-win32-x64-user-<version>-<build>.exe`，国际版的文件名带 `AI`。

## 2. NSIS 安装包要拆两层

`7z x` 官方 exe 只得到 NSIS 外壳，真正的程序在
`$PLUGINSDIR/app-64.7z`。再解这一层才是 `app/`。另外安装器会把
`resources/` 下两个 mp4 加载动画直接铺进安装目录，解包时也要合并进
`app/resources/`，否则部分界面会缺动画。

## 3. 必须加 `--no-sandbox`

Electron 在 Wine 下开着 Chromium sandbox 会起不来（或子进程反复崩）。
启动器固定传 `--no-sandbox`。

## 4. `mscoree,mshtml` 要禁用

程序会探测 .NET / IE 组件，Wine 自带的 `mscoree`、`mshtml` 会把它带进
多余的安装流程甚至崩溃。启动器设置：

```bash
WINEDLLOVERRIDES='mscoree,mshtml='
```

## 5. GDI 那一路的汉字要单独喂

Electron 界面走 DirectWrite，能落到 Wine 的 Noto Sans CJK 回退链；但部分
原生控件按家族名取 Tahoma，而 Wine 自带的 `tahoma.ttf` 没有汉字。
`make-tahoma.py` 从文泉驿微米黑派生 `tahoma.ttf` / `tahomabd.ttf`，装进
prefix 的 `C:\windows\Fonts`，再应用替换表。启动器只在文件变化时覆盖。

## 6. 托盘图标黑底加边框

Wine 会给托盘里的 XEMBED 窗口设 bounding shape（实测 22x22 窗口有 9 个
矩形，其中还有越界的 `[22,0 106x3]`），GTK 状态栏按 shape 画出来就是
一块黑底加边框。

`no-shaped-systray.c` 编成 `.so` 后由启动器 `LD_PRELOAD`。它只处理同时
满足以下条件的窗口，避免误伤正常窗口：

- compositor 正在运行；
- 窗口 depth 是 32；
- 宽高都不超过 64；
- 带 `_XEMBED_INFO`，或者已被 reparent 到托盘。

只 `XShapeCombineMask(..., ShapeBounding, ..., None, ...)`，不碰
`ShapeInput`，也不设空矩形（空矩形会让窗口点不到）。

## 7. 企业微信提示“WorkBuddy 版本过低”

**现象**：WorkBuddy 已经是最新版，企业微信机器人会话里点跳转登录仍弹
“版本过低”。

**原因**：企业微信 5.0.13 的 `WorkBuddyLauncher` 会：

1. 读 `HKCR\workbuddy\shell\open\command` 里的 exe 路径；
2. 用 `GetFileVersionInfoW` 读它的 `FileVersion`；
3. 和系统配置 `workbuddy_aibot_min_version_pc` 比较。

协议如果直接指向 `winebrowser.exe`，这个纯 Wine 程序没有 VERSIONINFO，
企业微信就得到“路径存在、版本读取失败”，走“版本过低”而不是“未安装”。

**解法**：本包提供 `workbuddy-protocol.exe`，构建期从 `pkgver` 生成
`FileVersion` / `ProductVersion`，行为和 winebrowser 一样（转发 URL 给
`winebrowser.exe`）。`wecom-win-bin` 的协议注册指向它。WorkBuddy 没装时
路径不存在，企业微信会正确显示“未安装”。

## 8. 版本门槛过了，还会因“签名者名字不对”报 Failed to open WorkBuddy

**现象**：版本误判修好后，点跳转登录不再说“版本过低”，改成
`Failed to open WorkBuddy. Please try again later.`，转发器一次都没被调用
（`%TEMP%\workbuddy-protocol.log` 里没有新行）。

**原因**：`WorkBuddyLauncher` 在 `ShellExecute` 之前还有一道校验，不是普通的
信任链校验：

1. `CryptQueryObject` 打开 exe 的 Authenticode 签名，取出签名者证书；
2. `CertGetNameStringW(..., CERT_NAME_SIMPLE_DISPLAY_TYPE, ...)` 取证书 **CN**；
3. 与 `WXWork.exe` 内置白名单比对，只认
   `Tencent Technology (Shenzhen) Company Limited` 和
   `Tencent Technology(Shenzhen) Company Limited`；
4. 不匹配就走 `invalid signature` 分支，界面上只留一句笼统的打开失败。

关键在于 Wine 的 `wintrust` 只核对签名结构和摘要，**不做信任链校验**，自签名
同样返回 0。所以“签名能通过校验”并不够，证书主题必须与白名单同名。

**解法**：`PKGBUILD` 在 `build()` 里用 openssl 现生成一次性 CA + leaf，leaf
主题逐字段照抄腾讯证书（含 `jurisdictionC/ST/L`、`businessCategory`、
`serialNumber`、`O`、`CN`），再用 `jsign` 做 Authenticode 签名。私钥和证书
只留在 `$srcdir`，不进包也不进仓库。

验证要点（`WINEDEBUG='+crypt'`）：修复前日志是
`rdn_value_to_strW returning 19 (L"workbuddy-protocol")`，修复后是
`returning 46 (L"Tencent Technology (Shenzhen) Company Limited")`，紧接着
转发器日志出现真实 mission URL。腾讯证书原文可从 `WXWork.exe` 里那段 DER
证书直接 `openssl x509 -inform der -noout -subject` 读出。

这是本地兼容用的自签证书，只因为 Wine 不校验信任链才成立；拿到真实 Windows
上不会被信任，也不应该那样用。

## 9. desktop 的 `Exec` 要用绝对路径

`Exec=workbuddy-win %u` 会按 `PATH` 解析，`~/.local/bin` 里如果还有旧的
同名启动器就会截胡，`workbuddy://` 被送进另一套安装。写成
`Exec=/usr/bin/workbuddy-win %u`。

同时必须声明：

```
MimeType=x-scheme-handler/workbuddy;
```

否则 `xdg-open workbuddy://...` 找不到宿主处理程序。

## 10. `WM_CLASS` 只能是 `workbuddy.exe`

上游 wine 按进程名生成 `WM_CLASS`，不认 `WINE_WMCLASS`（那是 deepin 给
winex11 打的补丁）。桌面入口的 `StartupWMClass=workbuddy.exe` 必须和实际
值一致，否则任务栏归组和窗口修复工具都认不出来。

## 11. 数据目录在 prefix 里

Electron 的 userData 在 `C:\users\<user>\.workbuddy`，也就是
`$WINEPREFIX/drive_c/users/<user>/.workbuddy`。删包不删 prefix 登录态就还在；
反过来，想彻底重来就删 prefix。

## 12. 升级时不要动 prefix

包升级只替换 `/usr/lib/workbuddy-win-bin/app`。prefix 里的
`.workbuddy`（登录态、会话、下载的二进制）保持不动。`pkgver` 变化后
转发器版本资源自动更新，企业微信下次点击时重新读取。

## 13. 托盘右键菜单弹不出来

**现象**：托盘图标单击、双击都正常，右键毫无反应，`xwininfo` 里看不到新的
菜单窗口。

**原因**：wine 的 explorer 在 `programs/explorer/systray.c` 里收到
`WM_RBUTTONUP` 后先 `SendMessageW(TTM_RELAYEVENT)`，再 `notify_owner()`
把点击转给应用。这条消息走的是 explorer 自己的输入队列，`user_time` 比应用
队列里的事件新。Electron 的托盘实现随后调用 `SetForegroundWindow()`，而
wineserver 的 `set_foreground_window()`（`server/queue.c`）会拿调用方 input
的 `user_time` 和当前前台 input 比较，小的一方直接返回
`STATUS_ACCESS_DENIED`。Electron 收到失败就放弃
`NotifyIcon::PopUpContextMenu()`，菜单窗口根本不会创建。所以表现是“右键
毫无反应”，而不是“菜单一闪而过”。

**解法**：给 explorer 打一个小补丁，在把点击转发给应用之前先替它
`SetForegroundWindow(icon->owner)`。explorer 就是桌面窗口的所属进程，这一步
不会被时间检查拒掉；应用成了前台之后，它自己再抢前台就通过了。

- 补丁：`explorer-systray-foreground.patch`（wine 11.22，
  `programs/explorer/systray.c`），产物和重建脚本在 `wine-fix` 仓库
- 启动时由 `wine-fix-run` 挂到 prefix 的 `C:\windows\system32\explorer.exe`
  和系统库目录的同名文件上；产物是去掉 `--wine-builtin` 标记重链过的 native
  镜像，不需要额外的 `WINEDLLOVERRIDES`

构建时有两个坑：

1. wine 顶层 Makefile 给 explorer 的链接命令带 `-Wl,--wine-builtin`，产物带
   `Wine builtin DLL` 标记。这份文件被 `explorer.exe=n` 按 native 加载时，
   wine 会以 `c0000135`（找不到依赖 DLL）直接起不来。重建脚本去掉这个标记
   重链一次。
2. 已经在跑的 explorer 是常驻进程，换掉 prefix 里的文件要重启 prefix 才生效。

菜单里的汉字是方框的话是另一回事：Chromium 启动时会缓存字体，字体替换表
必须在应用启动之前就位。`workbuddy-win fontfix` 之后要重启应用（或
`wineserver -k`）才看得到效果。

## 14. stdout/stderr 不能是 socket 或目录

用 `systemd-run --user ... /usr/bin/workbuddy-win` 这种脱离终端的方式启动时，
systemd 默认把输出接到 journal 的 unix socket 上。Wine 里 Electron 的 main
进程会给 stdout/stderr 各建一个可写流，而这一步在 Wine 下会按 fd 再 open
一次；fd 是 socket（或目录）时 `node:net` 直接抛：

```text
Uncaught Exception: Error: open EISDIR
  at new Socket (node:net:437:13)
  at createWritableStdioStream ...
```

启动器界面起来后弹一个「A JavaScript error occurred in the main process」
对话框，但应用本身还在跑，日志里也看不出别的问题。跟 explorer 托盘补丁无关。

启动器现在会先看 `/proc/self/fd/1` 的类型，是 socket、FIFO 或 directory
就追加到
`$XDG_STATE_HOME/<pkg>/launch.log`（默认 `~/.local/state`）。绕开的方法有三个，
任选一个：显式给 systemd 一个文件
（`--property=StandardOutput=append:/tmp/wb.log`）、接伪终端
（`systemd-run --user --pty`）、或者不脱终端直接跑。

## 15. 托盘右键菜单点外面不消失

**现象**：右键菜单能正常弹出，菜单项也能点，但点桌面、点别的窗口、点
WorkBuddy 主窗口都不会关闭菜单；`Esc` 同样无效，只能选中某一项或再右键一次。

**原因**：Electron 的托盘菜单是 Chromium 自绘的 popup。菜单弹出后应用把前台
还给托盘宿主窗口，菜单本身失活，Chromium 开始等鼠标 capture 带来的取消事件。
Wine 的 mouse capture 只在同一个进程的输入队列里生效，win32u 并不会为它做
`XGrabPointer`；点其他 X 客户端时，事件根本不会进入 WorkBuddy 的队列。放在
Wine 里轮询 `GetAsyncKeyState` 或装 `WH_MOUSE_LL` 也没用，它们同样只覆盖本
进程。所以应用完全不知道菜单外面发生了点击。

**解法**：启动器在 `exec WorkBuddy.exe` 之前拉起一对同包 helper：

- `tray-dismiss-x11`：在根窗口上收 XInput2 的 `XI_RawButtonPress` /
  `XI_RawKeyPress`。raw 事件不抢输入、不需要 grab，按下沿由 X server 直接
  送出，跨客户端的点击也能看到；坐标在收到事件时用 `XQueryPointer` 现取。
  没有 XInput2 时退回 `XQueryPointer` / `XQueryKeymap` 轮询，极短的点击
  可能漏掉。事件写进 `$XDG_RUNTIME_DIR/workbuddy-win-bin/tray-events.bin`；
- `tray-dismiss.exe`：跑在同一个 prefix 里，枚举可见、无标题、带
  `WS_POPUP` 的 `Chrome_WidgetWin_*` 菜单窗口。事件坐标落在所有菜单矩形
  之外，或事件是 `Esc` 时，向这些窗口 `PostMessage` 一个
  `WM_KEYDOWN`/`VK_ESCAPE`；点在菜单内不发，避免抢掉菜单自己的点击。

弹出菜单的那一下右键也会被 X11 侧记录，所以 Wine helper 第一次看到菜单时
只把当前序号记为基线，不处理这次事件。

这里不能用 `WM_CANCELMODE`：它只会把原生窗口藏起来，Chromium 的
`views::MenuController` 仍然以为菜单在运行，托盘下一次
`popUpContextMenu()` 会被直接忽略，表现成“点外面关掉后，马上再右键没反应”。
`VK_ESCAPE` 的按键消息才会走菜单自己的取消路径，窗口真正销毁，下一次右键
立刻恢复。

也不要再补发鼠标点击：外部那一下点击已经由 X server 送到目标窗口了，补发
等于把同一下点两遍；更麻烦的是 Xfce 面板这种区域不在 wine 桌面范围内，补发
的点击没有落点，菜单反而要等好几秒才被别的路径收掉（实测面板 1.4 秒、
WorkBuddy 标题栏 4.6 秒）。

同理不能用 `SendInput` 补 Esc：点了别的 X 客户端之后 wine 已经失去焦点，
合成的按键没有接收窗口。`PostMessage` 直接送到菜单窗口句柄，不依赖焦点，
实测面板 212ms、标题栏 44~78ms、其他窗口 47~82ms 关闭，且都能立刻重开。

两个 helper 都用单实例锁：X11 侧是事件文件旁的 `flock`，Wine 侧是
`Local\WorkBuddyTrayDismiss` mutex。应用正常重启时旧 helper 会继续复用；
Wine helper 启动后 120 秒没看到 `WorkBuddy.exe`，或看到后消失 30 秒，就自行
退出；X11 helper 在 Wine 侧心跳过期 5 秒后退出。

## 16. 输入框唤不起输入法，只能打英文

**现象**：WorkBuddy 里所有输入框都只能输入 ASCII，按切换键没反应，ibus 的
候选窗口根本不出现；宿主其它应用输入法正常。

**原因**：Wine 的 imm32 走 X11 XIM 转发，只有进程环境里有 `XMODIFIERS`
（如 `@im=ibus`）时才会去连宿主 XIM 服务器。启动器为了对齐界面语言把
`LANG`/`LC_ALL` 统一改写了，却没有补这个变量，于是 Wine 进程以「没有输入法」
的状态启动，TSF/IMM 两条路径都拿不到 composition。

**验证方法**：写个只处理 `WM_CHAR` 的 Win32 探针，分别在有无 `XMODIFIERS`
下用 xdotool 打 `nihao`：

- 不带 `XMODIFIERS`：只收到 ASCII 的 `n i h a o`，没有任何
  `WM_IME_*` 消息；
- 带 `XMODIFIERS=@im=ibus`（locale 用 `en_US.UTF-8` 也一样）：收到
  `WM_IME_STARTCOMPOSITION`、`WM_IME_COMPOSITION`，最终 `WM_CHAR` 是
  中文字符。

说明限制在 XIM 变量本身，与 locale 是不是 `zh_CN` 无关，也不需要
`GTK_IM_MODULE` / `QT_IM_MODULE`（那是 GTK/Qt 自己的输入模块，Win32
程序用不上）。

**解法**：启动器 `setup_wine_env()` 里补
`export XMODIFIERS="${XMODIFIERS:-@im=ibus}"`。用 `${VAR:-默认值}` 是为了
让换 fcitx5 的人从外面带 `XMODIFIERS=@im=fcitx` 就能覆盖。

## 17. 输入法候选框贴在窗口左下角，不跟随光标

**现象**：第 16 条修完之后中文能打了，但 ibus 的候选窗总是出现在 WorkBuddy
窗口左下角（`x` 等于窗口左边，`y` 等于窗口底边），不管光标在输入框哪个位置。

**原因**：两段，缺一段都不行。

一是 wine 建 XIC 时默认选 `XIMPreeditCallbacks | XIMStatusCallbacks`
风格（组字串交给应用自己画，最接近 Windows 的 IMM 语义）。而
`dlls/winex11.drv/xim.c` 的 `X11DRV_SetIMECompositionRect()` 只在
`XIMPreeditPosition` 风格下才把应用给的光标位置转成 `XNSpotLocation` 发给
输入法，callbacks 风格直接 `return FALSE`。ibus 的 XIM 前端
（`client/x11/main.c`）收不到 `XNSpotLocation` 时，退回到
`XTranslateCoordinates(w, root, 0, w.height)`，也就是窗口左下角。

二是这条链路上根本没人给 wine 光标位置。Chromium 138（Electron 37）里
`ui/base/ime/win/imm32_manager.cc` 已经删了，`RenderWidgetHostViewAura` /
`HWNDMessageHandler` / `legacy_render_widget_host_win` 里都搜不到
`WM_IME_COMPOSITION`、`ImmSetCompositionWindow`，也就是说早先「Chromium 每次
移动光标都会带 `CFS_POINT` 调一次」的判断是错的，实测日志里
`ImmSetCompositionWindow` 一次都没出现。真正的位置出口只有
`InputMethodWinBase::OnImeRequest()`：应用收到
`WM_IME_REQUEST / IMR_QUERYCHARPOSITION` 时，`OnQueryCharPosition()` 才用
`TextInputClient::GetCaretBounds()` 或
`GetCompositionCharacterBounds()` 现算一个屏幕矩形回过去。wine 从来不问，
于是位置一直是空的。

**验证方法**：

1. 先确认 Chromium 走的是哪条输入链：`/proc/<pid>/maps` 里有 `msctf.dll`、
   日志里有 `msctf:TextStoreACPSink_OnTextChange`，说明 TSF 客户端已起来，
   但组字串仍由 wine 的 imm32/XIM 送 `WM_IME_COMPOSITION`。
2. 用 `probe-ime-charpos.c` 往 `Chrome_WidgetWin_1` 顶层窗口直接发
   `IMR_QUERYCHARPOSITION`：输入框没有焦点时返回 0，说明这个请求只能踩在实际
   组字上下文里问，单独的探针问不出东西。
3. 真实输入验证：CDP 读 Slate 输入框的光标矩形是 `(2561,470)` 高 19，日志里
   对应 `app caret rect 2561,470 height 19`；`xdotool getwindowgeometry` 量
   `ibus-ui-gtk3` 的候选窗口正好落在 `(2561,470)`。把光标挪到该行行首后，
   日志变成 `2531,470`，候选窗同步挪到 `(2531,470)`。

顺带确认 `InputStyle=overthespot` 不是解法：它换来 `XIMPreeditPosition`
风格，但组字串不再回传给应用，而且候选窗位置照样不对。

**解法**：两个补丁各修一半。

`wine-xim-query-caret.patch` 在组字流程里主动问应用要光标矩形：`post_ime_update()`
在下发组字串前后各查一次，组字开始（`xic_preedit_start`）再查一次，拿到
`IMECHARPOSITION` 的屏幕坐标后交给 `X11DRV_SetIMECompositionRect()`。只在
当前线程的窗口上 `SendMessage`，避免跨线程阻塞输入线程。

`wine-xim-spot-callbacks.patch` 把 `X11DRV_SetIMECompositionRect()` 的判断改成

```c
if (!(input_style & (XIMPreeditPosition | XIMPreeditCallbacks)))
    return FALSE;
```

这样两种风格都发 `XNSpotLocation`，组字方式仍是 callbacks。

**spot 的纵向偏移**：ibus 的 XIM 前端把 `XNSpotLocation` 直接当作候选窗位置，
callbacks 模式下它拿不到预编辑高度。若 spot 取光标行的上沿
（`pos.pt.y`），候选窗会正好压住正在输入的那一行；改成行底
（`pos.pt.y + pos.cLineHeight`）后，候选窗落在下一行，和 GTK 应用观感一致。

**打包方式**：补丁、产物和重建脚本都在 `wine-fix` 仓库，启动器用
`/usr/bin/wine-fix-run` 代替 `wine`。它在只属于本次启动的挂载命名空间里把
`winex11.so`、`imm32.dll`、`explorer.exe` 单点挂到系统路径上，系统文件和
prefix 都不动。

**注意**：

- unix 库的 ABI 与 wine 版本绑定，`wine-fix` 里带着
  `wine-version`，`wine-fix-run` 拿它跟 `wine --version` 比对，对不上就不挂
  `winex11`（输入法仍能用，只是候选框回到左下角），不会把 wine 弄崩。
- `bwrap` 起不来（用户命名空间被内核或 LSM 限制）时退回原版 wine，
  `wine-fix-run` 把原因写到 stderr，也就是启动器的
  `$XDG_STATE_HOME/workbuddy-win-bin/launch.log`。
- wine 升版后在 `wine-fix` 仓库跑 `build.sh` 按对应版本重编，别拿旧版本的
  .so 硬顶。`wine-fix-status` 能看当前挂载了什么、跑的 wineserver 是不是从
  `wine-fix-run` 起来的。
- 排查用的 `probe-ime-charpos.c`、`probe-enum-windows.c` 只留在仓库里当诊断
  工具，不进包：前者往指定窗口发 `IMR_QUERYCHARPOSITION` 看应用回不回矩形，
  后者枚举进程里的 Win32 窗口和类名。交叉编译：
  `x86_64-w64-mingw32-gcc -municode -o probe.exe probe-ime-charpos.c -luser32`。

## 18. 窗口左下角多画一份正在输入的字

**现象**：输入法能打，候选框也跟着光标，但焦点窗口左下角（窗口外、紧贴窗口
下边）额外画着正在输入的那几个字，和输入框里内联的那份重复。

**原因**：不是 ibus 画的。`xwininfo -root -tree` 里能看到一个 42x36 的窗口，
`WM_CLASS=workbuddy.exe`、`WM_TRANSIENT_FOR` 指向主窗口，位置在焦点窗口
左下角，窗里就是两个 8x13 字形。这是 wine `dlls/imm32/ime.c` 的回退 IME UI
窗：`ime_ui_update_window()` 在 `CFS_DEFAULT` 时把组字串画出来，按 Windows
的老规矩摆在左下角。Chromium/Electron 自己已经把组字串内联画进页面，于是
出现两份。

**解法**：`wine-imm32-no-ime-ui-window.patch` 让
`ime_ui_update_window()` 始终 `ShowWindow( hwnd, SW_HIDE )`，组字串仍照常经
`WM_IME_COMPOSITION` 送进应用。补丁和产物在 `wine-fix` 仓库，`wine-fix-run`
把 `imm32.dll` 挂到系统库目录和 prefix 的 `system32` 上——PE 内置库建 prefix
时会被复制进 `drive_c/windows/system32`，只换系统库目录那份不生效。

**注意**：这个改动假定应用自己画组字串。真给不画 preedit 的老 Win32 程序用，
会把回退显示一起关掉，用 `WINE_FIX_DISABLE=imm32` 跳过。

## 19. Bash 工具跑不了任何外部命令

**现象**：WorkBuddy 的 Bash 工具里 `echo` 有输出，`/bin/echo`、`ls`、`git`、
`python3` 全都没有输出，退出码 1。界面看起来“命令执行了”，实际只有 shell
内建生效。

**原因**：应用自带 PortableGit 的 Cygwin 运行时在 Wine 下报
`Couldn't compute FAST_CWD pointer`，`fork()` 直接失败。连 `cmd.exe`、
`node.exe` 这类原生 Windows 程序也起不来，所以“放个假 exe 转调宿主命令”的
想法也不行：那个 exe 本身就没进程能启动。

网上常见的做法是文件队列：让 WorkBuddy 把脚本写到 `jobs/`，宿主 watcher 轮询
执行，结果写到 `out/`，再往 MEMORY 里写使用规则。能用，但每次执行都要轮询和
读写文件，Bash 工具变成异步的，模型还容易忘记规则。

**解法**：应用认 `CODEBUDDY_CODE_GIT_BASH_PATH`，只要这个路径存在就把它当
Git Bash。于是编一个原生 PE `wb-bash-proxy.exe` 顶上去，它收到
`bash.exe -c <script>` 后通过 Wine 的 loopback 网络把脚本交给宿主守护进程
`wb-bash-bridge.py`，后者用 `/bin/bash -lc` 同步执行，流式回传
stdout/stderr 和退出码。启动器在拉起 WorkBuddy 前启动 bridge，并设置
`CODEBUDDY_CODE_GIT_BASH_PATH=Z:\usr\lib\workbuddy-win-bin\wb-bash-proxy.exe`。

**踩过的细节**：

- 代理的 socket 不能在连接成功后还留着 `SO_RCVTIMEO`。最初设了 2 秒，任何
  超过 2 秒且暂时没有输出的命令都会报 “bridge closed the connection”；正确
  做法是只限制 `connect`，连上后阻塞等结果，由 WorkBuddy 的超时杀代理进程，
  连接断开后守护进程再杀掉宿主命令的整个进程组。
- 守护进程要用登录 shell（`bash -lc`），否则从桌面环境继承的 PATH 里没有
  `JAVA_HOME`、coursier 这些，`java`/`mvn` 仍然找不到。
- `bash -lc` 只读 `~/.profile`，不会读 `~/.profile_lowrisk`。需要 Maven 那套
  环境时用 `WORKBUDDY_BASH_PROFILE=~/.profile_lowrisk` 启动，bridge 会在每条
  命令前先 source 它。
- 脚本要先把 Windows 路径换成宿主路径再交给 bash。未加引号的
  `Z:\home\pek` 在 bash 里会被反斜杠转义成 `Z:homepek`，必须在执行前替换；
  带空格的路径要连同引号一起替换。
- 代理写 stdout/stderr 时必须区分管道和 Windows 控制台。控制台句柄会按当前
  OEM 代码页转换字节，UTF-8 中文直接 `fwrite` 就成乱码；控制台要走
  `WriteConsoleW`，并缓存上一帧末尾没拼完的最多 3 个字节。
- bridge 由启动器在 `setup_wine_env` 之前拉起，并且显式清掉
  `LD_PRELOAD`/`WINEPREFIX`，否则托盘 shape 修复库会被预加载进每一个宿主
  命令。
- 只监听 `127.0.0.1`，每次启动生成随机 token，状态文件 0600；本机其它用户
  连不上，也猜不到 token。

## 20. 窗口失焦时任务完成，整个 WorkBuddy 进程消失

**现象**：让 agent 跑一个任务，然后把窗口切到别处。任务完成的一瞬间，
WorkBuddy 窗口、托盘图标、进程树全部消失，没有报错弹窗。`main.log` 最后
一行停在：

```
[TaskNotify] onEvent
```

没有 `completed:` 那一行，也没有 OOM、crashpad 报告或 Wine page fault。

**原因**：任务完成时如果窗口没有焦点，主进程会走系统通知：

```js
if (!electron.Notification.isSupported()) return !1;
...
let e = new electron.Notification({ title: r, body: i, silent: !0 });
```

Electron 的 `isSupported()` 在 Windows 上只判断系统版本，Wine 里返回
`true`；但 Wine 没有可用的 WinRT toast 实现，`show()` 直接把主进程带走，
连异常都来不及抛。用主进程 inspector 直接执行
`new Notification({title:"x",body:"y"}).show()` 可以稳定复现：WebSocket
立刻断开，进程树清空。

**解法**：`patch-workbuddy-asar.py` 在构建期改 `app.asar`，把四处
`new electron.Notification({...})` 换成等长的空对象桩：

```js
{show(){},close(){},on(){return this}}
```

`on()` / `show()` / `close()` 调用点全部保留，函数照常返回 `true`，应用内
完成提示和提示音不受影响，只是没有系统级 toast。

**踩过的细节**：

- 只替换 `showTaskNotification` 一处不够。`electron.Notification` 在
  `main-bootstrap.js` 里共有四处（任务通知、desktop host 的
  `sendNotification`、`SystemNotification`、关窗到托盘的提示），都受同一个
  `isSupported()` 谎报影响，全部换掉。
- 替换必须等长。`app.asar` 里其它文件的 offset 是绝对偏移，改长度就得重排
  整个归档；用块注释把桩补齐到原表达式长度最省事。
- 改完要重算 header 里这个文件的 SHA256（`integrity.hash` 和
  `integrity.blocks`），否则 Electron 读 asar 会直接报校验失败。
- 只改 asar 还不够：`WorkBuddy.exe` 里
  `EnableEmbeddedAsarIntegrityValidation` fuse 是开的，校验的是打包时的
  asar 哈希。脚本同时把这一个 fuse 从 `1` 翻成 `0`（fuse wire 里第 5 位，
  前面 4 位是 RunAsNode / CookieEncryption / NodeOptionsEnv /
  NodeCliInspect）。

## 21. PortableGit 提取覆盖 Git Bash 代理，CLI 又继承了 `LD_PRELOAD`

**现象**：第 19 条的代理已经通了，Bash 工具却换了种坏法：每条命令都失败，
报错是

```
Cygwin ... bash.exe 加载 Z:\usr\lib\workbuddy-win-bin\no-shaped-systray.so 失败
```

而且每次新建 shell 的 PID 都不同、错在同一处。我一开始按“偶发环境问题”
处理，实测两次报错后三次正常，其实是没看清触发条件，不是随机。

**原因**：两个独立的坑叠在一起。

1. `app.asar` 里的 `main/shell-env-async.js` 在 PortableGit 解压完成后，
   无条件把 `process.env.CODEBUDDY_CODE_GIT_BASH_PATH` 改写成自带的
   `bin/bash.exe`，启动器设好的 `wb-bash-proxy.exe` 在运行时被覆盖，Bash
   工具又退回 Cygwin 那套跑不起来的东西。
2. 托盘图标修复给主进程挂了 `LD_PRELOAD=no-shaped-systray.so`，主进程必须
   留着它；但 Wine 里所有子进程都会继承。Cygwin/MSYS2 把 `LD_PRELOAD` 当成
   自己的预加载列表，按 MSYS 的虚拟根去打开 `/usr/lib/...so`，于是每个
   shell 都死在加载一个宿主 .so 上。

**解法**：都在 `patch-workbuddy-asar.py` 里，构建期一次改完。

- `shell-env-async.js` 改成“环境变量里已有可用路径就保留”，不够长的地方用
  块注释补齐，偏移不变。
- CLI 入口 `cli/dist/codebuddy-lite-wb.mjs`、`cli/bin/codebuddy`、
  `cli/bin/cbc-prewarm`、`cli/bin/codebuddy-lowmem` 顶部插一行
  `delete process.env.LD_PRELOAD;`。只清 CLI 进程及其子进程，主进程的 X11
  拦截库照常生效。

**踩过的细节**：

- 有 shebang 的入口必须插在 shebang 行**之后**，否则脚本不再可执行；判断
  时还要认 `\r\n`。
- 改写后的 JS 要过一遍 `node --check`。有一次 `SHELL_ENV_NEW` 少写了一个右
  花括号，主进程直接卡住没窗口，日志里也没有明显线索；加上语法检查后在
  makepkg 阶段就报出来了。
- 等长替换仍是硬约束：错一个字节，后面所有文件的 offset 全乱。
- 这条和超时、bridge、`bash -lc` 无关，宿主侧进程根本没被调到。先看报错里
  加载的是哪个库，就能立刻区分是这条还是第 19 条。
