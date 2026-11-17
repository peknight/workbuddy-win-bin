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
- `wb-bash-proxy.exe` / `wb-bash-bridge.py`：Bash 工具的 Linux 工具链桥。
- `patch-workbuddy-asar.py`：把 Windows 原生 toast 换成空实现，否则任务
  完成时进程会直接消失，见 `PITFALLS.md` 第 20 条。

需要 `7zip`、`imagemagick`、`python-fonttools`、`mingw-w64-gcc`。运行时依赖
[`wine-fix`](https://github.com/peknight/wine-fix)：输入法候选框跟随光标、
不再重复画组字串、托盘右键菜单可用，三处都要替换 wine 自己的二进制，启动器
用 `wine-fix-run` 代替 `wine` 把它挂进去。

下载的安装包约 460M，装完约 1.3G。

## 首次运行

直接启动 `workbuddy-win`（或应用菜单里的 WorkBuddy）。首次会：

1. 在 `$XDG_DATA_HOME/wineprefixes/workbuddy-win` 建 prefix；
2. 往 prefix 里装派生 Tahoma、应用字体替换表；
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
| `workbuddy-win bridge` | 启动 Linux 工具链桥并打印端口 |
| `workbuddy-win help` | 帮助 |

其余参数原样传给 `WorkBuddy.exe`。

| 环境变量 | 说明 |
|---|---|
| `WORKBUDDY_WIN_PREFIX` | 指定 prefix 路径，覆盖默认位置 |
| `WORKBUDDY_NO_LINUX_BASH` | 设为 `1` 时关闭 Bash 工具的 Linux 工具链桥 |
| `WORKBUDDY_BASH_PROFILE` | 每条命令前额外 `source` 的 shell 文件，如 `~/.profile_lowrisk` |

## Bash 工具用宿主 Linux 工具链

WorkBuddy 自带的 PortableGit 在 Wine 下只能跑 shell 内建命令，`git`、`python3`、
`make`、`Rscript` 这类外部程序一个都起不来。启动器默认会拉起一个小守护进程，
把 Bash 工具的脚本转给宿主 `/bin/bash -lc` 执行，stdout/stderr 和退出码原样
回传：

```
WorkBuddy (Wine)                          Linux 宿主
wb-bash-proxy.exe ── 127.0.0.1:随机端口 ──▶ wb-bash-bridge.py
       ▲                                      │ /bin/bash -lc
       └──────── stdout / stderr / rc ────────┘
```

这条链路是同步的，Bash 工具看到的仍是一条命令执行完再返回；不需要轮询文件，
也不用往 MEMORY 里写“改用文件队列”的规则。守护进程只监听 loopback，每次启动
生成随机 token，状态文件在 `$XDG_RUNTIME_DIR/workbuddy-win-bin/bash-bridge.state`
（权限 0600），空闲 30 分钟自动退出。命令以当前用户身份、不加沙箱地执行，权限
边界等同于你自己的终端。

脚本里的 `Z:\home\pek\...`、`C:\users\pek\...`、`/z/home/pek/...` 会分别转换成
Linux 路径，带引号的路径（含空格）也能处理。命令以宿主用户身份运行，登录 shell
会加载 `~/.profile`；Maven、Java 这类只写在 `~/.profile_lowrisk` 里的环境不会
自动带上，需要时把 `WORKBUDDY_BASH_PROFILE` 指过去：

```bash
WORKBUDDY_BASH_PROFILE=~/.profile_lowrisk workbuddy-win
```

几个边界：

- 这是 Bash 工具专用的路径，不会把 Wine 里所有 `git.exe` 调用都换掉；
- 命令里不能再调用 Windows 的 `.exe`（`cmd.exe`、`where.exe` 等），要用宿主
  对应的 Linux 命令；
- 交互式命令（等 stdin 的 `read`、不带 `-m` 的 `git commit`）会直接拿到 EOF；
- 需要临时回到自带 Git Bash 时，设 `WORKBUDDY_NO_LINUX_BASH=1` 再启动。

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
- 窗口左下角重复画一份正在输入的字：补丁版 `imm32.dll` 不再显示回退的 IME
  UI 窗，见 `PITFALLS.md` 第 18 条
- Bash 工具只能跑 shell 内建：启动器拉起宿主侧 bridge，PE 代理转发脚本和输出，
  见 `PITFALLS.md` 第 19 条

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
