// order.h
#ifndef ORDER_H
#define ORDER_H
#include <stdio.h>

void handle_order(FILE* out, const char* body);  // 주문 생성
void handle_orders(FILE* out);                   // 진행중 주문 조회
void handle_order_delete(FILE* out, int order_id); // 주문 취소

#endif

