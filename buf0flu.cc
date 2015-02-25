#include "buf0flu.h"

#include "ut0byte.h"
#include "ut0lst.h"
#include "fil0fil.h"
#include "buf0buf.h"
#include "buf0lru.h"
#include "buf0rea.h"
#include "ibuf0ibuf.h"
#include "log0log.h"
#include "os0file.h"
#include "trx0sys.h"

/*flushˢ�̵�ҳ��*/
#define BUF_FLUSH_AREA		ut_min(BUF_READ_AHEAD_AREA, buf_pool->curr_size / 16)

/*�ж�flush list�ĺϷ���*/
static ibool buf_flush_validate_low();

/*��һ����ҳ��Ӧ��block���뵽flush list����*/
void buf_flush_insert_into_flush_list(buf_block_t* block)
{
	ut_ad(mutex_own(&(buf_pool->mutex)));

	ut_ad((UT_LIST_GET_FIRST(buf_pool->flush_list) == NULL)
		|| (ut_dulint_cmp((UT_LIST_GET_FIRST(buf_pool->flush_list))->oldest_modification, block->oldest_modification) <= 0));

	UT_LIST_ADD_FIRST(flush_list, buf_pool->flush_list, block);

	ut_ad(buf_flush_validate_low());
}

/*��start_lsn���ɴ�С��˳��block���뵽flush list���У�ֻ����redo log���ݵĹ��̲Ż���ô˺���*/
void buf_flush_insert_sorted_into_flush_list(buf_block_t* block)
{
	buf_block_t*	prev_b;
	buf_block_t*	b;

	ut_ad(mutex_own(&(buf_pool->mutex)));

	prev_b = NULL;
	b = UT_LIST_GET_FIRST(buf_pool->flush_list);
	/*�ҵ���LSN�ɴ�С�����λ�ã���Ϊˢ���ǰ���LSN��С����ˢ�̣���flush list��ĩβ��ʼˢ��*/
	while(b && ut_ulint_cmp(b->oldest_modification, block->oldest_modification) > 0){
		prev_b = b;
		b = UT_LIST_GET_NEXT(flush_list, b);
	}

	/*prev_b == NULL,˵��block->start_lsn�ȶ������κ�block��Ҫ�����Բ��뵽flush list��ͷ��*/
	if(prev_b == NULL)
		UT_LIST_ADD_FIRST(flush_list, buf_pool->flush_list, block);
	else
		UT_LIST_INSERT_AFTER(flush_list, buf_pool->flush_list, prev_b, block);

	ut_ad(buf_flush_validate_low());
}

/*���block�Ƿ���Խ����û���̭�������IO��������fix latch���ڡ��Ѿ����޸Ĺ���û��ˢ����̣����ܽ����û�*/
ibool buf_flush_ready_for_replace(buf_block_t* block)
{
	ut_ad(mutex_own(&(buf_pool->mutex)));
	ut_ad(block->state == BUF_BLOCK_FILE_PAGE);

	if((ut_dulint_cmp(block->oldest_modification, ut_dulint_zero) > 0) 
		|| block->buf_fix_count != 0 || block->io_fix != 0)
		return FALSE;
	
	return TRUE;
}

/*���block��Ӧ��page����ҳ�����ҿ��Խ���flush��������*/
UNIV_INLINE ibool buf_flush_ready_for_flush(buf_block_t* block, ulint flush_type)
{
	ut_ad(mutex_own(&(buf_pool->mutex)));
	ut_ad(block->state == BUF_BLOCK_FILE_PAGE);

	if(ut_dulint_cmp(block->oldest_modification, ut_dulint_zero) > 0 && block->io_fix == 0){
		if(flush_type != BUF_FLUSH_LRU)
			return TRUE;
		else if(block->buf_fix_count == 0)
			return TRUE;
	}

	return FALSE;
}

/*��һ��flush����źŵ���ʱ���޸Ķ�Ӧ��״̬��Ϣ*/
void buf_flush_write_complete(buf_block_t* block)
{
	ut_ad(block);
	ut_ad(mutex_own(&(buf_pool->mutex)));
	/*��start_lsn����Ϊ0����ʾ�Ѿ�����ҳˢ������*/
	block->oldest_modification = ut_dulint_zero;
	/*��block��flush list��ɾ��*/
	UT_LIST_REMOVE(flush_list, buf_pool->flush_list, block);

	ut_d(UT_LIST_VALIDATE(flush_list, buf_block_t, buf_pool->flush_list));
	/*�޸�flush�ļ�����,����������Ǽ�¼����flushing��page����*/
	(buf_pool->n_flush[block->flush_type]) --;

	if(block->flush_type == BUF_FLUSH_LRU){
		buf_LRU_make_block_old(block); /*��block����lru old listĩβ�Ա���̭*/
		buf_pool->LRU_flush_ended ++;
	}

	/*�������flush�Ƿ���ɣ������ɣ�����һ���������FLUSH���ź�*/
	if(buf_pool->n_flush[block->flush_type] == 0 && buf_pool->init_flush[block->flush_type] == FALSE)
		os_event_set(buf_pool->no_flush[block->flush_type]);
}

/*��doublewrite�ڴ��е����ݺͶ�Ӧ��ҳˢ��disk���һ���aio�߳�,ҳ����ˢ������ͨ���첽��ʽˢ���*/
static void buf_flush_buffered_writes()
{
	buf_block_t*	block;
	ulint			len;
	ulint			i;

	if(trx_doublewrite == NULL){
		os_aio_simulated_wake_handler_threads();
		return ;
	}

	/*���latch��ʱ����ܱȽϳ�����Ϊ�漰��ͬ��IO����*/
	mutex_enter(&(trx_doublewrite->mutex));
	
	if(trx_doublewrite->first_free == 0){
		mutex_exit(&(trx_doublewrite->mutex));
		return;
	}

	/*ȷ��ˢ�����ݵĳ���*/
	if (trx_doublewrite->first_free > TRX_SYS_DOUBLEWRITE_BLOCK_SIZE)
		len = TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * UNIV_PAGE_SIZE;
	else
		len = trx_doublewrite->first_free * UNIV_PAGE_SIZE;
	/*ˢ���һ��doublewrite���ݿ飬������ͬ��ˢ�룬��Ϊdoublewrite��Ϊ�˱�֤������ɲ���ʧ��Ƶģ��������첽IOˢ��*/
	fil_io(OS_FILE_WRITE, TRUE, TRX_SYS_SPACE,
		trx_doublewrite->block1, 0, len, (void*)trx_doublewrite->write_buf, NULL);

	/*ˢ��ڶ���doublewrite���ݿ�*/
	if (trx_doublewrite->first_free > TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
		len = (trx_doublewrite->first_free - TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) * UNIV_PAGE_SIZE;

		fil_io(OS_FILE_WRITE, TRUE, TRX_SYS_SPACE, trx_doublewrite->block2, 0, len,
			(void*)(trx_doublewrite->write_buf + TRX_SYS_DOUBLEWRITE_BLOCK_SIZE * UNIV_PAGE_SIZE), NULL);
	}

	fil_flush(TRX_SYS_SPACE);

	/*����Ӧ����ҳˢ�����*/
	for (i = 0; i < trx_doublewrite->first_free; i++) {
		block = trx_doublewrite->buf_block_arr[i];
		/*�첽��pageˢ��*/
		fil_io(OS_FILE_WRITE | OS_AIO_SIMULATED_WAKE_LATER, FALSE, block->space, block->offset, 0, UNIV_PAGE_SIZE,
			(void*)block->frame, (void*)block);
	}

	os_aio_simulated_wake_handler_threads();

	/*�ȴ�aio���������ź�Ϊ��,Ҳ���ǵȴ����е�write����ȫ�����*/
	os_aio_wait_until_no_pending_writes();

	fil_flush_file_spaces(FIL_TABLESPACE);

	/*��֤doublewrite memory�е�����ȫ��ˢ�����*/
	trx_doublewrite->first_free = 0;

	mutex_exit(&(trx_doublewrite->mutex));
}

/*��block��Ӧ��pageд��doublewrite��*/
static void buf_flush_post_to_doublewrite_buf(buf_block_t* block)
{
try_again:
	mutex_enter(&(trx_doublewrite->mutex));
	/*doublewrite ����̫�࣬��Ҫ����ǿ��ˢ��*/
	if (trx_doublewrite->first_free >= 2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
			mutex_exit(&(trx_doublewrite->mutex));
			buf_flush_buffered_writes();

			goto try_again;
	}
	/*��block��Ӧ��pageд�뵽doublewrite�ڴ��У��Ա㱣��*/
	ut_memcpy(trx_doublewrite->write_buf + UNIV_PAGE_SIZE * trx_doublewrite->first_free, block->frame, UNIV_PAGE_SIZE);

	trx_doublewrite->buf_block_arr[trx_doublewrite->first_free] = block;
	trx_doublewrite->first_free ++;

	if (trx_doublewrite->first_free >= 2 * TRX_SYS_DOUBLEWRITE_BLOCK_SIZE) {
			mutex_exit(&(trx_doublewrite->mutex));
			buf_flush_buffered_writes();

			return;
	}

	mutex_exit(&(trx_doublewrite->mutex));
}

/*��page��LSN��space page_no����Ϣд��page header/tailer*/
void buf_flush_init_for_writing(byte* page, dulint newest_lsn, ulint space, ulint page_no)
{
	/*������page�޸ĵ�lsn��ֵд��page��ͷβ*/
	mach_write_to_8(page + FIL_PAGE_LSN, newest_lsn);
	mach_write_to_8(page + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN, newest_lsn);

	mach_write_to_4(page + FIL_PAGE_SPACE, space);
	mach_write_to_4(page + FIL_PAGE_OFFSET, page_no);

	/*��ҳ��checksumд��ҳβ*/
	mach_write_to_4(page + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN, buf_calc_page_checksum(page));
}

/*��block��Ӧ��page��redo logˢ�����*/
static void buf_flush_write_block_low(buf_block_t* block)
{
	ut_ad(!ut_dulint_is_zero(block->newest_modification));
	
	/*ǿ�ƽ�block->newest_modification��Ϊˢ�̵㣬С�����ֵ��LSN��redo logȫ��ˢ��*/
	log_flush_up_to(block->newest_modification, LOG_WAIT_ALL_GROUPS);

	buf_flush_init_for_writing(block->frame, block->newest_modification, block->space, block->offset);
	/*û��doublewrite,ֱ���첽ˢ�̣��п��ܻ�������ݶ�ʧ����Ϊredo log�п���û��ˢ����̣�
	��Ϊredo LOG�ǰ���512Ϊһ�����checksumˢ����̵ģ��������512ˢ�̣���ô��redo log��ȡ��ʱ���У��checksum,
	�п��ܻὫ��������������һ��Ҫ����doublewrite��������֤���ݲ���ʧ*/
	if (!trx_doublewrite) 
		fil_io(OS_FILE_WRITE | OS_AIO_SIMULATED_WAKE_LATER, FALSE, block->space, block->offset, 0, UNIV_PAGE_SIZE, (void*)block->frame, (void*)block);
	else
		buf_flush_post_to_doublewrite_buf(block);
}

static ulint buf_flush_try_page(ulint space, ulint offset, ulint flush_type)
{
	buf_block_t*	block;
	ibool		locked;

	ut_ad(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST || flush_type == BUF_FLUSH_SINGLE_PAGE);

	mutex_enter(&(buf_pool->mutex));

	block = buf_page_hash_get(space, offset);
	/*flush list�е�blockˢ��*/
	if(flush_type == BUF_FLUSH_LIST && block != NULL && buf_flush_ready_for_flush(block)){
		block->io_fix = BUF_IO_WRITE;
		block->flush_type = flush_type;

		if(buf_pool->no_flush[flush_type] == 0)
			os_event_reset(buf_pool->no_flush[flush_type]);

		(buf_pool->n_flush[flush_type])++;

		locked = FALSE;

		if(block->buf_fix_count == 0){
			rw_lock_s_lock_gen(&(block->lock), BUF_IO_WRITE);
			locked = TRUE;
		}

		mutex_exit(&(buf_pool->mutex));

		/*��IO���������block�ϣ�ֱ��ˢ��doublewrite��������*/
		if(!locked){
			buf_flush_buffered_writes();
			rw_lock_s_lock_gen(&(block->lock), BUF_IO_WRITE);
		}

		if (buf_debug_prints) {
			printf("Flushing page space %lu, page no %lu \n", block->space, block->offset);
		}

		/*��block��Ӧ��page��redo log���̣�����pageд�뵽doublewrite buf��*/
		buf_flush_write_block_low(block);

		return 1;
	}
	else if(flush_type == BUF_FLUSH_LRU && block != NULL && buf_flush_ready_for_flush(block, flush_type)){
		block->io_fix = BUF_IO_WRITE;
		block->flush_type = flush_type;

		if (buf_pool->n_flush[flush_type] == 0)
			os_event_reset(buf_pool->no_flush[flush_type]);

		(buf_pool->n_flush[flush_type])++;

		rw_lock_s_lock_gen(&(block->lock), BUF_IO_WRITE);

		mutex_exit(&(buf_pool->mutex));

		buf_flush_write_block_low(block);
		/*�����ҳ����ˢ���̣�����redo log checkpoint������ʱ��*/
		return 1;
	}
	else if(flush_type == BUF_FLUSH_SINGLE_PAGE && block != NULL && buf_flush_ready_for_flush(block, flush_type)){
		block->io_fix = BUF_IO_WRITE;
		block->flush_type = flush_type;

		if (buf_pool->n_flush[block->flush_type] == 0)
			os_event_reset(buf_pool->no_flush[block->flush_type]);

		(buf_pool->n_flush[flush_type])++;

		mutex_exit(&(buf_pool->mutex));

		rw_lock_s_lock_gen(&(block->lock), BUF_IO_WRITE);

		if (buf_debug_prints)
			printf("Flushing single page space %lu, page no %lu \n",block->space, block->offset);

		buf_flush_write_block_low(block);
		return 1;
	}
	else{
		mutex_exit(&(buf_pool->mutex));
		return(0);
	}
}

/*��(space, offset)��Ӧ��ҳλ����Χ��ҳȫ��ˢ������*/
static ulint buf_flush_try_neighbors(ulint space, ulint offset, ulint flush_type)
{
	buf_block_t*	block;
	ulint		low, high;
	ulint		count		= 0;
	ulint		i;

	ut_ad(flush_type == BUF_FLUSH_LRU || flush_type == BUF_FLUSH_LIST);

	low = (offset / BUF_FLUSH_AREA) * BUF_FLUSH_AREA;
	high = (offset / BUF_FLUSH_AREA + 1) * BUF_FLUSH_AREA;

	if(UT_LIST_GET_LEN(buf_pool->LRU) < BUF_LRU_OLD_MIN_LEN){
		low = offset;
		high = offset + 1;
	}
	else if(flush_type == BUF_FLUSH_LIST){
		low = offset;
		high = offset + 1;
	}

	if(high > fil_space_get_size(space))
		high = fil_space_get_size(space);

	mutex_enter(&(buf_pool->mutex));

	for(i = low; i < high; i ++){
		block = buf_page_hash_get(space, i);
		if(block && flush_type == BUF_FLUSH_LRU && i != offset && !block->old)
			continue;

		if(block != NULL && buf_flush_ready_for_flush(block, flush_type)){
			mutex_exit(&(buf_pool->mutex));

			count += buf_flush_try_page(space, i, flush_type);
			mutex_enter(&(buf_pool->mutex));
		}
	}

	mutex_exit(&(buf_pool->mutex));

	return count;
}

/*������ҳˢ�������*/
ulint buf_flush_batch(ulint flush_type, ulint min_n, dulint lsn_limit)
{
	buf_block_t*	block;
	ulint		page_count 	= 0;
	ulint		old_page_count;
	ulint		space;
	ulint		offset;
	ibool		found;

	ut_ad((flush_type == BUF_FLUSH_LRU) || (flush_type == BUF_FLUSH_LIST)); 
	ut_ad((flush_type != BUF_FLUSH_LIST) || sync_thread_levels_empty_gen(TRUE));

	mutex_enter(&(buf_pool->mutex));

	if(buf_pool->n_flush[flush_type] > 0 || buf_pool->init_flush[flush_type] == TRUE){
		mutex_exit(&(buf_pool->mutex));
		return ULINT_UNDEFINED;
	}

	(buf_pool->init_flush)[flush_type] = TRUE;

	for(;;){
		if(page_count >= min_n)
			break;

		if(flush_type == BUF_FLUSH_LRU)
			block = UT_LIST_GET_LAST(buf_pool->LRU);
		else{
			ut_ad(flush_type == BUF_FLUSH_LIST);
			block = UT_LIST_GET_LAST(buf_pool->flush_list);

			if(block != NULL || ut_dulint_cmp(block->oldest_modification, lsn_limit) >= 0)
				break;
		}

		found = FALSE;
		/*���Խ���Χ����flush*/
		while(block != NULL && !found){
			if(buf_flush_ready_for_flush(block, flush_type)){
				found = TRUE;
				space = block->space;
				offset = block->offset;

				mutex_exit(&(buf_pool->mutex));

				old_page_count = page_count;
				page_count += buf_flush_try_neighbors(space, offset, flush_type);
				mutex_enter(&(buf_pool->mutex));
			}
			else if(flush_type == BUF_FLUSH_LRU)
				block = UT_LIST_GET_PREV(LRU, block);
			else {
				ut_ad(flush_type == BUF_FLUSH_LIST);
				block = UT_LIST_GET_PREV(flush_type, block);
			}
		}

		if(!found)
			break;
	}

	/*��������ˢ����ź�*/
	(buf_pool->init_flush)[flush_type] = FALSE;
	if ((buf_pool->n_flush[flush_type] == 0) && (buf_pool->init_flush[flush_type] == FALSE)){
		os_event_set(buf_pool->no_flush[flush_type]);
	}

	mutex_exit(&(buf_pool->mutex));
	
	buf_flush_buffered_writes();

	if (buf_debug_prints && page_count > 0) {
		if (flush_type == BUF_FLUSH_LRU)
			printf("Flushed %lu pages in LRU flush\n", page_count);
		else if (flush_type == BUF_FLUSH_LIST)
			printf("Flushed %lu pages in flush list flush\n",
				page_count);
		else 
			ut_error;
	}

	return page_count;
}

/*��һ��pages batch flush�ȴ������*/
void buf_flush_wait_batch_end(ulint type)
{
	ut_ad((type == BUF_FLUSH_LRU) || (type == BUF_FLUSH_LIST));
	os_event_wait(buf_pool->no_flush[type]);
}

/*����������ͬʱˢ�̵�LRU�е�page�ĸ���*/
static ulint buf_flush_LRU_recommendation()
{
	buf_block_t*	block;
	ulint		n_replaceable;
	ulint		distance	= 0;

	mutex_enter(&(buf_pool->mutex));

	n_replaceable = UT_LIST_GET_LEN(buf_pool->free);
	block = UT_LIST_GET_LAST(buf_pool->LRU);

	while(block != NULL && n_replaceable < BUF_FLUSH_FREE_BLOCK_MARGIN + BUF_FLUSH_EXTRA_MARGIN
		&& distance < BUF_LRU_FREE_SEARCH_LEN){
			if(buf_flush_ready_for_replace(block))
				n_replaceable ++;

			distance ++;
			block = UT_LIST_GET_PREV(LRU, block);
	}

	mutex_exit(&(buf_pool->mutex));

	if(n_replaceable >= BUF_FLUSH_FREE_BLOCK_MARGIN)
		return 0;

	return (BUF_FLUSH_FREE_BLOCK_MARGIN + BUF_FLUSH_EXTRA_MARGIN - n_replaceable);
}

void buf_flush_free_margin()
{
	ulint n_to_flush = buf_flush_LRU_recommendation();
	if(n_to_flush > 0)
		buf_flush_batch(BUF_FLUSH_LRU, n_to_flush, ut_dulint_zero);
}

/*���block��start_lsn��˳��*/
static ibool buf_flush_validate_low(void)
{
	buf_block_t*	block;
	dulint		om;

	UT_LIST_VALIDATE(flush_list, buf_block_t, buf_pool->flush_list);

	block = UT_LIST_GET_FIRST(buf_pool->flush_list);

	while (block != NULL) {
		om = block->oldest_modification;
		ut_a(block->state == BUF_BLOCK_FILE_PAGE);
		ut_a(ut_dulint_cmp(om, ut_dulint_zero) > 0);

		block = UT_LIST_GET_NEXT(flush_list, block);

		if (block)
			ut_a(ut_dulint_cmp(om, block->oldest_modification) >= 0);
	}

	return(TRUE);
}

ibool buf_flush_validate()
{
	ibool	ret;

	mutex_enter(&(buf_pool->mutex));

	ret = buf_flush_validate_low();

	mutex_exit(&(buf_pool->mutex));

	return(ret);
}
