#ifndef PASER_H
#define PASER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ITEMS 20
#define MAX_NAME_LEN 50

int parse_order_string(const char* input);
int get_menu_quantity(const char* menu_name);
int get_total_amount(void);
int get_item_count(void);
void reset_order_data(void);
int get_total_quantity(void);

#endif //PASER_H
