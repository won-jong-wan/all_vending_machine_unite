/* 서울기술교육센터 AIoT */
/* iot_server_1.c — 지속로그인 / 원샷 버그 수정(콜론 사전판별) / 비번 rstrip
 * + [SERVER]order@... 브로드캐스트
 * + 원샷 소켓 close()
 * + IP+주문내용 기반 디바운스(최근 3초 내 동일건 무시, 32개 기억)
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

#define BUF_SIZE 512
#define MAX_CLNT 64
#define ID_SIZE  10
#define ARR_CNT  5

typedef struct {
    int  fd;
    char *from;
    char *to;
    char *msg;
    int  len;
} MSG_INFO;

typedef struct {
    int  index;
    int  fd;
    char ip[20];
    char id[ID_SIZE];
    char pw[ID_SIZE];
} CLIENT_INFO;

/* ===== fwd decls ===== */
void * clnt_connection(void * arg);
void send_msg(MSG_INFO * msg_info, CLIENT_INFO * first_client_info);
void error_handling(char * msg);
void log_file(char * msgstr);
void getlocaltime(char * buf);

/* 우측 개행/공백 제거 */
static void rstrip(char *s){
    int n = (int)strlen(s);
    while (n>0 && (s[n-1]=='\r' || s[n-1]=='\n' || s[n-1]==' ' || s[n-1]=='\t')) s[--n]=0;
}

/* "ORDER 2*1" -> "order@2@1", "ORDER 1*2,3*1" -> "order@1@2@3@1" */
static void to_server_format(const char* in, char* out, size_t outsz){
    size_t di = 0;
    for (size_t si = 0; in[si] && di + 1 < outsz; si++){
        unsigned char c = (unsigned char)in[si];
        if (c=='\r' || c=='\n') continue;            /* 개행 제거 */
        if (c==' ' || c=='*' || c==',') c='@';       /* 구분자 -> '@' */
        if ('A'<=c && c<='Z') c = (char)(c - 'A' + 'a'); /* 소문자화 */
        out[di++] = (char)c;
    }
    out[di] = '\0';
}

/* 로그인된 모든 클라에게 한 줄을 브로드캐스트 */
static void broadcast_to_all(CLIENT_INFO *clients, const char *line){
    size_t len = strlen(line);
    for (int i=0; i<MAX_CLNT; ++i){
        if (clients[i].fd != -1){
            (void)write(clients[i].fd, line, len);
        }
    }
}

/* ===== 중복 억제: 최근 3초 내 동일(IP+문구) 무시 ===== */
typedef struct {
    char ip[20];
    char conv[128];
    struct timeval tv;
    int used;
} Recent;
static Recent recent[32];
static int recent_cursor = 0;

static int is_recent_duplicate_ipconv(const char* ip, const char* conv, int window_ms){
    struct timeval now; gettimeofday(&now, NULL);
    for (int i=0;i<32;i++){
        if (!recent[i].used) continue;
        if (strcmp(recent[i].ip, ip) != 0) continue;
        if (strcmp(recent[i].conv, conv) != 0) continue;
        long dt = (now.tv_sec - recent[i].tv.tv_sec) * 1000L
                + (now.tv_usec - recent[i].tv.tv_usec) / 1000L;
        if (dt >= 0 && dt < window_ms) return 1;
    }
    return 0;
}
static void remember_ipconv(const char* ip, const char* conv){
    int idx = recent_cursor++ % 32;
    snprintf(recent[idx].ip,   sizeof(recent[idx].ip),   "%s", ip);
    snprintf(recent[idx].conv, sizeof(recent[idx].conv), "%s", conv);
    gettimeofday(&recent[idx].tv, NULL);
    recent[idx].used = 1;
}

int clnt_cnt=0;
pthread_mutex_t mutx;

int main(int argc, char *argv[])
{
    int serv_sock, clnt_sock;
    struct sockaddr_in serv_adr, clnt_adr;
    socklen_t clnt_adr_sz;
    int sock_option  = 1;
    pthread_t t_id[MAX_CLNT] = {0};
    int str_len = 0;
    int i=0;
    char *pToken;
    char *pArray[ARR_CNT]={0};
    char msg[BUF_SIZE];

    /* ===== 계정 테이블 로딩 ===== */
    FILE * idFd = fopen("idpasswd.txt","r");
    if(idFd == NULL) { perror("fopen(\"idpasswd.txt\",\"r\") "); exit(1); }

    char id[ID_SIZE], pw[ID_SIZE];
    CLIENT_INFO * client_info = (CLIENT_INFO *)calloc(MAX_CLNT, sizeof(CLIENT_INFO));
    if(client_info == NULL) { perror("calloc()"); exit(1); }

    int loaded = 0;
    while (fscanf(idFd, "%9s %9s", id, pw) == 2) {
        if (loaded >= MAX_CLNT) {
            fprintf(stderr, "warning: idpasswd.txt has more than %d entries — extras ignored.\n", MAX_CLNT);
            break;
        }
        client_info[loaded].fd = -1;
        snprintf(client_info[loaded].id, ID_SIZE, "%s", id);
        snprintf(client_info[loaded].pw, ID_SIZE, "%s", pw);
        loaded++;
    }
    fclose(idFd);
    printf("Loaded %d accounts (MAX_CLNT=%d)\n", loaded, MAX_CLNT);

    if(argc != 2) { printf("Usage : %s <port>\n",argv[0]); exit(1); }
    fputs("IoT Server Start!!\n",stdout);

    if(pthread_mutex_init(&mutx, NULL)) error_handling("mutex init error");

    serv_sock = socket(PF_INET, SOCK_STREAM, 0);

    memset(&serv_adr, 0, sizeof(serv_adr));
    serv_adr.sin_family=AF_INET;
    serv_adr.sin_addr.s_addr=htonl(INADDR_ANY);
    serv_adr.sin_port=htons(atoi(argv[1]));

    setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, (void*)&sock_option, sizeof(sock_option));
    if(bind(serv_sock, (struct sockaddr *)&serv_adr, sizeof(serv_adr))==-1) error_handling("bind() error");
    if(listen(serv_sock, 5) == -1) error_handling("listen() error");

    while(1) {
        clnt_adr_sz = sizeof(clnt_adr);
        clnt_sock = accept(serv_sock, (struct sockaddr *)&clnt_adr, &clnt_adr_sz);
        if(clnt_cnt >= MAX_CLNT) { printf("socket full\n"); shutdown(clnt_sock,SHUT_WR); continue; }
        if(clnt_sock < 0) { perror("accept()"); continue; }

        /* === 첫 패킷 수신 (로그인 or 원샷) === */
        char idpasswd[BUF_SIZE];
        str_len = read(clnt_sock, idpasswd, sizeof(idpasswd)-1);
        if (str_len <= 0) { close(clnt_sock); continue; }
        idpasswd[str_len] = '\0';

        /* ★ strtok 전에 콜론 유무 판단(원샷/로그인 분기용) */
        char raw[BUF_SIZE];
        snprintf(raw, sizeof(raw), "%s", idpasswd);
        int had_colon = (strchr(raw, ':') != NULL);

        /* 토큰화 (ID:PW or CMD:DATA 등) */
        for (int k=0;k<ARR_CNT;k++) pArray[k]=NULL;
        int tk = 0;
        pToken = strtok(idpasswd, "[:]");
        while (pToken && tk < ARR_CNT) { pArray[tk++] = pToken; pToken = strtok(NULL, "[:]"); }

        /* ===== 로그인 시도 ===== */
        for(i=0;i<MAX_CLNT;i++) {
            if(!strcmp(client_info[i].id, pArray[0] ? pArray[0] : "")) {
                if(client_info[i].fd != -1) {
                    snprintf(msg,sizeof(msg),"[%s] Already logged!\n",pArray[0]);
                    write(clnt_sock, msg,strlen(msg));
                    log_file(msg);
                    close(clnt_sock);
#if 1   // for MCU
                    client_info[i].fd = -1;
#endif
                    break;
                }

                /* ▼ 비번 rstrip 후 비교 */
                char pw_in[ID_SIZE];
                snprintf(pw_in, sizeof(pw_in), "%s", pArray[1] ? pArray[1] : "");
                rstrip(pw_in);

                if(!strcmp(client_info[i].pw, pw_in)) {
                    snprintf(client_info[i].ip, sizeof(client_info[i].ip), "%s", inet_ntoa(clnt_adr.sin_addr));
                    pthread_mutex_lock(&mutx);
                    client_info[i].index = i;
                    client_info[i].fd    = clnt_sock;
                    clnt_cnt++;
                    pthread_mutex_unlock(&mutx);
                    snprintf(msg,sizeof(msg),"[%s] New connected! (ip:%s,fd:%d,sockcnt:%d)\n",
                             pArray[0], inet_ntoa(clnt_adr.sin_addr), clnt_sock, clnt_cnt);
                    log_file(msg);
                    write(clnt_sock, msg,strlen(msg));

                    pthread_create(&t_id[i], NULL, clnt_connection, (void *)(client_info + i));
                    pthread_detach(t_id[i]);
                    break;  /* 로그인 성공 */
                }
            }
        }

        /* ===== 로그인 매칭 실패 → had_colon 기준으로 분기 ===== */
        if(i == MAX_CLNT) {
            if (!had_colon) {
                /* 원샷: 콘솔 로그 + 로그인 세션으로 브로드캐스트 (IP+주문 디바운스) */
                rstrip(raw);
                char conv[BUF_SIZE], line[BUF_SIZE];
                to_server_format(raw, conv, sizeof(conv));     /* ex) order@5@1 */

                const char* src_ip = inet_ntoa(clnt_adr.sin_addr);
                if (!is_recent_duplicate_ipconv(src_ip, conv, 3000)) {  /* 3초 내 동일건 무시 */
                    snprintf(line, sizeof(line), "[SERVER]%s\n", conv);
                    log_file(line);                             /* 콘솔 */
                    broadcast_to_all(client_info, line);        /* 로그인 세션들에게 전송 */
                    remember_ipconv(src_ip, conv);
                }

                /* 요청 소켓에는 OK 응답 후 '완전 종료' */
                const char *ok = "[OK] one-shot accepted\n";
                (void)write(clnt_sock, ok, strlen(ok));
                close(clnt_sock);                               /* 완전 종료 */
                continue;                                       /* 다음 accept */
            } else {
                /* ID:PW 형식인데 매칭 실패 → 인증 에러 */
                snprintf(msg,sizeof(msg),"[%s] Authentication Error!\n",
                         pArray[0] ? pArray[0] : "UNKNOWN");
                write(clnt_sock, msg,strlen(msg));
                log_file(msg);
                close(clnt_sock);
            }
        }
    }
    return 0;
}

void * clnt_connection(void *arg)
{
    CLIENT_INFO * client_info = (CLIENT_INFO *)arg;
    int str_len = 0;
    int index = client_info->index;
    char msg[BUF_SIZE];
    char to_msg[MAX_CLNT*ID_SIZE+1];
    int i=0;
    char *pToken;
    char *pArray[ARR_CNT]={0};
    char strBuff[BUF_SIZE*2]={0};

    MSG_INFO msg_info;
    CLIENT_INFO  * first_client_info;

    first_client_info = client_info - index;

    while(1)
    {
        memset(msg,0x0,sizeof(msg));
        str_len = read(client_info->fd, msg, sizeof(msg)-1);
        if(str_len <= 0) break;

        msg[str_len] = '\0';
        pToken = strtok(msg,"[:]");
        i = 0;
        while(pToken != NULL)
        {
            pArray[i] =  pToken;
            if(i++ >= ARR_CNT) break;
            pToken = strtok(NULL,"[:]");
        }

        msg_info.fd   = client_info->fd;
        msg_info.from = client_info->id;
        msg_info.to   = pArray[0] ? pArray[0] : (char*)"";
        snprintf(to_msg, sizeof(to_msg), "[%s]%s", msg_info.from, pArray[1] ? pArray[1] : "");
        msg_info.msg  = to_msg;
        msg_info.len  = (int)strlen(to_msg);

        snprintf(strBuff,sizeof(strBuff),"msg : [%s->%s] %s",
                 msg_info.from, msg_info.to, pArray[1] ? pArray[1] : "");
        log_file(strBuff);
        send_msg(&msg_info, first_client_info);
    }

    close(client_info->fd);

    snprintf(strBuff,sizeof(strBuff),"Disconnect ID:%s (ip:%s,fd:%d,sockcnt:%d)\n",
             client_info->id, client_info->ip, client_info->fd, clnt_cnt-1);
    log_file(strBuff);

    pthread_mutex_lock(&mutx);
    clnt_cnt--;
    client_info->fd = -1;
    pthread_mutex_unlock(&mutx);

    return 0;
}

void send_msg(MSG_INFO * msg_info, CLIENT_INFO * first_client_info)
{
    int i=0;

    if(!strcmp(msg_info->to,"ALLMSG"))
    {
        for(i=0;i<MAX_CLNT;i++)
            if((first_client_info+i)->fd != -1)
                write((first_client_info+i)->fd, msg_info->msg, msg_info->len);
    }
    else if(!strcmp(msg_info->to,"IDLIST"))
    {
        char* idlist = (char *)malloc(ID_SIZE * MAX_CLNT);
        if (!idlist) return;
        if (msg_info->len>0) msg_info->msg[msg_info->len - 1] = '\0';
        strcpy(idlist,msg_info->msg);

        for(i=0;i<MAX_CLNT;i++)
        {
            if((first_client_info+i)->fd != -1)
            {
                strcat(idlist,(first_client_info+i)->id);
                strcat(idlist," ");
            }
        }
        strcat(idlist,"\n");
        write(msg_info->fd, idlist, strlen(idlist));
        free(idlist);
    }
    else if(!strcmp(msg_info->to,"GETTIME"))
    {
        sleep(1);
        getlocaltime(msg_info->msg);
        write(msg_info->fd, msg_info->msg, strlen(msg_info->msg));
    }
    else
        for(i=0;i<MAX_CLNT;i++)
            if((first_client_info+i)->fd != -1)
                if(!strcmp(msg_info->to,(first_client_info+i)->id))
                    write((first_client_info+i)->fd, msg_info->msg, msg_info->len);
}

void error_handling(char *msg)
{
    fputs(msg, stderr);
    fputc('\n', stderr);
    exit(1);
}

void log_file(char * msgstr)
{
    fputs(msgstr,stdout);
}

void  getlocaltime(char * buf)
{
    struct tm *t;
    time_t tt;
    char wday[7][4] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    tt = time(NULL);
    if(errno == EFAULT) perror("time()");
    t = localtime(&tt);
    sprintf(buf,"[GETTIME]%02d.%02d.%02d %02d:%02d:%02d %s",
            t->tm_year+1900-2000,t->tm_mon+1,t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec,wday[t->tm_wday]);
}

