#ifndef IB_LIST_H
#define IB_LIST_H

#include "univ.h"
#include "mem0mem.h"

/*list�ṹ*/
struct ib_list_t
{
	ib_list_node_t* first;
	ib_list_node_t* last;
	ibool			is_heap_list; /*�Ƿ�ͨ��mem heap�����ڴ�*/
};

/*list�Ľڵ����ݽṹ*/
struct ib_list_node_t
{
	ib_list_node_t* prev;		/*��һ���ڵ�*/
	ib_list_node_t* next;		/*��һ���ڵ�*/
	void*			data;		/*����ָ��*/
};

struct ib_list_helper_t
{
	mem_heap_t* heap; /*�ڴ�ض�*/
	void*		data;
};

UNIV_INTERN ib_list_t* ib_list_create(void);
UNIV_INTERN ib_list_t* ib_list_create_heap(mem_heap_t* heap);

UNIV_INTERN void ib_list_free(ib_list_t* list);

UNIV_INTERN ib_list_node_t* ib_list_add_first(ib_list_t* list, void* data, mem_heap_t* heap);
UNIV_INTERN ib_list_node_t* ib_list_add_last(ib_list_t* list, void* data, mem_heap_t* heap);
UNIV_INTERN ib_list_node_t* ib_list_add_after(ib_list_t* list, ib_list_node_t* prev_node, void* data, mem_heap_t* heap);

UNIV_INTERN void ib_list_remove(ib_list_t* list, ib_list_node_t* node);

UNIV_INTERN ib_list_node_t* ib_list_get_first(ib_list_t* list);
UNIV_INTERN ib_list_node_t* ib_list_get_last(ib_list_t* list);

UNIV_INTERN ibool ib_list_is_empty(const ib_list_t* list);


#endif


