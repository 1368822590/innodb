#include "buf0flu.h"
#include "buf0lru.h"
#include "buf0rea.h"
#include "mtr0mtr.h"

extern ulint buf_dbg_counter;

/*�ж�block�Ƿ���Է���younger list����*/
UNIV_INLINE ibool buf_block_peek_if_too_old(buf_block_t* block)
{
	if(buf_pool->freed_page_clock >= block->freed_page_clock + 1 + (buf_pool->curr_size / 1024))
		return TRUE;

	return FALSE;
}

/*��õ�ǰ�����ʹ�ÿռ��С*/
UNIV_INLINE ulint buf_pool_get_curr_size()
{
	return buf_pool->curr_size * UNIV_PAGE_SIZE;
}

/*��û�������ռ��С����mysql�������ļ���Ϊbuffer_pool_size������*/
UNIV_INLINE buf_block_t* buf_pool_get_max_size()
{
	return buf_pool->max_size * UNIV_PAGE_SIZE;
}

/*���buffer pool�еĵ�i��block*/
UNIV_INLINE buf_block_t* buf_pool_get_nth_block(buf_pool_t* pool, ulint i)
{
	ut_ad(buf_pool);
	ut_ad(i < buf_pool->max_size);

	return i + buf_pool->blocks;
}

/*���ptr�Ƿ���buffer pool blocks�е�ָ��*/
UNIV_INLINE ibool buf_pool_is_block(void* ptr)
{
	if(buf_pool->blocks <= (buf_block_t*)ptr && (buf_block_t*)ptr < buf_pool->blocks + buf_pool->max_size)
		return TRUE;

	return FALSE;
}

/*���lru�����������޸ĵ�block�Ķ�Ӧlsn*/
UNIV_INLINE dulint buf_pool_get_oldest_modification(void)
{
	buf_block_t*	block;
	dulint			lsn;

	mutex_enter(&(buf_pool->mutex));

	block = UT_LIST_GET_LAST(buf_pool->flush_list);
	if(block == NULL)
		lsn = ut_dulint_zero;
	else
		lsn = block->oldest_modification;

	mutex_exit(&(buf_pool->mutex));

	return lsn;
}

/*pool clock �Լ�1*/
UNIV_INLINE ulint buf_pool_clock_tic()
{
	ut_ad(mutex_own(&(buf_pool->mutex)));

	buf_pool->ulint_clock ++;

	return buf_pool->ulint_clock;
}

/*���block��Ӧ��frameָ��*/
UNIV_INLINE buf_frame_t* buf_block_get_frame(buf_block_t* block)
{
	ut_ad(block);
	ut_ad(block >= buf_pool->blocks);
	ut_ad(block < buf_pool->blocks + buf_pool->max_size);
	ut_ad(block->state != BUF_BLOCK_NOT_USED); 
	ut_ad((block->state != BUF_BLOCK_FILE_PAGE) || (block->buf_fix_count > 0));

	return block->frame;
}

/*���block��Ӧ��space id*/
UNIV_INLINE ulint buf_block_get_space(buf_block_t* block)
{
	ut_ad(block);
	ut_ad(block >= buf_pool->blocks);
	ut_ad(block < buf_pool->blocks + buf_pool->max_size);
	ut_ad(block->state == BUF_BLOCK_FILE_PAGE);
	ut_ad(block->buf_fix_count > 0);

	return block->space;
}

/*���block��Ӧ��page no*/
UNIV_INLINE ulint buf_block_get_page_no(buf_block_t* block)
{
	ut_ad(block);
	ut_ad(block >= buf_pool->blocks);
	ut_ad(block < buf_pool->blocks + buf_pool->max_size);
	ut_ad(block->state == BUF_BLOCK_FILE_PAGE);
	ut_ad(block->buf_fix_count > 0);

	return block->offset;
}

/*����ptr���ڵ�blockָ���ַ*/
UNIV_INLINE buf_block_t* buf_block_align(byte* ptr)
{
	buf_block_t* block;
	buf_frame_t* frame_zero;

	ut_ad(ptr);

	frame_zero = buf_pool->frame_zero;
	ut_ad((ulint)ptr >= (ulint)frame_zero);

	block = buf_pool_get_nth_block(buf_pool, ((ulint)(ptr - frame_zero)) >> UNIV_PAGE_SIZE_SHIFT);
	/*block����buf pool��blocks��ַ��Χ�У����쳣���*/
	if(block < buf_pool->blocks || block >= buf_pool->blocks + buf_pool->max_size){
		fprintf(stderr,
			"InnoDB: Error: trying to access a stray pointer %lx\n"
			"InnoDB: buf pool start is at %lx, number of pages %lu\n", (ulint)ptr,
			(ulint)frame_zero, buf_pool->max_size);

		ut_a(0);
	}

	return block;
}

/*��buf_block_align������ͬ*/
UNIV_INLINE buf_block_t* buf_block_align_low(byte* ptr)
{
	buf_block_t*	block;
	buf_frame_t*	frame_zero;

	ut_ad(ptr);

	frame_zero = buf_pool->frame_zero;

	ut_ad((ulint)ptr >= (ulint)frame_zero);

	block = buf_pool_get_nth_block(buf_pool, ((ulint)(ptr - frame_zero)) >> UNIV_PAGE_SIZE_SHIFT);
	if (block < buf_pool->blocks || block >= buf_pool->blocks + buf_pool->max_size) {

			fprintf(stderr,
				"InnoDB: Error: trying to access a stray pointer %lx\n"
				"InnoDB: buf pool start is at %lx, number of pages %lu\n", (ulint)ptr,
				(ulint)frame_zero, buf_pool->max_size);
			ut_a(0);
	}

	return block;
}

/*���ptr���ڵ�pageָ���ַ*/
UNIV_INLINE buf_frame_t* buf_frame_align(byte* ptr)
{
	buf_frame_t* frame;

	ut_ad(ptr);

	frame = ut_align_down(ptr, UNIV_PAGE_SIZE);

	if ((void *)frame < (void*)(buf_pool->frame_zero)
		|| (void *)frame > (void *)(buf_pool_get_nth_block(buf_pool, buf_pool->max_size - 1)->frame)){
			fprintf(stderr,
				"InnoDB: Error: trying to access a stray pointer %lx\n"
				"InnoDB: buf pool start is at %lx, number of pages %lu\n", (ulint)ptr,
				(ulint)(buf_pool->frame_zero), buf_pool->max_size);
			ut_a(0);
	}

	return frame;
}

/*���ptr����block��page no,Ҳ����ͨ��pageָ���ö�Ӧpage��block��Ϣ*/
UNIV_INLINE ulint buf_frame_get_page_no(byte* ptr)
{
	return buf_block_get_page_no(buf_block_align(ptr));
}

/*���ptr����block��space id*/
UNIV_INLINE ulint buf_frame_get_space_id(byte* ptr)
{
	return buf_block_get_space(buf_block_align(ptr));
}

/*ͨ��ptr��ȡһ�����������λ����Ϣ(fil_addr_t)*/
UNIV_INLINE void buf_ptr_get_fsp_addr(byte* ptr, ulint* space, fil_addr_t* addr)
{
	buf_block_t* block;

	block = buf_block_align(ptr);

	*space = buf_block_get_space(block);
	addr->page = buf_block_get_page_no(block);
	addr->boffset = ptr - buf_frame_align(ptr);
}

UNIV_INLINE ulint buf_frame_get_lock_hash_val(byte* ptr)
{
	buf_block_t* block;
	block = buf_block_align(ptr);

	return block->lock_hash_val;
}

UNIV_INLINE mutex_t* buf_frame_get_lock_mutex(byte* ptr)
{
	buf_block_t* block;
	block = buf_block_align(ptr);

	return block->lock_mutex;
}
/*��frame�����ݿ�����buf��*/
UNIV_INLINE byte* buf_frame_copy(byte* buf, buf_frame_t* frame)
{
	ut_ad(buf && frame);

	ut_memcpy(buf, frame, UNIV_PAGE_SIZE);
}

/*��space��page no����һ��page��fold��Ϣ*/
UNIV_INLINE ulint buf_page_address_fold(ulint space, ulint offset)
{
	return((space << 20) + space + offset);
}

/*�ж�һ��io�����Ƿ�����������block��Ӧ��page*/
UNIV_INLINE ibool buf_page_io_query(buf_block_t* block)
{
	mutex_enter(&(buf_pool->mutex));

	ut_ad(block->state == BUF_BLOCK_FILE_PAGE);
	ut_ad(block->buf_fix_count > 0);

	if(block->io_fix != 0){
		mutex_exit(&(buf_pool->mutex));
		return TRUE;
	}

	mutex_exit(&(buf_pool->mutex));

	return FALSE;
}

/*���frame��Ӧ��block��newest modification��LSN��*/
UNIV_INLINE dulint buf_frame_get_newest_modification(buf_frame_t* frame)
{
	buf_block_t*	block;
	dulint			lsn;

	ut_ad(frame);

	block = buf_block_align(frame);

	mutex_enter(&(buf_pool->mutex));

	if (block->state == BUF_BLOCK_FILE_PAGE)
		lsn = block->newest_modification;
	else 
		lsn = ut_dulint_zero;

	mutex_exit(&(buf_pool->mutex));

	return lsn;
}

/*��frame��Ӧ��block��modify_clock���Լ�*/
UNIV_INLINE dulint buf_frame_modify_clock_inc(buf_frame_t* frame)
{
	buf_block_t*	block;

	ut_ad(frame);

	block = buf_block_align_low(frame);
	ut_ad((mutex_own(&(buf_pool->mutex)) && (block->buf_fix_count == 0)) || rw_lock_own(&(block->lock), RW_LOCK_EXCLUSIVE));

	UT_DULINT_INC(block->modify_clock);

	return block->modify_clock;
}

/*���frame��Ӧblock��modify_clock*/
UNIV_INLINE dulint buf_frame_get_modify_clock(buf_frame_t*	frame)
{
	buf_block_t*	block;

	ut_ad(frame);

	block = buf_block_align(frame);

	ut_ad(rw_lock_own(&(block->lock), RW_LOCK_SHARED)
		|| rw_lock_own(&(block->lock), RW_LOCK_EXCLUSIVE));

	return block->modify_clock;
}

UNIV_INLINE void buf_block_buf_fix_inc_debug(buf_block_t* block, char* file, ulint line)
{
	ibool	ret;
	ret = rw_lock_s_lock_func_nowait(&(block->debug_latch), file, line);
	ut_ad(ret);

	block->buf_fix_count ++;
}

UNIV_INLINE void buf_block_buf_fix_inc(buf_block_t* block)
{
	block->buf_fix_count ++;
}

/*����space id��page no��buf pool���Ҷ�Ӧ��block,���û�б�����ػ���Ļ�������ΪNULL*/
UNIV_INLINE buf_block_t* buf_page_hash_get(ulint space, ulint offset)
{
	buf_block_t*	block;
	ulint			fold;

	ut_ad(buf_pool);
	ut_ad(mutex_own(&(buf_pool->mutex)));

	fold = buf_page_address_fold(space, offset);

	/*����fold��buf pool��page hash�в��Ҷ�Ӧ��block*/
	HASH_SEARCH(hash, buf_pool->page_hash, fold, block, (block->space == space) && (block->offset == offset));

	return block;
}

/*���Ի��һ��page,���page���ڻ�����У��ͻᴥ��file io�Ӵ��̵��뵽������У���ʱ��Ҫ����page�����е�mtr latchȫ��release*/
UNIV_INLINE buf_frame_t* buf_page_get_release_on_io(ulint space, ulint offset, buf_frame_t* guess, ulint rw_latch, ulint savepoint, mtr_t* mtr)
{
	buf_frame_t*	frame;

	frame = buf_page_get_gen(space, offset, rw_latch, guess, BUF_GET_IF_IN_POOL, __FILE__, __LINE__, mtr);
	if(frame != NULL)
		return frame;

	mtr_rollback_to_savepoint(mtr, savepoint);
	buf_page_get(space, offset, RW_S_LATCH, mtr);
	mtr_rollback_to_savepoint(mtr, savepoint);

	return NULL;
}

/*��block��buf_fix_count�����Լ�������release ָ����rw_latch��block->lock*/
UNIV_INLINE void buf_page_release(buf_block_t* block, ulint rw_latch, mtr_t* mtr)
{
	ulint	buf_fix_count;

	ut_ad(block);

	mutex_enter_fast(&(buf_pool->mutex));

	ut_ad(block->state == BUF_BLOCK_FILE_PAGE);
	ut_ad(block->buf_fix_count > 0);

	if (rw_latch == RW_X_LATCH && mtr->modifications) 
		buf_flush_note_modification(block, mtr);

#ifdef UNIV_SYNC_DEBUG
	rw_lock_s_unlock(&(block->debug_latch));
#endif
	/*Ϊʲô��ôд���ѵ���Ϊ�˲�����*/
	buf_fix_count = block->buf_fix_count;
	block->buf_fix_count = buf_fix_count - 1;

	mutex_exit(&(buf_pool->mutex));

	/*�ͷ�ָ�����͵�rw_lock*/
	if (rw_latch == RW_S_LATCH)
		rw_lock_s_unlock(&(block->lock));
	else if (rw_latch == RW_X_LATCH)
		rw_lock_x_unlock(&(block->lock));
}

void buf_page_dbg_add_level(buf_frame_t* frame, ulint level)
{
#ifdef UNIV_SYNC_DEBUG
	sync_thread_add_level(&(buf_block_align(frame)->lock), level);
#endif
}
