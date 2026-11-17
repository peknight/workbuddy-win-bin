# Maintainer: Peknight <JKpeknight@gmail.com>

pkgname=workbuddy-win-bin
pkgver=5.11.3.43806934
pkgrel=23
pkgdesc='腾讯 WorkBuddy 国内版 Windows 客户端，基于发行版 wine 11 运行 | Tencent WorkBuddy (CN) Windows build on upstream wine'
arch=('x86_64')
url='https://copilot.tencent.com/'
license=('LicenseRef-proprietary')
options=(!debug !strip)

depends=(
    'wine-fix'      # 按进程挂载的 wine 补丁（XIM 光标位置、IME UI 窗、托盘前台）
    'fontconfig'
    'hicolor-icon-theme'
    'libx11'        # 托盘图标修复用的 LD_PRELOAD 小库
    'libxext'
    'libxi'         # 托盘菜单辅助进程用 XInput2 raw 事件记录点击/Esc
    'python'        # Linux 工具链桥守护进程
    'wqy-microhei' # 中文字形来源，替换表把它填给 Tahoma / Noto Sans CJK 等家族
)
makedepends=(
    '7zip'             # NSIS 安装包 → $PLUGINSDIR/app-64.7z → app/
    'imagemagick'      # 从官方 256px 素材缩出 64 / 128
    'python-fonttools' # 派生带汉字的 Tahoma，给 GDI 那一路取字用
    'mingw-w64-gcc'    # 编 workbuddy:// 转发器
    'openssl'          # 给转发器出一次性自签名证书
    'java-runtime-headless' # jsign 签转发器，企业微信要校验 Authenticode
)

# 国内版（copilot.tencent.com / workbuddy.cn）主程序是 WorkBuddy.exe，数据目录
# C:\users\<user>\.workbuddy；国际版（workbuddy.ai）主程序叫 WorkBuddyAI.exe、
# 数据目录 .workbuddy-ai，是另一条产品线，企业账号和企微入口只在国内版上通。
_app_id='workbuddy-win'
_bin='workbuddy-win'
_app_name='WorkBuddy'
# 官方文件名尾部的构建号，和版本号一起出现在更新接口的返回里
_build='ecace0f7'
_exe="WorkBuddy-win32-x64-user-${pkgver}-${_build}.exe"

source=(
    "${_exe}::https://download.codebuddy.cn/workbuddy/saas/win32-x64-user/${_exe}"
    'make-tahoma.py'
    'workbuddy-win.sh'
    'workbuddy-win.desktop'
    'workbuddy-win-fonts.reg'
    'no-shaped-systray.c'
    'tray-dismiss.c'
    'tray-dismiss-x11.c'
    'workbuddy-protocol.c'
    'workbuddy-protocol.rc'
    'wb-bash-bridge.py'
    'wb-bash-proxy.c'
    'patch-workbuddy-asar.py'
    'jsign-7.6.jar::https://github.com/ebourg/jsign/releases/download/7.6/jsign-7.6.jar'
)
# NSIS 安装包由 prepare() 自己解；交给 makepkg 只会解出一层壳和 Uninstall.exe
noextract=("${_exe}")
sha256sums=(
    '2fd40059eea2f9a2d641c675a66f2476d71b49d9eb32f19db8c318768133b56b'
    'SKIP'
    'SKIP'
    'SKIP'
    'SKIP'
    'SKIP'
    'SKIP'
    'SKIP'
    'SKIP'
    'SKIP'
    'SKIP'
    'SKIP'
    'SKIP'
    '6cf58aa080e7ff82656f320cff8d4a3bff30408df8766806dcba3740dd839fa5'
)

prepare() {
    cd "$srcdir"

    # NSIS 外壳里真正的内容是 $PLUGINSDIR/app-64.7z。另有几个散落在
    # resources/ 下的 mp4 加载动画，安装器会原样铺进安装目录，这里一并合过去。
    mkdir -p nsis
    7z x -y -onsis "${_exe}" >/dev/null
    [[ -f 'nsis/$PLUGINSDIR/app-64.7z' ]] || error "安装包里找不到 \$PLUGINSDIR/app-64.7z"

    mkdir -p app
    7z x -y -oapp 'nsis/$PLUGINSDIR/app-64.7z' >/dev/null
    [[ -f app/WorkBuddy.exe ]] || error 'app-64.7z 解出来没有 WorkBuddy.exe'
    if [[ -d nsis/resources ]]; then
        cp -a nsis/resources/. app/resources/
    fi
}

build() {
    cd "$srcdir"

    # Wine 没有可用的 WinRT toast：任务完成通知会执行
    # `new electron.Notification(...).show()`，主进程当场消失。
    # 补丁把这几处换成空实现，并关掉 asar 完整性 fuse。见 PITFALLS.md 第 20 条。
    # 同一个脚本还修两处 Bash 工具的问题：vendor 提取不再覆盖启动器设好的
    # Git Bash 代理路径，CLI 入口启动时清掉继承来的 LD_PRELOAD。见第 21 条。
    python patch-workbuddy-asar.py app

    # Electron 的界面走 DirectWrite，自己带回退链；GDI 那一路（部分原生控件）
    # 按家族名取 Tahoma，而 wine 自带的 tahoma.ttf 没有汉字，所以派生一份。
    python make-tahoma.py \
        '/usr/share/fonts/wenquanyi/wqy-microhei/wqy-microhei.ttc' fonts

    local assets='app/Assets' size
    mkdir -p icons
    # 安装包自带 16/24/32/48/256 的目标尺寸素材，64/128 由 256 缩出来
    for size in 16 24 32 48 256; do
        install -Dm644 "${assets}/Square44x44Logo.targetsize-${size}.png" "icons/${size}.png"
    done
    for size in 64 128; do
        magick "${assets}/Square44x44Logo.targetsize-256.png" \
            -resize "${size}x${size}" "icons/${size}.png"
    done

    # 托盘图标黑底：编一个 LD_PRELOAD 小库，在 wine 进程里拦掉
    # 托盘窗口的 bounding shape，详见 no-shaped-systray.c 开头
    cc -shared -fPIC -O2 -Wall -Wextra \
        -o no-shaped-systray.so no-shaped-systray.c -ldl -lX11 -lXext

    # 托盘菜单“点外面不消失”的辅助进程，说明见 PITFALLS.md 第 15 条。
    # Wine 侧枚举 Chromium 菜单窗口，X11 侧负责记录跨客户端的鼠标按下。
    x86_64-w64-mingw32-gcc -O2 -s -municode -mwindows \
        -o tray-dismiss.exe tray-dismiss.c -luser32
    cc -O2 -Wall -Wextra -o tray-dismiss-x11 tray-dismiss-x11.c -lXi -lX11

    # Bash 工具的 Linux 桥：PE 侧只负责转发，宿主守护进程用登录 shell 执行。
    # 详见 PITFALLS.md 第 19 条。
    x86_64-w64-mingw32-gcc -O2 -s -municode \
        -o wb-bash-proxy.exe wb-bash-proxy.c -lws2_32 -lshell32

    # 企业微信会读 workbuddy:// 注册表指向的 exe 的 FileVersion 做版本门槛。
    # winebrowser 没有版本资源，所以编一个和 WorkBuddy.exe 同版本号的转发器，
    # 详见 workbuddy-protocol.c 开头。
    local major minor patch
    IFS=. read -r major minor patch _ <<<"$pkgver"
    local ver="${major}.${minor}.${patch}"
    sed "s/@VER@/${ver}/g; s/@VERCOMMA@/${major},${minor},${patch},0/g" \
        workbuddy-protocol.rc >workbuddy-protocol.build.rc
    x86_64-w64-mingw32-windres workbuddy-protocol.build.rc \
        -O coff -o workbuddy-protocol.res.o
    x86_64-w64-mingw32-gcc -O2 -s -municode -mwindows \
        -o workbuddy-protocol.exe workbuddy-protocol.c workbuddy-protocol.res.o \
        -lshell32

    # 企业微信 5.0.13 的 WorkBuddyLauncher 在 ShellExecute 之前还有一道签名校验：
    # 未签名的 exe 直接走 "invalid signature" 分支，界面上只留一句
    # Failed to open WorkBuddy。校验方式为 CryptQueryObject 取签名者证书，
    # 再用 CertGetNameStringW(CERT_NAME_SIMPLE_DISPLAY_TYPE) 取 CN，跟内置白名单
    # "Tencent Technology (Shenzhen) Company Limited" 比对；Wine 的 wintrust
    # 只核对签名结构和摘要、不做信任链校验（自签名一样返回 0），所以证书主题
    # 必须与白名单同名，否则名字比对必失败。私钥留在 $srcdir，不进包。
    local leaf_subject
    leaf_subject='/jurisdictionC=CN/jurisdictionST=Guangdong Province'
    leaf_subject+='/jurisdictionL=Shenzhen/businessCategory=Private Organization'
    leaf_subject+='/serialNumber=9144030071526726XG'
    leaf_subject+='/C=CN/ST=Guangdong Province/L=Shenzhen'
    leaf_subject+='/O=Tencent Technology (Shenzhen) Company Limited'
    leaf_subject+='/CN=Tencent Technology (Shenzhen) Company Limited'
    local sign='workbuddy-protocol-sign'
    mkdir -p "$sign"
    openssl req -x509 -newkey rsa:3072 -sha256 -days 3650 -nodes \
        -keyout "$sign/ca.key" -out "$sign/ca.crt" \
        -subj '/C=CN/O=WorkBuddy wine build/CN=WorkBuddy wine build signing root' \
        -addext 'basicConstraints=critical,CA:TRUE' \
        -addext 'keyUsage=critical,keyCertSign,cRLSign' >/dev/null 2>&1
    openssl req -newkey rsa:3072 -sha256 -nodes \
        -keyout "$sign/leaf.key" -out "$sign/leaf.csr" \
        -subj "$leaf_subject" >/dev/null 2>&1
    printf '%s\n' 'basicConstraints=critical,CA:FALSE' \
        'keyUsage=critical,digitalSignature' \
        'extendedKeyUsage=codeSigning' >"$sign/leaf.ext"
    openssl x509 -req -in "$sign/leaf.csr" -CA "$sign/ca.crt" \
        -CAkey "$sign/ca.key" -CAcreateserial -days 1825 -sha256 \
        -extfile "$sign/leaf.ext" -out "$sign/leaf.crt" >/dev/null 2>&1
    cat "$sign/leaf.crt" "$sign/ca.crt" >"$sign/chain.crt"
    openssl pkcs12 -export -inkey "$sign/leaf.key" -in "$sign/chain.crt" \
        -name workbuddy-protocol -passout pass: -out "$sign/sign.p12"
    java -jar jsign-7.6.jar --keystore "$sign/sign.p12" --storepass '' \
        --storetype PKCS12 --alias workbuddy-protocol workbuddy-protocol.exe
}

_update_caches() {
    gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor 2>/dev/null || true
    update-desktop-database -q /usr/share/applications 2>/dev/null || true
}

post_install() {
    _update_caches
}

post_upgrade() {
    _update_caches
}

package() {
    local share="$pkgdir/usr/lib/$pkgname"

    # 版本号和标识在构建期固定进启动器，避免装完包和安装包版本不一致
    install -Dm755 "$srcdir/workbuddy-win.sh" "$pkgdir/usr/bin/$_bin"
    sed -i "s/@PKGNAME@/$pkgname/g; s/@PKGVER@/$pkgver/g; s/@APP_ID@/$_app_id/g; s/@BIN@/$_bin/g" \
        "$pkgdir/usr/bin/$_bin"

    install -Dm644 "$srcdir/workbuddy-win.desktop" \
        "$pkgdir/usr/share/applications/$pkgname.desktop"
    sed -i "s/@PKGNAME@/$pkgname/g; s/@APP_ID@/$_app_id/g; s/@BIN@/$_bin/g; s/@APP_NAME@/$_app_name/g" \
        "$pkgdir/usr/share/applications/$pkgname.desktop"

    # 程序本体放 /usr/lib（root 拥有、普通用户只读）。Electron 的用户数据全在
    # prefix 的 C:\users\<user>\.workbuddy 下，包升级不动 prefix，登录态保留。
    install -d "$share"
    cp -a "$srcdir/app" "$share/app"
    install -Dm644 "$srcdir/fonts/tahoma.ttf" "$share/fonts/tahoma.ttf"
    install -Dm644 "$srcdir/fonts/tahomabd.ttf" "$share/fonts/tahomabd.ttf"
    install -Dm644 "$srcdir/workbuddy-win-fonts.reg" "$share/workbuddy-win-fonts.reg"
    install -Dm755 "$srcdir/no-shaped-systray.so" \
        "$pkgdir/usr/lib/$pkgname/no-shaped-systray.so"
    install -Dm755 "$srcdir/tray-dismiss.exe" \
        "$pkgdir/usr/lib/$pkgname/tray-dismiss.exe"
    install -Dm755 "$srcdir/tray-dismiss-x11" \
        "$pkgdir/usr/lib/$pkgname/tray-dismiss-x11"
    install -Dm644 "$srcdir/workbuddy-protocol.exe" \
        "$pkgdir/usr/lib/$pkgname/workbuddy-protocol.exe"
    install -Dm755 "$srcdir/wb-bash-bridge.py" \
        "$pkgdir/usr/lib/$pkgname/wb-bash-bridge.py"
    install -Dm755 "$srcdir/wb-bash-proxy.exe" \
        "$pkgdir/usr/lib/$pkgname/wb-bash-proxy.exe"

    local png size
    for png in "$srcdir"/icons/*.png; do
        size=$(basename "$png" .png)
        install -Dm644 "$png" \
            "$pkgdir/usr/share/icons/hicolor/${size}x${size}/apps/$pkgname.png"
    done

    install -d "$pkgdir/usr/share/licenses/$pkgname"
    echo 'https://copilot.tencent.com/' >"$pkgdir/usr/share/licenses/$pkgname/ToS.txt"
    # 包内 tahoma.ttf/tahomabd.ttf 是文泉驿微米黑的派生物，附上原字体许可
    install -Dm644 /usr/share/licenses/wqy-microhei/FONTEXCEPTION \
        "$pkgdir/usr/share/licenses/$pkgname/FONTEXCEPTION"
}
