#include "menu.h"
#include "queries.h"

#include <mysql/mysql.h>

void handle_menu(FILE* out) {
    MYSQL* conn = db_connect();
    if(!conn) {
        fprintf(out, "HTTP/1.1 500 Internal Server Error\r\n\r\nDB connect error");
        return;
    }

    if(mysql_query(conn, SQL_SELECT_MENU)) {
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
        fprintf(out, "{\"id\":%s,\"name\":\"%s\",\"price\":%s,\"image\":\"%s\"}", 
                row[0], row[1], row[2], row[3]);
        first = 0;
    }
    fprintf(out, "]");
    mysql_free_result(res);
    db_disconnect(conn);
}

