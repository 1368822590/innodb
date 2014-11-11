#include "mem0mem.h"
#include "mem0pool.h"
#include "mach0data.h"
#include "buf0buf.h"
#include "btr0sea.h"
#include "srv0srv.h"
#include "mem0dbg.h"

mem_block_t*	mem_heap_create_block(mem_heap_t* heap, ulint n, void* init_block, ulint type, char* file_name, ulint line);
void			mem_heap_block_free(mem_heap_t* heap, mem_block_t* block);
void			mem_heap_free_block_free(mem_heap_t* heap);
mem_block_t*	mem_heap_add_block(mem_heap_t* heap, ulint n);

UNIV_INLINE void mem_block_set_len(mem_block_t* block, ulint len)
{
	ut_ad(len > 0);
	block->len = len;
}


UNIV_INLINE ulint mem_block_get_len(mem_block_t* block)
{
	return block->len;
}

UNIV_INLINE void mem_block_set_type(mem_block_t* block, ulint type)
{
	ut_ad((type == MEM_HEAP_DYNAMIC) || (type == MEM_HEAP_BUFFER)
		|| (type == MEM_HEAP_BUFFER + MEM_HEAP_BTR_SEARCH));

	block->type = type;
}

UNIV_INLINE void mem_block_set_free(mem_block_t* block, ulint free)
{
	ut_ad(free > 0);
	ut_ad(free <= mem_block_get_len(block));

	block->free = free;
}

UNIV_INLINE ulint mem_block_get_free(mem_block_t* block)
{
	return block->free;
}

UNIV_INLINE void mem_block_set_start(mem_block_t* block, ulint start)
{
	ut_ad(start > 0);
	block->start = start;
}

UNIV_INLINE ulint mem_block_get_start(mem_block_t* block)
{
	return block->start;
}

UNIV_INLINE void* mem_heap_alloc(mem_heap_t* heap, ulint n)
{
	mem_block_t*	block;
	void*			buf;
	ulint			free;

	ut_ad(mem_heap_check(heap));

	/*��ȡ���һ��block*/
	block = UT_LIST_GET_LAST(heap_base);
	ut_ad(!(block->type & MEM_HEAP_BUFFER) || (n <= MEM_MAX_ALLOC_IN_BUF)); /*n��С��page size - 200*/

	/*block�Ŀ��д�С�������䣬���¹���һ�����Է���n��С��block*/
	if(mem_block_get_free(block) + MEM_SPACE_NEEDED(n) > mem_block_get_len(block)){
		block = mem_heap_add_block(heap, n);
		if(block == NULL)
			return NULL;
	}

	free = mem_block_get_free(block);
	buf = (byte*)block + free;
	mem_block_set_free(block, free + MEM_SPACE_NEEDED(n));

	return buf;
}

UNIV_INLINE byte* mem_heap_get_heap_top(mem_heap_t* heap)
{
	mem_block_t*	block;
	byte*			buf;
	ut_ad(mem_heap_check(heap));
	block = UT_LIST_GET_LAST(heap->base);
	buf = (byte*)block + mem_block_get_free(block);

	return buf;
}

UNIV_INLINE void mem_heap_free_heap_top(mem_heap_t* heap, byte* old_top)
{
	mem_block_t*	block;
	mem_block_t*	prev_block;

	ut_ad(mem_heap_check(heap));
	
	block = UT_LIST_GET_LAST(heap->base);
	while(block != NULL){
		if((byte*)block + mem_block_get_free(block) >= old_top && (byte*)block <= old_top) /*block�����ͷţ���Ϊ���������ı�������ڴ���ʹ��*/
			break;

		/*�ͷŵ�old top���ϵ��ڴ��*/
		prev_block = UT_LIST_GET_PREV(list, block);
		mem_heap_block_free(heap, block);

		block = prev_block;
	}

	/*��������free�ĳߴ�*/
	ut_ad(block);
	mem_block_set_free(block, old_top - (byte*)block); 

	/*���еĿ�block,�����ͷţ�������list��ɾ��*/
	if((heap != block) && (mem_block_get_free(block) == mem_block_get_start(block))){
		mem_heap_block_free(heap, block);
	}
}

UNIV_INLINE void mem_heap_empty(mem_heap_t* heap)
{
	mem_heap_free_heap_top(heap, (byte*)heap + mem_block_get_start(heap));
	if(heap->free_block != NULL)
		mem_heap_free_block_free(heap);
}

UNIV_INLINE void* mem_heap_get_top(mem_heap_t* heap, ulint n)
{
	mem_block_t*	block;
	void*			buf;

	ut_ad(mem_heap_check(heap));

	/*���heap������block*/
	block = UT_LIST_GET_LAST(heap->base);
	/*��ñ�����n���ȵ��ڴ�ָ��*/
	buf = (byte*)block + mem_block_get_free(block) - MEM_SPACE_NEEDED(n);

	return buf;
}

UNIV_INLINE void mem_heap_free_top(mem_heap_t* heap, ulint n)
{
	mem_block_t* block;
	block = UT_LIST_GET_LAST(heap->base);
	mem_block_set_free(block , mem_block_get_free(block) - MEM_SPACE_NEEDED(n));

	/*���еĿ�block,�����ͷţ�������list��ɾ��*/
	if((heap != block) && (mem_block_get_free(block) == mem_block_get_start(block))){
		mem_heap_block_free(heap, block);
	}
}

UNIV_INLINE mem_heap_t* mem_heap_create_func(ulint n, void* init_block, ulint type, char* file_name, ulint line)
{
	mem_block_t* block;
	if(n > 0)
		block = mem_heap_create_block(NULL, n, init_block, type, file_name, line);
	else
		block = mem_heap_create_block(NULL, MEM_BLOCK_START_SIZE, init_block, type, file_name, line);

	ut_ad(block);

	UT_LIST_INIT(block->base);
	UT_LIST_ADD_FIRST(list, block->base, block);

	return block;
}

UNIV_INLINE void mem_heap_free_func(mem_heap_t* heap, char* file_name, ulint line)
{
	mem_block_t* block;
	mem_block_t* prev_block;

	ut_ad(mem_heap_check(heap));
	block = UT_LIST_GET_LAST(heap->base);
	if(heap->free_block != NULL)
		mem_heap_free_block_free(heap);

	while(block != NULL){
		prev_block = UT_LIST_GET_PREV(list, block);
		mem_heap_block_free(heap, block);
		block = prev_block;
	}
}

UNIV_INLINE void* mem_alloc_func(ulint n, char* file_name, ulint line)
{
#ifdef notdefined
	/*��ȫ�ֵ�buddy alloc�Ϸ���һ���ڴ�*/
	void* buf = mem_area_alloc(n, mem_comm_pool);
	return buf;
#else
	mem_heap_t*	heap;
	void*		buf;
	/*����һ��heap_t*/
	heap = mem_heap_create_func(n, NULL, MEM_HEAP_DYNAMIC, file_name, line);
	if(heap == NULL)
		return NULL;

	/*��heap block�Ϸ���һ��n��С���ڴ�*/
	buf = mem_heap_alloc(heap, n);
	/*����ĺϷ���*/
	ut_a((byte*)heap == (byte*)buf - MEM_BLOCK_HEADER_SIZE - MEM_FIELD_HEADER_SIZE);

	return buf;
#endif
}

UNIV_INLINE void mem_free_func(void* ptr, char* file_name, ulint line)
{
#ifdef notdefined
	/*��ȫ�ֵ�buddy alloc���ͷ�*/
	mem_area_free(ptr, mem_comm_pool);
#else
	mem_heap_t* heap;
	/*ȷ��heap��ָ��λ��*/
	heap = (mem_heap_t*)((byte*)ptr - MEM_BLOCK_HEADER_SIZE - MEM_FIELD_HEADER_SIZE);
	mem_heap_free_func(heap, file_name, line);
#endif
}

UNIV_INLINE ulint mem_heap_get_size(mem_heap_t* heap)
{
	mem_block_t* block;
	ulint size = 0;

	ut_ad(mem_heap_check(heap));
	block = heap;
	while(block != NULL){
		size += mem_block_get_len(block);
		block = UT_LIST_GET_NEXT(list, block);
	}

	/**/
	if(heap->free_block)
		size += UNIV_PAGE_SIZE;

	return size;
}

void mem_alloc_func_noninline(ulint n, char* file_name, ulint line)
{
	return mem_alloc_func(n, file_name, line);
}

mem_block_t* mem_heap_create_block(mem_heap_t* heap, ulint n, void* init_block, ulint type, char* file_name, ulint line)
{
	mem_block_t* block;
	ulint len;

	ut_ad((type == MEM_HEAP_DYNAMIC) || (type == MEM_HEAP_BUFFER)
		|| (type == MEM_HEAP_BUFFER + MEM_HEAP_BTR_SEARCH));

	/*��heapħ���ֵ��ж�*/
	if(heap && heap->magic_n != MEM_BLOCK_MAGIC_N)
		mem_analyze_corruption((byte*)heap);

	/*��block���жϺ�ȡֵ*/
	if(init_block != NULL){
		ut_ad(type == MEM_HEAP_DYNAMIC);
		ut_ad(n > MEM_BLOCK_START_SIZE + MEM_BLOCK_HEADER_SIZE);
		len = n;
		block = (mem_block_t*)init_block;
	}
	else if(type == MEM_HEAP_DYNAMIC){ /*��buddy alloc�Ϸ���һ��block*/
		len = MEM_BLOCK_HEADER_SIZE + MEM_SPACE_NEEDED(n);
		block = (mem_block_t*)mem_area_alloc(len, mem_comm_pool);
	}
	else{
		ut_ad(n <= MEM_MAX_ALLOC_IN_BUF);
		/*ȷ��block�Ĵ�С*/
		len = MEM_BLOCK_HEADER_SIZE + MEM_SPACE_NEEDED(n);
		if(len < UNIV_PAGE_SIZE / 2)/*�������С��page size��һ�룬������buddy alloc�Ϸ���*/
			block = mem_area_alloc(len, mem_comm_pool);
		else{
			len = UNIV_PAGE_SIZE; /*������һ��ҳ*/
			if((type & MEM_HEAP_BTR_SEARCH) && heap){
				block = (mem_block_t*)heap->free_block;
				heap->free_block = NULL;
			}
			else /*��buf frame�з��䣬�����ûŪ���ף���*/
				block = (mem_block_t*)buf_frame_alloc();
		}
	}

	if(block == NULL) /*����ʧ��*/
		return NULL;

	block->magic_n = MEM_BLOCK_MAGIC_N;
	ut_memcpy(&(block->file_name), file_name + ut_strlen(file_name) - 7, 7);
	block->file_name[7] = '\0';
	block->line = line;

	mem_block_set_len(block, len);
	mem_block_set_type(block, type);
	mem_block_set_free(block, MEM_BLOCK_HEADER_SIZE); /*���ñ�ռ�õ��ڴ��ֽ���*/
	mem_block_set_start(block, MEM_BLOCK_HEADER_SIZE); /*������ʼ����λ��*/

	block->free_block = NULL;
	if(init_block != NULL)
		block->init_block = TRUE;
	else
		block->init_block = FALSE;

	return block;
}

mem_block_t* mem_heap_add_block(mem_heap_t* heap, ulint n)
{
	mem_block_t* block;
	mem_block_t* new_block;
	ulint new_size;

	ut_ad(mem_heap_check(heap));
	block = UT_LIST_GET_LAST(heap->base);

	/*����һ������һ��block 2����С��block*/
	new_size = 2 * mem_block_get_len(block);
	if(heap->type != MEM_HEAP_DYNAMIC){
		ut_a(n < MEM_MAX_ALLOC_IN_BUF); /*nһ��ҪС�����ɷ���ĳߴ磬���һ��page size*/

		/*ʹ��new size������MEM_MAX_ALLOC_IN_BUF*/
		if(new_size > MEM_MAX_ALLOC_IN_BUF)
			new_size = MEM_MAX_ALLOC_IN_BUF;
	}
	else if(new_size > MEM_BLOCK_STANDARD_SIZE){
		new_size = MEM_BLOCK_STANDARD_SIZE;
	}

	/*���new size����С��N,ֱ����N��Ϊ��С����block*/
	if(new_size < n)
		new_size = n;

	/*����һ��block*/
	new_block = mem_heap_create_block(heap, new_size, NULL, heap->type, heap->file_name, heap->line);
	if(new_block != NULL) /*��block���뵽last block�ĺ��棬Ҳ����heap�Ķ���*/
		UT_LIST_INSERT_AFTER(list, heap->base, block, new_block);

	return new_block;
}

void mem_heap_block_free(mem_heap_t* heap, mem_block_t* block)
{
	ulint	type;
	ulint	len;
	ibool	init_block;

	if(block->magic_n != MEM_BLOCK_MAGIC_N)
		mem_analyze_corruption((byte*)block);

	UT_LIST_REMOVE(list, heap->base, block);

	type = heap->type;
	len = block->len;
	init_block = block->init_block;
	block->magic_n = MEM_FREED_BLOCK_MAGIC_N;
	if(init_block){ /*���ⲿ���ڴ洴����block*/

	}
	else if(type == MEM_HEAP_DYNAMIC){ /*��buddy alloc���ͷ�*/
		mem_area_free(block, mem_comm_pool);
	}
	else{
		ut_ad(type & MEM_HEAP_BUFFER);
		if(len >= UNIV_PAGE_SIZE / 2)
			buf_frame_free((byte*)block);
		else
			mem_area_free(block, mem_comm_pool);
	}
}

void mem_heap_free_block_free(mem_heap_t* heap)
{
	/*��ʲô�ط�heap->free_block�᲻ΪNULL?!����Ҫ�ں��ڵĴ����������ϸ�µĲ鿴*/
	if(heap->free_block != NULL){
		buf_frame_free(heap->free_block);
		heap->free_block = NULL;
	}
}








