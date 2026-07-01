#ifndef TETRISH_VECTOR_H
#define TETRISH_VECTOR_H

#include <stdlib.h>

typedef struct Vector {
    void* arr_;
    size_t elem_size_;
    size_t size_;
    size_t capacity_;
} Vector;

#define VECTOR_AT(vec, idx, type) (((type)*)vector_at(vec, idx))

int vector_init(Vector* vector, size_t elem_size);
size_t vector_size(const Vector* vec);
int vector_reserve(Vector* vec, size_t new_capacity);
int vector_push(Vector* vec, const void* elem);
void* vector_at(const Vector* vec, size_t idx);
void vector_erase(Vector* vec, size_t idx);
void vector_free(Vector* vec);
int vector_insert(Vector* vec, const void* elem, size_t idx);

#endif