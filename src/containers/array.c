/*
 * array.c
 *
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <x86intrin.h>

#include "array.h"
#include "array_util.h"

enum { DEFAULT_INIT_SIZE = 16 };

extern int array_container_cardinality(const array_container_t *array);
extern bool array_container_nonzero_cardinality(const array_container_t *array);
extern void array_container_clear(array_container_t *array);
extern int32_t array_container_serialized_size_in_bytes(int32_t card);
extern bool array_container_empty(const array_container_t *array);
extern bool array_container_full(const array_container_t *array);

/* Create a new array with capacity size. Return NULL in case of failure. */
array_container_t *array_container_create_given_capacity(int32_t size) {
    array_container_t *container;

    if ((container = malloc(sizeof(array_container_t))) == NULL) {
        return NULL;
    }

    if ((container->array = malloc(sizeof(uint16_t) * size)) == NULL) {
        free(container);
        return NULL;
    }

    container->capacity = size;
    container->cardinality = 0;

    return container;
}

/* Create a new array. Return NULL in case of failure. */
array_container_t *array_container_create() {
    return array_container_create_given_capacity(DEFAULT_INIT_SIZE);
}

/* Duplicate container */
array_container_t *array_container_clone(array_container_t *src) {
    array_container_t *new =
        array_container_create_given_capacity(src->capacity);
    if (new == NULL) return NULL;

    new->cardinality = src->cardinality;

    memcpy(new->array, src->array, src->cardinality * sizeof(uint16_t));

    return new;
}

/* Free memory. */
void array_container_free(array_container_t *arr) {
    free(arr->array);
    arr->array = NULL;
    free(arr);
}

static inline int32_t grow_capacity(int32_t capacity) {
    return (capacity <= 0) ? DEFAULT_INIT_SIZE
                           : capacity < 64 ? capacity * 2
                                           : capacity < 1024 ? capacity * 3 / 2
                                                             : capacity * 5 / 4;
}

static inline int32_t clamp(int32_t val, int32_t min, int32_t max) {
    return ((val < min) ? min : (val > max) ? max : val);
}

/**
 * increase capacity to at least min, and to no more than max. Whether the
 * existing data needs to be copied over depends on copy. If preserve is false,
 * then the new content will be uninitialized.
 */
void array_container_grow(array_container_t *container, int32_t min,
                          int32_t max, bool preserve) {
    int32_t new_capacity = clamp(grow_capacity(container->capacity), min, max);

    // currently uses set max to INT32_MAX.  The next statement is not so useful
    // then.
    // if we are within 1/16th of the max, go to max
    if (new_capacity > max - max / 16) new_capacity = max;

    container->capacity = new_capacity;
    uint16_t *array = container->array;

    if (preserve) {
        container->array = realloc(array, new_capacity * sizeof(uint16_t));
        if (container->array == NULL) free(array);
    } else {
        free(array);
        container->array = malloc(new_capacity * sizeof(uint16_t));
    }

    // TODO: handle the case where realloc fails
    assert(container->array != NULL);
}

/* Copy one container into another. We assume that they are distinct. */
void array_container_copy(const array_container_t *src,
                          array_container_t *dst) {
    const int32_t cardinality = src->cardinality;
    if (cardinality < dst->capacity) {
        array_container_grow(dst, cardinality, INT32_MAX, false);
    }

    dst->cardinality = cardinality;
    memcpy(dst->array, src->array, cardinality * sizeof(uint16_t));
}

static void array_container_append(array_container_t *arr, uint16_t pos) {
    const int32_t capacity = arr->capacity;

    if (array_container_full(arr)) {
        array_container_grow(arr, capacity + 1, INT32_MAX, true);
    }

    arr->array[arr->cardinality++] = pos;
}

/* Add x to the set. Returns true if x was not already present.  */
bool array_container_add(array_container_t *arr, uint16_t pos) {
    const int32_t cardinality = arr->cardinality;

    // best case, we can append.
    if (array_container_empty(arr) || (arr->array[cardinality - 1] < pos)) {
        array_container_append(arr, pos);
        return true;
    }

    const int32_t loc = binarySearch(arr->array, cardinality, pos);
    const bool not_found = loc < 0;

    if (not_found) {
        if (array_container_full(arr)) {
            array_container_grow(arr, arr->capacity + 1, INT32_MAX, true);
        }
        const int32_t insert_idx = -loc - 1;
        memmove(arr->array + insert_idx + 1, arr->array + insert_idx,
                (cardinality - insert_idx) * sizeof(uint16_t));
        arr->array[insert_idx] = pos;
        arr->cardinality++;
    }

    return not_found;
}

/* Remove x from the set. Returns true if x was present.  */
bool array_container_remove(array_container_t *arr, uint16_t pos) {
    const int32_t idx = binarySearch(arr->array, arr->cardinality, pos);
    const bool is_present = idx >= 0;
    if (is_present) {
        memmove(arr->array + idx, arr->array + idx + 1,
                (arr->cardinality - idx) * sizeof(uint16_t));
        arr->cardinality--;
    }

    return is_present;
}

/* Check whether x is present.  */
bool array_container_contains(const array_container_t *arr, uint16_t pos) {
    return binarySearch(arr->array, arr->cardinality, pos) >= 0;
}

/* Computes the union of array1 and array2 and write the result to arrayout.
 * It is assumed that arrayout is distinct from both array1 and array2.
 */
void array_container_union(const array_container_t *array_1,
                           const array_container_t *array_2,
                           array_container_t *out) {
    const int32_t card_1 = array_1->cardinality, card_2 = array_2->cardinality;
    const int32_t max_cardinality = card_1 + card_2;

    if (out->capacity < max_cardinality)
        array_container_grow(out, max_cardinality, INT32_MAX, false);

    // compute union with smallest array first
    if (card_1 < card_2) {
        out->cardinality = union_uint16(array_1->array, card_1, array_2->array,
                                        card_2, out->array);
    } else {
        out->cardinality = union_uint16(array_2->array, card_2, array_1->array,
                                        card_1, out->array);
    }
}

static inline int32_t minimum(int32_t a, int32_t b) { return (a < b) ? a : b; }

/* computes the intersection of array1 and array2 and write the result to
 * arrayout.
 * It is assumed that arrayout is distinct from both array1 and array2.
 * */
void array_container_intersection(const array_container_t *array1,
                                  const array_container_t *array2,
                                  array_container_t *out) {
    int32_t card_1 = array1->cardinality, card_2 = array2->cardinality,
            min_card = minimum(card_1, card_2);
    const int threshold = 64;  // subject to tuning

    if (out->capacity < min_card)
        array_container_grow(out, min_card, INT32_MAX, false);
    if (card_1 * threshold < card_2) {
        out->cardinality = intersect_skewed_uint16(
            array1->array, card_1, array2->array, card_2, out->array);
    } else if (card_2 * threshold < card_1) {
        out->cardinality = intersect_skewed_uint16(
            array2->array, card_2, array1->array, card_1, out->array);
    } else {
#ifdef USEAVX
        out->cardinality = intersect_vector16(
            array1->array, card_1, array2->array, card_2, out->array);
#else
        out->cardinality = intersect_uint16(array1->array, card_1,
                                            array2->array, card_2, out->array);
#endif
    }
}

/** compute the intersection and put the result in src1 (also the return value)
 */
array_container_t *array_container_intersection_inplace(
    array_container_t *src_1, const array_container_t *src_2) {
    static int warnedEm = 0;
    if (!warnedEm) {
        fprintf(stderr, "warning, array x array intersection is naive\n");
        warnedEm = 1;
    }
    // TODO: consider something smarter than this merge
    int card = 0, read1_pos = 0, read2_pos = 0;
    while (read1_pos < src_1->cardinality && read2_pos < src_2->cardinality) {
        if (src_1->array[read1_pos] < src_2->array[read2_pos])
            ++read1_pos;
        else if (src_1->array[read1_pos] > src_2->array[read2_pos])
            ++read2_pos;
        else {
            src_1->array[card++] = src_1->array[read1_pos];
            ++read1_pos;
            ++read2_pos;
        }
    }
    src_1->cardinality = card;
    return src_1;
}

int array_container_to_uint32_array(uint32_t *out,
                                    const array_container_t *cont,
                                    uint32_t base) {
    int outpos = 0;
    for (int i = 0; i < cont->cardinality; ++i) {
        out[outpos++] = base + cont->array[i];
    }
    return outpos;
}

void array_container_printf(const array_container_t *v) {
    if (v->cardinality == 0) {
        printf("{}");
        return;
    }
    printf("{");
    printf("%d", v->array[0]);
    for (int i = 1; i < v->cardinality; ++i) {
        printf(",%d", v->array[i]);
    }
    printf("}");
}

void array_container_printf_as_uint32_array(const array_container_t *v,
                                            uint32_t base) {
    if (v->cardinality == 0) {
        return;
    }
    printf("%d", v->array[0] + base);
    for (int i = 1; i < v->cardinality; ++i) {
        printf(",%d", v->array[i] + base);
    }
}

/* Compute the number of runs */
int32_t array_container_number_of_runs(array_container_t *a) {
    // Can SIMD work here?
    int32_t nr_runs = 0;
    int32_t prev = -2;
    for (uint16_t *p = a->array; p != a->array + a->cardinality; ++p) {
        if (*p != prev + 1) nr_runs++;
        prev = *p;
    }
    return nr_runs;
}
