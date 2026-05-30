#include <glib.h>
#include <stdint.h>
#include <stdlib.h>

#include <src/fsearch_array.h>

typedef struct Version {
    int major;
    int minor;
} Version;

static int32_t
sort_int_descending(void **a, void **b, void *data) {
    int32_t ia = GPOINTER_TO_INT(*a);
    int32_t ib = GPOINTER_TO_INT(*b);
    return ib - ia;
}

static int32_t
sort_int_ascending(void **a, void **b, void *data) {
    return -1 * sort_int_descending(a, b, data);
}

static void
test_main(void) {
    DynamicArray *array = darray_new(10);
    g_assert_true(darray_get_size(array) == 10);

    const int32_t upper_limit = 128;
    for (int32_t i = 0; i < upper_limit; ++i) {
        darray_add_item(array, GINT_TO_POINTER(i));
    }
    for (int32_t i = 0; i < upper_limit; ++i) {
        int32_t j = GPOINTER_TO_INT(darray_get_item(array, i));
        g_assert_true(i == j);
    }
    g_assert_true(darray_get_num_items(array) == upper_limit);

    darray_sort(array, (DynamicArrayCompareDataFunc)sort_int_descending, NULL, NULL);
    for (int32_t i = 0; i < upper_limit; ++i) {
        int32_t j = GPOINTER_TO_INT(darray_get_item(array, i));
        int32_t expected_val = upper_limit - i - 1;
        if (expected_val != j) {
            g_print("[sort] Expect %d at index %d. Result: %d\n", expected_val, i, j);
        }
        g_assert_true(expected_val == j);
        uint32_t matched_idx = 0;
        if (darray_binary_search_with_data(array,
                                           GINT_TO_POINTER(i),
                                           (DynamicArrayCompareDataFunc)sort_int_descending,
                                           NULL,
                                           &matched_idx)) {
            if (matched_idx != expected_val) {
                g_print("[bin_search] Expect %d to be at idx %d\n", i, expected_val);
            }
            g_assert_true(matched_idx == expected_val);
        }
        else {
            g_print("[bin_search] Didn't find %d!\n", i);
            g_assert_not_reached();
        }
    }

    darray_sort_multi_threaded(array, (DynamicArrayCompareDataFunc)sort_int_ascending, NULL, NULL);
    for (int32_t i = 0; i < upper_limit; ++i) {
        int32_t j = GPOINTER_TO_INT(darray_get_item(array, i));
        g_print("%d:%d\n", i, j);
    }
    for (int32_t i = 0; i < upper_limit; ++i) {
        int32_t j = GPOINTER_TO_INT(darray_get_item(array, i));
        int32_t expected_val = i;
        if (expected_val != j) {
            g_print("[threaded_sort] Expect %d at index %d. Result: %d\n", expected_val, i, j);
        }
        g_assert_true(expected_val == j);
        uint32_t matched_idx = 0;
        if (darray_binary_search_with_data(array,
                                           GINT_TO_POINTER(i),
                                           (DynamicArrayCompareDataFunc)sort_int_ascending,
                                           NULL,
                                           &matched_idx)) {
            if (expected_val != matched_idx) {
                g_print("[bin_search] Expect %d to be at idx %d\n", i, expected_val);
            }
            g_assert_true(matched_idx == expected_val);
        }
        else {
            g_print("[bin_search] Didn't find %d!\n", i);
            g_assert_not_reached();
        }
    }

    for (uint32_t i = 0; i < upper_limit - 1; ++i) {
        int32_t i1 = GPOINTER_TO_INT(darray_get_item(array, i));
        g_assert_true(i1 == i);
        uint32_t i2_idx = 0;
        int32_t i2 = GPOINTER_TO_INT(
            darray_get_item_next(array, GINT_TO_POINTER(i1), (DynamicArrayCompareDataFunc)sort_int_ascending, NULL, &i2_idx));
        g_assert_true(i2 == i1 + 1);
        g_assert_true(i2_idx == i1 + 1);
    }

    g_clear_pointer(&array, darray_unref);
}

static void
same_elements(void) {
    DynamicArray *array = darray_new(10);

    uint32_t element = 42;
    for (int32_t i = 0; i < 10; ++i) {
        darray_add_item(array, GINT_TO_POINTER(element));
    }

    for (uint32_t i = 0; i < element * 2; ++i) {
        if (i == element) {
            continue;
        }
        uint32_t matched_idx = 0;
        if (darray_binary_search_with_data(array,
                                           GINT_TO_POINTER(i),
                                           (DynamicArrayCompareDataFunc)sort_int_ascending,
                                           NULL,
                                           &matched_idx)) {
            g_assert_not_reached();
        }
    }

    g_clear_pointer(&array, darray_unref);
}

static int32_t
sort_version(void **a, void **b, void *data) {
    Version *v1 = *a;
    Version *v2 = *b;
    return v1->major - v2->major;
}

static void
test_single_and_multi_threaded_sort(DynamicArray *array, DynamicArrayCompareDataFunc comp_func) {
    DynamicArray *a1 = darray_copy(array);
    DynamicArray *a2 = darray_copy(array);
    darray_sort(a1, (DynamicArrayCompareDataFunc)sort_version, NULL, NULL);
    darray_sort_multi_threaded(a2, (DynamicArrayCompareDataFunc)sort_version, NULL, NULL);
    for (uint32_t i = 0; i < darray_get_num_items(a1); ++i) {
        Version *v1 = darray_get_item(a1, i);
        Version *v2 = darray_get_item(a2, i);
        g_print("%d.%d / %d.%d\n", v1->major, v1->minor, v2->major, v2->minor);
        g_assert(v1 == v2);
    }
}

static void
test_sort(void) {
    Version versions[] = {
        {3, 0},
        {4, 1},
        {4, 3},
        {1, 5},
        {1, 4},
        {2, 6},
        {0, 7},
        {2, 8},
        {1, 9},
        {0, 9},
        {0, 9},
        {0, 9},
        {4, 2},
        {0, 9},
        {0, 9},
        {0, 9},
    };

    DynamicArray *array = darray_new(10);
    for (uint32_t i = 0; i < G_N_ELEMENTS(versions); i++) {
        darray_add_item(array, &versions[i]);
    }
    test_single_and_multi_threaded_sort(array, (DynamicArrayCompareDataFunc)sort_version);
}

static void
test_search(void) {
    same_elements();
}

static void
test_insert_remove(void) {
    DynamicArray *array = darray_new(10);

    // Insert items maintaining ascending order
    for (int32_t i = 0; i < 10; i += 2) {
        uint32_t pos = darray_binary_search_insert_pos(array, GINT_TO_POINTER(i),
                                                        (DynamicArrayCompareDataFunc)sort_int_ascending, NULL);
        darray_insert_item_at(array, pos, GINT_TO_POINTER(i));
    }
    // array should be: 0, 2, 4, 6, 8
    g_assert_true(darray_get_num_items(array) == 5);

    // Insert odd numbers
    for (int32_t i = 1; i < 10; i += 2) {
        uint32_t pos = darray_binary_search_insert_pos(array, GINT_TO_POINTER(i),
                                                        (DynamicArrayCompareDataFunc)sort_int_ascending, NULL);
        darray_insert_item_at(array, pos, GINT_TO_POINTER(i));
    }
    // array should be: 0, 1, 2, 3, 4, 5, 6, 7, 8, 9
    g_assert_true(darray_get_num_items(array) == 10);
    for (int32_t i = 0; i < 10; i++) {
        g_assert_true(GPOINTER_TO_INT(darray_get_item(array, i)) == i);
    }

    // Remove item at index 5 (value 5)
    darray_remove_item_at(array, 5);
    g_assert_true(darray_get_num_items(array) == 9);
    g_assert_true(GPOINTER_TO_INT(darray_get_item(array, 5)) == 6);

    // Remove first item
    darray_remove_item_at(array, 0);
    g_assert_true(darray_get_num_items(array) == 8);
    g_assert_true(GPOINTER_TO_INT(darray_get_item(array, 0)) == 1);

    // Remove last item
    darray_remove_item_at(array, darray_get_num_items(array) - 1);
    g_assert_true(darray_get_num_items(array) == 7);

    g_clear_pointer(&array, darray_unref);
}

static void
test_binary_search_insert_pos(void) {
    DynamicArray *array = darray_new(10);

    // Empty array
    uint32_t pos = darray_binary_search_insert_pos(array, GINT_TO_POINTER(5),
                                                    (DynamicArrayCompareDataFunc)sort_int_ascending, NULL);
    g_assert_true(pos == 0);

    // Build sorted array: 10, 20, 30, 40, 50
    for (int32_t i = 1; i <= 5; i++) {
        darray_add_item(array, GINT_TO_POINTER(i * 10));
    }

    // Insert at beginning
    pos = darray_binary_search_insert_pos(array, GINT_TO_POINTER(5),
                                           (DynamicArrayCompareDataFunc)sort_int_ascending, NULL);
    g_assert_true(pos == 0);

    // Insert in middle
    pos = darray_binary_search_insert_pos(array, GINT_TO_POINTER(25),
                                           (DynamicArrayCompareDataFunc)sort_int_ascending, NULL);
    g_assert_true(pos == 2);

    // Insert at end
    pos = darray_binary_search_insert_pos(array, GINT_TO_POINTER(55),
                                           (DynamicArrayCompareDataFunc)sort_int_ascending, NULL);
    g_assert_true(pos == 5);

    // Insert duplicate value - should find existing position
    pos = darray_binary_search_insert_pos(array, GINT_TO_POINTER(30),
                                           (DynamicArrayCompareDataFunc)sort_int_ascending, NULL);
    g_assert_true(pos == 2);  // at or before existing 30

    g_clear_pointer(&array, darray_unref);
}

int
main(int argc, char *argv[]) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/FSearch/array/main", test_main);
    g_test_add_func("/FSearch/array/sort", test_sort);
    g_test_add_func("/FSearch/array/search", test_search);
    g_test_add_func("/FSearch/array/insert_remove", test_insert_remove);
    g_test_add_func("/FSearch/array/binary_search_insert_pos", test_binary_search_insert_pos);
    return g_test_run();
}
