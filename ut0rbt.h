#ifndef INNOBASE_UT0RBT_H
#define INNOBASE_UT0RBT_H

#if !defined(IB_RBT_TESTING)
#include "univ.h"
#include "ut0mem.h"
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define	ut_malloc	malloc
#define	ut_free		free
#define	ulint		unsigned long
#define	ut_a(c)		assert(c)
#define ut_error	assert(0)
#define	ibool		unsigned int
#define	TRUE		1
#define	FALSE		0
#endif

struct ib_rbt_node_t;
/*��ӡ����ָ��*/
typedef void (*ib_rbt_print_node)(const ib_rbt_node_t* node);
/*ֵ�ȽϺ���*/
typedef int (*ib_rbt_compare)(const void* p1, const void* p2);
/*�����Ƚϣ���ʲô��ͼ��*/
typedef int (*ib_rbt_arg_compare)(const void* p, const void* p1, const void* p2);

enum ib_rbt_color_t
{
	IB_RBT_RED,
	IB_RBT_BLACK,
};

/*������ڵ�ṹ*/
struct ib_rbt_node_t
{
	ib_rbt_color_t	color;	/*�ڵ����ɫ*/
	ib_rbt_node_t*  left;	/*���Ҷ��*/
	ib_rbt_node_t*	right;	/*�ұ�Ҷ��*/
	ib_rbt_node_t*	parent; /*���׽ڵ�*/
	
	char	value[1];
};

struct ib_rbt_t
{
	ib_rbt_node_t*		nil;
	ib_rbt_node_t*		root;
	ulint				n_nodes;
	ib_rbt_compare		compare;
	ib_rbt_arg_compare	compare_with_arg;
	ulint				sizeof_value;
	void*				cmp_arg;
};

struct ib_rbt_bound_t
{
	const ib_rbt_node_t*	last;
	int						result;
};

/*����ļ�������ib_rbt_t�ķ��ʺ�*/
#define rbt_size(t)	(t->n_nodes)
#define rbt_empty(t) (rbt_size(t) == 0)
#define rbt_value(t, n) ((t*) &n->value[0])
#define rbt_compare(t, k, n) (t->compare(k, n->value))

/*rb tree�ķ�������*/
/*����һ��rb tree,�����ò����ȽϺ����ͱȽϵĲ���*/
UNIV_INTERN ib_rbt_t* rbt_create_arg_cmp(size_t sizeof_value, ib_rbt_arg_compare compare, void* cmp_arg);
/*����һ��rb tree,���û����ıȽϺ���*/
UNIV_INTERN ib_rbt_t* rbt_create(size_t sizeof_value, ib_rbt_compare compare);

UNIV_INTERN void rbt_free(ib_rbt_t* tree);

UNIV_INTERN ibool rbt_delete(ib_rbt_t* tree, const void* key);

UNIV_INTERN ib_rbt_node_t* rbt_lookup(const ib_rbt_t* tree, const void* key);

UNIV_INTERN ib_rbt_node_t* rbt_remove_node(ib_rbt_t* tree, const ib_rbt_node_t* node);

UNIV_INTERN const ib_rbt_node_t* rbt_insert(ib_rbt_t* tree, const void* key, const void* value);

UNIV_INTERN CONST ib_rbt_node_t* rbt_insert(ib_rbt_t* tree, const void* key, const void* value);

UNIV_INTERN const ib_rbt_node_t* rbt_first(const ib_rbt_t* tree);

UNIV_INTERN const ib_rbt_node_t* rbt_last(const ib_rbt_t* tree);

UNIV_INTERN const ib_rbt_node_t* rbt_next(const ib_rbt_t* tree, const ib_rbt_node_t* current);

UNIV_INTERN const ib_rbt_node_t* rbt_prev(const ib_rbt_t* tree, const ib_rbt_node_t* current);

/*����>=key�����нڵ�*/
UNIV_INTERN const ib_rbt_node_t* rbt_lower_bound(const ib_rbt_t* tree, const void* key);
/*����<=key�����нڵ�*/
UNIV_INTERN const ib_rbt_node_t* rbt_upper_bound(const ib_rbt_t* tree, const void* key);

UNIV_INTERN int rbt_search(const ib_rbt_t* tree, ib_rbt_bound_t* parent, const void* key);

UNIV_INTERN int rbt_search_cmp(const ib_rbt_t* tree, ib_rbt_bound_t* parent, const void* key, ib_rbt_compare compare, ib_rbt_arg_compare arg_compare);

UNIV_INTERN void rbt_clear(ib_rbt_t* tree);

UNIV_INTERN ulint rbt_merge_uniq(ib_rbt_t* dst, const ib_dst_t* src);

UNIV_INTERN ulint rbt_merge_uniq_destructive(ib_rbt_t* dst, ib_rbt_t* src);

UNIV_INTERN ibool rbt_validate(const ib_rbt_t* tree);

UNIV_INTERN void rbt_print(const ib_rbt_t* tree, ib_rbt_print_node print);
#endif





