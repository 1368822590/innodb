#include "buf0buf.h"
#include "mem0mem.h"
#include "btr0btr.h"
#include "fil0fil.h"
#include "lock0lock.h"
#include "btr0sea.h"
#include "ibuf0ibuf.h"
#include "dict0dict.h"
#include "log0recv.h"
#include "trx0undo.h"
#include "srv0srv.h"

buf_pool_t* buf_pool		= NULL;
ulint buf_dbg_counter		= 0;
ibool buf_debug_prints		= FALSE;

/*����һ��page���ݵ�hashֵ*/
ulint buf_calc_page_checksum(byte* page)
{
	ulint checksum;

	checksum = ut_fold_binary(page, FIL_PAGE_FILE_FLUSH_LSN) + ut_fold_binary(page + FIL_PAGE_DATA, UNIV_PAGE_SIZE - FIL_PAGE_DATA - FIL_PAGE_END_LSN);
	checksum = checksum & 0xFFFFFFFF;

	return checksum;
}

/*�ж�page�Ƿ�����,һ���ǴӴ��̽�page���뻺���ʱҪ���ж�*/
ibool buf_page_is_corrupted(byte* read_buf)
{
	ulint checksum;

	checksum = buf_calc_page_checksum(read_buf);
	/*У��page��LSN��checksum*/
	if((mach_read_from_4(read_buf + FIL_PAGE_LSN + 4) != mach_read_from_4(read_buf + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN + 4))
		|| (checksum != mach_read_from_4(read_buf + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN)
		&& mach_read_from_4(read_buf + FIL_PAGE_LSN) != mach_read_from_4(read_buf + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN)))
		return TRUE;

	return FALSE;
}

/*��ӡpage�Ĵ�����Ϣ,һ������buf_page_is_corrupted�жϺ����������*/
void buf_page_print(byte* read_buf)
{
	dict_index_t*	index;
	ulint		checksum;
	char*		buf;

	buf = mem_alloc(4 * UNIV_PAGE_SIZE);

	ut_sprintf_buf(buf, read_buf, UNIV_PAGE_SIZE);

	ut_print_timestamp(stderr);
	fprintf(stderr, "  InnoDB: Page dump in ascii and hex (%lu bytes):\n%s", (ulint)UNIV_PAGE_SIZE, buf);
	fprintf(stderr, "InnoDB: End of page dump\n");

	mem_free(buf);

	checksum = buf_calc_page_checksum(read_buf);

	ut_print_timestamp(stderr);
	fprintf(stderr, "  InnoDB: Page checksum %lu stored checksum %lu\n",
		checksum, mach_read_from_4(read_buf+ UNIV_PAGE_SIZE - FIL_PAGE_END_LSN)); 

	fprintf(stderr, "InnoDB: Page lsn %lu %lu, low 4 bytes of lsn at page end %lu\n",
		mach_read_from_4(read_buf + FIL_PAGE_LSN),
		mach_read_from_4(read_buf + FIL_PAGE_LSN + 4),
		mach_read_from_4(read_buf + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN + 4));

	if (mach_read_from_2(read_buf + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE) == TRX_UNDO_INSERT)
		fprintf(stderr, "InnoDB: Page may be an insert undo log page\n");
	else if(mach_read_from_2(read_buf + TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_TYPE) == TRX_UNDO_UPDATE)
		fprintf(stderr, "InnoDB: Page may be an update undo log page\n");

	if(fil_page_get_type(read_buf) == FIL_PAGE_INDEX) {
		fprintf(stderr, "InnoDB: Page may be an index page ");

		fprintf(stderr, "where index id is %lu %lu\n",
			ut_dulint_get_high(btr_page_get_index_id(read_buf)),
			ut_dulint_get_low(btr_page_get_index_id(read_buf)));

		index = dict_index_find_on_id_low(btr_page_get_index_id(read_buf));
		if (index)
			fprintf(stderr, "InnoDB: and table %s index %s\n", index->table_name, index->name);
	}
}

/*��ʼ��һ��buf_block*/
static void buf_block_init(buf_block_t* block, byte* frame)
{
	/*��ʼ��״̬��Ϣ*/
	block->state = BUF_BLOCK_NOT_USED;
	block->frame = frame;
	block->modify_clock = ut_dulint_zero;
	block->file_page_was_freed = FALSE;

	/*��ʼ��latch*/
	rw_lock_create(&(block->lock));
	ut_ad(rw_lock_validate(&(block->lock)));

	rw_lock_create(&(block->read_lock));
	rw_lock_set_level(&(block->read_lock), SYNC_NO_ORDER_CHECK);

	rw_lock_create(&(block->debug_latch));
	rw_lock_set_level(&(block->debug_latch), SYNC_NO_ORDER_CHECK);
}

/*����buf_poot_t����*/
static buf_pool_t* buf_pool_create(ulint max_size, ulint curr_size)
{
	byte*			frame;
	ulint			i;
	buf_block_t*	block;

	ut_a(max_size == curr_size);

	buf_pool = mem_alloc(sizeof(buf_pool_t));

	/*��ʼ���������*/
	mutex_create(&(buf_pool->mutex));
	mutex_set_level(&(buf_pool->mutex), SYNC_BUF_POOL);

	mutex_enter(&(uf_pool->mutex));

	/*����һ��max_size��page size��С���ڴ���Ϊ�����,�����+1��Ϊ��UNIV_PAGE_SIZE����*/
	buf_pool->frame_mem = ut_malloc(UNIV_PAGE_SIZE * (max_size + 1));
	if(buf_pool->frame_mem == NULL)
		return NULL;

	/*����һ������Ϊmax_size��block����*/
	buf_pool->blocks = ut_malloc(max_size * sizeof(buf_block_t));
	if(buf_pool->blocks == NULL)
		return NULL;

	buf_pool->max_size = max_size;
	buf_pool->curr_size = curr_size;

	/*��frame_mem�Ϸ���һ��frame page,��Ϊframe_zero*/
	frame = ut_align(buf_pool->frame_mem, UNIV_PAGE_SIZE);
	buf_pool->frame_zero = frame;
	buf_pool->high_end = frame + UNIV_PAGE_SIZE * curr_size;

	/*������block���г�ʼ���������������frame֮��Ĺ�ϵ*/
	for(i = 0; i < max_size; i ++){
		block = buf_pool_get_nth_block(buf_pool, i);
		buf_block_init(block, frame);
		frame = frame + UNIV_PAGE_SIZE;
	}

	buf_pool->page_hash = hash_create(2 * max_size);
	buf_pool->n_pend_reads = 0;
	buf_pool->last_printout_time = time(NULL);

	buf_pool->n_pages_read = 0;
	buf_pool->n_pages_written = 0;
	buf_pool->n_pages_created = 0;

	buf_pool->n_page_gets = 0;
	buf_pool->n_page_gets_old = 0;
	buf_pool->n_pages_read_old = 0;
	buf_pool->n_pages_written_old = 0;
	buf_pool->n_pages_created_old = 0;

	/*��flush list����ʼ��*/
	UT_LIST_INIT(buf_pool->flush_list);
	for(i = BUF_FLUSH_LRU; i <= BUF_FLUSH_LIST; i ++){
		buf_pool->n_flush[i] = 0;
		buf_pool->init_flush[i] =  FALSE;
		buf_pool->no_flush[i] = os_event_create(NULL);
	}

	buf_pool->LRU_flush_ended = 0;
	buf_pool->ulint_clock = 1;
	buf_pool->freed_page_clock = 0;

	/*��LRU LIST��ʼ��*/
	UT_LIST_INIT(buf_pool->LRU);
	buf_pool->LRU_old = 0;

	UT_LIST_INIT(buf_pool->free);
	for(i = 0; i < curr_size; i ++){
		block = buf_pool_get_nth_block(buf_pool, i);
		memset(block->frame, '\0', UNIV_PAGE_SIZE);

		UT_LIST_ADD_FIRST(free, buf_pool->free, block);
	}

	mutex_exit(&(buf_pool->mutex));
	/*��������ӦHASH����*/
	btr_search_sys_create(curr_size * UNIV_PAGE_SIZE / sizeof(void*) / 64);

	return buf_pool;
}

/*��ʼ������أ�һ����MYSQL������ʱ�����*/
void buf_pool_init(ulint max_size, ulint curr_size)
{
	ut_a(buf_pool == NULL);

	buf_pool_create(max_size, curr_size);

	ut_ad(buf_validate());
}

/*����һ��buf block*/
UNIV_INLINE buf_block_t* buf_block_alloc()
{
	return buf_LRU_get_free_block();
}

/*��block��old LRU LIST�Ƶ�young�У�����LRU List�Ŀ�ʼλ��*/
UNIV_INLINE void buf_block_make_young(buf_block_t* block)
{
	if(buf_pool->freed_page_clock >= block->freed_page_clock + 1 + (buf_pool->curr_size / 1024))
		buf_LRU_make_block_young(block);
}

/*�ͷ�һ��block*/
UNIV_INLINE void buf_block_free(buf_block_t* block)
{
	ut_ad(block->state != BUF_BLOCK_FILE_PAGE);

	mutex_enter(&(buf_pool->mutex));
	buf_LRU_block_free_non_file_page(block);
	mutex_exit(&(buf_pool->mutex));
}

/*����һ��buffer frame*/
buf_frame_t* buf_frame_alloc()
{
	return buf_block_alloc()->frame;
}
/*�ͷ�һ��buffer frame*/
void buf_frame_free(buf_frame_t* frame)
{
	buf_block_free(buf_block_align(frame));
}

/*ͨ��space id��page no��λ����Ӧ��buf_block*/
buf_block_t* buf_page_peek_block(ulint space, ulint offset)
{
	buf_block_t* block;

	mutex_enter_fast(&(buf_pool->mutex));
	block = buf_page_hash_get(space, offset);
	mutex_exit(&(buf_pool->mutex));
}

/*ͨ��space id��page no���Ҷ�Ӧ��page�Ƿ��й�ϣ����*/
ibool buf_page_peek_if_search_hashed(ulint space, ulint offset)
{
	buf_block_t* block;
	ibool is_hashed;

	mutex_enter_fast(&(buf_pool->mutex));
	
	block = buf_page_hash_get(space, offset);
	if(block != NULL)
		is_hashed = FALSE;
	else
		is_hashed = block->is_hashed;

	mutex_exit(&(buf_pool->mutex));

	return is_hashed;
}

/*�ж�space id��page no��Ӧ��page�Ƿ���buf pool���л���*/
ibool buf_page_peek(ulint space, ulint offset)
{
	if(buf_page_peek_block(space, offset))
		return TRUE;

	return FALSE;
}
/*����block->file_page_was_freedΪTRUE*/
buf_block_t* buf_page_set_file_page_was_freed(ulint space, ulint offset)
{
	buf_block_t* block;

	mutex_enter_fast(&(buf_pool->mutex));

	block = buf_page_hash_get(space, offset);
	if(block)
		block->file_page_was_freed = TRUE;

	mutex_exit(&(buf_pool->mutex));

	return block;
}
/*����block->file_page_was_freedΪFALSE*/
buf_block_t* buf_page_reset_file_page_was_freed(ulint space, ulint offset)
{
	buf_block_t*	block;

	mutex_enter_fast(&(buf_pool->mutex));

	block = buf_page_hash_get(space, offset);
	if (block) 
		block->file_page_was_freed = FALSE;

	mutex_exit(&(buf_pool->mutex));

	return(block);
}

/*ͨ��space id��page no��ö�Ӧpage��buf_pool�е�frame��ַ������һ���̿��ܻᴥ��page�Ӵ��̵��뵽buf_pool��*/
buf_frame_t* buf_page_get_gen(ulint space, ulint offset, ulint rw_latch, buf_frame_t* guess, ulint mode, char* file, ulint line, mtr_t* mtr)
{
	buf_block_t*	block;
	ibool		accessed;
	ulint		fix_type;
	ibool		success;
	ibool		must_read;

	ut_ad(mtr);
	ut_ad((rw_latch == RW_S_LATCH) || (rw_latch == RW_X_LATCH) || (rw_latch == RW_NO_LATCH));
	ut_ad((mode != BUF_GET_NO_LATCH) || (rw_latch == RW_NO_LATCH));
	ut_ad((mode == BUF_GET) || (mode == BUF_GET_IF_IN_POOL) || (mode == BUF_GET_NO_LATCH) || (mode == BUF_GET_NOWAIT));

	buf_pool->n_page_gets ++;

loop:
	mutex_enter_fast(&(buf_pool->mutex));
	
	block = NULL;
	if(guess){
		block = buf_block_align(guess);
		/*block�Ͷ�Ӧ��space id��page no��ƥ��*/
		if(offset != block->offset || space != block->space || block->state != BUF_BLOCK_FILE_PAGE)
			block = NULL;
	}

	/*��buf_pool->hash_table����*/
	if(block == NULL)
		block = buf_page_hash_get(space, offset);

	/*page ���ڻ������*/
	if(block == NULL){
		mutex_exit(&(buf_pool->mutex));
		if(mode == BUF_GET_IF_IN_POOL)
			return NULL;

		/*�Ӵ����϶����Ӧ��ҳ��buf pool��*/
		buf_read_page(space, offset);

		goto loop;
	}

	must_read = FALSE;
	if(block->io_fix == BUF_IO_READ){ /*IO������*/
		must_read = TRUE;
		if(mode == BUF_GET_IF_IN_POOL){
			mutex_exit(&(buf_pool->mutex));
			return NULL;
		}
	}

	buf_block_buf_fix_inc(block);

	buf_block_make_young(block);

	accessed = block->accessed;
	block->accessed = TRUE;

	mutex_exit(&(buf_pool->mutex));

	ut_ad(block->buf_fix_count > 0);
	ut_ad(block->state == BUF_BLOCK_FILE_PAGE);

	/*��block��latch��*/
	if(mode == BUF_GET_NOWAIT){
		if(rw_latch == RW_S_LATCH){
			success = rw_lock_s_lock_func_nowait(&(block->lock), file, line);
			fix_type = MTR_MEMO_PAGE_S_FIX;
		}
		else{
			ut_ad(rw_latch == RW_X_LATCH);
			success = rw_lock_x_lock_func_nowait(&(block->lock), file, line);
			fix_type = MTR_MEMO_PAGE_X_FIX;
		}

		/*��latch lockʧ��*/
		if(!success){
			mutex_enter(&(buf_pool->mutex));
			block->buf_fix_count--;
			mutex_exit(&(buf_pool->mutex));

			return NULL;
		}
	}
	else if(rw_latch == RW_NO_LATCH){
		if (must_read) {
			rw_lock_x_lock(&(block->read_lock));
			rw_lock_x_unlock(&(block->read_lock));
		}
		fix_type = MTR_MEMO_BUF_FIX;
	}
	else if(rw_latch == RW_S_LATCH){
		rw_lock_s_lock_func(&(block->lock), 0, file, line);
		fix_type = MTR_MEMO_PAGE_S_FIX;
	}
	else{
		rw_lock_x_lock_func(&(block->lock), 0, file, line);
		fix_type = MTR_MEMO_PAGE_X_FIX;
	}

	mtr_memo_push(mtr, block, fix_type);

	/*����Ԥ������Ϊblock�ǵ�һ�ζ�ȡ��buffer pool��*/
	if(!accessed)
		buf_read_ahead_linear(space, offset);

	return block->frame;
}

/*�ж��Ƿ�������ֹ۷�ʽ(��ǰpage���Ӵ��̶�ȡ)����һ��page*/
ibool buf_page_optimistic_get_func(ulint rw_latch, buf_frame_t* guess, dulint modify_clock, char* file, ulint line, mtr_t* mtr)
{
	buf_block_t*	block;
	ibool		accessed;
	ibool		success;
	ulint		fix_type;

	ut_ad(mtr && guess);
	ut_ad((rw_latch == RW_S_LATCH) || (rw_latch == RW_X_LATCH));

	buf_pool->n_page_gets ++;

	block = buf_block_align(guess);
	
	mutex_enter(&(buf_pool->mutex));

	/*����BLOK FILE PAGE*/
	if(block->state != BUF_BLOCK_FILE_PAGE){
		mutex_exit(&(buf_pool->mutex));
		return FALSE;
	}

	buf_block_buf_fix_inc(block);

	buf_block_make_young(block);

	accessed = block->accessed;
	block->accessed = TRUE;

	ut_ad(!ibuf_inside() || ibuf_page(block->space, block->offset));

	/*��block��latch��*/
	if (rw_latch == RW_S_LATCH) {
		success = rw_lock_s_lock_func_nowait(&(block->lock), file, line);
		fix_type = MTR_MEMO_PAGE_S_FIX;
	} else {
		success = rw_lock_x_lock_func_nowait(&(block->lock),file, line);
		fix_type = MTR_MEMO_PAGE_X_FIX;
	}

	if(!success){
		mutex_enter(&(buf_pool->mutex));
		block->buf_fix_count --;
		mutex_exit(&(buf_pool->mutex));

		return FALSE;
	}

	/*modify_clock��ƥ��*/
	if(!UT_DULINT_EQ(modify_clock, block->modify_clock)){
		buf_page_dbg_add_level(block->frame, SYNC_NO_ORDER_CHECK);
		/*�ͷų��е�latch locker*/
		if(rw_latch == RW_S_LATCH)
			rw_lock_s_unlock(&(block->lock));
		else
			rw_lock_x_unlock(&(block->lock));

		block->buf_fix_count --;

		mutex_exit(&(buf_pool->mutex));

		return FALSE;
	}

	mtr_memo_push(mtr, block, fix_type);

	ut_ad(block->buf_fix_count > 0);
	ut_ad(block->state == BUF_BLOCK_FILE_PAGE);

	/*����Ԥ��*/
	if(!accessed)
		buf_read_ahead_linear(buf_frame_get_space_id(guess), buf_frame_get_page_no(guess));

	return TRUE;
}

/*�ж��Ƿ������nowait��ʽ����һ����֪��page*/
ibool buf_page_get_known_nowait(ulint rw_latch, buf_frame_t* guess, ulint mode, char* file, ulint line, mtr_t* mtr)
{
	buf_block_t*	block;
	ibool		success;
	ulint		fix_type;

	ut_ad(mtr);
	ut_ad((rw_latch == RW_S_LATCH) || (rw_latch == RW_X_LATCH));

	buf_pool->n_page_gets ++;
	block = buf_block_align(guess);

	mutex_enter(&(buf_pool->mutex));
	
	/*�����߳̿��ܸո��Ѿ������buf block��LRU list��ɾ����,�޷������޵ȴ�����page*/
	if(block->state == BUF_BLOCK_REMOVE_HASH){
		mutex_exit(&(buf_pool->mutex));
		return FALSE;
	}

	buf_block_buf_fix_inc(block);

	if(mode == BUF_MAKE_YOUNG)
		buf_block_make_young(block);

	mutex_exit(&(buf_pool->mutex));

	ut_ad(!ibuf_inside() || mode == BUF_KEEP_OLD);

	if (rw_latch == RW_S_LATCH) {
		success = rw_lock_s_lock_func_nowait(&(block->lock), file, line);
		fix_type = MTR_MEMO_PAGE_S_FIX;
	} else {
		success = rw_lock_x_lock_func_nowait(&(block->lock), file, line);
		fix_type = MTR_MEMO_PAGE_X_FIX;
	}
	/*����ʧ��*/
	if(!success){
		mutex_enter(&(buf_pool->mutex));
		block->buf_fix_count--;
		mutex_exit(&(buf_pool->mutex));

		return FALSE;
	}

	mtr_memo_push(mtr, block, fix_type);

	ut_ad(block->buf_fix_count > 0);
	ut_ad(block->state == BUF_BLOCK_FILE_PAGE);

	return TRUE;
}

/*Ϊ��ibbackup�ָ�page�����е�block��ʼ��*/
void buf_page_init_for_backup_restore(ulint space, ulint offset, buf_block_t* block)
{
	/* Set the state of the block */
	block->magic_n		= BUF_BLOCK_MAGIC_N;

	block->state 		= BUF_BLOCK_FILE_PAGE;
	block->space 		= space;
	block->offset 		= offset;

	block->lock_hash_val	= 0;
	block->lock_mutex	= NULL;

	block->freed_page_clock = 0;

	block->newest_modification = ut_dulint_zero;
	block->oldest_modification = ut_dulint_zero;

	block->accessed		= FALSE;
	block->buf_fix_count 	= 0;
	block->io_fix		= 0;

	block->n_hash_helps	= 0;
	block->is_hashed	= FALSE;
	block->n_fields         = 1;
	block->n_bytes          = 0;
	block->side             = BTR_SEARCH_LEFT_SIDE;

	block->file_page_was_freed = FALSE;
}

/*��ʼ��һ��buffer pool page*/
static void buf_page_init(ulint space, ulint offset, buf_block_t* block)
{
	ut_ad(mutex_own(&(buf_pool->mutex)));
	ut_ad(block->state == BUF_BLOCK_READY_FOR_USE);

	block->magic_n		= BUF_BLOCK_MAGIC_N;

	block->state 		= BUF_BLOCK_FILE_PAGE;
	block->space 		= space;
	block->offset 		= offset;

	block->lock_hash_val	= lock_rec_hash(space, offset);
	block->lock_mutex	= NULL;

	/*��space page_no��block��page_hash�н�����Ӧ��ϵ*/
	HASH_INSERT(buf_block_t, hash, buf_pool->page_hash, buf_page_address_fold(space, offset), block);

	block->freed_page_clock = 0;

	block->newest_modification = ut_dulint_zero;
	block->oldest_modification = ut_dulint_zero;

	block->accessed		= FALSE;
	block->buf_fix_count 	= 0;
	block->io_fix		= 0;

	block->n_hash_helps	= 0;
	block->is_hashed	= FALSE;
	block->n_fields     = 1;
	block->n_bytes      = 0;
	block->side         = BTR_SEARCH_LEFT_SIDE;

	block->file_page_was_freed = FALSE;
}

/************************************************************************
Function which inits a page for read to the buffer buf_pool. If the page is
already in buf_pool, does nothing. Sets the io_fix flag to BUF_IO_READ and
sets a non-recursive exclusive lock on the buffer frame. The io-handler must
take care that the flag is cleared and the lock released later. This is one
of the functions which perform the state transition NOT_USED => FILE_PAGE to
a block (the other is buf_page_create). 
**************************************************************************/ 
buf_block_t* buf_page_init_for_read(ulint mode, ulint space, ulint offset)
{
	buf_block_t*	block;
	mtr_t			mtr;

	if(mode == BUF_READ_IBUF_PAGES_ONLY){
		ut_ad(!ibuf_bitmap_page(offset));
		ut_ad(ibuf_inside());

		mtr_start(&mtr);
		/*page����ibuf��һ��page,�ύmini transction*/
		if(!ibuf_page_low(space, offset, &mtr)){
			mtr_commit(&mtr);
			return NULL;
		}
	}
	else
		ut_ad(mode == BUF_READ_ANY_PAGE);

	/*��buf pool�Ϸ���һ��block*/
	block = buf_block_alloc();
	ut_ad(block);

	mutex_enter(&(buf_pool->mutex));
	/*������Ӧhash�������Ѿ�����ͬ��һ��block,˵�����page�Ѿ��ڻ������,ֱ���ͷŷ���*/
	if(NULL != buf_page_hash_get(space, offset)){
		mutex_exit(&(buf_pool->mutex));
		buf_block_free(block);

		if(mode == BUF_READ_IBUF_PAGES_ONLY)
			mtr_commit(&mtr);

		return NULL;
	}

	ut_ad(block);

	/*��page��Ӧ��block���г�ʼ��*/
	buf_page_init(space, offset, block);
	/*��blockѹ��LRU List��old����*/
	buf_LRU_add_block(block, TRUE); 

	block->io_fix = BUF_IO_READ;
	block->n_pend_reads ++;

	rw_lock_x_lock_gen(&(block->lock), BUF_IO_READ);
	rw_lock_x_lock_gen(&(block->read_lock), BUF_IO_READ);

	mutex_exit(&(buf_pool->mutex));

	if(mode == BUF_READ_IBUF_PAGES_ONLY)
		mtr_commit(&mtr);

	return block;
}

buf_frame_t* buf_page_create(ulint space, ulint offset, mtr_t* mtr)
{
	buf_frame_t*	frame;
	buf_block_t*	block;
	buf_block_t*	free_block	= NULL;

	ut_ad(mtr);

	free_block = buf_LRU_get_free_block();

	/*����ibuf�����ݼ�¼��ɾ��
	Delete possible entries for the page from the insert buffer:
	such can exist if the page belonged to an index which was dropped*/
	ibuf_merge_or_delete_for_page(NULL, space, offset);

	mutex_enter(&(buf_pool->mutex));

	block = buf_page_hash_get(space, offset);
	if(block != NULL){
		block->file_page_was_freed = FALSE;
		
		mutex_exit(&(buf_pool->mutex));
		buf_block_free(block);

		/*����ҳ�Ѿ���buf pool���У�ֱ�ӷ���frame����*/
		frame = buf_page_get_with_no_latch(space, offset, mtr);

		return frame;
	}

	if(buf_debug_prints)
		printf("Creating space %lu page %lu to buffer\n", space, offset);

	block = free_block;
	/*��ʼ��page��block�����ϵ�������뵽LRU��*/
	buf_page_init(space, offset, block);
	buf_LRU_add_block(block, FALSE);

	buf_block_buf_fix_inc(block);

	mtr_memo_push(mtr, block, MTR_MEMO_BUF_FIX);

	block->accessed = TRUE;
	buf_pool->n_pages_created ++;

	mutex_exit(&(buf_pool->mutex));

	frame = block->frame;

	return frame;
}

