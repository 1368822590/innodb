#include "mem0pool.h"

#include "sync0sync.h"
#include "ut0mem.h"
#include "ut0lst.h"
#include "ut0byte.h"

/*mem area free�ı�־�������־size_and_free�����һ��bit�ϱ���*/
#define MEM_AREA_FREE			1
/*mem_area_t��С�Ĵ�С������2��mem_area_t 8�ֽڶ����Ĵ�С*/
#define MEM_AREA_MIN_SIZE		(2 * MEM_AREA_EXTRA_SIZE)

/*mem_pool_t�Ľṹ*/
struct mem_pool_struct
{
	byte*		buf;			/*�����ڴ�ľ��*/
	ulint		size;			/*�ڴ�ش�С*/
	ulint		reserved;		/*��ǰ�����ȥ�����ڴ��С*/
	mutex_t		mutex;			/*���̻߳�����*/
	UT_LIST_BASE_NODE_T(mem_area_t) free_list[64]; /*area_t��������*/
};

mem_pool_t*		mem_comm_pool = NULL;
/*�����ڴ�ص����Ĵ���*/
ulint			mem_out_of_mem_err_msg_count = 0;

void mem_pool_mutex_enter()
{
	mutex_enter(&(mem_comm_pool->mutex));
}

void mem_pool_mutex_exit()
{
	mutex_enter(&(mem_comm_pool->mutex));
}

/*area_sizeһ����2��N�η����������һλ�������ͷű�־*/
UNIV_INLINE ulint mem_area_get_size(mem_area_t* area)
{
	return area->size_and_free & ~MEM_AREA_FREE;
}

UNIV_INLINE void mem_area_set_size(mem_area_t* area, ulint size)
{
	area->size_and_free = (area->size_and_free & MEM_AREA_FREE) | (size & MEM_AREA_FREE);
}

UNIV_INLINE ibool mem_area_get_free(mem_area_t* area)
{
	ut_ad(TRUE == MEM_AREA_FREE);
	return area->size_and_free & MEM_AREA_FREE;
}

UNIV_INLINE void mem_area_set_free(mem_area_t* area, ibool free)
{
	ut_ad(TRUE == MEM_AREA_FREE);
	area->size_and_free = (area->size_and_free & ~MEM_AREA_FREE) | free;
}

mem_pool_t* mem_pool_create(ulint size)
{
	mem_pool_t*	pool;
	mem_area_t* area;
	ulint		i;
	ulint		used;

	ut_a(size >= 1024);
	pool = ut_malloc(sizeof(mem_pool_t));
	/*����pool���ڴ��*/
	pool->buf = ut_malloc_low(size, FALSE);
	pool->size = size;

	mutex_create(&(pool->mutex));
	mutex_set_level(&(pool->mutex), SYNC_MEM_POOL);

	for(i = 0; i < 64; i++){
		UT_LIST_INIT(pool->free_list[i]);
	}

	used = 0;

	/*����һ��buddy alloc�ڴ�ϵͳ*/
	while(size - used >= MEM_AREA_MIN_SIZE){
		/*����Ӧ��Ӧ������free_list�����е����*/
		i = ut_2_log(size);
		if(ut_2_exp(i) > size - used)
			i --;

		area = (mem_area_t*)(pool->buf + used);
		mem_area_set_size(area, ut_2_exp(i));
		mem_area_set_free(area, TRUE);

		/*��area���뵽��Ӧ��ŵ�������*/
		UT_LIST_ADD_FIRST(free_list, pool->free_list[i], area);

		used += ut_2_exp(i);
	}

	ut_ad(size>= used);
	pool->reserved = 0;
}

static ibool mem_pool_fill_free_list(ulint i, mem_pool_t* pool)
{
	mem_area_t* area;
	mem_area_t* area2;
	ibool		ret;
	char		err_buf[512];

	ut_ad(mutex_own(&(pool->mutex)));

	/*�������ڴ�ص����Χ*/
	if(i >= 63){
		if(mem_out_of_mem_err_msg_count % 1000000000 == 0){ /*���������־*/
			ut_print_timestamp(stderr);

			fprintf(stderr,
				"  InnoDB: Out of memory in additional memory pool.\n"
				"InnoDB: InnoDB will start allocating memory from the OS.\n"
				"InnoDB: You may get better performance if you configure a bigger\n"
				"InnoDB: value in the MySQL my.cnf file for\n"
				"InnoDB: innodb_additional_mem_pool_size.\n");
		}

		mem_out_of_mem_err_msg_count ++;

		return FALSE;
	}

	area = UT_LIST_GET_FIRST(pool->free_list[i + 1]);
	if(area == NULL){
		if (UT_LIST_GET_LEN(pool->free_list[i + 1]) > 0) {
			ut_print_timestamp(stderr);

			fprintf(stderr, "  InnoDB: Error: mem pool free list %lu length is %lu\n"
				"InnoDB: though the list is empty!\n",
				i + 1, UT_LIST_GET_LEN(pool->free_list[i + 1]));
		}

		/*�ݹ��ٵ�i+1�����Һ��ʵ��ڴ�飬iԽ���ڴ��Խ��*/
		ret = mem_pool_fill_free_list(i + 1, pool);
		if(!ret)
			return FALSE;

		area = UT_LIST_GET_FIRST(pool->free_list[i + 1]);
	}

	if(UT_LIST_GET_LEN(pool->free_list[i + 1]) == 0){
		ut_sprintf_buf(err_buf, ((byte*)area) - 50, 100);
		fprintf(stderr, "InnoDB: Error: Removing element from mem pool free list %lu\n"
			"InnoDB: though the list length is 0! Dump of 100 bytes around element:\n%s\n",
			i + 1, err_buf);
		ut_a(0);
	}
	/*����һ���list��ɾ��area*/
	UT_LIST_REMOVE(free_list, pool->free_list[i + 1], area);

	/*��i����з���2����ͬ��С��area*/
	area2 = (mem_area_t*)(((byte*)area) + ut_2_exp(i));

	mem_area_set_size(area2, ut_2_exp(i));
	mem_area_set_free(area2, TRUE);
	UT_LIST_ADD_FIRST(free_list, pool->free_list[i], area2);

	mem_area_set_size(area, ut_2_exp(i));
	UT_LIST_ADD_FIRST(free_list, pool->free_list[i], area);

	return TRUE;
}

void* mem_area_alloc(ulint size, mem_pool_t* pool)
{
	mem_area_t* area;
	ulint		n;
	ibool		ret;
	char		err_buf[512];

	n = ut_2_log(ut_max(size + MEM_AREA_EXTRA_SIZE, MEM_AREA_MIN_SIZE));

	mutex_enter(&(pool->mutex));
	area = UT_LIST_GET_FIRST(pool->free_list[n]);
	if(area == NULL){
		/*���ϲ���з��ѵõ���Ӧarea*/
		ret = mem_pool_fill_free_list(n, pool);
		if(!ret){ /*��pool�з���ʧ�ܣ���os�����*/
			mutex_exit(&(pool->mutex));
			retrun (ut_malloc(size));
		}

		area = UT_LIST_GET_FIRST(pool->free_list[n]);
	}

	if(!mem_area_get_free(area)){
		ut_sprintf_buf(err_buf, ((byte*)area) - 50, 100);
		fprintf(stderr,
			"InnoDB: Error: Removing element from mem pool free list %lu though the\n"
			"InnoDB: element is not marked free! Dump of 100 bytes around element:\n%s\n",
			n, err_buf);
		ut_a(0);
	}

	if (UT_LIST_GET_LEN(pool->free_list[n]) == 0) {
		ut_sprintf_buf(err_buf, ((byte*)area) - 50, 100);
		fprintf(stderr,
			"InnoDB: Error: Removing element from mem pool free list %lu\n"
			"InnoDB: though the list length is 0! Dump of 100 bytes around element:\n%s\n",
			n, err_buf);
		ut_a(0);
	}

	ut_ad(mem_area_get_size(area) == ut_2_exp(n));	
	mem_area_set_free(area, FALSE);
	/*�ӿ�������ɾ��*/
	UT_LIST_REMOVE(free_list, pool->free_list[n], area);
	/*�޸���ʹ�õĴ�С*/
	pool->reserved += mem_area_get_size(area);
	mutex_exit(&(pool->mutex));

	/*����ָ��*/
	return((void*)(MEM_AREA_EXTRA_SIZE + ((byte*)area))); 
}

UNIV_INLINE mem_area_t* mem_area_get_buddy(mem_area_t* area, ulint size, mem_pool_t* pool)
{
	mem_area_t* buddy;

	/*��ú�area��صĻ��*/
	ut_ad(size != 0);
	/*��ø�λ���*/
	if(((((byte*)area) - pool->buf) % (2 * size)) == 0){
		buddy = (mem_area_t*)(((byte*)area) + size);
		if((((byte*)buddy) - pool->buf) + size > pool->size){ /*�ڴ泬��pool�Ĵ�С*/
			buddy = NULL;
		}
	}
	else{/*��õ�λ���*/
		buddy = (mem_area_t*)(((byte*)area) - size);
	}

	return buddy;
}

void mem_area_free(void* ptr, mem_pool_t* pool)
{
	mem_area_t* area;
	mem_area_t* buddy;
	void*		new_ptr;
	ulint		size;
	ulint		n;
	char		err_buf[512];

	/*�з���out of memory���ͱ����ж��Ƿ��Ǵ�pool�з���ģ�������ǣ���Ҫ��ϵͳ��free�����ͷ�*/
	if(mem_out_of_mem_err_msg_count > 0){
		if ((byte*)ptr < pool->buf || (byte*)ptr >= pool->buf + pool->size){ /*����pool�ڴ淶Χ��ֱ���ͷ�*/
				ut_free(ptr);
				return;
		}
	}

	/*���area���*/
	area = (mem_area_t*)(((byte*)ptr) - MEM_AREA_EXTRA_SIZE);
	if(mem_area_get_free(area)){ /*״̬����*/
		ut_sprintf_buf(err_buf, ((byte*)area) - 50, 100);
		fprintf(stderr,
			"InnoDB: Error: Freeing element to mem pool free list though the\n"
			"InnoDB: element is marked free! Dump of 100 bytes around element:\n%s\n",
			err_buf);
		ut_a(0);
	}

	size = mem_area_get_size(area);
	/*���area�Ļ��*/
	buddy = mem_area_get_buddy(area, size, pool);
	n = ut_2_log(size);

	mutex_enter(&(pool->mutex));
	if(buddy != NULL && mem_area_get_free(buddy) && (size == mem_area_get_size(buddy))){ /*buddy��һ�����е�area, ����area�ϲ����뵽��һ��*/
		if((byte*)buddy < (byte*)area){
			new_ptr = ((byte*)buddy) + MEM_AREA_EXTRA_SIZE;
			mem_area_set_size(buddy, 2 * size);
			mem_area_set_free(buddy, FALSE);
		}
		else{
			new_ptr = ptr;
			mem_area_set_size(area, 2 * size);
		}
		/*�ڱ���ɾ��buddy*/
		UT_LIST_REMOVE(free_list, pool->free_list[n], buddy);
		/*�ںϲ���ʱ��area�ǲ���free list�У����������Ǳ�ռ�õ�*/
		pool->reserved += ut_2_exp(n);
		mutex_exit(&(pool->mutex));
		/*�ϲ����ϲ㣬�������ͷ�*/
		mem_area_free(new_ptr, pool);
	}
	else{ /*����Ǳ�ʹ�õ�*/
		UT_LIST_ADD_FIRST(free_list, pool->free_list[n], area);
		mem_area_set_free(area, TRUE);
		ut_ad(pool->reserved >= size);
		pool->reserved -= size;
	}
	mutex_exit(&(pool->mutex));
	/*pool�İ�ȫ���*/
	ut_ad(mem_pool_validate(pool));
}

ibool mem_pool_validate(mem_pool_t* pool)
{
	mem_pool_t* pool;
	mem_area_t* buddy;
	ulint		i;
	ulint		free;

	mutex_enter(&(pool->mutex));
	free = 0;

	for(i = 0; i < 64; i++){
		UT_LIST_VALIDATE(free_list, mem_area_t, pool->free_list[i]);
		area = UT_LIST_GET_FIRST(pool->free_list[i]);
		while(area != NULL){
			ut_a(mem_area_get_free(area));
			ut_a(mem_area_get_size(area) == ut_2_exp(i));

			buddy = mem_area_get_buddy(area, ut_2_exp(i), pool);
			ut_a(!buddy || !mem_area_get_free(buddy) || (ut_2_exp(i) != mem_area_get_size(buddy))); /*����Լ��ͻ�鶼��free״̬�������������*/
			area = UT_LIST_GET_NEXT(free_list, area);
			free += ut_2_exp(i);
		}
	}

	/*�����ж�*/
	ut_a(free + pool->reserved == pool->size - (pool->size % MEM_AREA_MIN_SIZE));
}

ulint mem_pool_get_reserved(mem_pool_t* pool)
{
	ulint reserved;

	mutex_enter(&(pool->mutex));
	reserved = pool->reserved;
	mutex_exit(&(pool->mutex));

	return reserved;
}



void mem_pool_print_info(FILE* outfile, mem_pool_t*	pool)
{
	ulint i;
	mem_pool_validate(pool);

	fprintf(outfile, "INFO OF A MEMORY POOL\n");

	mutex_enter(&(pool->mutex));
	for (i = 0; i < 64; i++) {
		if (UT_LIST_GET_LEN(pool->free_list[i]) > 0) {
			fprintf(outfile, "Free list length %lu for blocks of size %lu\n", UT_LIST_GET_LEN(pool->free_list[i]), ut_2_exp(i));
		}	
	}

	fprintf(outfile, "Pool size %lu, reserved %lu.\n", pool->size, pool->reserved);
	mutex_exit(&(pool->mutex));
}


















