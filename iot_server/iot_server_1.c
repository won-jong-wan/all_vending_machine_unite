/* 서울기술교육센터 AIoT */
/* author : KSH (patched) */
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

#define BUF_SIZE 512          // 입력/원샷 메시지 여유
#define MAX_CLNT 64           // 동접/계정 수 여유
#define ID_SIZE  10
#define ARR_CNT  5

#define DEBUG

typedef struct {
    int  fd;          // (fix) was char
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

/* ====== forward decls ====== */
void * clnt_connection(void * arg);
void send_msg(MSG_INFO * msg_info, CLIENT_INFO * first_client_info);
void error_handling(char * msg);
void log_file(char * msgstr);
void getlocaltime(char * buf);

/* helpers added */
static void rstrip(char *s){
    int n = (int)strlen(s);
    while (n>0 && (s[n-1]=='\r' || s[n-1]=='\n' || s[n-1]==' ' || s[n-1]=='\t')) s[--n]=0;
}

/* "ORDER 1*2,3*1" -> "order@1@2@3@1" */
static void to_server_format(const char* in, char* out, size_t outsz){
    size_t di = 0;
    for (size_t si = 0; in[si] && di + 1 < outsz; si++){
        unsigned char c = (unsigned char)in[si];
        if (c=='\r' || c=='\n') continue;        // 개행 제거
        if (c==' ' || c=='*' || c==',') c='@';   // 구분자 -> '@'
        if ('A'<=c && c<='Z') c = (char)(c-'A'+'a'); // 소문자
        out[di++] = (char)c;
    }
    out[di] = '\0';
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

    /* ===== 계정 테이블 로딩 (안전하게) ===== */
    FILE * idFd = fopen("idpasswd.txt","r");
    if(idFd == NULL) {
        perror("fopen(\"idpasswd.txt\",\"r\") ");
        exit(1);
    }

    char id[ID_SIZE], pw[ID_SIZE];
    CLIENT_INFO * client_info = (CLIENT_INFO *)calloc(MAX_CLNT, sizeof(CLIENT_INFO));
    if(client_info == NULL) {
        perror("calloc()");
        exit(1);
    }

    int loaded = 0;
    while (fscanf(idFd, "%9s %9s", id, pw) == 2) { // 각 9자 제한(+NUL)
        if (loaded >= MAX_CLNT) {
            fprintf(stderr, "warning: idpasswd.txt has more than %d entries — extras will be ignored.\n", MAX_CLNT);
            break;
        }
        client_info[loaded].fd = -1;
        strncpy(client_info[loaded].id, id, ID_SIZE-1);
        strncpy(client_info[loaded].pw, pw, ID_SIZE-1);
        client_info[loaded].id[ID_SIZE-1] = '\0';
        client_info[loaded].pw[ID_SIZE-1] = '\0';
        loaded++;
    }
    fclose(idFd);
    printf("Loaded %d accounts (MAX_CLNT=%d)\n", loaded, MAX_CLNT);

    if(argc != 2) {
        printf("Usage : %s <port>\n",argv[0]);
        exit(1);
    }
    fputs("IoT Server Start!!\n",stdout);

    if(pthread_mutex_init(&mutx, NULL))
        error_handling("mutex init error");

    serv_sock = socket(PF_INET, SOCK_STREAM, 0);

    memset(&serv_adr, 0, sizeof(serv_adr));
    serv_adr.sin_family=AF_INET;
    serv_adr.sin_addr.s_addr=htonl(INADDR_ANY);
    serv_adr.sin_port=htons(atoi(argv[1]));

    setsockopt(serv_sock, SOL_SOCKET, SO_REUSEADDR, (void*)&sock_option, sizeof(sock_option));
    if(bind(serv_sock, (struct sockaddr *)&serv_adr, sizeof(serv_adr))==-1)
        error_handling("bind() error");

    if(listen(serv_sock, 5) == -1)
        error_handling("listen() error");

    while(1) {
        clnt_adr_sz = sizeof(clnt_adr);
        clnt_sock = accept(serv_sock, (struct sockaddr *)&clnt_adr, &clnt_adr_sz);
        if(clnt_cnt >= MAX_CLNT) {
            printf("socket full\n");
            shutdown(clnt_sock,SHUT_WR);
            continue;
        } else if(clnt_sock < 0) {
            perror("accept()");
            continue;
        }

        /* === 첫 패킷 수신 (로그인 or 원샷 메시지) === */
        char idpasswd[BUF_SIZE];                 // (fix) 넉넉히
        str_len = read(clnt_sock, idpasswd, sizeof(idpasswd)-1);
        if (str_len <= 0) { shutdown(clnt_sock, SHUT_WR); continue; }
        idpasswd[str_len] = '\0';

        /* 토큰 나누기 (ID:PW 또는 "ORDER ...") */
        i=0;
        pToken = strtok(idpasswd,"[:]");
        while(pToken != NULL) {
            pArray[i] =  pToken;
            if(i++ >= ARR_CNT) break;
            pToken = strtok(NULL,"[:]");
        }

        /* ===== 로그인 시도 ===== */
        for(i=0;i<MAX_CLNT;i++) {
            if(!strcmp(client_info[i].id, pArray[0])) {
                if(client_info[i].fd != -1) {
                    snprintf(msg,sizeof(msg),"[%s] Already logged!\n",pArray[0]);
                    write(clnt_sock, msg,strlen(msg));
                    log_file(msg);
                    shutdown(clnt_sock,SHUT_WR);
#if 1   //for MCU
                    client_info[i].fd = -1;
#endif
                    break;
                }
                if(!strcmp(client_info[i].pw, pArray[1])) {
                    strcpy(client_info[i].ip, inet_ntoa(clnt_adr.sin_addr));
                    pthread_mutex_lock(&mutx);
                    client_info[i].index = i;
                    client_info[i].fd    = clnt_sock;
                    clnt_cnt++;
                    pthread_mutex_unlock(&mutx);
                    snprintf(msg,sizeof(msg),"[%s] New connected! (ip:%s,fd:%d,sockcnt:%d)\n",
                             pArray[0], inet_ntoa(clnt_adr.sin_addr), clnt_sock, clnt_cnt);
                    log_file(msg);
                    write(clnt_sock, msg,strlen(msg));

                    pthread_create(t_id+i, NULL, clnt_connection, (void *)(client_info + i));
                    pthread_detach(t_id[i]);
                    break;
                }
            }
        }

        /* ===== 로그인 매칭 실패 → 원샷 허용 또는 인증 에러 ===== */
        if(i == MAX_CLNT) {
            if (strchr(idpasswd, ':') == NULL) {
                // 콜론이 없으면 원샷 메시지로 간주
                rstrip(idpasswd);
                if (pArray[0]) rstrip(pArray[0]);

                // 콘솔에 원하는 포맷 출력
                char conv[BUF_SIZE];
                to_server_format(pArray[0] ? pArray[0] : idpasswd, conv, sizeof(conv));
                snprintf(msg, sizeof(msg), "[SERVER]%s\n", conv);
                log_file(msg);

                // (선택) OK 한 줄 응답
                const char *ok = "[OK] one-shot accepted\n";
                write(clnt_sock, ok, strlen(ok));

                shutdown(clnt_sock, SHUT_WR);
            } else {
                // 여전히 ID:PW 포맷인데 계정 매칭 실패 → 인증 에러
                snprintf(msg,sizeof(msg),"[%s] Authentication Error!\n", pArray[0] ? pArray[0] : "UNKNOWN");
                write(clnt_sock, msg,strlen(msg));
                log_file(msg);
                shutdown(clnt_sock,SHUT_WR);
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

    /* (fix) 배열 첫 원소 포인터 계산 간단화 */
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
        msg_info.to   = pArray[0];
        snprintf(to_msg, sizeof(to_msg), "[%s]%s", msg_info.from, pArray[1] ? pArray[1] : "");
        msg_info.msg  = to_msg;
        msg_info.len  = (int)strlen(to_msg);

        snprintf(strBuff,sizeof(strBuff),"msg : [%s->%s] %s", msg_info.from, msg_info.to, pArray[1] ? pArray[1] : "");
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
        msg_info->msg[strlen(msg_info->msg) - 1] = '\0';
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
    if(errno == EFAULT)
        perror("time()");
    t = localtime(&tt);
    sprintf(buf,"[GETTIME]%02d.%02d.%02d %02d:%02d:%02d %s",
            t->tm_year+1900-2000,t->tm_mon+1,t->tm_mday,t->tm_hour,t->tm_min,t->tm_sec,wday[t->tm_wday]);
    return;
}

