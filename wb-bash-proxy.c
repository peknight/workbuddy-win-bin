/*
 * Windows-side bash proxy for WorkBuddy running under Wine.
 *
 * WorkBuddy invokes Git Bash as `bash.exe -c "<script>"`.  The PortableGit
 * shipped with the app uses an old Cygwin runtime that cannot fork under Wine,
 * so external commands do not run at all.  This proxy is a native Windows PE:
 * it forwards the script to a small daemon on the Linux host and relays the
 * daemon's stdout, stderr and exit status back to WorkBuddy.
 *
 * The daemon authenticates requests with a per-launch random token and only
 * listens on 127.0.0.1.  See wb-bash-bridge.py for the host side.
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define DEFAULT_PORT 39876
#define MAX_REQUEST (32 * 1024 * 1024)

static void die(const char *msg) {
    fprintf(stderr, "wb-bash-proxy: %s\n", msg);
    exit(127);
}

static char *wide_to_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) {
        return NULL;
    }
    char *s = (char *)malloc((size_t)n);
    if (!s) {
        return NULL;
    }
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) {
        free(s);
        return NULL;
    }
    return s;
}

static char *read_all_stdin(void) {
    size_t cap = 4096, len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        return NULL;
    }
    for (;;) {
        if (len + 2048 + 1 > cap) {
            cap *= 2;
            char *next = (char *)realloc(buf, cap);
            if (!next) {
                free(buf);
                return NULL;
            }
            buf = next;
        }
        size_t got = fread(buf + len, 1, 2048, stdin);
        len += got;
        if (got == 0) {
            break;
        }
    }
    buf[len] = '\0';
    return buf;
}

static int append_json_string(char **out, size_t *cap, size_t *len, const char *s) {
    const unsigned char *p = (const unsigned char *)s;

    if (*len + 2 > *cap) {
        return 0;
    }
    (*out)[(*len)++] = '"';
    while (*p) {
        const char *esc = NULL;
        char tmp[8];
        switch (*p) {
            case '"': esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\b': esc = "\\b"; break;
            case '\f': esc = "\\f"; break;
            case '\n': esc = "\\n"; break;
            case '\r': esc = "\\r"; break;
            case '\t': esc = "\\t"; break;
            default:
                if (*p < 0x20) {
                    snprintf(tmp, sizeof(tmp), "\\u%04x", *p);
                    esc = tmp;
                }
                break;
        }
        size_t need = esc ? strlen(esc) : 1;
        if (*len + need + 2 > *cap) {
            return 0;
        }
        if (esc) {
            memcpy(*out + *len, esc, need);
            *len += need;
        } else {
            (*out)[(*len)++] = (char)*p;
        }
        p++;
    }
    (*out)[(*len)++] = '"';
    (*out)[*len] = '\0';
    return 1;
}

static int send_all(SOCKET sock, const char *buf, int len) {
    while (len > 0) {
        int n = send(sock, buf, len, 0);
        if (n <= 0) {
            return 0;
        }
        buf += n;
        len -= n;
    }
    return 1;
}

static int recv_all(SOCKET sock, char *buf, int len) {
    while (len > 0) {
        int n = recv(sock, buf, len, 0);
        if (n <= 0) {
            return 0;
        }
        buf += n;
        len -= n;
    }
    return 1;
}

static uint32_t read_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void write_be32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

/*
 * Write one stdout/stderr chunk.  A console handle converts bytes using the
 * active code page, which turns UTF-8 from the Linux bridge into mojibake, so
 * console output goes through WriteConsoleW instead.  Up to three bytes of a
 * multibyte sequence may arrive in the next frame; they are kept in *pending.
 */
static void write_stream(FILE *stream, const char *buf, size_t n,
                         char pending[4], size_t *pending_len) {
    intptr_t os_handle = _get_osfhandle(_fileno(stream));
    DWORD mode;

    if (os_handle == -1 || !GetConsoleMode((HANDLE)os_handle, &mode)) {
        if (*pending_len) {
            fwrite(pending, 1, *pending_len, stream);
            *pending_len = 0;
        }
        fwrite(buf, 1, n, stream);
        fflush(stream);
        return;
    }

    size_t total = *pending_len + n;
    char *joined = (char *)malloc(total ? total : 1);
    if (!joined) {
        return;
    }
    if (*pending_len) {
        memcpy(joined, pending, *pending_len);
    }
    if (n) {
        memcpy(joined + *pending_len, buf, n);
    }

    /* Hold back a trailing partial UTF-8 sequence until the next frame. */
    size_t usable = total;
    for (size_t back = 1; back <= 3 && back <= total; back++) {
        unsigned char c = (unsigned char)joined[total - back];
        if ((c & 0xC0) == 0x80) {
            continue;
        }
        size_t need = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        if (need > back) {
            usable = total - back;
        }
        break;
    }
    *pending_len = total - usable;
    if (*pending_len) {
        memcpy(pending, joined + usable, *pending_len);
    }

    if (usable == 0) {
        free(joined);
        return;
    }

    int wlen = MultiByteToWideChar(CP_UTF8, 0, joined, (int)usable, NULL, 0);
    if (wlen > 0) {
        wchar_t *wide = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
        if (wide) {
            if (MultiByteToWideChar(CP_UTF8, 0, joined, (int)usable, wide, wlen) == wlen) {
                DWORD written = 0;
                WriteConsoleW((HANDLE)os_handle, wide, (DWORD)wlen, &written, NULL);
            }
            free(wide);
        }
    }
    free(joined);
}

/*
 * Find the script argument.  WorkBuddy normally uses "-c", but "-lc" and
 * "--command" are also accepted so the proxy keeps working if the invocation
 * changes.
 */
static char *script_from_args(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-c") == 0 || strcmp(a, "--command") == 0) {
            return (i + 1 < argc) ? argv[i + 1] : NULL;
        }
        if (a[0] == '-' && a[1] != '\0' && strchr(a, 'c') != NULL) {
            return (i + 1 < argc) ? argv[i + 1] : NULL;
        }
    }
    return NULL;
}

int wmain(int argc, wchar_t **wargv) {
    WSADATA wsa;
    SOCKET sock = INVALID_SOCKET;
    char *script = NULL;
    char *cwd = NULL;
    char *prefix = NULL;
    char *request = NULL;
    int rc = 127;
    char out_pending[4] = {0};
    char err_pending[4] = {0};
    size_t out_pending_len = 0;
    size_t err_pending_len = 0;

    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);

    char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (!argv) {
        die("out of memory");
    }
    for (int i = 0; i < argc; i++) {
        argv[i] = wide_to_utf8(wargv[i]);
        if (!argv[i]) {
            die("cannot decode command line");
        }
    }

    const char *arg_script = script_from_args(argc, argv);
    if (arg_script) {
        script = _strdup(arg_script);
    } else if (_isatty(_fileno(stdin)) == 0) {
        script = read_all_stdin();
    }
    if (!script) {
        fprintf(stderr, "wb-bash-proxy: expected -c <script>\n");
        rc = 2;
        goto done;
    }

    wchar_t cwd_w[32768];
    DWORD cwd_len = GetCurrentDirectoryW((DWORD)(sizeof(cwd_w) / sizeof(cwd_w[0])), cwd_w);
    if (cwd_len > 0 && cwd_len < sizeof(cwd_w) / sizeof(cwd_w[0])) {
        cwd = wide_to_utf8(cwd_w);
    }
    if (!cwd) {
        cwd = _strdup("");
    }

    const char *prefix_env = getenv("WINEPREFIX");
    prefix = _strdup(prefix_env ? prefix_env : "");
    const char *token = getenv("WORKBUDDY_BASH_BRIDGE_TOKEN");
    if (!token) {
        token = "";
    }
    const char *port_env = getenv("WORKBUDDY_BASH_BRIDGE_PORT");
    int port = port_env && *port_env ? atoi(port_env) : DEFAULT_PORT;

    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        die("WSAStartup failed");
    }
    sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        die("socket failed");
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    DWORD connect_timeout_ms = 3000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
               (const char *)&connect_timeout_ms, sizeof(connect_timeout_ms));
    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "wb-bash-proxy: cannot reach Linux bridge on 127.0.0.1:%d\n", port);
        goto done;
    }

    /*
     * The proxy must be able to wait for a command that produces no output for
     * minutes.  Only the TCP connect is bounded (above); after it succeeds the
     * socket blocks indefinitely.  WorkBuddy enforces the command timeout by
     * killing this process, which closes the connection and makes the daemon
     * kill the host-side process group.
     */
    DWORD no_timeout_ms = 0;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
               (const char *)&no_timeout_ms, sizeof(no_timeout_ms));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               (const char *)&no_timeout_ms, sizeof(no_timeout_ms));

    size_t cap = strlen(script) * 6 + strlen(cwd) * 6 + strlen(prefix) * 6 + 512;
    request = (char *)malloc(cap);
    if (!request) {
        die("out of memory");
    }
    size_t len = 0;
    len += (size_t)snprintf(request + len, cap - len, "{\"token\":");
    if (!append_json_string(&request, &cap, &len, token)) {
        die("request too large");
    }
    len += (size_t)snprintf(request + len, cap - len, ",\"cwd\":");
    if (!append_json_string(&request, &cap, &len, cwd)) {
        die("request too large");
    }
    len += (size_t)snprintf(request + len, cap - len, ",\"prefix\":");
    if (!append_json_string(&request, &cap, &len, prefix)) {
        die("request too large");
    }
    len += (size_t)snprintf(request + len, cap - len, ",\"script\":");
    if (!append_json_string(&request, &cap, &len, script)) {
        die("request too large");
    }
    len += (size_t)snprintf(request + len, cap - len, "}");
    if (len > MAX_REQUEST) {
        die("request too large");
    }

    unsigned char hdr[4];
    write_be32(hdr, (uint32_t)len);
    if (!send_all(sock, (const char *)hdr, 4) || !send_all(sock, request, (int)len)) {
        die("failed to send request");
    }

    for (;;) {
        unsigned char frame[5];
        if (!recv_all(sock, (char *)frame, 5)) {
            fprintf(stderr, "wb-bash-proxy: bridge closed the connection\n");
            break;
        }
        unsigned char type = frame[0];
        uint32_t n = read_be32(frame + 1);
        if (n > MAX_REQUEST) {
            fprintf(stderr, "wb-bash-proxy: oversized response frame\n");
            break;
        }
        char *payload = (char *)malloc(n ? n : 1);
        if (!payload) {
            die("out of memory");
        }
        if (n && !recv_all(sock, payload, (int)n)) {
            free(payload);
            fprintf(stderr, "wb-bash-proxy: truncated response\n");
            break;
        }
        if (type == 1) {
            write_stream(stdout, payload, n, out_pending, &out_pending_len);
        } else if (type == 2) {
            write_stream(stderr, payload, n, err_pending, &err_pending_len);
        } else if (type == 3 && n == 4) {
            rc = (int)read_be32((const unsigned char *)payload);
            free(payload);
            break;
        }
        free(payload);
    }

done:
    if (out_pending_len) {
        fwrite(out_pending, 1, out_pending_len, stdout);
        fflush(stdout);
    }
    if (err_pending_len) {
        fwrite(err_pending, 1, err_pending_len, stderr);
        fflush(stderr);
    }
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
    }
    WSACleanup();
    for (int i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
    free(script);
    free(cwd);
    free(prefix);
    free(request);
    return rc;
}
