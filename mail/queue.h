#pragma once
#include <stddef.h>
void queue_start(void);
int queue_add(const char *from, char rcpts[][320], int n, const char *msg, size_t len);
