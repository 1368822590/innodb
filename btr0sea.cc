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

/*���hash�����ɹ�������btr_search_t�Ľṹ��Ϣ*/
static void btr_search_info_update_hash(btr_search_t* info, btr_cur_t* cursor)
{
	dict_index_t*	index;
	ulint			n_unique;
	int				cmp;

	ut_ad(!rw_lock_own(&btr_search_latch, RW_LOCK_SHARED) && !rw_lock_own(&btr_search_latch, RW_LOCK_EX));

	index = cursor->index;
	if(index->type & DICT_IBUF)
		return ;

	/*���Ψһ������ֵ*/
	n_unique = dict_index_get_n_unique_in_tree(index);

	if(info->n_hash_potential == 0)
		goto set_new_recomm;

	if(info->n_fields >= n_unique && cursor->up_match >= n_unique){
		info->n_hash_potential ++;
		return ;
	}

	/*��fields n_bytes���бȽ�*/
	cmp = ut_pair_cmp(info->n_fields, info->n_bytes, cursor->low_match, cursor->low_bytes);

	if ((info->side == BTR_SEARCH_LEFT_SIDE && cmp <= 0) || (info->side == BTR_SEARCH_RIGHT_SIDE && cmp > 0))
			goto set_new_recomm;

	info->n_hash_potential ++;
	return ;

set_new_recomm:
	info->hash_analysis = 0;

	cmp = ut_pair_cmp(cursor->up_match, cursor->up_bytes, cursor->low_match, cursor->low_bytes);
	if(cmp == 0){
		info->n_hash_potential = 0;
		info->n_fields = 1;
		info->n_bytes = 0;
		info->side = BTR_SEARCH_LEFT_SIDE;
	}
	else if(cmp > 0){
		info->n_hash_potential = 1;
		if(cursor->up_match >= n_unique){
			info->n_fields = n_unique;
			info->n_bytes = 0;
		}
		else if(cursor->low_match < cursor->up_match){
			info->n_fields = cursor->low_match + 1;
			info->n_bytes = cursor->low_bytes + 1;
		}
		info->side = BTR_SEARCH_LEFT_SIDE;
	}
	else{
		info->n_hash_potential = 1;
		if (cursor->low_match >= n_unique) {
			info->n_fields = n_unique;
			info->n_bytes = 0;
		} 
		else if (cursor->low_match > cursor->up_match) {
			info->n_fields = cursor->up_match + 1;
			info->n_bytes = 0;
		} 
		else {		
			info->n_fields = cursor->up_match;
			info->n_bytes = cursor->up_bytes + 1;
		}

		info->side = BTR_SEARCH_RIGHT_SIDE;
	}
}

/*������ӦHASH�����ɹ��󣬸��¶�Ӧibuf block�е�״̬��Ϣ*/
static ibool btr_search_update_block_hash_info(btr_search_t* info, buf_block_t* block, btr_cur_t* cursor)
{
	ut_ad(!rw_lock_own(&btr_search_latch, RW_LOCK_SHARED) && !rw_lock_own(&btr_search_latch, RW_LOCK_EX));
	ut_ad(rw_lock_own(&(block->lock), RW_LOCK_SHARED) || rw_lock_own(&(block->lock), RW_LOCK_EX));
	ut_ad(cursor);

	info->last_hash_succ = FALSE;

	/*ħ����У��*/
	ut_a(block->magic_n == BUF_BLOCK_MAGIC_N);
	ut_a(info->magic_n == BTR_SEARCH_MAGIC_N);

	if(block->n_hash_helps > 0 && info->n_hash_potential > 0 && block->n_fields == info->n_fields
		&& block->n_bytes == info->n_bypes && block->side == info->side){
			if(block->is_hashed && block->curr_n_fields == info->n_fields 
				&& block->curr_n_bytes == info->n_bytes && block->curr_side == info->side)
				info->last_hash_succ = TRUE;
			block->n_hash_fields ++;
	}
	else{
		block->n_hash_helps = 1;
		block->n_fields = info->n_fields;
		block->n_bytes = info->n_bytes;
		block->side = info->side;
	}

	if(block->n_hash_helps > page_get_n_recs(block->frame) / BTR_SEARCH_PAGE_BUILD_LIMIT &&
		info->n_hash_potential >= BTR_SEARCH_BUILD_LIMIT){
			if ((!block->is_hashed) || (block->n_hash_helps > 2 * page_get_n_recs(block->frame))
					|| (block->n_fields != block->curr_n_fields) || (block->n_bytes != block->curr_n_bytes) || (block->side != block->curr_side))
					return TRUE;
	}

	return FALSE;
}

static void btr_search_update_hash_ref(btr_search_t* info, buf_block_t* block, btr_cur_t* cursor)
{
	ulint	fold;
	rec_t*	rec;
	dulint	tree_id;

	ut_ad(cursor->flag == BTR_CUR_HASH_FAIL);
	ut_ad(rw_lock_own(&btr_search_latch, RW_LOCK_EX));
	ut_ad(rw_lock_own(&(block->lock), RW_LOCK_SHARED) || rw_lock_own(&(block->lock), RW_LOCK_EX));

	/*�ж��Ƿ��ǿ�����Ϊhash����������Ϣ*/
	if(block->is_hashed && info->n_hash_potential > 0 && block->curr_n_fields == info->n_fields
		&& block->curr_n_bytes == info->n_bytes && block->curr_side == info->side){
			rec = btr_cur_get_rec(cursor);
			/*��hash�����еļ�¼һ�����û��ļ�¼*/
			if(!page_rec_is_user_rec(rec))
				return;

			tree_id = cursor->index->tree->id;

			fold = rec_fold(rec, block->curr_n_fields, block->curr_n_bytes, tree_id);
			ut_ad(rw_lock_own(&btr_search_latch, RW_LOCK_EX));
			/*���뵽hash����*/
			ha_insert_for_fold(btr_search_sys->hash_index, fold, rec);
	}
}

/*����cursor����btr_search_t��Ϣ���п��ܻ����һ��hash����*/
void btr_search_info_update_slow(btr_search_t* info, btr_cur_t* cursor)
{
	buf_block_t*	block;
	ibool			build_index;

	ut_ad(!rw_lock_own(&btr_search_latch, RW_LOCK_SHARED) && !rw_lock_own(&btr_search_latch, RW_LOCK_EX));

	block = buf_block_align(btr_cur_get_rec(cursor));
	/*����btr_search_t��״̬��Ϣ*/
	btr_search_info_update_hash(info, cursor);

	/*�ж��Ƿ���Ϊ��λ�õ�ԭ��Ҫ�ؽ�hash ����*/
	build_index = btr_search_update_block_hash_info(info, block, cursor);
	if(build_index || cursor->flag == BTR_CUR_HASH_FAIL) /*�����Խ���hash�������ڴ�ռ�*/
		btr_search_check_free_space_in_heap();

	if(cursor->flag == BTR_CUR_HASH_FAIL){
		btr_search_n_hash_fail++;
		rw_lock_x_lock(&btr_search_latch);
		/*���뵽hash����*/
		btr_search_update_hash_ref(info, block, cursor);

		rw_lock_x_unlock(&btr_search_latch);
	}

	if(build_index)
		btr_search_build_page_hash_index(block->frame, block->n_fields,block->n_bytes, block->side);
}

/*���²��btree cursor��λ���Ƿ���ȷ*/
static ibool btr_search_check_guess(btr_cur_t* cursor, ibool can_only_compare_to_cursor_rec, dtuple_t* tuple, ulint mode, mtr_t* mtr)
{
	page_t*	page;
	rec_t*	rec;
	rec_t*	prev_rec;
	rec_t*	next_rec;
	ulint	n_unique;
	ulint	match;
	ulint	bytes;
	int		cmp;

	n_unique = dict_index_get_n_unique_in_tree(cursor->index);

	rec = btr_cur_get_rec(cursor);
	page = buf_frame_align(rec);

	ut_ad(page_rec_is_user_rec(rec));

	match = 0;
	bytes = 0;
	cmp = page_cmp_dtuple_rec_with_match(tuple, rec, &match, &bytes);
	if(mode == PAGE_CUR_GE){ /*>=, rec��tuple����ߣ�����ȷ*/
		if(cmp == 1)
			return FALSE;

		cursor->up_match = match;
		if(match >= n_unique)
			return TRUE;
	}
	else if(mode == PAGE_CUR_LE){/*<=, rec��tuple���ұߣ�����ȷ*/
		if(cmp == -1)
			return FALSE;

		cursor->low_match = match;
	}
	else if(mode == PAGE_CUR_G){
		if(cmp != -1)
			return FALSE;
	}
	else if(mode == PAGE_CUR_L){
		if(cmp != 1)
			return FALSE;
	}

	if (can_only_compare_to_cursor_rec)
		return(FALSE);

	match = 0;
	bytes = 0;
	if((mode == PAGE_CUR_G) || (mode == PAGE_CUR_GE)){
		ut_ad(rec != page_get_infimum_rec(page));

		prev_rec = page_rec_get_prev(rec);
		if(prev_rec == page_get_infimum_rec(page)){ 
			if(btr_page_get_prev(page, mtr) != FIL_NULL)/*����btree����ʼλ����*/
				return FALSE;

			return TRUE;
		}

		/*��ǰһ����¼���бȽ�,���tuple����С��prev_rec,˵��λ�û��ǲ���*/
		cmp = page_cmp_dtuple_rec_with_match(tuple, prev_rec, &match, &bytes);
		if(mode == PAGE_CUR_GE){
			if(cmp != 1)
				return FALSE;
		}
		else{ 
			if(cmp == -1)
				return FALSE;
		}

		return TRUE;
	}

	ut_ad(rec != page_get_supremum_rec(page));

	next_rec = page_rec_get_next(rec);
	if (next_rec == page_get_supremum_rec(page)) {
		if (btr_page_get_next(page, mtr) == FIL_NULL) {
			cursor->up_match = 0;
			return(TRUE);
		}

		return(FALSE);
	}

	cmp = page_cmp_dtuple_rec_with_match(tuple, next_rec, &match, &bytes);
	if (mode == PAGE_CUR_LE) {
		if (cmp != -1) 
			return(FALSE);

		cursor->up_match = match;
	} else {
		if (cmp == 1)
			return(FALSE);
	}

	return TRUE;
}

/*ͨ��tuple����Ϣ��hash�����в��Ҷ�Ӧָ��ļ�¼������btree cursorָ���ҵ��ļ�¼��*/
ibool btr_search_guess_on_hash(dict_index_t* index, btr_search_t* info, dtuple_t* tuple, ulint mode, ulint latch_mode,
	btr_cur_t* cursor, ulint has_search_latch, mtr_t* mtr)
{
	buf_block_t*	block;
	rec_t*		rec;
	page_t*		page;
	ibool		success;
	ulint		fold;
	ulint		tuple_n_fields;
	dulint		tree_id;
	ibool       can_only_compare_to_cursor_rec = TRUE;

	ut_ad(index && info && tuple && cursor && mtr);
	ut_ad((latch_mode == BTR_SEARCH_LEAF) || (latch_mode == BTR_MODIFY_LEAF));

	if(info->n_hash_potential == 0)
		return FALSE;

	cursor->n_fields = info->n_fields;
	cursor->n_bytes = info->n_bypes;

	tuple_n_fields = dtuple_get_n_fields(tuple);
	if(tuple_n_fields < cursor->n_fields)
		return FALSE;

	if(cursor->n_bytes > 0 && tuple_n_fields <= cursor->n_fields)
		return FALSE;

	tree_id = index->tree->id;
	/*��������hash*/
	fold = dtuple_fold(tuple, cursor->n_fields, cursor->n_bytes, tree_id);
	cursor->fold = fold;
	cursor->flag = BTR_CUR_HASH;

	if(!has_search_latch)
		rw_lock_s_lock(&btr_search_latch);

	ut_a(btr_search_latch.writer != RW_LOCK_EX);
	ut_a(btr_search_latch.reader_count > 0);

	/*��hash����ͨ��������ȡ����Ӧ�ļ�¼ָ��*/
	rec = ha_search_and_get_data(btr_search_sys->hash_index, fold);
	if (!rec) {
		if (!has_search_latch)
			rw_lock_s_unlock(&btr_search_latch);

		goto failure;
	}

	page = buf_frame_align(rec);
	if(!has_search_latch){ /*����¼У��*/
		success = buf_page_get_known_nowait(latch_mode, page, BUF_MAKE_YOUNG, IB__FILE__, __LINE__, mtr);
		rw_lock_s_unlock(&btr_search_latch);
		if(!success)
			goto failure;

		can_only_compare_to_cursor_rec = FALSE;
		buf_page_dbg_add_level(page, SYNC_TREE_NODE_FROM_HASH);
	}

	block = buf_block_align(page);

	if (block->state == BUF_BLOCK_REMOVE_HASH) {
		if (!has_search_latch)
			btr_leaf_page_release(page, latch_mode, mtr);

		goto failure;
	}

	ut_ad(block->state == BUF_BLOCK_FILE_PAGE);
	ut_ad(page_rec_is_user_rec(rec));

	/*�����α�λ��*/
	btr_cur_position(index, rec, cursor);

	/*btree id����ͬ��˵���Ǵ����*/
	if(0 != ut_dulint_cmp(tree_id, btr_page_get_index_id(page)))
		success = FALSE;
	else{
		/*���ݲ�ѯ����ȷ��cursor�Ƿ���ȷ*/
		success = btr_search_check_guess(cursor, can_only_compare_to_cursor_rec, tuple, mode, mtr);
	}

	if(!success){
		if (!has_search_latch)
			btr_leaf_page_release(page, latch_mode, mtr);

		goto failure;
	}

	if (info->n_hash_potential < BTR_SEARCH_BUILD_LIMIT + 5)
		info->n_hash_potential++;

	if (!info->last_hash_succ)
		info->last_hash_succ = TRUE;

	if (!has_search_latch && buf_block_peek_if_too_old(block))
		buf_page_make_young(page);

	buf_pool->n_page_gets ++;
	
	return TRUE;

failure:
	info->n_hash_fail ++;
	cursor->flag = BTR_CUR_HASH_FAIL;

	return FALSE;
}

/*ɾ��һ��page���м�¼��hash����*/
void btr_search_drop_page_hash_index(page_t* page)
{
	hash_table_t*	table;
	buf_block_t*	block;
	ulint		n_fields;
	ulint		n_bytes;
	rec_t*		rec;
	rec_t*		sup;
	ulint		fold;
	ulint		prev_fold;
	dulint		tree_id;
	ulint		n_cached;
	ulint		n_recs;
	ulint*		folds;
	ulint		i;

	ut_ad(!rw_lock_own(&btr_search_latch, RW_LOCK_SHARED) && !rw_lock_own(&btr_search_latch, RW_LOCK_EX));

	rw_lock_s_lock(&btr_search_latch);

	block = buf_block_align(page);
	if(!block->is_hashed){ /*���block��û���κ�hash������Ӧ��ϵ*/
		rw_lock_s_unlock(&btr_search_latch);
		return 0;
	}

	table = btr_search_sys->hash_index;

	ut_ad(rw_lock_own(&(block->lock), RW_LOCK_SHARED) || rw_lock_own(&(block->lock), RW_LOCK_EX) || (block->buf_fix_count == 0));

	n_fields = block->curr_n_fields;
	n_bytes = block->curr_n_bytes;

	ut_a(n_fields + n_bytes > 0);

	rw_lock_s_unlock(&btr_search_latch);

	n_recs = page_get_n_recs(page);

	folds = mem_alloc(n_recs * sizeof(ulint));
	n_cached = 0;

	sup = page_get_supremum_rec(page);
	rec = page_get_infimum_rec(page);
	rec = page_rec_get_next(rec);

	if(rec != sup){
		ut_a(n_fields <= rec_get_n_fields(rec));
		if(n_bytes > 0)
			ut_a(n_fields < rec_get_n_fields(rec));
	}

	tree_id = btr_page_get_index_id(page);

	prev_fold = 0;
	/*����ÿ����¼��fold hash,������folds������*/
	while(rec != sup){
		fold = rec_fold(rec, n_fields, n_bytes, tree_id);

		if (fold == prev_fold && prev_fold != 0)
			goto next_rec;

		folds[n_cached] = fold;
		n_cached++;
next_rec:
		rec = page_rec_get_next(rec);
		prev_fold = fold;
	}

	/*��folds�е�hashֵ���δ�hash��������ɾ��*/
	rw_lock_x_lock(&btr_search_latch);

	for(i = 0; i < n_cached; i ++)
		ha_remove_all_nodes_to_page(table, folds[i], page);
	block->is_hashed = FALSE;

	rw_lock_x_unlock(&btr_search_latch);

	/*�ͷ���ʱ�洢��folds*/
	mem_free(folds);
}

/*��һ��page��free��ɾ���������м�¼��hash����*/
void btr_search_drop_page_hash_when_freed(ulint space, ulint pge_no)
{
	ibool is_hashed;
	page_t* page;
	mtr_t* mtr;

	is_hashed = buf_page_peek_if_search_hashed(space, page_no);

	if(!is_hashed)
		return;

	mtr_start(&mtr);

	/*��ø���space id��page no���page*/
	page = buf_page_get(space, page_no, RW_S_LATCH, &mtr);
	buf_page_dbg_add_level(page, SYNC_TREE_NODE_FROM_HASH);
	/*ɾ��ҳ�����м�¼��hash����*/
	btr_search_drop_page_hash_index(page);

	mtr_commit(mtr);
}

/*���¹���hash�����������µ���λ�ý��й���*/
static void btr_search_build_page_hash_index(page_t* page, ulint n_fields, ulint n_bytes, ulint side)
{
	hash_table_t*	table;
	buf_block_t*	block;
	rec_t*		rec;
	rec_t*		next_rec;
	rec_t*		sup;
	ulint		fold;
	ulint		next_fold;
	dulint		tree_id;
	ulint		n_cached;
	ulint		n_recs;
	ulint*		folds;
	rec_t**		recs;
	ulint		i;

	block = buf_block_align(page);
	/*���ȫ�ֵ�hash��*/
	table = btr_search_sys->hash_index;

	ut_ad(!rw_lock_own(&btr_search_latch, RW_LOCK_EX));
	ut_ad(rw_lock_own(&(block->lock), RW_LOCK_SHARED) || rw_lock_own(&(block->lock), RW_LOCK_EX));

	rw_lock_s_lock(&btr_search_latch);

	/*��¼���в�ƥ�����е�hash������������Ҫ��page�������м�¼��hash����ɾ��*/
	if (block->is_hashed && ((block->curr_n_fields != n_fields)
		|| (block->curr_n_bytes != n_bytes) || (block->curr_side != side))){
			rw_lock_s_unlock(&btr_search_latch);
			btr_search_drop_page_hash_index(page);
	}
	else
		rw_lock_s_unlock(&btr_search_latch);

	/*ҳ��û���û���¼*/
	n_recs = page_get_n_recs(page);
	if(n_recs == 0)
		return ;

	/*û��hash������Ӧ����,�ǲ���������ں���ǰ��ȽϺã�*/
	if(n_fields + n_bytes == 0)
		return;

	folds = mem_alloc(n_recs * sizeof(ulint));
	recs = mem_alloc(n_recs * sizeof(rec_t*));

	n_cached = 0;

	tree_id = btr_page_get_index_id(page);

	sup = page_get_supremum_rec(page);

	rec = page_get_infimum_rec(page);
	rec = page_rec_get_next(rec);

	/*��ָ����hash������λ�úϷ����ж�*/
	if (rec != sup) {
		ut_a(n_fields <= rec_get_n_fields(rec));

		if (n_bytes > 0) 
			ut_a(n_fields < rec_get_n_fields(rec));
	}

	/*����rec fold hash*/
	fold = rec_fold(rec, n_fields, n_bytes, tree_id);
	if(side == BTR_SEARCH_LEFT_SIDE){
		folds[n_cached] = fold;
		recs[n_cached] = rec;
		n_cached++;
	}

	for(;;){
		next_rec = page_rec_get_next(rec);
		if (next_rec == sup) {
			if (side == BTR_SEARCH_RIGHT_SIDE) {
				folds[n_cached] = fold;
				recs[n_cached] = rec;
				n_cached++;
			}

			break;
		}

		/*������һ����¼��fold hash*/
		next_fold = rec_fold(next_rec, n_fields, n_bytes, tree_id);

		if(fold != next_fold){ /*�е�ֵ�п����ظ�������ֻ������ǰ��������¼��Ϊhash����*/
			if (side == BTR_SEARCH_LEFT_SIDE) {
				folds[n_cached] = next_fold;
				recs[n_cached] = next_rec;
				n_cached++;
			} 
			else{/*���ҵ���*/
				folds[n_cached] = fold;
				recs[n_cached] = rec;
				n_cached++;
			}
		}

		rec = next_rec;
		fold = next_fold;
	}

	/*��hash������Ҫ���ڴ�ռ������*/
	btr_search_check_free_space_in_heap();

	rw_lock_x_lock(&btr_search_latch);
	/*�Ѿ��������б���е�hash�����ˣ�ֻ�з���*/
	if(block->is_hashed && ((block->curr_n_fields != n_fields) || (block->curr_n_bytes != n_bytes) || (block->curr_side != side))){
			rw_lock_x_unlock(&btr_search_latch);
			mem_free(folds);
			mem_free(recs);
			return;
	}

	/*���ڴ��е�block��hash ������ص���Ϣ������*/
	block->is_hashed = TRUE;
	block->n_hash_helps = 0;
	block->curr_n_fields = n_fields;
	block->curr_n_bytes = n_bytes;
	block->curr_side = side;

	/*��folds��recsȫ�����뵽*/
	for(i = 0; i < n_cached; i ++)
		ha_insert_for_fold(table, folds[i], recs[i]);

	rw_lock_x_unlock(&btr_search_latch);

	mem_free(folds);
	mem_free(recs);
}

/*ҳת�ƻ���ɾ�����hash��Ҫ����*/
void btr_search_move_or_delete_hash_entries(page_t* new_page, page_t* page)
{
	buf_block_t*	block;
	buf_block_t*	new_block;
	ulint			n_fields;
	ulint			n_bytes;
	ulint			side;

	block = buf_block_align(page);
	new_block = buf_block_align(new_page);

	ut_ad(rw_lock_own(&(block->lock), RW_LOCK_EX) && rw_lock_own(&(new_block->lock), RW_LOCK_EX));

	rw_lock_s_lock(&btr_search_latch);

	/*ɾ�������е�hash����*/
	if(new_block->is_hashed){
		rw_lock_s_unlock(&btr_search_latch);
		btr_search_drop_page_hash_index(page);

		return;
	}

	if(block->is_hashed){
		n_fields = block->curr_n_fields;
		n_bytes = block->curr_n_bytes;
		side = block->curr_side;

		new_block->n_fields = block->curr_n_fields;
		new_block->n_bytes = block->curr_n_bytes;
		new_block->side = block->curr_side;
		
		rw_lock_s_unlock(&btr_search_latch);

		ut_a(n_fields + n_bytes > 0);

		/*���¹�������*/
		btr_search_build_page_hash_index(new_page, n_fields, n_bytes, side);

		ut_a(n_fields == block->curr_n_fields);
		ut_a(n_bytes == block->curr_n_bytes);
		ut_a(side == block->curr_side);

		return ;
	}

	rw_lock_s_unlock(&btr_search_latch);
}

/*��¼ɾ������hash����*/
void btr_search_update_hash_on_delete(btr_cur_t* cursor)
{
	hash_table_t*	table;
	buf_block_t*	block;
	rec_t*		rec;
	ulint		fold;
	dulint		tree_id;
	ibool		found;

	rec = btr_cur_get_rec(cursor);
	block = buf_block_align(rec);

	ut_ad(rw_lock_own(&(block->lock), RW_LOCK_EX));

	if(!block->is_hashed)
		return ;

	ut_a(block->curr_n_fields + block->curr_n_bytes > 0);

	table = btr_search_sys->hash_index;
	tree_id = cursor->index->tree_id;

	fold = rec_fold(rec, block->curr_n_fields, block->curr_n_bytes, tree_id);

	rw_lock_x_lock(&btr_search_latch);
	/*��hash��������ɾ��*/
	found = ha_search_and_delete_if_found(table, fold, rec);

	rw_lock_x_unlock(&btr_search_latch);
}

void btr_search_update_hash_node_on_insert(btr_cur_t* cursor)
{
	hash_table_t*	table;
	buf_block_t*	block;
	rec_t*			rec;

	rec = btr_cur_get_rec(cursor);
	block = buf_block_align(rec);

	ut_ad(rw_lock_own(&(block->lock), RW_LOCK_EX));
	if(!block->is_hashed)
		return ;

	rw_lock_x_lock(&btr_search_latch);
	/**/
	if((cursor->flag == BTR_CUR_HASH) && (cursor->n_fields == block->curr_n_fields)
		&& (cursor->n_bytes == block->curr_n_bytes) && (block->curr_side == BTR_SEARCH_RIGHT_SIDE)){
			table = btr_search_sys->hash_index;
			/*�滻��¼��ָ��ֵ�Ϳ���*/
			ha_search_and_update_if_found(table, cursor->fold, rec, page_rec_get_next(rec));
			rw_lock_x_unlock(&btr_search_latch);
	}
	else{
		rw_lock_x_unlock(&btr_search_latch);
		/*����һ���µ�����hash*/
		btr_search_update_hash_on_insert(cursor);
	}
}

/*TODO:���������Ҫ��ϸ�о��������������¼�󲻽��������һ��������¼��Ӧ��hash������������²����¼�����¼��Ӧ��hash����*/
void btr_search_update_hash_on_insert(btr_cur_t* cursor)
{
	hash_table_t*	table; 
	buf_block_t*	block;
	page_t*		page;
	rec_t*		rec;
	rec_t*		ins_rec;
	rec_t*		next_rec;
	dulint		tree_id;
	ulint		fold;
	ulint		ins_fold;
	ulint		next_fold;
	ulint		n_fields;
	ulint		n_bytes;
	ulint		side;
	ibool		locked	= FALSE;

	table = btr_search_sys->hash_index;

	btr_search_check_free_space_in_heap();
	
	rec = btr_cur_get_rec(cursor);
	block = buf_block_align(rec);

	ut_ad(rw_lock_own(&(block->lock), RW_LOCK_EX));
	if(!block->is_hashed)
		return ;

	tree_id = cursor->index->tree->id;

	n_fields = block->curr_n_fields;
	n_bytes = block->curr_n_bytes;
	side = block->curr_side;

	ins_rec = page_rec_get_next(rec);
	next_rec = page_rec_get_next(ins_rec);

	page = buf_frame_align(rec);
	ins_fold = rec_fold(ins_rec, n_fields, n_bytes, tree_id);

	if(next_rec != page_get_supremum_rec(page))
		next_fold = rec_fold(next_rec, n_fields, n_bytes, tree_id);

	if(rec != page_get_infimum_rec(page))
		fold = rec_fold(rec, n_fields, n_bytes, tree_id);
	else{
		if (side == BTR_SEARCH_LEFT_SIDE) {
			rw_lock_x_lock(&btr_search_latch);
			locked = TRUE;
			ha_insert_for_fold(table, ins_fold, ins_rec);
		}

		goto check_next_rec;
	}

	if(fold != ins_fold){
		if(!locked){
			rw_lock_x_lock(&btr_search_latch);
			locked = TRUE;
		}

		if(side == BTR_SEARCH_RIGHT_SIDE)
			ha_insert_for_fold(table, fold, rec); /*ins_rec��recǰ�棬����һ��rec��hash����*/
		else
			ha_insert_for_fold(table, ins_fold, ins_rec); /*ins_rec��rec�ĺ��棬����һ��ins_rec�Ĺ�ϣ�������ں�����Ҫ����next_rec��hash����*/
	}

check_next_rec:
	if (next_rec == page_get_supremum_rec(page)) {
		if (side == BTR_SEARCH_RIGHT_SIDE) {
			if (!locked) {
				rw_lock_x_lock(&btr_search_latch);
				locked = TRUE;
			}

			ha_insert_for_fold(table, ins_fold, ins_rec);
		}

		goto function_exit;
	}

	if(ins_fold != next_fold){
		if(!locked){
			rw_lock_x_lock(&btr_search_latch);
			locked = TRUE;
		}
		/*�ڴ˸��»��߲����������¼��hash����*/
		if(side == BTR_SEARCH_RIGHT_SIDE)
			ha_insert_for_fold(table, ins_fold, ins_rec);
		else
			ha_insert_for_fold(table, next_fold, next_rec);
	}

function_exit:
	if(locked)
		rw_lock_x_unlock(&btr_search_latch);
}

void btr_search_print_info(void)
{
	printf("SEARCH SYSTEM INFO\n");

	rw_lock_x_lock(&btr_search_latch);
	/*	ha_print_info(btr_search_sys->hash_index); */
	rw_lock_x_unlock(&btr_search_latch);
}

void btr_search_index_print_info(dict_index_t* index)
{
	btr_search_t*	info;

	printf("INDEX SEARCH INFO\n");

	rw_lock_x_lock(&btr_search_latch);

	info = btr_search_get_info(index);

	printf("Searches %lu, hash succ %lu, fail %lu, patt succ %lu\n",
		info->n_searches, info->n_hash_succ, info->n_hash_fail,
		info->n_patt_succ);

	printf("Total of page cur short succ for all indexes %lu\n", page_cur_short_succ);
	rw_lock_x_unlock(&btr_search_latch);
}

void btr_search_table_print_info(char*	name)
{
	dict_table_t*	table;
	dict_index_t*	index;

	mutex_enter(&(dict_sys->mutex));

	table = dict_table_get_low(name);

	ut_a(table);

	mutex_exit(&(dict_sys->mutex));

	index = dict_table_get_first_index(table);

	while (index) {
		btr_search_index_print_info(index);
		index = dict_table_get_next_index(index);
	}
}

ibool btr_search_validate(void)
{
	buf_block_t*	block;
	page_t*		page;
	ha_node_t*	node;
	ulint		n_page_dumps	= 0;
	ibool		ok		= TRUE;
	ulint		i;
	char		rec_str[500];

	rw_lock_x_lock(&btr_search_latch);

	for (i = 0; i < hash_get_n_cells(btr_search_sys->hash_index); i++) {
		node = hash_get_nth_cell(btr_search_sys->hash_index, i)->node;

		while (node != NULL) {
			block = buf_block_align(node->data);
			page = buf_frame_align(node->data);

			if (!block->is_hashed
				|| node->fold != rec_fold((rec_t*)(node->data),
				block->curr_n_fields,
				block->curr_n_bytes,
				btr_page_get_index_id(page))) {
					ok = FALSE;
					ut_print_timestamp(stderr);

					fprintf(stderr,
						"  InnoDB: Error in an adaptive hash index pointer to page %lu\n"
						"ptr mem address %lu index id %lu %lu, node fold %lu, rec fold %lu\n",
						buf_frame_get_page_no(page),
						(ulint)(node->data),
						ut_dulint_get_high(btr_page_get_index_id(page)),
						ut_dulint_get_low(btr_page_get_index_id(page)),
						node->fold, rec_fold((rec_t*)(node->data),
						block->curr_n_fields,
						block->curr_n_bytes,
						btr_page_get_index_id(page)));

					rec_sprintf(rec_str, 450, (rec_t*)(node->data));

					fprintf(stderr,
						"InnoDB: Record %s\n"
						"InnoDB: on that page.", rec_str);

					fprintf(stderr,
						"Page mem address %lu, is hashed %lu, n fields %lu, n bytes %lu\n"
						"side %lu\n",
						(ulint)page, block->is_hashed, block->curr_n_fields,
						block->curr_n_bytes, block->curr_side);

					if (n_page_dumps < 20) {	
						buf_page_print(page);
						n_page_dumps++;
					}
			}

			node = node->next;
		}
	}

	if (!ha_validate(btr_search_sys->hash_index)) {

		ok = FALSE;
	}

	rw_lock_x_unlock(&btr_search_latch);

	return(ok);
}



