#!/usr/bin/env python3
"""Wine 适配补丁：关掉 WorkBuddy 的 Windows 原生 toast 和 asar 完整性校验。

Wine 没有可用的 WinRT toast 实现，但 Electron 的
`Notification.isSupported()` 在 Windows 上只按系统版本判断，返回 true。
于是窗口失焦、任务完成时主进程会执行 `new Notification(...).show()`，
进程当场消失：没有异常栈、没有 crashpad 报告，日志只停在
`[TaskNotify] onEvent` 上。

这里把 app.asar 里四处 `new electron.Notification({...})` 换成空对象，
保留 `on()` / `show()` 调用点，函数照常返回 true，应用内提示和提示音不受
影响。替换等长，asar 里的 offset 不用变，只重算该文件的 SHA256 完整性。

改 app.asar 会触发 Electron 的 EmbeddedAsarIntegrityValidation（fuse 里
记录着二进制打包时的 asar 哈希），所以同时把 WorkBuddy.exe 的 fuse 里
EnableEmbeddedAsarIntegrityValidation 从 1 翻成 0。
"""

import hashlib
import struct
import sys
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


def patch_asar(path: Path) -> None:
    raw = path.read_bytes()
    _, _, _, header_len = struct.unpack("<4I", raw[:16])
    header = raw[16 : 16 + header_len]
    data_base = 16 + header_len

    import json

    index = json.loads(header.decode("utf-8"))
    entry = index["files"]["main"]["files"]["main-bootstrap.js"]
    offset = data_base + int(entry["offset"])
    size = int(entry["size"])
    original = raw[offset : offset + size]
    if len(original) != size:
        raise SystemExit("app.asar 里的 main-bootstrap.js 越界")

    integrity = entry.get("integrity")
    if not integrity:
        raise SystemExit("main-bootstrap.js 没有 integrity 记录，不能安全改写")
    if hashlib.sha256(original).hexdigest() != integrity["hash"]:
        raise SystemExit("main-bootstrap.js 的 SHA256 与 header 不符，asar 结构异常")

    patched = original
    if b"new electron.Notification(" not in original:
        if original.count(STUB.encode("utf-8")) == len(TARGETS):
            print("asar: main-bootstrap.js 已经打过补丁")
            return
        raise SystemExit("app.asar 里既没有原来的 Notification，也没有本脚本的桩")
    for target in TARGETS:
        old = target.encode("utf-8")
        if patched.count(old) != 1:
            raise SystemExit(f"app.asar 里没找到唯一的待替换片段：{target}")
        patched = patched.replace(old, pad_to(STUB, len(old)).encode("utf-8"))

    if len(patched) != size:
        raise SystemExit("替换后长度变了，会打乱 asar 偏移")
    if b"new electron.Notification(" in patched:
        raise SystemExit("还有没替换掉的 electron.Notification")

    block_size = int(integrity.get("blockSize") or (4 << 20))
    blocks = [
        hashlib.sha256(patched[i : i + block_size]).hexdigest()
        for i in range(0, len(patched), block_size)
    ]
    if len(blocks) != 1:
        raise SystemExit("main-bootstrap.js 跨多个 integrity block，本脚本还不支持")
    new_hash = blocks[0]

    old_hex = integrity["hash"].encode("ascii")
    new_header = header.replace(old_hex, new_hash.encode("ascii"))
    if new_header == header:
        raise SystemExit("header 里没有可替换的旧哈希")
    if len(new_header) != len(header):
        raise SystemExit("哈希替换后 header 长度变了")

    with path.open("r+b") as handle:
        handle.seek(16)
        handle.write(new_header)
        handle.seek(offset)
        handle.write(patched)

    check = path.read_bytes()
    written = check[offset : offset + size]
    if hashlib.sha256(written).hexdigest() != new_hash:
        raise SystemExit("写回后校验失败")
    print(f"asar: 4 处 Windows toast 已替换为空实现，integrity -> {new_hash[:12]}...")


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


if __name__ == "__main__":
    main()
