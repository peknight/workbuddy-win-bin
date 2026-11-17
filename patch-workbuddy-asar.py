#!/usr/bin/env python3
"""Wine 适配补丁：关掉 WorkBuddy 的 Windows 原生 toast、修 Git Bash 路径覆盖。

Wine 没有可用的 WinRT toast 实现，但 Electron 的
`Notification.isSupported()` 在 Windows 上只按系统版本判断，返回 true。
于是窗口失焦、任务完成时主进程会执行 `new Notification(...).show()`，
进程当场消失：没有异常栈、没有 crashpad 报告，日志只停在
`[TaskNotify] onEvent` 上。

这里把 app.asar 里四处 `new electron.Notification({...})` 换成空对象，
保留 `on()` / `show()` 调用点，函数照常返回 true，应用内提示和提示音不受
影响。替换等长，asar 里的 offset 不用变，只重算该文件的 SHA256 完整性。

另外 vendor 资源提取完成后，`shell-env-async.js` 会把
`CODEBUDDY_CODE_GIT_BASH_PATH` 无条件改写成自带的 PortableGit。Wine 下那条
bash 的 Cygwin 运行时 fork 不了外部进程，启动器设好的 `wb-bash-proxy.exe`
会被覆盖掉，Bash 工具又退回坏的 PortableGit。这里改成“已有可用路径就保留”。

最后，启动器为了托盘图标给 WorkBuddy 主进程挂了 `LD_PRELOAD`。Wine 里所有
子进程都会继承它，而 Cygwin/MSYS2 会把 `LD_PRELOAD` 当成自己的预加载列表，
按自己的虚拟根去打开 `/usr/lib/...so`，直接启动失败。CLI 入口启动时把这条
环境变量删掉，只影响 CLI 及其子进程，主进程的 X11 拦截库照常生效。

改 app.asar 会触发 Electron 的 EmbeddedAsarIntegrityValidation（fuse 里
记录着二进制打包时的 asar 哈希），所以同时把 WorkBuddy.exe 的 fuse 里
EnableEmbeddedAsarIntegrityValidation 从 1 翻成 0。
"""

import hashlib
import struct
import subprocess
import sys
import tempfile
import shutil
from pathlib import Path

FUSE_SENTINEL = b"dL7pKGdnNz796PbbjQWNKmHXBZaB9tsX"
# Electron fuse 顺序：RunAsNode, CookieEncryption, NodeOptionsEnv,
# NodeCliInspect, EmbeddedAsarIntegrityValidation, OnlyLoadAppFromAsar, ...
FUSE_INTEGRITY_INDEX = 4

STUB = "{show(){},close(){},on(){return this}}"
TARGETS = (
    # 任务完成/待处理通知（本次闪退的触发点）
    "new electron.Notification({title:r,body:i,silent:!0})",
    # desktop host 的 sendNotification
    "new electron.Notification({title:n,...r?{body:r}:{}})",
    # SystemNotification
    "new electron.Notification({title:e.title,body:e.body,...n?{silent:!0}:{}})",
    # 关闭到托盘的提示
    "new electron.Notification({title:require_menu_i18n.a(`windowLifecycle.closeToTrayHint.title`),"
    "body:require_menu_i18n.a(`windowLifecycle.closeToTrayHint.body`),icon:e,silent:!1})",
)

# vendor 提取完成后，只有当前没有可用路径时才写入 PortableGit。
# 新代码比原表达式短，尾部用块注释补齐，保持 asar 偏移不变。
SHELL_ENV_OLD = (
    "onExtracted:e=>{let t=path.join(e,`bin`,`bash.exe`);if(!isExistingFile(t)){"
    "clearStaleGitBashEnv(`extracted-bash-missing`),console.warn("
    "`[VendorAssets] PortableGit: bash.exe not found at ${t} after extraction, "
    "skipping env set`);return}bundledPortableGitBashPath=t,"
    "process.env.CODEBUDDY_CODE_GIT_BASH_PATH=t,logAssetEnv(`PortableGit`,"
    "`CODEBUDDY_CODE_GIT_BASH_PATH`,t,`onExtracted`)}"
)
SHELL_ENV_NEW = (
    "onExtracted:e=>{let t=path.join(e,`bin`,`bash.exe`);if(!isExistingFile(t)){"
    "clearStaleGitBashEnv(`extracted-bash-missing`),"
    "console.warn(`[VendorAssets] PortableGit bash.exe missing:${t}`);return}"
    "bundledPortableGitBashPath=t;let n=process.env.CODEBUDDY_CODE_GIT_BASH_PATH"
    "?.trim();if(n&&isExistingFile(n))return;"
    "process.env.CODEBUDDY_CODE_GIT_BASH_PATH=t}"
)

LD_PRELOAD_GUARD = b"delete process.env.LD_PRELOAD;"


def assert_js_syntax(source: bytes, label: str) -> None:
    """改写后的 JS 至少要被 node 解析通过，别把主进程改到起不来。"""
    node = shutil.which("node")
    if not node:
        print(f"js: 没有 node，跳过 {label} 语法检查")
        return
    with tempfile.NamedTemporaryFile("wb", suffix=".js") as handle:
        handle.write(source)
        handle.flush()
        result = subprocess.run(
            [node, "--check", handle.name], capture_output=True, text=True
        )
    if result.returncode != 0:
        tail = (result.stderr or result.stdout).strip().splitlines()
        raise SystemExit(f"{label} 语法检查失败：{tail[-1] if tail else '未知错误'}")


def pad_to(stub: str, width: int) -> str:
    """用块注释把桩补齐到和原表达式一样长。"""
    fill = width - len(stub) - 4
    if fill < 0:
        raise SystemExit(f"桩比原表达式长：{len(stub)} > {width}")
    return stub + "/*" + "-" * fill + "*/"


def disable_asar_integrity_fuse(exe: Path) -> None:
    data = bytearray(exe.read_bytes())
    if data.count(FUSE_SENTINEL) != 1:
        raise SystemExit(f"{exe} 里找不到唯一的 fuse sentinel")
    pos = data.find(FUSE_SENTINEL) + len(FUSE_SENTINEL)
    version, count = data[pos], data[pos + 1]
    if version != 1 or count < FUSE_INTEGRITY_INDEX + 1:
        raise SystemExit(f"fuse 格式不认识：version={version} count={count}")
    wire = bytearray(data[pos + 2 : pos + 2 + count])
    if wire[FUSE_INTEGRITY_INDEX] == ord("0"):
        print("fuse: EmbeddedAsarIntegrityValidation 已经关着")
        return
    if wire[FUSE_INTEGRITY_INDEX] != ord("1"):
        raise SystemExit(f"fuse 状态值不认识：{wire[FUSE_INTEGRITY_INDEX]!r}")
    wire[FUSE_INTEGRITY_INDEX] = ord("0")
    data[pos + 2 : pos + 2 + count] = wire
    exe.write_bytes(data)
    print("fuse: EmbeddedAsarIntegrityValidation 1 -> 0")


def read_asar(path: Path):
    raw = path.read_bytes()
    _, _, _, header_len = struct.unpack("<4I", raw[:16])
    header = raw[16 : 16 + header_len]
    data_base = 16 + header_len

    import json

    index = json.loads(header.decode("utf-8"))
    return raw, header, data_base, index


def entry_of(index: dict, parts: tuple[str, ...]) -> dict:
    node = index
    for part in parts:
        node = node["files"][part]
    return node


def replace_in_entry(
    path: Path,
    parts: tuple[str, ...],
    old: bytes,
    new: bytes,
    label: str,
) -> None:
    """等长改写 asar 里的一个文件，只重算它自己的 integrity。"""
    raw, header, data_base, index = read_asar(path)
    entry = entry_of(index, parts)
    offset = data_base + int(entry["offset"])
    size = int(entry["size"])
    original = raw[offset : offset + size]
    if len(original) != size:
        raise SystemExit(f"app.asar 里的 {label} 越界")

    integrity = entry.get("integrity")
    if not integrity:
        raise SystemExit(f"{label} 没有 integrity 记录，不能安全改写")
    if hashlib.sha256(original).hexdigest() != integrity["hash"]:
        raise SystemExit(f"{label} 的 SHA256 与 header 不符，asar 结构异常")

    if original.count(old) != 1:
        raise SystemExit(f"{label} 里没找到唯一的待替换片段")
    patched = original.replace(old, new)

    if len(patched) != size:
        raise SystemExit(f"{label} 替换后长度变了，会打乱 asar 偏移")

    block_size = int(integrity.get("blockSize") or (4 << 20))
    blocks = [
        hashlib.sha256(patched[i : i + block_size]).hexdigest()
        for i in range(0, len(patched), block_size)
    ]
    if len(blocks) != 1:
        raise SystemExit(f"{label} 跨多个 integrity block，本脚本还不支持")
    new_hash = blocks[0]

    old_hex = integrity["hash"].encode("ascii")
    new_header = header.replace(old_hex, new_hash.encode("ascii"))
    if new_header == header:
        raise SystemExit(f"{label}: header 里没有可替换的旧哈希")
    if len(new_header) != len(header):
        raise SystemExit(f"{label}: 哈希替换后 header 长度变了")

    with path.open("r+b") as handle:
        handle.seek(16)
        handle.write(new_header)
        handle.seek(offset)
        handle.write(patched)

    check = path.read_bytes()
    written = check[offset : offset + size]
    if hashlib.sha256(written).hexdigest() != new_hash:
        raise SystemExit(f"{label}: 写回后校验失败")
    assert_js_syntax(written, label)
    print(f"asar: {label} 已改写，integrity -> {new_hash[:12]}...")


def patch_asar(path: Path) -> None:
    raw, _, data_base, index = read_asar(path)
    entry = entry_of(index, ("main", "main-bootstrap.js"))
    offset = data_base + int(entry["offset"])
    original = raw[offset : offset + int(entry["size"])]

    if b"new electron.Notification(" not in original:
        if original.count(STUB.encode("utf-8")) == len(TARGETS):
            print("asar: main-bootstrap.js 已经打过补丁")
            return
        raise SystemExit("app.asar 里既没有原来的 Notification，也没有本脚本的桩")

    for target in TARGETS:
        old = target.encode("utf-8")
        replace_in_entry(
            path,
            ("main", "main-bootstrap.js"),
            old,
            pad_to(STUB, len(old)).encode("utf-8"),
            f"main-bootstrap.js ({target[:40]}...)",
        )
    if b"new electron.Notification(" in path.read_bytes():
        raise SystemExit("还有没替换掉的 electron.Notification")


def patch_shell_env(path: Path) -> None:
    raw = path.read_bytes()
    if SHELL_ENV_NEW.encode("utf-8") in raw:
        print("asar: shell-env-async.js 已经打过补丁")
        return
    old = SHELL_ENV_OLD.encode("utf-8")
    stub = SHELL_ENV_NEW.encode("utf-8") + b"/*" + b"-" * (len(old) - len(SHELL_ENV_NEW) - 4) + b"*/"
    if len(stub) != len(old):
        raise SystemExit("shell-env 桩长度算错了")
    replace_in_entry(
        path,
        ("main", "shell-env-async.js"),
        old,
        stub,
        "shell-env-async.js (vendor 覆盖 Git Bash 路径)",
    )


def patch_cli_launchers(app: Path) -> None:
    """CLI 进程启动时摘掉 LD_PRELOAD，避免 Cygwin 子进程加载宿主的 X11 库。"""
    cli = app / "resources" / "app.asar.unpacked" / "cli"
    targets = (
        cli / "dist" / "codebuddy-lite-wb.mjs",
        cli / "bin" / "codebuddy",
        cli / "bin" / "cbc-prewarm",
        cli / "bin" / "codebuddy-lowmem",
    )
    for target in targets:
        data = target.read_bytes()
        if LD_PRELOAD_GUARD in data[:4096]:
            print(f"cli: {target.name} 已经打过补丁")
            continue
        if data.startswith(b"#!"):
            # 有 shebang 的入口必须插在 shebang 之后，否则脚本不再可执行
            head, sep, rest = data.partition(b"\n")
            if not sep:
                raise SystemExit(f"{target} 找不到 shebang 行尾")
            eol = b"\r\n" if head.endswith(b"\r") else b"\n"
            patched = head + sep + LD_PRELOAD_GUARD + eol + rest
        else:
            patched = LD_PRELOAD_GUARD + b"\n" + data
        target.write_bytes(patched)
        print(f"cli: {target.name} 已在启动时清除 LD_PRELOAD")


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit(f"用法: {Path(sys.argv[0]).name} <app 目录>")
    app = Path(sys.argv[1])
    exe = app / "WorkBuddy.exe"
    asar = app / "resources" / "app.asar"
    for file in (exe, asar):
        if not file.is_file():
            raise SystemExit(f"缺少 {file}")
    disable_asar_integrity_fuse(exe)
    patch_asar(asar)
    patch_shell_env(asar)
    patch_cli_launchers(app)


if __name__ == "__main__":
    main()
