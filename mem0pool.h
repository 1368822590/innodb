#ifndef __MEM0POOL_H_
#define __MEM0POOL_H_

#include "univ.h"
#include "os0file.h"
#include "ut0lst.h"

typedef struct mem_area_struct mem_area_t;
typedef struct mem_pool_struct mem_pool_t;

extern mem_pool_t* mem_comm_pool;

struct mem_area_struct
{
	ulint	size_and_free;
	UT_LIST_NODE_T(mem_area_t) free_list;
};

/*8�ֽڶ���*/
#define MEM_AREA_EXTRA_SIZE (ut_calc_align(sizeof(struct mem_area_struct), UNIV_MEM_ALIGNMENT))

/*����һ��mem_pool_t*/
mem_pool_t* mem_pool_create(ulint size);
/*��mem_pool_t�Ϸ���һ���ڴ�*/
void* mem_area_alloc(ulint size, mem_pool_t* pool);
/*�ͷ�һ���ڴ��mem pool*/
void mem_area_free(void* ptr, mem_pool_t* pool);
/*���mem pool���Ѿ�ʹ�õĴ�С*/
ulint mem_pool_get_reserved(mem_pool_t* pool);
/*lock mem pool*/
void mem_pool_mutex_enter(void);
/*unlock mem pool*/
void mem_pool_mutex_exit(void);

ibool mem_pool_validate(mem_pool_t* pool);

void mem_pool_print_info(FILE* out_file, mem_pool_t* pool);

#endif







