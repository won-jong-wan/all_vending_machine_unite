#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <ctype.h>
#include <sys/stat.h>
#include <fcntl.h>

#define PORT 8000
#define SUMMARY_SERVER_PORT 5000
#define SUMMARY_SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 1024
#define HTML_BUFFER_SIZE 4096
#define MAX_CLIENTS 10
#define TARGET_HEADER "[ALLMSG]"
#define ID "20"
#define PASSWORD "PASSWD"

const char* class_list[] = {
    "Pizza",
    "Burger",
    "Pasta",
    "Salad"
};

// 스레드에 전달할 클라이언트 정보 구조체
struct client_info {
    int socket;
    struct sockaddr_in address;
};

struct order_summary
{
    int class0_count;
    int class1_count;
    int class2_count;
    int class3_count;
    int total_price;
};


char* read_html_file() {
    FILE *file = fopen("index.html", "r");
    if (file == NULL) {
        perror("Failed to open HTML file");
        return NULL;
    }

    char* buffer = (char*)malloc(HTML_BUFFER_SIZE);
    size_t bytes_read = fread(buffer, 1, HTML_BUFFER_SIZE - 1, file);
    buffer[bytes_read] = '\0';
    
    fclose(file);
    return buffer;
}

// JSON 파싱을 위한 간단한 함수들
char* extract_json_array(const char* buffer) {
    const char* start = strchr(buffer, '[');
    if (!start) return NULL;
    const char* end = strrchr(buffer, ']');
    if (!end) return NULL;
    
    int length = end - start + 1;
    char* json = malloc(length + 1);
    strncpy(json, start, length);
    json[length] = '\0';

    return json;
}

struct order_summary item_paser(char* json) {
    struct order_summary summary = {0};
    char *saveptr;  // strtok_r용 포인터

    char* token = strtok_r(json, "{},", &saveptr);
    while (token != NULL) {
        if (strstr(token, "\"item\"")) {
            char* colon = strchr(token, ':');
            if (colon) {
                char* value = colon + 1;
                while (isspace(*value)) value++;
                if (*value == '\"') value++;
                
                // Count items
                if (strstr(value, class_list[0])) summary.class0_count++;
                else if (strstr(value, class_list[1])) summary.class1_count++;
                else if (strstr(value, class_list[2])) summary.class2_count++;
                else if (strstr(value, class_list[3])) summary.class3_count++;
            }
        }
        else if (strstr(token, "\"price\"")) {
            char* colon = strchr(token, ':');
            if (colon) {
                summary.total_price += atoi(colon + 1);
            }
        }
        token = strtok_r(NULL, "{},", &saveptr);
    }

    // Print summary
    printf("주문 내역:\n");
    if (summary.class0_count > 0) printf("%s: %d개\n", class_list[0], summary.class0_count);
    if (summary.class1_count > 0) printf("%s: %d개\n", class_list[1], summary.class1_count);
    if (summary.class2_count > 0) printf("%s: %d개\n", class_list[2], summary.class2_count);
    if (summary.class3_count > 0) printf("%s: %d개\n", class_list[3], summary.class3_count);
    printf("총 가격: $%d\n", summary.total_price);

    return summary;
}

void send_order_summary(struct order_summary summary) {
    int sock = 0;
    struct sockaddr_in serv_addr;
    char buffer[1024] = {0};
    
    // 소켓 생성
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        printf("Summary Server: Socket creation error\n");
        return;
    }
    
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(SUMMARY_SERVER_PORT);
    
    // IP 주소 변환
    if (inet_pton(AF_INET, SUMMARY_SERVER_IP, &serv_addr.sin_addr) <= 0) {
        printf("Summary Server: Invalid address\n");
        close(sock);
        return;
    }
    
    // 서버 연결
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        printf("Summary Server: Connection Failed\n");
        close(sock);
        return;
    }

    // ID 20 전송
    char id[50];
    snprintf(id, sizeof(id), "[%s:%s]", ID, PASSWORD);
    // 왜 서버쪽에서 segmentation fault가 뜰까?
    // send() 함수에서 id의 크기를 sizeof(id)로 주었기 때문입니다.
    // 실제로 전송되는 데이터의 크기는 strlen(id) + 1 (null terminator)입니다.
    // 잘된다야.
    printf("send %s\n\n", id);
    if (send(sock, id, strlen(id), 0) < 0) {
        printf("Failed to send ID\n");
        close(sock);
        return;
    }

    // 서버가 준비될 시간을 줍니다.
    usleep(100000);  // 0.1초 대기

    // 주문 데이터 전송
    snprintf(buffer, sizeof(buffer), 
        "%s%s@%d@%s@%d@%s@%d@%s@%d@total@%d\n",
        TARGET_HEADER, class_list[0], summary.class0_count, class_list[1], summary.class1_count, 
        class_list[2], summary.class2_count, class_list[3], summary.class3_count, 
        summary.total_price);
    
    if (send(sock, buffer, strlen(buffer), 0) < 0) {
        printf("Failed to send order summary\n");
    } else {
        printf("Order summary sent successfully\n");
    }
    
    close(sock);
}

const char* get_content_type(const char* path) {
    const char* ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    
    if (strcmp(ext, ".html") == 0) return "text/html";
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".gif") == 0) return "image/gif";
    
    return "application/octet-stream";
}

void send_file(int client_socket, const char* path) {
    // 현재 실행 경로를 기준으로 파일 경로 생성
    char full_path[512];
    if (path[0] == '/') {
        // 앞에 슬래시가 있으면 제거
        snprintf(full_path, sizeof(full_path), ".%s", path);
    } else {
        snprintf(full_path, sizeof(full_path), "./%s", path);
    }

    printf("Trying to open file: %s\n", full_path);  // 디버그용 출력

    int fd = open(full_path, O_RDONLY);
    if (fd < 0) {
        char *response = "HTTP/1.1 404 Not Found\nContent-Type: text/plain\nConnection: close\n\nFile not found";
        printf("Error: Cannot open file %s\n", full_path);
        write(client_socket, response, strlen(response));
        return;
    }

    // 파일 크기 얻기
    struct stat file_stat;
    fstat(fd, &file_stat);
    off_t file_size = file_stat.st_size;

    // 응답 버퍼 준비 (헤더 + 파일 내용)
    char header[512];
    int header_len = snprintf(header, sizeof(header), 
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %ld\r\n"
        "Connection: close\r\n"
        "\r\n",
        get_content_type(path), file_size);

    // 헤더 전송
    if (write(client_socket, header, header_len) < 0) {
        close(fd);
        return;
    }

    // 파일 내용 전송
    char buffer[4096];
    ssize_t bytes_read;
    while ((bytes_read = read(fd, buffer, sizeof(buffer))) > 0) {
        ssize_t bytes_sent = write(client_socket, buffer, bytes_read);
        if (bytes_sent < 0) {
            break;  // 전송 오류 발생
        }
    }

    close(fd);
}

void handle_request(int socket, char* buffer) {    
    if (strstr(buffer, "POST /order") != NULL) {
        // POST 요청 처리
        char* json = extract_json_array(strstr(buffer, "\r\n\r\n"));
        
        if (json) {
            struct order_summary summary = item_paser(json);
            send_order_summary(summary);
            free(json);
            
            char *response = "HTTP/1.1 200 OK\nContent-Type: text/plain\nConnection: close\n\nOrder received successfully!";
            write(socket, response, strlen(response));
            return;
        }
    } else {
        // GET 요청 처리
        char* path_start = strchr(buffer, ' ') + 1;
        char* path_end = strchr(path_start, ' ');
        size_t path_length = path_end - path_start;
        
        char path[256];
        strncpy(path, path_start, path_length);
        path[path_length] = '\0';
        
        if (strcmp(path, "/") == 0) {
            send_file(socket, "./index.html");
        } else {
            send_file(socket, path);  // 경로 그대로 전달
        }
        return;
    }
}

void *handle_client(void *arg) {
    struct client_info *client = (struct client_info *)arg;
    char buffer[BUFFER_SIZE] = {0};
    
    // 클라이언트 주소 정보 출력
    char client_ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &(client->address.sin_addr), client_ip, INET_ADDRSTRLEN);
    printf("New client connected from %s:%d\n", 
        client_ip, 
        ntohs(client->address.sin_port));

    // 클라이언트 요청 한 번만 읽기
    if(read(client->socket, buffer, BUFFER_SIZE) > 0) {
        printf("Received request from %s:\n%s\n\n", client_ip, buffer);
        // 요청 처리
        handle_request(client->socket, buffer);
    }
    
    // 연결 종료
    close(client->socket);
    free(client);
    pthread_exit(NULL);
}

int main() {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    pthread_t thread_id;
    
    // 서버 소켓 생성
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
    
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("Setsockopt failed");
        exit(EXIT_FAILURE);
    }
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);
    
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }
    
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    
    printf("Server is running on port %d...\n", PORT);
    
    while(1) {
        struct client_info *client = malloc(sizeof(struct client_info));
        client->address.sin_family = AF_INET;
        socklen_t client_len = sizeof(client->address);
        
        // 새로운 클라이언트 연결 수락
        client->socket = accept(server_fd, 
                              (struct sockaddr *)&(client->address), 
                              &client_len);
        
        if (client->socket < 0) {
            perror("Accept failed");
            free(client);
            continue;
        }
        
        // 새로운 스레드 생성
        if (pthread_create(&thread_id, NULL, handle_client, (void *)client) < 0) {
            perror("Could not create thread");
            free(client);
            continue;
        }
        
        // 스레드를 분리(detach)하여 자동으로 리소스가 해제되도록 설정
        pthread_detach(thread_id);
    }
    
    return 0;
}