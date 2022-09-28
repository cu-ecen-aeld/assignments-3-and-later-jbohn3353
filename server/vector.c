// Simple monotonic dynamic array
// Author: James Bohn

#include "vector.h"
#include <stdlib.h>
#include <string.h>

/// @brief Intializes provided vector by allocating memory for it
/// @param vec pointer to vector to initialize
/// @return 0 on success, -1 on failure
int vector_init(vector *vec){
    vec->buf = malloc(VECTOR_BASE_SIZE);
    if(vec->buf == NULL){
        return -1;
    }
    
    vec->len = 0;
    vec->cap = VECTOR_BASE_SIZE;
    memset(vec->buf, 0, vec->cap);

    return 0;
}

/// @brief Appends data to the provided vector, allocating more memory if necessary
/// @param vec pointer to vector to append data to
/// @param data pointer to data to append
/// @param len length in bytes of data to append
/// @return 0 on success, -1 on failure
int vector_append(vector *vec, void *data, size_t len){
    while(vec->cap < vec->len + len){
        void *new_buf = malloc(2*vec->cap);
        if(new_buf == NULL){
            return -1;
        }

        vec->cap = 2*vec->cap;
        memset(new_buf, 0, vec->cap);
        memcpy(new_buf, vec->buf, vec->len);
        
        free(vec->buf);
        vec->buf = new_buf;
    }

    memcpy(vec->buf + vec->len, data, len);
    vec->len += len;

    return 0;
}

/// @brief Free's memory associated with vector and marks it unusable until
///        reinitialized
/// @param vec pointer to vector to close
void vector_close(vector *vec){
    free(vec->buf);
    vec->len = 0;
    vec->cap = 0;
}