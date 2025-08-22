// order.c
#include "order.h"
#include "queries.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <mysql/mysql.h>

/**
 * 주문 생성 (POST /api/order)
 * body 예시: "menu_id=1&qty=2"
 */
void handle_order(FILE* out, const char* body) {
    MYSQL* conn = db_connect();
    if(!conn) {
        fprintf(out, "HTTP/1.1 500 Internal Server Error\r\n\r\nDB connect error");
        return;
    }

    int menu_id = 0, qty = 0;
    sscanf(body, "menu_id=%d&qty=%d", &menu_id, &qty);

    if(menu_id <= 0 || qty <= 0) {
        fprintf(out, "HTTP/1.1 400 Bad Request\r\n\r\n{\"ok\":false,\"msg\":\"invalid params\"}");
        db_disconnect(conn);
        return;
    }

    // 새로운 order_id 생성
    if(mysql_query(conn, SQL_NEXT_ORDER_ID)) {
        fprintf(out, "HTTP/1.1 500 Internal Server Error\r\n\r\n{\"ok\":false,\"msg\":\"order_id error\"}");
        db_disconnect(conn);
        return;
    }
    MYSQL_RES* res = mysql_store_result(conn);
    MYSQL_ROW row = mysql_fetch_row(res);
    int order_id = atoi(row[0]);
    mysql_free_result(res);

    // 새로운 item_id 생성
    if(mysql_query(conn, SQL_NEXT_ITEM_ID)) {
        fprintf(out, "HTTP/1.1 500 Internal Server Error\r\n\r\n{\"ok\":false,\"msg\":\"item_id error\"}");
        db_disconnect(conn);
        return;
    }
    res = mysql_store_result(conn);
    row = mysql_fetch_row(res);
    int item_id = atoi(row[0]);
    mysql_free_result(res);

    // 해당 메뉴 가격 가져오기
    char query[512];
    snprintf(query, sizeof(query), SQL_SELECT_MENU_PRICE_BY_ID_TMPL, menu_id);
    if(mysql_query(conn, query)) {
        fprintf(out, "HTTP/1.1 500 Internal Server Error\r\n\r\n{\"ok\":false,\"msg\":\"menu price error\"}");
        db_disconnect(conn);
        return;
    }
    res = mysql_store_result(conn);
    row = mysql_fetch_row(res);
    int price = atoi(row[0]);
    mysql_free_result(res);

    int total_price = price * qty;

    // orders 테이블에 주문 프레임 삽입
    snprintf(query, sizeof(query), SQL_INSERT_ORDER_TMPL, order_id);
    if(mysql_query(conn, query)) {
        fprintf(out, "HTTP/1.1 500 Internal Server Error\r\n\r\n{\"ok\":false,\"msg\":\"insert order error\"}");
        db_disconnect(conn);
        return;
    }

    // order_items 테이블에 아이템 삽입
    snprintf(query, sizeof(query), SQL_INSERT_ORDER_ITEM_TMPL, item_id, order_id, menu_id, qty, total_price);
    if(mysql_query(conn, query)) {
        fprintf(out, "HTTP/1.1 500 Internal Server Error\r\n\r\n{\"ok\":false,\"msg\":\"insert item error\"}");
        db_disconnect(conn);
        return;
    }

    // 합계 업데이트
    snprintf(query, sizeof(query), SQL_UPDATE_ORDER_TOTALS_TMPL, qty, total_price, order_id);
    mysql_query(conn, query);

    fprintf(out, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n"
                 "{\"ok\":true,\"order_id\":%d,\"item_id\":%d,\"total_price\":%d}",
                 order_id, item_id, total_price);

    db_disconnect(conn);
}


/**
 * 주문 조회 (GET /api/orders)
 */
void handle_orders(FILE* out) {
    MYSQL* conn = db_connect();
    if(!conn) {
        fprintf(out, "HTTP/1.1 500 Internal Server Error\r\n\r\nDB connect error");
        return;
    }

    if(mysql_query(conn, SQL_SELECT_ORDERS)) {
        fprintf(out, "HTTP/1.1 500 Internal Server Error\r\n\r\nDB query error");
        db_disconnect(conn);
        return;
    }

    MYSQL_RES* res = mysql_store_result(conn);
    MYSQL_ROW row;

    fprintf(out, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n[");
    int first = 1;
    while((row = mysql_fetch_row(res))) {
        if(!first) fprintf(out, ",");
        fprintf(out, "{\"order_id\":%s,\"menu\":\"%s\",\"qty\":%s,\"item_id\":%s}",
                row[0], row[1], row[2], row[3]);
        first = 0;
    }
    fprintf(out, "]");
    mysql_free_result(res);
    db_disconnect(conn);
}


/**
 * 주문 취소 (DELETE /api/order?id=1)
 */
void handle_order_delete(FILE* out, int order_id) {
    MYSQL* conn = db_connect();
    if(!conn) {
        fprintf(out, "HTTP/1.1 500 Internal Server Error\r\n\r\nDB connect error");
        return;
    }

    char query[256];

    snprintf(query, sizeof(query), SQL_DELETE_ORDER_ITEMS_TMPL, order_id);
    mysql_query(conn, query);

    snprintf(query, sizeof(query), SQL_DELETE_ORDER_TMPL, order_id);
    mysql_query(conn, query);

    fprintf(out, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n{\"ok\":true}");

    db_disconnect(conn);
}

