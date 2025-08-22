// server.c
// 빌드: gcc -O2 -Wall -std=c11 -pthread -o server server.c -lmysqlclient
// 실행: ./server 8080

#define _GNU_SOURCE
#include "queries.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <strings.h>   // strncasecmp
#include <ctype.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>

#define BUF_SIZE   4096
#define PATH_BUF   512
#define WWW_ROOT   "www"      // 정적 파일 루트 (HTML/CSS/이미지)

// === RAW TCP 목적지 (고정용) 필요시 수정 ===

#define FWD_IP   "10.10.14.87"
#define FWD_PORT 6000

// === 콘솔 로그(printf) 매크로 ===
#define LOG(...) do { printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

/* ===== 내부 함수 프로토타입 ===== */
static void* request_handler(void* arg);
static void   send_error(FILE* out, int code, const char* msg);
static void   send_json (FILE* out, int code, const char* body);
static void   send_file (FILE* out, const char* filename);
static int    safe_path(const char* path);
static int    get_query_param(const char* path, const char* key, char* out, size_t outsz);
static void   url_decode(const char* src, char* dst, size_t dstsz);

// 경로 매칭 헬퍼: "/api/xxx", "/api/xxx/" 또는 쿼리까지 허용
static int path_is(const char* path, const char* pfx){
    size_t n = strlen(pfx);
    return strncmp(path, pfx, n)==0 && (path[n]=='\0' || path[n]=='/' || path[n]=='?');
}

// RAW TCP forward 프로토타입
static int connect_with_timeout(int sock, const struct sockaddr *addr, socklen_t addrlen, int timeout_ms);
static int send_all(int sock, const void* buf, size_t len);
static int forward_raw_tcp_fixed(const void* body, size_t content_len, int timeout_ms);
static int forward_raw_tcp_to(const char* ip, int port, const void* body, size_t len, int timeout_ms);

/* ===== 메인 ===== */
int main(int argc, char* argv[]) {
    int serv_sock;
    struct sockaddr_in serv_adr;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    serv_sock = socket(PF_INET, SOCK_STREAM, 0);
    if (serv_sock == -1) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&serv_adr, 0, sizeof(serv_adr));
    serv_adr.sin_family = AF_INET;
    serv_adr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_adr.sin_port = htons(atoi(argv[1]));

    if (bind(serv_sock, (struct sockaddr*)&serv_adr, sizeof(serv_adr)) == -1) {
        perror("bind"); close(serv_sock); return 1;
    }
    if (listen(serv_sock, 64) == -1) {
        perror("listen"); close(serv_sock); return 1;
    }

    printf("✅ Server running: http://0.0.0.0:%s\n", argv[1]);
    printf("Routes: /kiosk /staff /dashboard  |  /static/*\n");
    printf("APIs  : /api/menu /api/order?items=1*2,3*1 /api/orders /api/pay?id=123\n");
    printf("Extra : /api/forward (POST raw→%s:%d), /api/forward_to?ip=a.b.c.d&port=NNNN (POST raw)\n", FWD_IP, FWD_PORT);
    fflush(stdout);

    while (1) {
        struct sockaddr_in clnt_adr; socklen_t clnt_adr_sz = sizeof(clnt_adr);
        int clnt_sock = accept(serv_sock, (struct sockaddr*)&clnt_adr, &clnt_adr_sz);
        if (clnt_sock == -1) continue;

        // 쓰레드에 안전하게 소켓 fd 전달
        int* pfd = malloc(sizeof(int));
        if (!pfd) { close(clnt_sock); continue; }
        *pfd = clnt_sock;

        pthread_t tid;
        if (pthread_create(&tid, NULL, request_handler, pfd) != 0) {
            close(clnt_sock);
            free(pfd);
            continue;
        }
        pthread_detach(tid);
    }
    // close(serv_sock);  // 도달하지 않음
    return 0;
}

/* ===== 요청 핸들러 ===== */
static void* request_handler(void* arg) {
    int clnt_sock = *((int*)arg);
    free(arg);

    FILE* in  = fdopen(clnt_sock, "r");
    FILE* out = fdopen(dup(clnt_sock), "w");
    if (!in || !out) { if(in) fclose(in); if(out) fclose(out); close(clnt_sock); return NULL; }

    char line[BUF_SIZE];
    char method[16], path[PATH_BUF];
    if (!fgets(line, sizeof(line), in)) { fclose(in); fclose(out); return NULL; }
    sscanf(line, "%15s %511s", method, path);

    // === 콘솔에 요청 URL 찍기 ===
    LOG("[REQ] %s %s", method, path);

    // --- 헤더 파싱 (Content-Length 추출) ---
    long content_len = 0;
    while (fgets(line, sizeof(line), in)) {
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) break;
        if (!strncasecmp(line, "Content-Length:", 15)) {
            content_len = strtol(line+15, NULL, 10);
        }
    }

    // --- 바디 읽기 (POST/PUT일 때) ---
    char* body = NULL;
    if (!strcasecmp(method,"POST") || !strcasecmp(method,"PUT")) {
        if (content_len > 0) {
            body = (char*)malloc((size_t)content_len);
            if (body) {
                size_t rd = 0;
                while (rd < (size_t)content_len) {
                    size_t n = fread(body + rd, 1, (size_t)content_len - rd, in);
                    if (n == 0) break;
                    rd += n;
                }
            }
        }
    }

    // --- CORS 프리플라이트 처리 ---
    if (!strcasecmp(method, "OPTIONS")) {
        fprintf(out,
            "HTTP/1.1 204 No Content\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n"
            "Content-Length: 0\r\n\r\n");
        fflush(out);
        if (body) free(body);
        fclose(in); fclose(out);
        return NULL;
    }

    /* ===== API 라우팅 ===== */

    // 0) RAW TCP로 바디 그대로 전달 (고정 목적지)
    if (path_is(path, "/api/forward") && !strcasecmp(method,"POST")) {
        LOG("[ROUTE] /api/forward content_len=%ld", content_len);
        LOG("[FWD] connect to %s:%d, len=%ld", FWD_IP, FWD_PORT, content_len);
        int ok = forward_raw_tcp_fixed(body, (size_t)content_len, 2000); // 2초 타임아웃
        fprintf(out,
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: application/json; charset=utf-8\r\n"
            "Access-Control-Allow-Origin: *\r\n\r\n"
            "{\"ok\":%s}",
            (ok==0)?200:502, (ok==0)?"OK":"Bad Gateway", (ok==0)?"true":"false");
        fflush(out);
        if (body) free(body);
        fclose(in); fclose(out);
        return NULL;
    }

    // 0-1) RAW TCP 목적지 동적 지정: /api/forward_to?ip=1.2.3.4&port=9000
    if (path_is(path, "/api/forward_to") && !strcasecmp(method,"POST")) {
        char ip[64]={0}, port_s[16]={0};
        if (!get_query_param(path,"ip",ip,sizeof(ip)) || !get_query_param(path,"port",port_s,sizeof(port_s))) {
            fprintf(out,
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: application/json; charset=utf-8\r\n"
                "Access-Control-Allow-Origin: *\r\n\r\n"
                "{\"ok\":false,\"msg\":\"need ip & port\"}");
            fflush(out);
            if (body) free(body);
            fclose(in); fclose(out);
            return NULL;
        }
        int port = atoi(port_s);
        LOG("[ROUTE] /api/forward_to ip=%s port=%d len=%ld", ip, port, content_len);
        LOG("[FWD] connect to %s:%d, len=%ld", ip, port, content_len);
        int ok = forward_raw_tcp_to(ip, port, body, (size_t)content_len, 2000);
        fprintf(out,
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: application/json; charset=utf-8\r\n"
            "Access-Control-Allow-Origin: *\r\n\r\n"
            "{\"ok\":%s}",
            (ok==0)?200:502, (ok==0)?"OK":"Bad Gateway", (ok==0)?"true":"false");
        fflush(out);
        if (body) free(body);
        fclose(in); fclose(out);
        return NULL;
    }

    // 1) 기존 API들
    if (path_is(path, "/api/menu")) {
        char* res = get_menu();
        send_json(out, 200, res);
        free(res);
    }
    else if (path_is(path, "/api/order")) {
        // GET /api/order?items=1*2,3*1
        char items[2048] = {0};
        int has_items = get_query_param(path, "items", items, sizeof(items));

        // --- URL/아이템 로그 ---
        if (has_items) LOG("[ORDER] %s  (items=%s)", path, items);
        else           LOG("[ORDER] %s  (items=NONE)", path);

        if (has_items) {
            // DB 저장
            char* res = create_order(items);   // items는 "1*2,3*1" 형식
            send_json(out, 200, res);

            // === 주문 들어오면 10.10.14.50:9000으로 RAW TCP 전송 ===
            char payload[4096];
            snprintf(payload, sizeof(payload), "ORDER %s\n", items);
            LOG("[FWD] (order) -> %s:%d : %s", FWD_IP, FWD_PORT, payload);
            int ok2 = forward_raw_tcp_to(FWD_IP, FWD_PORT, payload, strlen(payload), 2000);
            if (ok2 != 0) { fprintf(stderr, "[FWD] (order) send failed errno=%d (%s)\n", errno, strerror(errno)); fflush(stderr); }
            else          { LOG("[FWD] (order) send ok"); }

            free(res);
        } else {
            send_json(out, 400, "{\"ok\":false,\"msg\":\"items required\"}");
        }
    }
    else if (path_is(path, "/api/orders")) {
        char* res = get_orders();
        send_json(out, 200, res);
        free(res);
    }
    else if (path_is(path, "/api/pay")) {
        char idbuf[64] = {0};
        if (get_query_param(path, "id", idbuf, sizeof(idbuf))) {
            LOG("[PAY] %s  (id=%s)", path, idbuf);
            char* res = pay_order(idbuf);
            send_json(out, 200, res);
            free(res);
        } else {
            send_json(out, 400, "{\"ok\":false,\"msg\":\"id required\"}");
        }
    }
    /* ===== HTML / 정적 파일 ===== */
    else if (path_is(path, "/kiosk")) {
        char file[PATH_BUF]; snprintf(file, sizeof(file), "%s/kiosk.html", WWW_ROOT);
        send_file(out, file);
    }
    else if (path_is(path, "/staff")) {
        char file[PATH_BUF]; snprintf(file, sizeof(file), "%s/staff.html", WWW_ROOT);
        send_file(out, file);
    }
    else if (path_is(path, "/dashboard")) {
        char file[PATH_BUF]; snprintf(file, sizeof(file), "%s/dashboard.html", WWW_ROOT);
        send_file(out, file);
    }
    else {
        // 정적 파일: /static/.. 또는 /index.html 등
        if (!safe_path(path)) {
            send_error(out, 403, "Forbidden");
        } else {
            char file[PATH_BUF];
            snprintf(file, sizeof(file), "%s%s", WWW_ROOT, path);
            // 루트로 접근하면 /kiosk 로 리다이렉트
            if (strcmp(path, "/") == 0) {
                snprintf(file, sizeof(file), "%s/kiosk.html", WWW_ROOT);
            }
            send_file(out, file);
        }
    }

    if (body) free(body);
    fclose(in);
    fclose(out);
    return NULL;
}

/* ===== RAW TCP Forward Helpers (구현) ===== */
static int connect_with_timeout(int sock, const struct sockaddr *addr, socklen_t addrlen, int timeout_ms) {
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(sock, addr, addrlen);
    if (rc == 0) { fcntl(sock, F_SETFL, flags); return 0; }
    if (errno != EINPROGRESS) return -1;

    fd_set wfds; FD_ZERO(&wfds); FD_SET(sock, &wfds);
    struct timeval tv = { .tv_sec = timeout_ms/1000, .tv_usec = (timeout_ms%1000)*1000 };
    rc = select(sock+1, NULL, &wfds, NULL, &tv);
    if (rc <= 0) return -1;

    int soerr=0; socklen_t slen=sizeof(soerr);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerr, &slen) < 0 || soerr != 0) return -1;

    fcntl(sock, F_SETFL, flags);
    return 0;
}

static int send_all(int sock, const void* buf, size_t len) {
    const char* p = (const char*)buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(sock, p, left, 0);
        if (n <= 0) return -1;
        p += n; left -= n;
    }
    return 0;
}

static int forward_raw_tcp_fixed(const void* body, size_t content_len, int timeout_ms) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;

    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(FWD_PORT);
    if (inet_pton(AF_INET, FWD_IP, &a.sin_addr) != 1) { close(s); return -1; }

    if (connect_with_timeout(s, (struct sockaddr*)&a, sizeof(a), timeout_ms) < 0) {
        LOG("[FWD] connect error errno=%d (%s)", errno, strerror(errno));
        close(s); return -1;
    }

    // (옵션) 전송/수신 타임아웃
    struct timeval tv = { .tv_sec = timeout_ms/1000, .tv_usec = (timeout_ms%1000)*1000 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    int ok = 0;
    if (content_len > 0 && body) ok = send_all(s, body, content_len);

    close(s);
    return ok;
}

static int forward_raw_tcp_to(const char* ip, int port, const void* body, size_t len, int timeout_ms){
    int s = socket(AF_INET, SOCK_STREAM, 0); if(s<0) return -1;
    struct sockaddr_in a; memset(&a,0,sizeof(a));
    a.sin_family=AF_INET; a.sin_port=htons(port);
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) { close(s); return -1; }

    if (connect_with_timeout(s, (struct sockaddr*)&a, sizeof(a), timeout_ms) < 0) {
        LOG("[FWD] connect error to %s:%d errno=%d (%s)", ip, port, errno, strerror(errno));
        close(s); return -1;
    }

    int ok = 0;
    if (len > 0 && body) ok = send_all(s, body, len);

    close(s);
    return ok;
}

/* ===== 헬퍼들 ===== */

static void send_error(FILE* out, int code, const char* msg) {
    fprintf(out, "HTTP/1.1 %d Error\r\n", code);
    fprintf(out, "Content-Type: text/plain; charset=utf-8\r\n\r\n");
    fprintf(out, "%s\n", msg ? msg : "");
    fflush(out);
}

static void send_json(FILE* out, int code, const char* body) {
    fprintf(out, "HTTP/1.1 %d OK\r\n", code);
    fprintf(out, "Content-Type: application/json; charset=utf-8\r\n");
    fprintf(out, "Access-Control-Allow-Origin: *\r\n\r\n");
    fprintf(out, "%s", body ? body : "{}");
    fflush(out);
}

static void send_file(FILE* out, const char* filename) {
    int fd = open(filename, O_RDONLY);
    if (fd == -1) { send_error(out, 404, "File Not Found"); return; }

    // Content-Type
    fprintf(out, "HTTP/1.1 200 OK\r\n");
    if      (strstr(filename, ".html")) fprintf(out, "Content-Type: text/html; charset=utf-8\r\n\r\n");
    else if (strstr(filename, ".css"))  fprintf(out, "Content-Type: text/css; charset=utf-8\r\n\r\n");
    else if (strstr(filename, ".js"))   fprintf(out, "Content-Type: application/javascript; charset=utf-8\r\n\r\n");
    else if (strstr(filename, ".png"))  fprintf(out, "Content-Type: image/png\r\n\r\n");
    else if (strstr(filename, ".jpg") || strstr(filename, ".jpeg")) fprintf(out, "Content-Type: image/jpeg\r\n\r\n");
    else if (strstr(filename, ".gif"))  fprintf(out, "Content-Type: image/gif\r\n\r\n");
    else                                fprintf(out, "Content-Type: application/octet-stream\r\n\r\n");

    char buf[BUF_SIZE];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)n, out);
    }
    close(fd);
    fflush(out);
}

// ".." 같은 경로 탈출 차단
static int safe_path(const char* path) {
    if (!path) return 0;
    if (strstr(path, "..")) return 0;
    return 1;
}

/* path에서 쿼리스트링 key=val 파싱하여 out에 URL-decoded 값 저장
   예: get_query_param("/api/order?items=1*2,3*1","items",out,sz) */
static int get_query_param(const char* path, const char* key, char* out, size_t outsz) {
    const char* q = strchr(path, '?');
    if (!q) return 0;
    q++; // '?' 다음

    size_t keylen = strlen(key);

    while (*q) {
        // key 시작
        if (strncmp(q, key, keylen) == 0 && q[keylen] == '=') {
            q += keylen + 1;
            // 값의 끝 찾기(& 또는 문자열 끝)
            const char* end = q;
            while (*end && *end != '&') end++;
            size_t rawlen = (size_t)(end - q);
            char tmp[2048];
            if (rawlen >= sizeof(tmp)) rawlen = sizeof(tmp) - 1;
            memcpy(tmp, q, rawlen);
            tmp[rawlen] = '\0';
            url_decode(tmp, out, outsz);
            return 1;
        }
        // 다음 파라미터로 이동
        while (*q && *q != '&') q++;
        if (*q == '&') q++;
    }
    return 0;
}

static int hexval(char c) {
    if ('0' <= c && c <= '9') return c - '0';
    if ('a' <= c && c <= 'f') return 10 + (c - 'a');
    if ('A' <= c && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* 간단 URL decode (%XX, '+') */
static void url_decode(const char* src, char* dst, size_t dstsz) {
    size_t di = 0;
    for (size_t si = 0; src[si] && di + 1 < dstsz; si++) {
        if (src[si] == '%' && src[si+1] && src[si+2]) {
            int h1 = hexval(src[si+1]);
            int h2 = hexval(src[si+2]);
            if (h1 >= 0 && h2 >= 0) {
                dst[di++] = (char)((h1 << 4) | h2);
                si += 2;
                continue;
            }
        }
        if (src[si] == '+') { dst[di++] = ' '; continue; }
        dst[di++] = src[si];
    }
    dst[di] = '\0';
}
// "1*2,3*1" -> "order@1@2@3@1"
static void to_server_format_items(const char* items, char* out, size_t outsz){
    size_t di = 0;
    // prefix "order@"
    const char* pfx = "order@";
    for (size_t i=0; pfx[i] && di+1<outsz; ++i) out[di++] = pfx[i];

    for (size_t si = 0; items[si] && di + 1 < outsz; si++){
        unsigned char c = (unsigned char)items[si];
        if (c=='\r' || c=='\n') continue;
        if (c=='*' || c==',') c='@';   // 구분자 -> '@'
        out[di++] = (char)c;
    }
    out[di] = '\0';
}

