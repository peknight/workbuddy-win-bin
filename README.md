# workbuddy-win-bin

腾讯 WorkBuddy 国内版 Windows 客户端，跑在发行版自带的 wine 11 上。

WorkBuddy 分两条产品线：

| 版本 | 主程序 | 数据目录 | 站点 |
|---|---|---|---|
| 国内版 | `WorkBuddy.exe` | `C:\users\<user>\.workbuddy` | `copilot.tencent.com` / `www.workbuddy.cn` |
| 国际版 | `WorkBuddyAI.exe` | `C:\users\<user>\.workbuddy-ai` | `workbuddy.ai` |

企业账号、企业微信里的 WorkBuddy 入口只认国内版，本包打的是国内版。

## 安装

```bash
makepkg -si
```

构建期把官方 NSIS 安装包拆开，取出 `$PLUGINSDIR/app-64.7z`；再从官方素材
生成 16~256 的图标、派生带汉字的 Tahoma、编译几个小工具：

- `no-shaped-systray.so`：修托盘图标黑底；
- `tray-dismiss.exe` / `tray-dismiss-x11`：托盘菜单点外面时负责关闭，
  见 `PITFALLS.md` 第 15 条；
- `workbuddy-protocol.exe`：带版本号的 `workbuddy://` 转发器，给企业微信
  的版本检查用；
- `winex11-xim-spot.so`：wine 的 X11 驱动，改了两处（组字时向应用查光标矩形、
  再把它转成 `XNSpotLocation`），让输入法候选框跟随光标，见 `PITFALLS.md`
  第 17 条。

另外随包带一份改过 `programs/explorer/systray.c` 的
`explorer-systray-foreground.exe`：wine 内置 explorer 在转发托盘右键点击时
不抢前台，Electron 的托盘菜单会被 wineserver 的时间检查挡掉、弹不出来。补丁
和重建脚本装在 `/usr/share/workbuddy-win-bin/`，详见 `PITFALLS.md` 第 13 条。

需要 `7zip`、`imagemagick`、`python-fonttools`、`mingw-w64-gcc`。
运行时用 `bubblewrap` 把 `winex11-xim-spot.so` 单点挂进 wine 的库路径。
下载的安装包约 460M，装完约 1.3G。

## 首次运行

直接启动 `workbuddy-win`（或应用菜单里的 WorkBuddy）。首次会：

1. 在 `$XDG_DATA_HOME/wineprefixes/workbuddy-win` 建 prefix；
2. 往 prefix 里装派生 Tahoma、应用字体替换表，替换 prefix 里的
   `explorer.exe`（修托盘右键菜单）；
3. 拉起托盘菜单辅助进程，再启动 `WorkBuddy.exe --no-sandbox`。

登录态和聊天数据都在 prefix 里，`pacman -Syu` 升级包不动 prefix，不用重新
登录。

## 子命令与环境变量

| 命令 | 用途 |
|---|---|
| `workbuddy-win` | 启动 |
| `workbuddy-win setup` | 只补建 prefix / 应用字体 |
| `workbuddy-win fontfix` | 重打字体替换表（界面出现方框时用） |
| `workbuddy-win prefix` | 打印当前 `WINEPREFIX` |
| `workbuddy-win help` | 帮助 |

其余参数原样传给 `WorkBuddy.exe`。

| 环境变量 | 说明 |
|---|---|
| `WORKBUDDY_WIN_PREFIX` | 指定 prefix 路径，覆盖默认位置 |

## 和企业微信的联动

企业微信里的 WorkBuddy 入口会 `ShellExecute` 打开 `workbuddy://...`。Wine
只内置 `http`/`https`/`mailto`，所以由 `wecom-win-bin` 在它的 prefix 里注册
这个协议，命令指向本包的 `workbuddy-protocol.exe`：

```
"Z:\usr\lib\workbuddy-win-bin\workbuddy-protocol.exe" "%1"
```

企业微信 5.0.13 起会读这个 exe 的 `FileVersion` 做版本门槛。转发器的版本
资源在构建期从 `pkgver` 生成，和 `WorkBuddy.exe` 一致；它收到 URL 后转调
`winebrowser.exe`，再经宿主 `xdg-open` 落到 `/usr/bin/workbuddy-win`。

版本之外，企业微信还会取这个 exe 的签名者 **CN** 和内置白名单比对（只认
`Tencent Technology (Shenzhen) Company Limited`），不匹配同样只弹一句
打开失败、且根本不会执行转发器。`PKGBUILD` 因此在构建期用 openssl 生成一次性
CA + leaf，leaf 主题照抄腾讯证书，再用 `jsign` 做 Authenticode 签名。Wine 的
`wintrust` 不做信任链校验，所以自签名足够；私钥不落仓库、不进包。

转发器每步都往 `%TEMP%\workbuddy-protocol.log` 记一行，排查时先看它有没有新
记录，能立刻区分“没被调用”和“被调用了但转发失败”。

宿主侧要求本包的 desktop 文件声明
`MimeType=x-scheme-handler/workbuddy;`，并且 `Exec` 用绝对路径。检查：

```bash
xdg-mime query default x-scheme-handler/workbuddy
# workbuddy-win-bin.desktop
```

## 已经处理掉的坑

- 托盘图标黑底加边框：`LD_PRELOAD` 摘掉 wine 给托盘窗口设的 bounding shape
- 托盘右键菜单点外面不消失：X11 侧记录点击，Wine 侧给菜单发 Esc 取消
- GDI 控件汉字方框：派生一份带汉字的 Tahoma
- Electron 在 Wine 下启动崩溃：`--no-sandbox`
- 企业微信提示“WorkBuddy 版本过低”：带版本资源的协议转发器
- 企业微信弹 `Failed to open WorkBuddy`：转发器签名者 CN 对齐腾讯白名单
- 输入框打不了中文：启动器补 `XMODIFIERS`，见 `PITFALLS.md` 第 16 条
- 输入法候选框贴在窗口左下角：补丁版 `winex11.so` 先向应用查
  `IMR_QUERYCHARPOSITION`，再转成 `XNSpotLocation`，见 `PITFALLS.md` 第 17 条

细节见 `PITFALLS.md`。

## 已知问题

- `WM_CLASS` 固定是 `workbuddy.exe`：上游 wine 只按进程名取，桌面入口的
  `StartupWMClass` 已按实际值写。
- 程序本体在 `/usr/lib` 只读，应用内的自动更新不可用；升级走 `pkgver`。

## 升级

1. `curl -s 'https://copilot.tencent.com/v2/update?platform=workbuddy-win32-x64-user'`
   拿最新版本号、下载地址和 sha256；
2. 更新 `PKGBUILD` 里的 `pkgver`、`_build`、`sha256sums`；
3. `makepkg -f` 后 `pacman -U`。

转发器的版本资源会自动跟着 `pkgver` 走，企业微信侧不用改注册表。

## 卸载

```bash
pacman -R workbuddy-win-bin
rm -r ~/.local/share/wineprefixes/workbuddy-win   # 连同登录态和数据
```
