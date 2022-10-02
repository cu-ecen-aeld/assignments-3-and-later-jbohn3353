// Simple monotonic dynamic array
// Author: James Bohn

#include <stdio.h>

#define VECTOR_BASE_SIZE 4096

typedef struct {
    void *buf;
    size_t len;
    size_t cap;
} vector;

int vector_init(vector *vec);
int vector_append(vector *vec, void *data, size_t len);
void *vector_find(vector *vec, int offset, char token);
void vector_carryover(vector *vec, int used);
void vector_close(vector *vec);