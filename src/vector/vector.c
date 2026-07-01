#include <vector.h>
#include <string.h>
#include <assert.h>

int vector_init(Vector* vector, size_t elem_size) {
    assert(elem_size > 0);
    const size_t INIT_CAPACITY = 8*elem_size;

    vector->arr_ = malloc(INIT_CAPACITY);
    vector->elem_size_ = elem_size;
    vector->size_ = 0;
    vector->capacity_ = INIT_CAPACITY;

    if (vector->arr_ == NULL) {
        return -1;
    }
    return 0;
}

size_t vector_size(const Vector* vec) {
    return vec->size_;
}

int vector_reserve(Vector* vec, size_t new_capacity) {
    void* tmp = realloc(vec->arr_, new_capacity);
    if (tmp == NULL) {
        return -1;
    }
    vec->arr_ = tmp;
    vec->capacity_ = new_capacity;
    return 0;
}

int vector_push(Vector* vec, const void* elem) {
    return vector_insert(vec, elem, vec->size_);
}

int vector_insert(Vector* vec, const void* elem, size_t idx) {
    assert(idx <= vec->elem_size_);
    if (vec->size_*vec->elem_size_ == vec->capacity_) {
        if (vector_reserve(vec, vec->capacity_*2) == -1) {
            return -1;
        }
    }
    unsigned char* base = (unsigned char*)vec->arr_ + idx*vec->elem_size_;
    memmove(
        base + vec->elem_size_, 
        base,
        (vec->size_ - idx)*vec->elem_size_);
    memcpy(base, (unsigned char*)elem, vec->elem_size_);
    vec->size_ += 1;
    return 0;
}

void* vector_at(const Vector* vec, size_t idx) {
    assert((idx < vec->size_));
    return (void*)&((unsigned char*)(vec->arr_))[idx*vec->elem_size_];
}

void vector_erase(Vector* vec, size_t idx) {
    assert((idx < vec->size_));
    const size_t buf_idx = vec->elem_size_*idx;
    memmove(
        (unsigned char*)vec->arr_ + buf_idx, 
        (unsigned char*)vec->arr_ + buf_idx + vec->elem_size_, 
        (vec->size_ - idx - 1)*vec->elem_size_);
    vec->size_ -= 1;
}

void vector_free(Vector* vec) {
    free(vec->arr_);
    memset(vec, 0, sizeof(Vector));
}