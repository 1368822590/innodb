#include "btr0sea.h"

#include "buf0buf.h"
#include "page0page.h"
#include "page0cur.h"
#include "btr0cur.h"
#include "btr0pcur.h"
#include "btr0btr.h"
#include "ha0ha.h"

ulint		btr_search_n_succ	= 0;
ulint		btr_search_n_hash_fail	= 0;

/*��Ϊbtr_search_latch_temp��btr_search_sys��Ƶ�����ã�����������䣬Ϊ��CPU cache����btr_search_latch_temp*/
byte		btr_sea_pad1[64];
rw_lock_t*	btr_search_latch_temp;

/*����������䣬Ϊ��CPU cache����btr_search_sys*/
byte		btr_sea_pad2[64];
btr_search_sys_t*	btr_search_sys;


#define BTR_SEARCH_PAGE_BUILD_LIMIT		16
#define BTR_SEARCH_BUILD_LIMIT			100


static void btr_search_build_page_hash_index(page_t* page, ulint n_fields, ulint n_bytes, ulint side);

/*���table��heap�Ƿ��п��пռ䣬���û�д�ibuf�л�ȡ�µ�*/
static void btr_search_check_free_space_in_heap(void)
{
	buf_frame_t*	frame;
	hash_table_t*	table;
	mem_heap_t*		heap;

	ut_ad(!rw_lock_own(&btr_search_latch, RW_LOCK_SHARED) && !rw_lock_own(&btr_search_latch, RW_LOCK_EX));

	table = btr_search_sys->hash_index;
	heap = table->heap;

	if(heap->free_block == NULL){
		frame = buf_frame_alloc(); /*������֮����free_block =���п��ܻ��ڴ�й¶*/

		rw_lock_x_lock(&btr_search_latch);

		if(heap->free_block == NULL)
			heap->free_block = frame;
		else /*��buf_frame_alloc��rw_lock_x_lock�������п���free_block��ֵ�ı���*/
			buf_frame_free(frame);

		rw_lock_x_unlock(&btr_search_latch);
	}
}

/*����ȫ�ֵ�����Ӧhash�����Ķ��󣬰���һ��rw_lock��hash table*/
void btr_search_sys_create(ulint hash_size)
{
	btr_search_latch_temp = mem_alloc(sizeof(rw_lock_t));

	rw_lock_create(&btr_search_latch);
	/*��������ӦHASH��*/
	btr_search_sys = mem_alloc(sizeof(btr_search_sys_t));
	btr_search_sys->hash_index = ha_create(TRUE, hash_size, 0, 0);

	rw_lock_set_level(&btr_search_latch, SYNC_SEARCH_SYS);
}

/*��������ʼ��һ��btr_search_t*/
btr_search_t* btr_search_info_create(mem_heap_t* heap)
{
	btr_search_t*	info;

	info = mem_heap_alloc(heap, sizeof(btr_search_t));
	info->magic_n = BTR_SEARCH_MAGIC_N;

	info->last_search = NULL;
	info->n_direction = 0;
	info->root_guess = NULL;

	info->hash_analysis = 0;
	info->n_hash_potential = 0;

	info->last_hash_succ = FALSE;

	info->n_hash_succ = 0;	
	info->n_hash_fail = 0;	
	info->n_patt_succ = 0;	
	info->n_searches = 0;	

	info->n_fields = 1;
	info->n_bytes = 0;

	/*Ĭ�ϴ�����*/
	info->side = BTR_SEARCH_LEFT_SIDE;

	return info;
}

