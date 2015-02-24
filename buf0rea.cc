#include "buf0rea.h"

#include "fil0fil.h"
#include "mtr0mtr.h"

#include "buf0buf.h"
#include "buf0flu.h"
#include "buf0lru.h"
#include "ibuf0ibuf.h"
#include "log0recv.h"
#include "trx0sys.h"
#include "os0file.h"
#include "srv0start.h"


#define BUF_READ_AHEAD_RANDOM_AREA			BUF_READ_AHEAD_AREA

#define BUF_READ_AHEAD_RANDOM_THRESHOLD		(5 + BUF_READ_AHEAD_RANDOM_AREA / 8)

#define BUF_READ_AHEAD_LINEAR_AREA			BUF_READ_AHEAD_AREA

#define BUF_READ_AHEAD_LINEAR_THRESHOLD		(3 * BUF_READ_AHEAD_LINEAR_AREA / 8)

#define BUF_READ_AHEAD_PEND_LIMIT			2

/*�Ӵ����϶�ȡһ��ҳ������*/
static ulint buf_read_page_low(ibool sync, ulint mode, ulint space, ulint offset)
{
	buf_block_t* block;
	ulint		 wake_later;

	wake_later = mode & OS_AIO_SIMULATED_WAKE_LATER;
	mode = mode & ~OS_AIO_SIMULATED_WAKE_LATER;

	/*������ı�ռ䣬����page_no����doublewrite page��֮��*/
	if(trx_doublewrite && space == TRX_SYS_SPACE 
		&& ((offset >= trx_doublewrite->block1 && offset < trx_doublewrite->block1 + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE)
		|| (offset >= trx_doublewrite->block2 && offset < trx_doublewrite->block2 + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE)))
		return 0;

	/*page_noָ���page��ibuf bitmap page������trx�����ͷҳ,����ͬ����ȡ*/
	if(ibuf_bitmap_page(offset) || trx_sys_hdr_page(space, offset))
		sync = TRUE;

	/*��page��Ӧ��buf_block���ж���ʼ��*/
	block = buf_page_init_for_read(mode, space, offset);
	if(block != NULL){
		if(buf_debug_prints)
			printf("Posting read request for page %lu, sync %lu\n", offset, sync);

		fil_io(OS_FILE_READ | wake_later, sync, space, offset, 0, UNIV_PAGE_SIZE, (void*)block->frame, (void*)block);
		if(sync) /*�����ͬ�����ж�ȡ�Ļ����Ϳ����ֽڵݽ�io_complete�¼�*/
			buf_page_io_complete(block);

		/*������첽��������fil_aio_wait�е���buf_page_io_complete*/
		return 1;
	}

	return 0;
}

/*���Ԥ��*/
static void ulint buf_read_ahead_random(ulint space, ulint offset)
{
	buf_block_t*	block;
	ulint		recent_blocks	= 0;
	ulint		count;
	ulint		LRU_recent_limit;
	ulint		ibuf_mode;
	ulint		low, high;
	ulint		i;

	if(srv_startup_is_before_trx_rollback_phase)
		return 0;

	/*ibuf bitmap page������ͷҳ�ǲ���Ԥ����*/
	if(ibuf_bitmap_page(offset) || trx_sys_hdr_page(space, offset))
		return 0;

	low  = (offset / BUF_READ_AHEAD_RANDOM_AREA) * BUF_READ_AHEAD_RANDOM_AREA;
	high = (offset / BUF_READ_AHEAD_RANDOM_AREA + 1) * BUF_READ_AHEAD_RANDOM_AREA;

	/*ȷ��high������table space����߷�Χ*/
	if(high > fil_space_get_size(space))
		high = fil_space_get_size(space);

	LRU_recent_limit = buf_LRU_get_recent_limit();

	mutex_enter(&(buf_pool->mutex));
	/*���ڶ��̵�page������buf_poolʹ�õ�page����һ�룬����Ԥ��*/
	if(buf_pool->n_pend_reads > buf_pool->curr_size / BUF_READ_AHEAD_PEND_LIMIT){
		mutex_exit(&(buf_pool->mutex));
		return 0;
	}

	/*����[low, high]֮���page�ж�����������ʹ��ģ�������buf_block����LRU���µ�buf_block*/
	for(i = low; i < high; i ++){
		block = buf_page_hash_get(space, i);
		if(block != NULL && block->LRU_position > LRU_recent_limit && block->accessed)
			recent_blocks++;
	}

	mutex_exit(&(buf_pool->mutex));
	/*���ʹ��Ŀ�̫����,����Ҫ����Ԥ��*/
	if(recent_blocks < BUF_READ_AHEAD_RANDOM_THRESHOLD)
		return 0;

	/*�ж��Ƿ��ȡ����ibuf�е�page*/
	if(ibuf_inside())
		ibuf_mode = BUF_READ_IBUF_PAGES_ONLY;
	else
		ibuf_mode = BUF_READ_ANY_PAGE;

	count = 0;

	for(i = low; i < high; i ++){
		if(!ibuf_bitmap_page(i))
			count += buf_read_page_low(FALSE, ibuf_mode | OS_AIO_SIMULATED_WAKE_LATER, space, i);
	}

	/*�������е�IO�����߳�*/
	os_aio_simulated_wake_handler_threads();

	if(buf_debug_prints && (count > 0))
		printf("Random read-ahead space %lu offset %lu pages %lu\n", space, offset, count);

	return count;
}

ulint buf_read_page(ulint space, ulint offset)
{
	ulint count, count2;

	count = buf_read_ahead_random(space, offset);
	/*ͬ����ȡ*/
	count2 = buf_read_page_low(TRUE, BUF_READ_ANY_PAGE, space, offset);

	buf_flush_free_margin();

	return count + count2;
}

/*����˳��Ԥ��*/
ulint buf_read_ahead_linear(ulint space, ulint offset)
{
	buf_block_t*	block;
	buf_frame_t*	frame;
	buf_block_t*	pred_block	= NULL;
	ulint		pred_offset;
	ulint		succ_offset;
	ulint		count;
	int		asc_or_desc;
	ulint		new_offset;
	ulint		fail_count;
	ulint		ibuf_mode;
	ulint		low, high;
	ulint		i;

	if(srv_startup_is_before_trx_rollback_phase)
		return 0;

	/*ibuf bitmap page������ͷҳ�ǲ���Ԥ����*/
	if(ibuf_bitmap_page(offset) || trx_sys_hdr_page(space, offset))
		return 0;

	low  = (offset / BUF_READ_AHEAD_LINEAR_AREA) * BUF_READ_AHEAD_LINEAR_AREA;
	high = (offset / BUF_READ_AHEAD_LINEAR_AREA + 1) * BUF_READ_AHEAD_LINEAR_AREA;

	/*page_no����[low, high)����ı߽���*/
	if(offset != low && offset != high - 1)
		return 0;

	if(high > fil_space_get_size(space))
		return 0;

	mutex_enter(&(buf_pool->mutex));

	if (buf_pool->n_pend_reads > buf_pool->curr_size / BUF_READ_AHEAD_PEND_LIMIT) {
		mutex_exit(&(buf_pool->mutex));
		return(0);
	}

	asc_or_desc = 1;
	for(i = low; i < high; i ++){
		block = buf_page_hash_get(space, i);
		if(block == NULL || !block->accessed)
			fail_count ++;
		else if(pred_block && ut_ulint_cmp(block->LRU_position, pred_block->LRU_position) != asc_or_desc){
			fail_count ++;
			pred_block = block;
		}
	}

	/*̫����Ҫ�Ӵ����϶�ȡ��page*/
	if (fail_count > BUF_READ_AHEAD_LINEAR_AREA - BUF_READ_AHEAD_LINEAR_THRESHOLD){
		mutex_exit(&(buf_pool->mutex));
		return 0;
	}

	block = buf_page_hash_get(space, offset);
	if(block == NULL){
		mutex_exit(&(buf_pool->mutex));
		return 0;
	}

	frame = block->frame;

	pred_offset = fil_page_get_prev(frame);
	succ_offset = fil_page_get_next(frame);

	mutex_exit(&(buf_pool->mutex));

	if(offset == low && succ_offset == offset + 1) /*��	ǰԤ��64��page*/
		new_offset = pred_offset;
	else if(offset == high - 1 && pred_offset == offset - 1) /*���Ԥ��64��page*/
		new_offset = succ_offset;
	else
		return 0;

	low  = (new_offset / BUF_READ_AHEAD_LINEAR_AREA) * BUF_READ_AHEAD_LINEAR_AREA;
	high = (new_offset / BUF_READ_AHEAD_LINEAR_AREA + 1) * BUF_READ_AHEAD_LINEAR_AREA;
	if(new_offset != low && new_offset != high - 1)
		return 0;

	if(high > fil_space_get_size(space))
		return 0;

	if(ibuf_inside())
		ibuf_mode = BUF_READ_IBUF_PAGES_ONLY;
	else
		ibuf_mode = BUF_READ_ANY_PAGE;

	count = 0;
	os_aio_simulated_put_read_threads_to_sleep();

	for(i = low; i < high; i ++){
		if (!ibuf_bitmap_page(i))
			count += buf_read_page_low(FALSE, ibuf_mode | OS_AIO_SIMULATED_WAKE_LATER, space, i);
	}

	os_aio_simulated_wake_handler_threads();

	buf_flush_free_margin();

	if(buf_debug_prints && count > 0)
		printf( "LINEAR read-ahead space %lu offset %lu pages %lu\n", space, offset, count);
	return count;
}

/*һ���԰���Ҫ�ϲ���ibuf tree�ļ�¼���ڵ�pageȫ�����뻺��أ���ҳ���뵽�����ʱ�����ibuf_merge_or_delete_for_page����ibuf��¼�鲢*/
void buf_read_ibuf_merge_pages(ibool sync, ulint space, ulint* page_nos, ulint n_stored)
{
	ut_ad(!buf_inside());

	/*̫��Ķ�IO����������ȴ�*/
	while(buf_pool->n_pend_reads > buf_pool->curr_size / BUF_READ_AHEAD_PEND_LIMIT)
		os_thread_sleep(500000);

	for(i = 0; i < n_stored; i ++){
		if(i + 1 == n_stored && sync)
			buf_read_page_low(TRUE, BUF_READ_ANY_PAGE, space, page_nos[i]);
		else
			buf_read_page_low(FALSE, BUF_READ_ANY_PAGE, space, page_nos[i]);
	}

	buf_flush_free_margin();

	if(buf_debug_prints)
		printf("Ibuf merge read-ahead space %lu pages %lu\n", space, n_stored);
}

/*��redo log���ݵ�ʱ���ȡ��Ҫ�޸ĵ�ҳ*/
void buf_read_recv_pages(iool sync, ulint space, ulint* page_nos, ulint n_stored)
{
	ulint	count;
	ulint	i;

	for(i = 0; i < n_stored; i ++){
		count = 0;
		os_aio_print_debug = FALSE;
		while(buf_pool->n_pend_reads >= RECV_POOL_N_FREE_BLOCKS / 2){
			os_aio_simulated_wake_handler_threads();
			os_thread_sleep(500000);

			count++;
			if(count > 100){
				fprintf(stderr, "InnoDB: Error: InnoDB has waited for 50 seconds for pending\n"
					"InnoDB: reads to the buffer pool to be finished.\n"
					"InnoDB: Number of pending reads %lu\n", buf_pool->n_pend_reads);

				os_aio_print_debug = TRUE;
			}
		}

		os_aio_print_debug = FALSE;

		if(i + 1 == n_stored && sync) /*���һ����Ϊͬ����ȡ����*/
			buf_read_page_low(TRUE, BUF_READ_ANY_PAGE, space, page_nos[i]);
		else
			buf_read_page_low(FALSE, BUF_READ_ANY_PAGE | OS_AIO_SIMULATED_WAKE_LATER, space, page_nos[i]);
	}

	os_aio_simulated_wake_handler_threads();

	buf_flush_free_margin();

	if(buf_debug_prints)
		printf("Recovery applies read-ahead pages %lu\n", n_stored);
}


