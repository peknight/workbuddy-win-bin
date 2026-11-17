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
  `programs/explorer/systray.c`）
- 产物：`explorer-systray-foreground.exe`，重建方法见 `build-explorer.sh`
- 启动器把它装进 prefix 的 `C:\windows\system32\explorer.exe`，并用
  `WINEDLLOVERRIDES` 里的 `explorer.exe=n` 强制按 native 加载

构建时有两个坑：

1. wine 顶层 Makefile 给 explorer 的链接命令带 `-Wl,--wine-builtin`，产物带
   `Wine builtin DLL` 标记。这份文件被 `explorer.exe=n` 按 native 加载时，
   wine 会以 `c0000135`（找不到依赖 DLL）直接起不来。重建脚本去掉这个标记
   重链一次。
2. 已经在跑的 explorer 是常驻进程，换掉 prefix 里的文件要重启 prefix 才生效。

菜单里的汉字是方框的话是另一回事：Chromium 启动时会缓存字体，字体替换表
必须在应用启动之前就位。`workbuddy-win fontfix` 之后要重启应用（或
`wineserver -k`）才看得到效果。
