#define _GNU_SOURCE

#include "queries.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===== DB 연결 ===== */
MYSQL* db_connect() {
    MYSQL *conn = mysql_init(NULL);
    if(!conn) return NULL;
    if(!mysql_real_connect(conn, DB_HOST, DB_USER, DB_PASS, DB_NAME, DB_PORT, NULL, 0)) {
        fprintf(stderr,"MySQL connect error: %s\n", mysql_error(conn));
        mysql_close(conn);
        return NULL;
    }
    return conn;
}

void db_disconnect(MYSQL* conn) {
    if(conn) mysql_close(conn);
}

/* ===== 메뉴 조회 ===== */
char* get_menu() {
    MYSQL* conn = db_connect();
    if (!conn) return strdup("{\"menu\":[]}");

    if (mysql_query(conn, "SELECT id,name,price,image FROM menu")) {
        fprintf(stderr,"Query error: %s\n", mysql_error(conn));
        db_disconnect(conn);
        return strdup("{\"menu\":[]}");
    }

    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) { db_disconnect(conn); return strdup("{\"menu\":[]}"); }

    char* json = malloc(8192);
    strcpy(json, "{\"menu\":[");
    int first = 1;
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res))) {
        if (!first) strcat(json, ",");
        first = 0;
        char item[512];
        snprintf(item,sizeof(item),
            "{\"id\":%s,\"name\":\"%s\",\"price\":%s,\"image\":\"%s\"}",
            row[0], row[1], row[2], row[3] ? row[3] : "");
        strcat(json,item);
    }
    strcat(json, "]}");

    mysql_free_result(res);
    db_disconnect(conn);
    return json;
}

/* ===== 주문 생성 ===== */
/* items 예: "1*2,3*1" → 메뉴 1번 2개, 메뉴 3번 1개 */
char* create_order(const char* items) {
    MYSQL* conn = db_connect();
    if (!conn) return strdup("{\"ok\":false}");

    char query[512];
    int total_qty = 0, total_price = 0;

    // 1) orders 테이블 새 row 생성 (합계 0으로 임시 저장)
    if (mysql_query(conn, "INSERT INTO orders(total_qty,total_price) VALUES(0,0)")) {
        fprintf(stderr,"Insert order error: %s\n", mysql_error(conn));
        db_disconnect(conn);
        return strdup("{\"ok\":false}");
    }
    int order_id = mysql_insert_id(conn);

    // 2) items 파싱해서 order_items 저장
    char* dup = strdup(items);
    char* token = strtok(dup, ",");
    while (token) {
        int menu_id, qty;
        if (sscanf(token, "%d*%d", &menu_id, &qty) == 2) {
            snprintf(query,sizeof(query),"SELECT price FROM menu WHERE id=%d", menu_id);
            if (mysql_query(conn, query)==0) {
                MYSQL_RES* res = mysql_store_result(conn);
                if (res) {
                    MYSQL_ROW row = mysql_fetch_row(res);
                    if (row) {
                        int price = atoi(row[0]);
                        int item_price = price * qty;
                        total_qty += qty;
                        total_price += item_price;

                        snprintf(query,sizeof(query),
                            "INSERT INTO order_items(order_id,menu_id,qty,price) VALUES(%d,%d,%d,%d)",
                            order_id, menu_id, qty, item_price);
                        if (mysql_query(conn,query)) {
                            fprintf(stderr,"Insert order_item error: %s\n", mysql_error(conn));
                        }
                    }
                    mysql_free_result(res);
                }
            }
        }
        token = strtok(NULL, ",");
    }
    free(dup);

    // 3) orders 테이블에 합계 업데이트
    snprintf(query,sizeof(query),
        "UPDATE orders SET total_qty=%d,total_price=%d WHERE order_id=%d",
        total_qty,total_price,order_id);
    mysql_query(conn, query);

    db_disconnect(conn);

    char* res = malloc(256);
    snprintf(res,256,"{\"ok\":true,\"order_id\":%d,\"total_qty\":%d,\"total_price\":%d}",
        order_id,total_qty,total_price);
    return res;
}

/* ===== 주문 조회 (미결제) ===== */
char* get_orders() {
    MYSQL* conn = db_connect();
    if (!conn) return strdup("{\"orders\":[]}");

    if (mysql_query(conn,"SELECT order_id,total_qty,total_price,created_at FROM orders WHERE paid=0")) {
        db_disconnect(conn);
        return strdup("{\"orders\":[]}");
    }

    MYSQL_RES* res = mysql_store_result(conn);
    if (!res) { db_disconnect(conn); return strdup("{\"orders\":[]}"); }

    char* json = malloc(8192); strcpy(json,"{\"orders\":[");
    int first=1; MYSQL_ROW row;
    while ((row=mysql_fetch_row(res))) {
        if (!first) strcat(json,","); first=0;
        char buf[256];
        snprintf(buf,sizeof(buf),
            "{\"order_id\":%s,\"total_qty\":%s,\"total_price\":%s,\"created_at\":\"%s\"}",
            row[0],row[1],row[2],row[3]);
        strcat(json,buf);
    }
    strcat(json,"]}");
    mysql_free_result(res);
    db_disconnect(conn);
    return json;
}

/* ===== 결제 완료 처리 ===== */
char* pay_order(const char* order_id) {
    MYSQL* conn = db_connect();
    if (!conn) return strdup("{\"ok\":false}");
    char query[128];
    snprintf(query,sizeof(query),"UPDATE orders SET paid=1 WHERE order_id=%s", order_id);
    if(mysql_query(conn,query)) {
        db_disconnect(conn);
        return strdup("{\"ok\":false}");
    }
    db_disconnect(conn);
    return strdup("{\"ok\":true}");
}

