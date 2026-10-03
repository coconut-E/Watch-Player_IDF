#ifndef ATOMIC_UTILS_H
#define ATOMIC_UTILS_H

#include <stdint.h>
#include <stdbool.h>

static inline bool atomic_load_bool(const volatile bool *p) {
    return __atomic_load_n(const_cast<const bool*>(p), __ATOMIC_SEQ_CST);
}

static inline void atomic_store_bool(volatile bool *p, bool val) {
    __atomic_store_n(const_cast<bool*>(p), val, __ATOMIC_SEQ_CST);
}

static inline int32_t atomic_load_int(const volatile int32_t *p) {
    return __atomic_load_n(const_cast<const int32_t*>(p), __ATOMIC_SEQ_CST);
}

static inline void atomic_store_int(volatile int32_t *p, int32_t val) {
    __atomic_store_n(const_cast<int32_t*>(p), val, __ATOMIC_SEQ_CST);
}

static inline int atomic_load_int(const volatile int *p) {
    return __atomic_load_n(const_cast<const int*>(p), __ATOMIC_SEQ_CST);
}

static inline void atomic_store_int(volatile int *p, int val) {
    __atomic_store_n(const_cast<int*>(p), val, __ATOMIC_SEQ_CST);
}

static inline float atomic_load_float(const volatile float *p) {
    float val;
    __atomic_load(const_cast<const float*>(p), &val, __ATOMIC_SEQ_CST);
    return val;
}

static inline void atomic_store_float(volatile float *p, float val) {
    __atomic_store(const_cast<float*>(p), &val, __ATOMIC_SEQ_CST);
}

#endif // ATOMIC_UTILS_H
