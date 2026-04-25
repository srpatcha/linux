// SPDX-License-Identifier: GPL-2.0-only
/*
 * KUnit test suite for lib/list_sort.c
 *
 * Extended tests covering edge cases, stability, and various list
 * configurations that the existing test_list_sort.c does not exercise
 * individually.
 */
#include <kunit/test.h>

#include <linux/kernel.h>
#include <linux/list_sort.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/random.h>

#define POISON1 0xDEADBEEF
#define POISON2 0xA324354C

struct test_element {
	unsigned int poison1;
	struct list_head list;
	unsigned int poison2;
	int value;
	unsigned int serial;
};

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static int cmp_asc(void *priv, const struct list_head *a,
		    const struct list_head *b)
{
	const struct test_element *ea = container_of(a, struct test_element, list);
	const struct test_element *eb = container_of(b, struct test_element, list);

	return ea->value - eb->value;
}

static int cmp_desc(void *priv, const struct list_head *a,
		     const struct list_head *b)
{
	return -cmp_asc(priv, a, b);
}

static int cmp_abs(void *priv, const struct list_head *a,
		    const struct list_head *b)
{
	const struct test_element *ea = container_of(a, struct test_element, list);
	const struct test_element *eb = container_of(b, struct test_element, list);
	int va = ea->value < 0 ? -ea->value : ea->value;
	int vb = eb->value < 0 ? -eb->value : eb->value;

	return va - vb;
}

static int cmp_zero(void *priv, const struct list_head *a,
		     const struct list_head *b)
{
	return 0;
}

static struct test_element *alloc_element(struct kunit *test, int value,
					  unsigned int serial)
{
	struct test_element *el;

	el = kunit_kmalloc(test, sizeof(*el), GFP_KERNEL);
	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, el);
	el->value   = value;
	el->serial  = serial;
	el->poison1 = POISON1;
	el->poison2 = POISON2;
	return el;
}

static void verify_sorted(struct kunit *test, struct list_head *head,
			   int expected_count,
			   int (*cmp_fn)(void *, const struct list_head *,
					 const struct list_head *))
{
	struct list_head *cur;
	int count = 0;

	/* Verify doubly-linked integrity and sort order */
	list_for_each(cur, head) {
		KUNIT_EXPECT_EQ_MSG(test, cur->next->prev, cur,
				    "corrupted next->prev link");
		if (cur->next != head) {
			KUNIT_EXPECT_LE_MSG(test, cmp_fn(NULL, cur, cur->next), 0,
					    "list is not sorted at position %d", count);
		}
		count++;
	}
	KUNIT_EXPECT_EQ_MSG(test, count, expected_count,
			    "expected %d elements, got %d", expected_count, count);
	/* last element's next must point back to head */
	KUNIT_EXPECT_EQ_MSG(test, head->prev->next, head,
			    "corrupted tail->next link");
}

static void verify_stability(struct kunit *test, struct list_head *head,
			      int (*cmp_fn)(void *, const struct list_head *,
					    const struct list_head *))
{
	struct list_head *cur;

	list_for_each(cur, head) {
		if (cur->next != head) {
			const struct test_element *a, *b;

			a = container_of(cur, struct test_element, list);
			b = container_of(cur->next, struct test_element, list);

			if (cmp_fn(NULL, cur, cur->next) == 0) {
				KUNIT_EXPECT_LT_MSG(test, a->serial, b->serial,
						    "stability violated: serial %u >= %u",
						    a->serial, b->serial);
			}
		}
	}
}

static void verify_poison(struct kunit *test, struct list_head *head)
{
	struct test_element *el;

	list_for_each_entry(el, head, list) {
		KUNIT_EXPECT_EQ_MSG(test, el->poison1, POISON1,
				    "poison1 corrupted at serial %u", el->serial);
		KUNIT_EXPECT_EQ_MSG(test, el->poison2, POISON2,
				    "poison2 corrupted at serial %u", el->serial);
	}
}

/* ------------------------------------------------------------------ */
/*  Test cases                                                         */
/* ------------------------------------------------------------------ */

static void list_sort_empty(struct kunit *test)
{
	LIST_HEAD(head);

	list_sort(NULL, &head, cmp_asc);

	KUNIT_EXPECT_TRUE_MSG(test, list_empty(&head),
			      "empty list should remain empty after sort");
}

static void list_sort_single(struct kunit *test)
{
	struct test_element *el;
	LIST_HEAD(head);

	el = alloc_element(test, 42, 0);
	list_add(&el->list, &head);

	list_sort(NULL, &head, cmp_asc);

	KUNIT_EXPECT_FALSE(test, list_empty(&head));
	verify_sorted(test, &head, 1, cmp_asc);
	verify_poison(test, &head);
}

static void list_sort_two_ordered(struct kunit *test)
{
	struct test_element *a, *b;
	LIST_HEAD(head);

	a = alloc_element(test, 1, 0);
	b = alloc_element(test, 2, 1);
	list_add_tail(&a->list, &head);
	list_add_tail(&b->list, &head);

	list_sort(NULL, &head, cmp_asc);

	verify_sorted(test, &head, 2, cmp_asc);

	KUNIT_EXPECT_EQ(test,
			container_of(head.next, struct test_element, list)->value, 1);
	KUNIT_EXPECT_EQ(test,
			container_of(head.prev, struct test_element, list)->value, 2);
}

static void list_sort_two_reversed(struct kunit *test)
{
	struct test_element *a, *b;
	LIST_HEAD(head);

	a = alloc_element(test, 5, 0);
	b = alloc_element(test, 3, 1);
	list_add_tail(&a->list, &head);
	list_add_tail(&b->list, &head);

	list_sort(NULL, &head, cmp_asc);

	verify_sorted(test, &head, 2, cmp_asc);

	KUNIT_EXPECT_EQ(test,
			container_of(head.next, struct test_element, list)->value, 3);
}

static void list_sort_already_sorted(struct kunit *test)
{
	int i;
	LIST_HEAD(head);

	for (i = 0; i < 64; i++)
		list_add_tail(&alloc_element(test, i, i)->list, &head);

	list_sort(NULL, &head, cmp_asc);

	verify_sorted(test, &head, 64, cmp_asc);
	verify_poison(test, &head);
}

static void list_sort_reverse_sorted(struct kunit *test)
{
	int i;
	LIST_HEAD(head);

	for (i = 63; i >= 0; i--)
		list_add_tail(&alloc_element(test, i, 63 - i)->list, &head);

	list_sort(NULL, &head, cmp_asc);

	verify_sorted(test, &head, 64, cmp_asc);
	verify_poison(test, &head);
}

static void list_sort_all_equal(struct kunit *test)
{
	int i;
	LIST_HEAD(head);

	for (i = 0; i < 32; i++)
		list_add_tail(&alloc_element(test, 7, i)->list, &head);

	list_sort(NULL, &head, cmp_asc);

	verify_sorted(test, &head, 32, cmp_asc);
	verify_stability(test, &head, cmp_asc);
	verify_poison(test, &head);
}

static void list_sort_stability(struct kunit *test)
{
	int i;
	LIST_HEAD(head);

	/*
	 * Create elements where value = serial / 4, so groups of 4 share the
	 * same sort key.  After sorting, each group must preserve the original
	 * insertion order (serial order).
	 */
	for (i = 0; i < 80; i++)
		list_add_tail(&alloc_element(test, i / 4, i)->list, &head);

	list_sort(NULL, &head, cmp_asc);

	verify_sorted(test, &head, 80, cmp_asc);
	verify_stability(test, &head, cmp_asc);
}

static void list_sort_descending(struct kunit *test)
{
	int i;
	LIST_HEAD(head);

	for (i = 0; i < 50; i++)
		list_add_tail(&alloc_element(test, get_random_u32_below(1000), i)->list,
			      &head);

	list_sort(NULL, &head, cmp_desc);

	verify_sorted(test, &head, 50, cmp_desc);
	verify_poison(test, &head);
}

static void list_sort_negative_values(struct kunit *test)
{
	int values[] = { -5, 3, -1, 0, 7, -3, 2, -8, 4, -2 };
	int i;
	LIST_HEAD(head);

	for (i = 0; i < ARRAY_SIZE(values); i++)
		list_add_tail(&alloc_element(test, values[i], i)->list, &head);

	list_sort(NULL, &head, cmp_asc);

	verify_sorted(test, &head, ARRAY_SIZE(values), cmp_asc);
}

static void list_sort_absolute_value(struct kunit *test)
{
	int values[] = { -5, 3, -1, 0, 7, -3, 2, -8, 4, -2 };
	int i;
	LIST_HEAD(head);

	for (i = 0; i < ARRAY_SIZE(values); i++)
		list_add_tail(&alloc_element(test, values[i], i)->list, &head);

	list_sort(NULL, &head, cmp_abs);

	verify_sorted(test, &head, ARRAY_SIZE(values), cmp_abs);
}

static void list_sort_zero_comparator(struct kunit *test)
{
	int i;
	LIST_HEAD(head);

	for (i = 0; i < 16; i++)
		list_add_tail(&alloc_element(test, i, i)->list, &head);

	list_sort(NULL, &head, cmp_zero);

	/* With cmp always returning 0, list should remain valid */
	verify_sorted(test, &head, 16, cmp_zero);
	verify_stability(test, &head, cmp_zero);
	verify_poison(test, &head);
}

static void list_sort_large_list(struct kunit *test)
{
	int i;
	LIST_HEAD(head);
	const int count = 1024;

	for (i = 0; i < count; i++)
		list_add_tail(&alloc_element(test, get_random_u32_below(count / 3),
					     i)->list, &head);

	list_sort(NULL, &head, cmp_asc);

	verify_sorted(test, &head, count, cmp_asc);
	verify_stability(test, &head, cmp_asc);
	verify_poison(test, &head);
}

static void list_sort_random_data(struct kunit *test)
{
	int i;
	LIST_HEAD(head);
	const int count = 256;

	for (i = 0; i < count; i++)
		list_add_tail(&alloc_element(test, (int)get_random_u32_below(10000) - 5000,
					     i)->list, &head);

	list_sort(NULL, &head, cmp_asc);

	verify_sorted(test, &head, count, cmp_asc);
	verify_poison(test, &head);
}

static void list_sort_power_of_two(struct kunit *test)
{
	int i;
	LIST_HEAD(head);

	/* Power-of-two length exercises specific merge tree shapes */
	for (i = 0; i < 128; i++)
		list_add_tail(&alloc_element(test, 128 - i, i)->list, &head);

	list_sort(NULL, &head, cmp_asc);

	verify_sorted(test, &head, 128, cmp_asc);
}

static void list_sort_power_of_two_plus_one(struct kunit *test)
{
	int i;
	LIST_HEAD(head);

	for (i = 0; i < 129; i++)
		list_add_tail(&alloc_element(test, 129 - i, i)->list, &head);

	list_sort(NULL, &head, cmp_asc);

	verify_sorted(test, &head, 129, cmp_asc);
}

static void list_sort_duplicates_at_boundaries(struct kunit *test)
{
	int i;
	LIST_HEAD(head);

	/* First and last elements share the same value */
	list_add_tail(&alloc_element(test, 50, 0)->list, &head);
	for (i = 1; i < 31; i++)
		list_add_tail(&alloc_element(test, i, i)->list, &head);
	list_add_tail(&alloc_element(test, 50, 31)->list, &head);

	list_sort(NULL, &head, cmp_asc);

	verify_sorted(test, &head, 32, cmp_asc);
	verify_stability(test, &head, cmp_asc);
}

static struct kunit_case list_sort_extended_cases[] = {
	KUNIT_CASE(list_sort_empty),
	KUNIT_CASE(list_sort_single),
	KUNIT_CASE(list_sort_two_ordered),
	KUNIT_CASE(list_sort_two_reversed),
	KUNIT_CASE(list_sort_already_sorted),
	KUNIT_CASE(list_sort_reverse_sorted),
	KUNIT_CASE(list_sort_all_equal),
	KUNIT_CASE(list_sort_stability),
	KUNIT_CASE(list_sort_descending),
	KUNIT_CASE(list_sort_negative_values),
	KUNIT_CASE(list_sort_absolute_value),
	KUNIT_CASE(list_sort_zero_comparator),
	KUNIT_CASE(list_sort_large_list),
	KUNIT_CASE(list_sort_random_data),
	KUNIT_CASE(list_sort_power_of_two),
	KUNIT_CASE(list_sort_power_of_two_plus_one),
	KUNIT_CASE(list_sort_duplicates_at_boundaries),
	{}
};

static struct kunit_suite list_sort_extended_suite = {
	.name = "list_sort_extended",
	.test_cases = list_sort_extended_cases,
};

kunit_test_suites(&list_sort_extended_suite);

MODULE_DESCRIPTION("Extended list_sort() KUnit test suite");
MODULE_LICENSE("GPL");
