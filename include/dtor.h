#ifndef TETRISH_DTOR_H
#define TETRISH_DTOR_H

#include <stddef.h>
#include <assert.h>

/*!
    @return a function name, prepended by `destructor_wrapper_` and
    appended by SHA-256 hash of `destructor_wrapper_`
*/
#define DTOR_WRAPPER_NAME(fn_name) \
destructor_wrapper_##fn_name##_5add31a8a5071c80a8b550f19f99f484725898345641388f1a0f9621291b6d7c

/*!
    @note it is recommended and possible to prepend modifier
    such as `static` and `inline`.
*/
#define DTOR_WRAPPER_DEFINE(fn_name) \
void DTOR_WRAPPER_NAME(fn_name)(void* arg) { \
    fn_name((arg)); \
}

/*!
    @note destructors is intended to be defined in functions.
    One can use it outside of function but there are very little
    use cases.
    @note capacity counts both registered destructors and scope 
    markers inserted with DTOR_SCOPE.
    @note reserving capacity less than actual maximum
    usage is undefined behavior
*/
#define DTOR_DEFINE(name, capacity) \
struct dtor_##name##_8d913b1b2eef79ad73397f788585472281eead219c6c216ec48103ebefdc63cf { \
    void (*fn_list[capacity])(void*); \
    void* args[capacity]; \
    size_t size; \
    size_t cap; \
}; \
struct dtor_##name##_8d913b1b2eef79ad73397f788585472281eead219c6c216ec48103ebefdc63cf name = {.size = 0, .cap = (capacity)}

/*!
    @param destructor a lvalue to a destructor
    @param fn the function to register, of signature `void(void*)`, receiving
    a pointer to the argument
    @param arg the argument to pass.

    @note one can use this with file descpritor as follows
    ```c
    int close_ptr(const int* ptr) {
        return close(*ptr);
    }
    static DTOR_WRAPPER_DEFINE(close_ptr)

    // in a function
    int fd = open(args);
    DTOR_INSERT(dtor, close_ptr, &fd);
    ```
    note that fd's lifetime must not end before the function.
*/
#define DTOR_INSERT(destructor, fn, arg) \
    do { \
        (void)fn; \
        assert((destructor).size < (destructor).cap); \
        (destructor).fn_list[(destructor).size] = DTOR_WRAPPER_NAME(fn); \
        (destructor).args[(destructor).size] = (arg); \
        (destructor).size++; \
    } while (0)

#define DTOR_SCOPE(destructor) \
    do { \
        assert((destructor).size < (destructor).cap); \
        (destructor).fn_list[(destructor).size] = NULL; \
        (destructor).args[(destructor).size] = NULL; \
        (destructor).size++; \
    } while (0)
/*!
    @note destroy is idempotent.
*/
#define DTOR_DESTROY(destructor) \
    do { \
        for (; (destructor).size > 0;) { \
            (destructor).size--; \
            if ((destructor).fn_list[((destructor).size)] == NULL) continue; \
            (destructor).fn_list[((destructor).size)]((destructor).args[((destructor).size)]); \
        } \
    } while (0)

#define DTOR_DESTROY_SCOPE(destructor) \
    do { \
        for (; (destructor).size > 0;) { \
            (destructor).size--; \
            if ((destructor).fn_list[((destructor).size)] == NULL) break; \
            (destructor).fn_list[((destructor).size)]((destructor).args[((destructor).size)]); \
        } \
    } while (0)

/*!
    @note shorthand for `DTOR_DESTROY` followed by `return`
*/
#define DTOR_RETURN(destructor, ...) \
    do { \
        DTOR_DESTROY(destructor); \
        return __VA_ARGS__; \
    } while (0)

/*!
    @note analogous to Zig's `errdefer`.
*/
#define DTOR_ERR_RETURN(err_destructor, destructor, ...) \
    do { \
        DTOR_DESTROY(err_destructor); \
        DTOR_RETURN(destructor, __VA_ARGS__); \
    } while (0)

/*!
    @note shorthand for `DTOR_DESTROY` followed by `return`
*/
#define DTOR_RETURN_VOID(destructor) \
    do { \
        DTOR_DESTROY(destructor); \
        return; \
    } while (0)

/*!
    @note analogous to Zig's `errdefer`.
*/
#define DTOR_ERR_RETURN_VOID(err_destructor, destructor) \
    do { \
        DTOR_DESTROY(err_destructor); \
        DTOR_RETURN_VOID(destructor); \
    } while (0)

#define DTOR_CONTINUE(dtor) if (1) { \
    DTOR_DESTROY_SCOPE(dtor); \
    continue; \
} else (void)0

#define DTOR_BREAK(dtor) if (1) { \
    DTOR_DESTROY_SCOPE(dtor); \
    break; \
} else (void)0

#endif