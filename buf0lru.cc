#include "buf0lru.h"
#include "srv0srv.h"

#include "ut0byte.h"
#include "ut0lst.h"
#include "ut0rnd.h"
#include "sync0sync.h"
#include "sync0rw.h"
#include "hash0hash.h"
#include "os0sync.h"
#include "fil0fil.h"
#include "btr0btr.h"
#include "buf0buf.h"
#include "buf0flu.h"
#include "buf0rea.h"
#include "btr0sea.h"
#include "os0file.h"
#include "log0recv.h"

#define BUF_LRU_OLD_TOLERANCE		20
/*LRU�ķָ������*/
#define BUF_LRU_INITIAL_RATIO		8


static void	buf_LRU_block_remove_hashed_page(buf_block_t* block);
static void buf_LRU_block_free_hashed_page(buf_block_t* block);

/*������µ�block->LRU_position - len / 8*/
ulint buf_LRU_recent_limit()
{
	buf_block_t*	block;
	ulint			len;
	ulint			limit;

	mutex_enter(&(buf_pool->mutex));

	len = UT_LIST_GET_LEN(buf_pool->LRU);
	if(len < BUF_LRU_OLD_MIN_LEN){ /*�����еĵ�ԪС����Сold���ȣ����Բ�����̭�κ�buf_block*/
		mutex_exit(&(buf_pool->mutex));
		return 0;
	}

	block = UT_LIST_GET_FIRST(buf_pool->LRU);
	limit = block->LRU_position - len / BUF_LRU_INITIAL_RATIO;

	mutex_exit(&(buf_pool->mutex));
}

/*�����Ƿ��п��Ա��û���buf_block,�������free buf_block��Ӧ��page*/
ibool buf_LRU_search_and_free_block(ulint n_iterations)
{
	buf_block_t*	block;
	ibool			freed;

	freed = FALSE;
	/*�Ӻ��濪ʼ����,��ΪLRU�����block��oldest������̭oldest,�ٿ�����̭new*/
	block = UT_LIST_GET_LAST(buf_pool->LRU);
	while(block != NULL){
		/*���Ա��û���LRU LIST*/
		if(buf_flush_ready_for_replace(block)){
			if (buf_debug_prints)
				printf("Putting space %lu page %lu to free list\n", lock->space, block->offset);

			buf_LRU_block_remove_hashed_page(block);

			mutex_exit(&(buf_pool->mutex));
			/*ɾ������Ӧhash����*/
			btr_search_drop_page_hash_index(block->frame);

			mutex_enter(&(buf_pool->mutex));

			/*����û��Fix Rule*/
			ut_a(block->buf_fix_count == 0);
			buf_LRU_block_free_hashed_page(block);

			freed = TRUE;
			break;
		}

		block = UT_LIST_GET_PREV(LRU, block);
	}

	/*ɾ��һ�������ҳflush����*/
	if(buf_pool->LRU_flush_ended > 0)
		buf_pool->LRU_flush_ended--;

	/*�������е���ҳ��û�����flush disk,��ôflush_ended��������Ҫ����Ϊ0,*/
	if(!freed)
		buf_pool->LRU_flush_ended = 0;

	mutex_exit(&(buf_pool->mutex));

	return freed;
}

/*���Դ�LRU list����̭һЩbuf_block*/
void buf_LRU_try_free_flushed_blocks()
{
	mutex_enter(&(buf_pool->mutex));

	while(buf_pool->LRU_flush_ended > 0){
		mutex_exit(&(buf_pool->mutex));
		buf_LRU_search_and_free_block(0);
		mutex_enter(&(buf_pool->mutex));
	}

	mutex_exit(&(buf_pool->mutex));
}

buf_block_t* buf_LRU_get_free_block()
{
	buf_block_t*	block		= NULL;
	ibool		freed;
	ulint		n_iterations	= 0;
	ibool		mon_value_was;
	ibool		started_monitor	= FALSE;

loop:
	mutex_enter(&(buf_pool->mutex));

	/*���LRU��buffer pool��free�Ŀռ��С��buffer pool size�� 1/10,˵������Ӧhash��rec lockռ�õĿռ�̫��*/
	if(!recv_recovery_on && UT_LIST_GET_LEN(buf_pool->free) + UT_LIST_GET_LEN(buf_pool->LRU) < buf_pool->max_size / 10){
		ut_print_timestamp(stderr);
		fprintf(stderr, "  InnoDB: ERROR: over 9 / 10 of the buffer pool is occupied by\n"
			"InnoDB: lock heaps or the adaptive hash index!\n"
			"InnoDB: We intentionally generate a seg fault to print a stack trace\n"
			"InnoDB: on Linux!\n");

		ut_a(0);
	}
	/*����80%��buffer pool size������Ӧhash��rec lockռ�ã��������ڴ�й¶����Ҫ�����ڴ���*/
	else if (!recv_recovery_on && UT_LIST_GET_LEN(buf_pool->free) + UT_LIST_GET_LEN(buf_pool->LRU) < buf_pool->max_size / 5){
		ut_print_timestamp(stderr);
		fprintf(stderr, "  InnoDB: WARNING: over 4 / 5 of the buffer pool is occupied by\n"
			"InnoDB: lock heaps or the adaptive hash index! Check that your\n"
			"InnoDB: transactions do not set too many row locks. Starting InnoDB\n"
			"InnoDB: Monitor to print diagnostics, including lock heap and hash index\n"
			"InnoDB: sizes.\n");

		srv_print_innodb_monitor = TRUE;
	}else if (!recv_recovery_on && UT_LIST_GET_LEN(buf_pool->free) + UT_LIST_GET_LEN(buf_pool->LRU) < buf_pool->max_size / 4)
		srv_print_innodb_monitor = FALSE;

	/*�п����û���buf_block,�Ƚ����ڴ�lru������ɾ��*/
	if(buf_pool->LRU_flush_ended > 0){
		mutex_exit(&(buf_pool->mutex));
		buf_LRU_try_free_flushed_blocks();
		mutex_exit(&(buf_pool->mutex));
	}
	
	/*buf_pool->free ��buf_block*/
	if(UT_LIST_GET_LEN(buf_pool->free) > 0){
		block = UT_LIST_GET_FIRST(buf_pool->free);
		UT_LIST_REMOVE(free, buf_pool->free, block);
		block->state = BUF_BLOCK_READY_FOR_USE;

		mutex_exit(&(buf_pool->mutex));

		if(started_monitor)
			srv_print_innodb_monitor = mon_value_was;

		return block;
	}

	mutex_exit(&(buf_pool->mutex));

	freed = buf_LRU_search_and_free_block(n_iterations);
	if(freed)
		goto loop;

	if(n_iterations > 30){
		ut_print_timestamp(stderr);
		fprintf(stderr,
			"InnoDB: Warning: difficult to find free blocks from\n"
			"InnoDB: the buffer pool (%lu search iterations)! Consider\n"
			"InnoDB: increasing the buffer pool size.\n",
			n_iterations);

		fprintf(stderr,
			"InnoDB: It is also possible that in your Unix version\n"
			"InnoDB: fsync is very slow, or completely frozen inside\n"
			"InnoDB: the OS kernel. Then upgrading to a newer version\n"
			"InnoDB: of your operating system may help. Look at the\n"
			"InnoDB: number of fsyncs in diagnostic info below.\n");

		fprintf(stderr,
			"InnoDB: Pending flushes (fsync) log: %lu; buffer pool: %lu\n",
			fil_n_pending_log_flushes, fil_n_pending_tablespace_flushes);

		fprintf(stderr,"InnoDB: %lu OS file reads, %lu OS file writes, %lu OS fsyncs\n",
			os_n_file_reads, os_n_file_writes, os_n_fsyncs);

		fprintf(stderr, "InnoDB: Starting InnoDB Monitor to print further\n"
			"InnoDB: diagnostics to the standard output.\n");

		mon_value_was = srv_print_innodb_monitor;
		started_monitor = TRUE;
		srv_print_innodb_monitor = TRUE;
	}

	buf_flush_free_margin();
	/*�������е�IO�����߳�*/
	os_aio_simulated_wake_handler_threads();
	if(n_iterations > 10)
		os_thread_sleep(500000);

	n_iterations ++;

	goto loop;
}

/*����ȷ��old/new list�ķֽ��*/
UNIV_INLINE void buf_LRU_old_adjust_len()
{
	ulint old_len;
	ulint new_len;

	ut_ad(buf_pool->LRU_old);
	ut_ad(mutex_own(&(buf_pool->mutex)));
	ut_ad(3 * (BUF_LRU_OLD_MIN_LEN / 8) > BUF_LRU_OLD_TOLERANCE + 5);

	for(;;){
		old_len = buf_pool->LRU_old_len;
		new_len = 3 * (UT_LIST_GET_LEN(buf_pool->LRU) / 8);

		/*old list̫�̣���Ҫ��������*/
		if(old_len < new_len - BUF_LRU_OLD_TOLERANCE){
			buf_pool->LRU_old = UT_LIST_GET_PREV(LRU, buf_pool->LRU_old);
			(buf_pool->LRU_old)->old = TRUE;
			buf_pool->LRU_old_len++;
		}
		else if(old_len > new_len + BUF_LRU_OLD_TOLERANCE){
			buf_pool->LRU_old->old = FALSE;
			buf_pool->LRU_old = UT_LIST_GET_NEXT(LRU, buf_pool->LRU_old);
			buf_pool->LRU_old_len--;
		}
		else{
			ut_ad(buf_pool->LRU_old);
			return ;
		}
	}
}

static void buf_LRU_old_init()
{
	buf_block_t* block;

	ut_ad(UT_LIST_GET_LEN(buf_pool->LRU) == BUF_LRU_OLD_MIN_LEN);

	/*��LRU�����е�block���ó�Ϊold*/
	block = UT_LIST_GET_FIRST(buf_pool->LRU);
	while(block != NULL){
		block->old = TRUE;
		block = UT_LIST_GET_NEXT(LRU, block);
	}

	buf_pool->LRU_old = UT_LIST_GET_FIRST(buf_pool->LRU);
	buf_pool->LRU_old_len = UT_LIST_GET_LEN(buf_pool->LRU);

	/*����old list��new list�ָ���ȷ��*/
	buf_LRU_old_adjust_len();
}

UNIV_INLINE void buf_LRU_remove_block(buf_block_t* block)
{
	ut_ad(buf_pool);
	ut_ad(block);
	ut_ad(mutex_own(&(buf_pool->mutex)));

	/*Ҫɾ����BLOCK������new old�ķֽ����*/
	if(block == buf_pool->LRU_old){
		buf_pool->LRU_old = UT_LIST_GET_PREV(LRU, block);
		buf_pool->LRU_old->old = TRUE;

		buf_pool->LRU_old_len ++;
		ut_ad(buf_pool->LRU_old);
	}

	UT_LIST_REMOVE(LRU, buf_pool->LRU, block);
	/*LRU���ܳ����Ѿ�С��old��С�����̵ĳ���*/
	if(UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN){
		buf_pool->LRU_old = NULL;
		return ;
	}

	ut_ad(buf_pool->LRU_old);
	if(block->old) /*���block����old list���У��޸�old_len*/
		buf_pool->LRU_old_len --;

	buf_LRU_old_adjust_len();
}

/*��LRU listĩβ����һ��buf_block*/
UNIV_INLINE void buf_LRU_add_block_to_end_low(buf_block_t* block)
{
	buf_block_t* last_block;

	ut_ad(buf_pool);
	ut_ad(block);
	ut_ad(mutex_own(&(buf_pool->mutex)));

	/*ĩβһ����old��ʾ��buf_block*/
	block->old = TRUE;

	last_block = UT_LIST_GET_LAST(buf_pool->LRU);
	if(last_block != NULL)/*ȷ��LRU_position*/
		block->LRU_position = last_block->LRU_position;
	else
		block->LRU_position = buf_pool_clock_tic();

	/*��block���뵽LRU��ĩβ*/
	UT_LIST_ADD_LAST(LRU, buf_pool->LRU, block);
	if(UT_LIST_GET_LEN(buf_pool->LRU) >= BUF_LRU_OLD_MIN_LEN)
		buf_pool->LRU_old_len ++;

	/*����new��old�ָ����ȷ��*/
	if(UT_LIST_GET_LEN(buf_pool->LRU) > BUF_LRU_OLD_MIN_LEN){ /*�����Ѿ�������BUF_LRU_OLD_MIN_LEN������ȷ���ָ���*/
		ut_ad(buf_pool->LRU_old);
		buf_LRU_old_adjust_len();
	}
	else if(UT_LIST_GET_LEN(buf_pool->LRU) == BUF_LRU_OLD_MIN_LEN) /*�ոմﵽold list�����̵���С���ȣ����Խ���ȷ��new list��old list�ķָ���*/
		buf_LRU_old_init();
}

UNIV_INLINE void buf_LRU_add_block_low(buf_block_t* block, ibool old)
{
	ulint	cl;

	ut_ad(buf_pool);
	ut_ad(block);
	ut_ad(mutex_own(&(buf_pool->mutex)));

	/*ȷ��block��λ�ú�����*/
	block->old = old;
	cl = buf_pool_clock_tic();

	/*���block�Ǽ��뵽new list����LRU�ĳ��Ȳ�����BUF_LRU_OLD_MIN_LEN,��ôblock���뵽lru ��һ��λ��*/
	if(!old || (UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN)){
		UT_LIST_ADD_FIRST(LRU, buf_pool->LRU, block);

		block->LRU_position = cl;		
		block->freed_page_clock = buf_pool->freed_page_clock;
	}
	else{ /*�����old����ô��block���뵽LRU_old_block�ĺ��棬Ҳ���Ƿָ���ĺ���*/
		UT_LIST_INSERT_AFTER(LRU, buf_pool->LRU, buf_pool->LRU_old, block);
		buf_pool->LRU_old_len++;

		block->LRU_position = buf_pool->LRU_old->LRU_position;
	}

	/*����ȷ���ָ���*/
	if(UT_LIST_GET_LEN(buf_pool->LRU) > BUF_LRU_OLD_MIN_LEN){
		ut_ad(buf_pool->LRU_old);
		buf_LRU_old_adjust_len();
	}
	else if(UT_LIST_GET_LEN(buf_pool->LRU) == BUF_LRU_OLD_MIN_LEN)
		buf_LRU_old_init();
}

void buf_LRU_add_block(buf_block_t* block, ibool old)
{
	buf_LRU_add_block_low(block, old);
}

/*��buf_block����new������*/
void buf_LRU_make_block_young(buf_block_t* block)
{
	buf_LRU_remove_block(block);
	buf_LRU_add_block_low(block, FALSE);
}

/*��buf_block����old������*/
void buf_LRU_make_block_old(buf_block_t* block)
{
	buf_LRU_remove_block(block);
	buf_LRU_add_block_to_end_low(block);
}

/*��buf_block���뵽buf_pool��free������*/
void buf_LRU_block_free_non_file_page(buf_block_t* block)
{
	ut_ad(mutex_own(&(buf_pool->mutex)));
	ut_ad(block);

	ut_ad((block->state == BUF_BLOCK_MEMORY) || (block->state == BUF_BLOCK_READY_FOR_USE));

	UT_LIST_ADD_FIRST(free, buf_pool->free, block);
}

/*��block��LRU��ɾ�������ҽ��buf_block�루space id, page_no���Ĺ�ϣ��Ӧ��ϵ*/
static void buf_LRU_block_remove_hashed_page(buf_block_t* block)
{
	ut_ad(mutex_own(&(buf_pool->mutex)));
	ut_ad(block);

	ut_ad(block->state == BUF_BLOCK_FILE_PAGE);

	ut_a(block->io_fix == 0);
	ut_a(block->buf_fix_count == 0);
	ut_a(ut_dulint_cmp(block->oldest_modification, ut_dulint_zero) == 0);

	/*��buf_block��LRU��ɾ��*/
	buf_LRU_remove_block(block);

	buf_pool->freed_page_clock ++;
	/*block->modify_clock�Լ�*/
	buf_frame_modify_clock_inc(block->frame);

	/*ɾ��(space id, page_no)��buf_block��hash���Ӧ��ϵ*/
	HASH_DELETE(buf_block_t, hash, buf_pool->page_hash, buf_page_address_fold(block->space, block->offset), block);

	block->state = BUF_BLOCK_REMOVE_HASH;
}

/*��block������free list*/
static void buf_LRU_block_free_hashed_page(buf_block_t* block)
{
	ut_ad(mutex_own(&(buf_pool->mutex)));
	ut_ad(block->state == BUF_BLOCK_REMOVE_HASH);

	block->state = BUF_BLOCK_MEMORY;

	buf_LRU_block_free_non_file_page(block);
}

/*���LRU list�ĺϷ���*/
ibool buf_LRU_validate(void)
{
	buf_block_t*	block;
	ulint		old_len;
	ulint		new_len;
	ulint		LRU_pos;
	
	ut_ad(buf_pool);
	mutex_enter(&(buf_pool->mutex));

	if (UT_LIST_GET_LEN(buf_pool->LRU) >= BUF_LRU_OLD_MIN_LEN) {

		ut_a(buf_pool->LRU_old);
		old_len = buf_pool->LRU_old_len;
		new_len = 3 * (UT_LIST_GET_LEN(buf_pool->LRU) / 8);
		ut_a(old_len >= new_len - BUF_LRU_OLD_TOLERANCE);
		ut_a(old_len <= new_len + BUF_LRU_OLD_TOLERANCE);
	}
		
	UT_LIST_VALIDATE(LRU, buf_block_t, buf_pool->LRU);

	block = UT_LIST_GET_FIRST(buf_pool->LRU);

	old_len = 0;

	while (block != NULL) {
		ut_a(block->state == BUF_BLOCK_FILE_PAGE);

		if (block->old)
			old_len++;

		if (buf_pool->LRU_old && (old_len == 1))
			ut_a(buf_pool->LRU_old == block);

		LRU_pos	= block->LRU_position;

		block = UT_LIST_GET_NEXT(LRU, block);

		if (block) {
			/* If the following assert fails, it may not be an error: just the buf_pool clockhas wrapped around */
			ut_a(LRU_pos >= block->LRU_position);
		}
	}

	if (buf_pool->LRU_old)
		ut_a(buf_pool->LRU_old_len == old_len);

	UT_LIST_VALIDATE(free, buf_block_t, buf_pool->free);

	block = UT_LIST_GET_FIRST(buf_pool->free);

	while (block != NULL) {
		ut_a(block->state == BUF_BLOCK_NOT_USED);
		block = UT_LIST_GET_NEXT(free, block);
	}

	mutex_exit(&(buf_pool->mutex));

	return(TRUE);
}

/*��LRU�е���Ϣ���д�ӡ*/
void buf_LRU_print(void)
{
	buf_block_t*	block;
	buf_frame_t*	frame;
	ulint		len;

	ut_ad(buf_pool);
	mutex_enter(&(buf_pool->mutex));

	printf("Pool ulint clock %lu\n", buf_pool->ulint_clock);

	block = UT_LIST_GET_FIRST(buf_pool->LRU);

	len = 0;

	while (block != NULL) {
		printf("BLOCK %lu ", block->offset);

		if (block->old)
			printf("old ");

		if (block->buf_fix_count)
			printf("buffix count %lu ", block->buf_fix_count);

		if (block->io_fix)
			printf("io_fix %lu ", block->io_fix);

		if (ut_dulint_cmp(block->oldest_modification, ut_dulint_zero) > 0)
				printf("modif. ");

		printf("LRU pos %lu ", block->LRU_position);

		frame = buf_block_get_frame(block);

		printf("type %lu ", fil_page_get_type(frame));
		printf("index id %lu ", ut_dulint_get_low(btr_page_get_index_id(frame)));

		block = UT_LIST_GET_NEXT(LRU, block);
		len++;
		if (len % 10 == 0)
			printf("\n");
	}

	mutex_exit(&(buf_pool->mutex));
}

