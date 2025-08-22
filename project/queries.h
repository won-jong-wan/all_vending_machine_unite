#ifndef QUERIES_H
#define QUERIES_H

#include <mysql/mysql.h>

/* ===== DB 설정 ===== */
#define DB_HOST "10.10.14.87"
#define DB_USER "project"
#define DB_PASS "1234"
#define DB_NAME "project_db"
#define DB_PORT 3306

/* ===== 함수 선언 ===== */
MYSQL* db_connect();
void db_disconnect(MYSQL* conn);

char* get_menu();
char* create_order(const char* items);
char* get_orders();
char* set_done(const char* order_id, const char* item_name, int done);
char* pay_order(const char* order_id);

#endif
