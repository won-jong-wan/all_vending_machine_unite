#include "paser.h"

// 전역 변수 선언
char menu_names[MAX_ITEMS][MAX_NAME_LEN];
int menu_quantities[MAX_ITEMS];
int item_count = 0;
int total_amount = 0;

// 문자열을 파싱하여 전역 변수에 저장하는 함수
int parse_order_string(const char* input) {
    if (input == NULL) {
        return -1;
    }

    // 정적 버퍼 사용 (STM32에서 스택 크기 고려)
    static char str_buffer[512];
    memset(str_buffer, 0, sizeof(str_buffer));

    // 입력 문자열 길이 체크
    if (strlen(input) >= sizeof(str_buffer)) {
        return -1;
    }

    strcpy(str_buffer, input);

    // 초기화
    item_count = 0;
    total_amount = 0;
    memset(menu_names, 0, sizeof(menu_names));
    memset(menu_quantities, 0, sizeof(menu_quantities));

    // '[' 제거
    char* start = str_buffer;
    if (*start == '[') {
        start++;
    }

    // 첫 번째 숫자 건너뛰기 (예: "20]")
    char* token = strchr(start, ']');
    if (token != NULL) {
        start = token + 1;
    }

    // '@'로 분리하여 파싱
    token = strtok(start, "@");

    while (token != NULL && item_count < MAX_ITEMS) {
        // 메뉴 이름 저장
        strncpy(menu_names[item_count], token, MAX_NAME_LEN - 1);
        menu_names[item_count][MAX_NAME_LEN - 1] = '\0';

        // 다음 토큰 (수량)
        token = strtok(NULL, "@");
        if (token == NULL) break;

        // "total"인지 확인
        if (strcmp(menu_names[item_count], "total") == 0) {
            total_amount = atoi(token);
            break;
        } else {
            menu_quantities[item_count] = atoi(token);
            item_count++;
        }

        // 다음 메뉴 이름
        token = strtok(NULL, "@");
    }

    return 0;
}

// 특정 메뉴의 수량을 반환하는 함수
int get_menu_quantity(const char* menu_name) {
    for (int i = 0; i < item_count; i++) {
        if (strcmp(menu_names[i], menu_name) == 0) {
            return menu_quantities[i];
        }
    }
    return -1; // 메뉴를 찾지 못한 경우
}

// 총 금액 반환 함수
int get_total_amount(void) {
    return total_amount;
}

// 전체 메뉴 개수 반환 함수
int get_item_count(void) {
    return item_count;
}

// 전역 변수 초기화 함수
void reset_order_data(void) {
    item_count = 0;
    total_amount = 0;
    memset(menu_names, 0, sizeof(menu_names));
    memset(menu_quantities, 0, sizeof(menu_quantities));
}

int get_total_quantity(void) {
    int total_qty = 0;
    for (int i = 0; i < item_count; i++) {
        total_qty += menu_quantities[i];
    }
    return total_qty;
}
