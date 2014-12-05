#ifndef __fut0lst_h_
#define __fut0lst_h_

#include "univ.h"

#include "fil0fil.h"
#include "mtr0mtr.h"

typedef byte		flst_base_node_t;
typedef byte		flst_node_t;

#define FLST_BASE_NODE_SIZE		(4 + 2 * FIL_ADDR_SIZE)
#define FLST_NODE_SIZE			(2 * FIL_ADDR_SIZE)

/*�����ϵĵ�������*/

/*��ʼ��һ����������*/
UNIV_INLINE void			flst_init(flst_base_node_t* base, mtr_t* mtr);
/*�������Ԫ����*/
UNIV_INLINE ulint			flst_get_len(flst_base_node_t* );
/*�������ĵ�һ��node��ַ*/
UNIV_INLINE fil_addr_t		flst_get_first(flst_base_node_t* base, mtr_t* mtr);
/*����������һ��node��ַ*/
UNIV_INLINE fil_addr_t		flst_get_last(flst_base_node_t* base, mtr_t* mtr);
/*���node��һ����Ԫ�ĵ�ַ*/
UNIV_INLINE	fil_addr_t		flst_get_next_addr(flst_base_node_t* node, mtr_t* mtr);
/*���node��һ����Ԫ�ĵ�ַ*/
UNIV_INLINE fil_addr_t		flst_get_prev_addr(flst_base_node_t* node, mtr_t* mtr);
/*�޸�node��Ӧ�ĵ�ַ*/
UNIV_INLINE void			flst_write_addr(fil_faddr_t* faddr, fil_addr_t addr, mtr_t* mtr);
/*��node��ȡ��Ӧ�ĵ�ַ*/
UNIV_INLINE fil_addr_t		flst_read_addr(fil_faddr_t* faddr, mtr_t* mtr);
/*��node���뵽��������*/
void						flst_add_last(flst_base_node_t* base, flst_node_t* node, mtr_t* mtr);
/*��������뵽�����ǰ��*/
void						flst_add_first(flst_base_node_t* base, flst_node_t* node, mtr_t* mtr);
/*��node1�ĺ������node2*/
void						flst_insert_after(flst_base_node_t* base, flst_node_t* node1, flst_node_t* node2, mtr_t* mtr);
/*��node3��ǰ�����node2*/
void						flst_insert_before(flst_base_node_t* base, flst_node_t* node2, flst_node_t* node3, mtr_t* mtr);
/*ɾ�������node2*/
void						flst_remove(flst_base_node_t* base, flst_node_t* node2, mtr_t* mtr);
/*ɾ������node2֮������нڵ�*/
void						flst_cur_end(flst_base_node_t* base, flst_node_t* node2, ulint n_nodes, mtr_t* mtr);

void						flst_truncate_end(flst_base_node_t* base, flst_node_t* node2, ulint n_nodes, mtr_t* mtr);

ibool						flst_validate(flst_base_node_t* base, mtr_t* mtr1);

void						flst_print(flst_base_node_t* base, mtr_t* mtr);

#include "fut0lst.inl"

#endif




