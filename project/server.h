#ifndef SERVER_H
#define SERVER_H

#include <stdio.h>

void* request_handler(void* arg);
static void send_error(FILE* out, int code, const char* msg);
static void send_json (FILE* out, int code, const char* body);
static void send_file (FILE* out, const char* filename);

#endif

