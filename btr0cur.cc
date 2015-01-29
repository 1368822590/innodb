#include "btr0cur.h"

#include "page0page.h"
#include "rem0rec.h"
#include "rem0cmp.h"
#include "btr0btr.h"
#include "btr0sea.h"
#include "row0upd.h"
#include "trx0rec.h"
#include "que0que.h"
#include "row0row.h"
#include "srv0srv.h"
#include "ibuf0ibuf.h"
#include "lock0lock.h"

ibool	btr_cur_print_record_ops = FALSE;

ulint	btr_cur_rnd = 0;

ulint	btr_cur_n_non_sea = 0;
ulint	btr_cur_n_sea = 0;
ulint	btr_cur_n_non_sea_old = 0;
ulint	btr_cur_n_sea_old = 0;


/*ҳ��������*/
#define BTR_CUR_PAGE_REORGANIZE_LIMIT	(UNIV_PAGE_SIZE / 32)

#define BTR_KEY_VAL_ESTIMATE_N_PAGES	8

/*BLOB�е�ͷ�ṹ*/
#define BTR_BLOB_HDR_PART_LEN			0	/*blob�ڶ�Ӧҳ�еĳ���*/
#define BTR_BLOB_HDR_NEXT_PAGE_NO		4	/*��һ������blob���ݵ�page no*/
#define BTR_BLOB_HDR_SIZE				8	


static void			btr_cur_add_path_info(btr_cur_t* cursor, ulint height, ulint root_height);
static void			btr_rec_free_updated_extern_fields(dict_index_t* index, rec_t* rec, upd_t* update, ibool do_not_free_inherited, mtr_t* mtr);
static ulint		btr_rec_get_externally_stored_len(rec_t* rec);

/*��page������latch����*/
static void btr_cur_latch_leaves(dict_tree_t* tree, page_t* page, ulint space, ulint page_no, ulint latch_mode, btr_cur_t* cursor, mtr_t* mtr)
{
	ulint	left_page_no;
	ulint	right_page_no;

	ut_ad(tree && page && mtr);

	if(latch_mode == BTR_SEARCH_LEAF) /*����ʱ������ֱ�ӻ�ȡһ��S-LATCH,*/
		btr_page_get(space, page_no, RW_S_LATCH, mtr);
	else if(latch_mode == BTR_MODIFY_LEAF) /*Ҷ�ӽڵ��޸ģ�ֱ�Ӷ�Ҷ�ӽڵ�x-latch����*/
		btr_page_get(space, page_no, RW_X_LATCH, mtr);
	else if(latch_mode == BTR_MODIFY_TREE){ /*�ֱ���ȡǰ����ҳ���Լ���x-latch*/
		left_page_no = btr_page_get_prev(page, mtr);
		if(left_pge_no != FIL_NULL) /*��ǰһҳ��ȡһ��X-LATCH*/
			btr_page_get(space, left_page_no, RW_X_LATCH, mtr);

		btr_page_get(space, page_no, RW_X_LATCH, mtr);

		/*�Ժ�һ��ҳҲ��ȡһ��X-LATCH*/
		right_page_no = btr_page_get_next(page, mtr);
		if(right_page_no != FIL_NULL)
			btr_page_get(space, right_page_no, RW_X_LATCH, mtr);
	}
	else if(latch_mode == BTR_SEARCH_PREV){ /*����ǰһҳ����ǰһҳ����һ��s-latch*/
		left_page_no = btr_page_get_prev(page, mtr);
		if(left_page_no != FIL_NULL)
			cursor->left_page = btr_page_get(space, left_page_no, RW_S_LATCH, mtr);

		btr_page_get(space, page_no, RW_S_LATCH, mtr);
	}
	else if(latch_mode == BTR_MODIFY_PREV){ /*����ǰһҳ����ǰһҳ����һ��x-latch*/
		left_page_no = btr_page_get_prev(page, mtr);
		if(left_page_no != FIL_NULL)
			cursor->left_page = btr_page_get(space, left_page_no, RW_X_LATCH, mtr);

		btr_page_get(space, page_no, RW_X_LATCH, mtr);
	}
	else
		ut_error;
}

void btr_cur_search_to_nth_level(dict_index_t* index, ulint level, dtuple_t* tuple, ulint latch_mode,
	btr_cur_t* cursor, ulint has_search_latch, mtr_t* mtr)
{
	dict_tree_t*	tree;
	page_cur_t*	page_cursor;
	page_t*		page;
	page_t*		guess;
	rec_t*		node_ptr;
	ulint		page_no;
	ulint		space;
	ulint		up_match;
	ulint		up_bytes;
	ulint		low_match;
	ulint 		low_bytes;
	ulint		height;
	ulint		savepoint;
	ulint		rw_latch;
	ulint		page_mode;
	ulint		insert_planned;
	ulint		buf_mode;
	ulint		estimate;
	ulint		ignore_sec_unique;
	ulint		root_height;

	btr_search_t* info;

	ut_ad(level == 0 || mode == PAGE_CUR_LE);
	ut_ad(dict_tree_check_search_tuple(index->tree, tuple));
	ut_ad(!(index->type & DICT_IBUF) || ibuf_inside());
	ut_ad(dtuple_check_typed(tuple));

	insert_planned = latch_mode & BTR_INSERT;
	estimate = latch_mode & BTR_ESTIMATE;			/*Ԥ������Ķ���*/
	ignore_sec_unique = latch_mode & BTR_IGNORE_SEC_UNIQUE;
	latch_mode = latch_mode & ~(BTR_INSERT | BTR_ESTIMATE | BTR_IGNORE_SEC_UNIQUE);

	ut_ad(!insert_planned || mode == PAGE_CUR_LE);

	cursor->flag = BTR_CUR_BINARY;
	cursor->index = index;

	/*������ӦHASH���в���*/
	info = btr_search_get_info(index);
	guess = info->root_guess;

#ifdef UNIV_SEARCH_PERF_STAT
	info->n_searches++;
#endif

	/*������Ӧhash���ҵ��˶�Ӧ�ļ�¼*/
	if (btr_search_latch.writer == RW_LOCK_NOT_LOCKED
		&& latch_mode <= BTR_MODIFY_LEAF && info->last_hash_succ
		&& !estimate
		&& btr_search_guess_on_hash(index, info, tuple, mode, latch_mode, cursor, has_search_latch, mtr)) {

			ut_ad(cursor->up_match != ULINT_UNDEFINED || mode != PAGE_CUR_GE);
			ut_ad(cursor->up_match != ULINT_UNDEFINED || mode != PAGE_CUR_LE);
			ut_ad(cursor->low_match != ULINT_UNDEFINED || mode != PAGE_CUR_LE);
			btr_cur_n_sea++;

			return;
	}

	btr_cur_n_sea ++;

	if(has_search_latch)
		rw_lock_s_unlock(&btr_search_latch);

	/*���mtr �ı������ݳ���*/
	savepoint = mtr_set_savepoint(mtr);

	tree = index->tree;

	/*��ȡһ��x-latch,�����п��ܻ��������*/
	if(latch_mode == BTR_MODIFY_TREE)
		mtr_x_lock(dict_tree_get_lock(tree), mtr);
	else if(latch_mode == BTR_CONT_MODIFY_TREE) 
		ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(tree), MTR_MEMO_X_LOCK));
	else /*��tree ������ȡһ��s-latch*/
		mtr_s_lock(dict_tree_get_lock(tree), mtr);

	page_cursor = btr_cur_get_page_cur(cursor);
	space = dict_tree_get_space(tree);
	page_no = dict_tree_get_page(tree);

	up_match = 0;
	up_bytes = 0;
	low_match = 0;
	low_bytes = 0;

	height = ULINT_UNDEFINED;
	rw_latch = RW_NO_LATCH;
	buf_mode = BUF_GET;

	/*ȷ��page��¼��ƥ��ģʽ*/
	if (mode == PAGE_CUR_GE)
		page_mode = PAGE_CUR_L;
	else if (mode == PAGE_CUR_G)
		page_mode = PAGE_CUR_LE;
	else if (mode == PAGE_CUR_LE)
		page_mode = PAGE_CUR_LE;
	else {
		ut_ad(mode == PAGE_CUR_L);
		page_mode = PAGE_CUR_L;
	}

	for(;;){
		if(height == 0 && latch_mode <= BTR_MODIFY_LEAF){
			rw_latch = latch_mode;
			/*���Խ�page���뵽ibuffer����*/
			if(insert_planned && ibuf_should_try(index, ignore_sec_unique))
				buf_mode = BUF_GET_IF_IN_POOL;
		}

retry_page_get:
		page = buf_page_get_gen(space, page_no, rw_latch, guess, buf_mode, IB__FILE__, __LINE__, mtr);
		if(page == NULL){
			ut_ad(buf_mode == BUF_GET_IF_IN_POOL);
			ut_ad(insert_planned);
			ut_ad(cursor->thr);

			/*page���ܲ��뵽insert buffer��,��������,֪��page���뵽ibuf��*/
			if(ibuf_should_try(index, ignore_sec_unique) && ibuf_insert(tuple, index, space, page_no, cursor->thr)){
				cursor->flag = BTR_CUR_INSERT_TO_IBUF;
				return ;
			}

			buf_mode = BUF_GET;
			goto retry_page_get;
		}

		ut_ad(0 == ut_dulint_cmp(tree->id, btr_page_get_index_id(page)));
		if(height == ULINT_UNDEFINED){
			height = btr_page_get_level(page, mtr);
			root_height = height;
			cursor->tree_height = root_height + 1;

			if(page != guess)
				info->root_guess = page;
		}

		if(height == 0){
			if(rw_latch == RW_NO_LATCH)
				btr_cur_latch_leaves(tree, page, space, page_no, latch_mode, cursor, mtr);
			/*�ͷ�savepoint���µ�mtr latch*/
			if(latch_mode != BTR_MODIFY_TREE && latch_mode != BTR_CONT_MODIFY_TREE)
				mtr_release_s_latch_at_savepoint(mtr, savepoint, dict_tree_get_lock(tree));

			page_mode = mode;
		}
		/*��ҳ�н��ж��ֲ��Ҷ�Ӧ�ļ�¼,���ֻ����page node ptr��¼*/
		page_cur_search_with_match(page, tuple, page_mode, &up_match, &up_bytes, &low_match, &low_bytes, page_cursor);
		if(estimate)
			btr_cur_add_path_info(cursor, height, root_height);

		if(level == height){ /*�Ѿ��ҵ���Ӧ�Ĳ��ˣ�����Ҫ������Ͳ���*/
			if(level > 0)
				btr_page_get(space, page_no, RW_X_LATCH, mtr);
			break;
		}

		ut_ad(height > 0);

		height --;
		guess = NULL;

		node_ptr = page_cur_get_rec(page_cursor);
		/*��ȡ���ӽڵ��page no*/
		page_no = btr_node_ptr_get_child_page_no(node_ptr);
	}

	if(level == 0){
		cursor->low_match = low_match;
		cursor->low_bytes = low_bytes;
		cursor->up_match = up_match;
		cursor->up_bytes = up_bytes;
		/*��������ӦHASH����*/
		btr_search_info_update(index, cursor);

		ut_ad(cursor->up_match != ULINT_UNDEFINED || mode != PAGE_CUR_GE);
		ut_ad(cursor->up_match != ULINT_UNDEFINED || mode != PAGE_CUR_LE);
		ut_ad(cursor->low_match != ULINT_UNDEFINED || mode != PAGE_CUR_LE);
	}

	if(has_search_latch)
		rw_lock_s_lock(&btr_search_latch);
}

/*��btree cursor��λ��index������Χ�Ŀ�ʼ����ĩβ��from_left = TRUE����ʾ��λ����ǰ��*/
void btr_cur_open_at_index_side(ibool from_left, dict_index_t* index, ulint latch_mode, btr_cur_t* cursor, mtr_t* mtr)
{
	page_cur_t*	page_cursor;
	dict_tree_t*	tree;
	page_t*		page;
	ulint		page_no;
	ulint		space;
	ulint		height;
	ulint		root_height;
	rec_t*		node_ptr;
	ulint		estimate;
	ulint       savepoint;

	estimate = latch_mode & BTR_ESTIMATE;
	latch_mode = latch_mode & ~BTR_ESTIMATE;

	tree = index->tree;

	savepoint = mtr_set_savepoint(mtr);
	if(latch_mode == BTR_MODIFY_TREE)
		mtr_x_lock(dict_tree_get_lock(tree), mtr);
	else
		mtr_s_lock(dict_tree_get_lock(tree), mtr);

	page_cursor = btr_cur_get_page_cur(cursor);
	cursor->index = index;

	space = dict_tree_get_space(tree);
	page_no = dict_tree_get_page(tree);

	height = ULINT_UNDEFINED;
	for(;;){
		page = buf_page_get_gen(space, page_no, RW_NO_LATCH, NULL,
			BUF_GET, IB__FILE__, __LINE__, mtr);

		ut_ad(0 == ut_dulint_cmp(tree->id, btr_page_get_index_id(page)));

		if(height == ULINT_UNDEFINED){
			height = btr_page_get_level(page, mtr);
			root_height = height;
		}

		if(height == 0){
			btr_cur_latch_leaves(tree, page, space, page_no, latch_mode, cursor, mtr);

			if(latch_mode != BTR_MODIFY_TREE && latch_mode != BTR_CONT_MODIFY_TREE)
				mtr_release_s_latch_at_savepoint(mtr, savepoint, dict_tree_get_lock(tree));
		}

		if(from_left) /*��ҳ��һ����¼*/
			page_cur_set_before_first(page, page_cursor);
		else /*��ҳ�����һ����¼��ʼ*/
			page_cur_set_after_last(page, page_cursor);

		if(height == 0){
			if(estimate)
				btr_cur_add_path_info(cursor, height, root_height);
			break;
		}

		ut_ad(height > 0);

		if(from_left)
			page_cur_move_to_next(page_cursor);
		else
			page_cur_move_to_prev(page_cursor);

		if(estimate)
			btr_cur_add_path_info(cursor, height, root_height);

		height --;
		node_ptr = page_cur_get_rec(page_cursor);

		page_no = btr_node_ptr_get_child_page_no(node_ptr);
	}
}

/*��btree index�Ĺ�Ͻ��Χ�������λ��һ��λ��*/
void btr_cur_open_at_rnd_pos(dict_index_t* index, ulint latch_mode, btr_cur_t* cursor, mtr_t* mtr)
{
	page_cur_t*	page_cursor;
	dict_tree_t*	tree;
	page_t*		page;
	ulint		page_no;
	ulint		space;
	ulint		height;
	rec_t*		node_ptr;

	tree = index->tree;
	if(latch_mode == BTR_MODIFY_TREE)
		mtr_x_lock(dict_tree_get_lock(tree), mtr);
	else
		mtr_s_lock(dict_tree_get_lock(tree), mtr);

	page_cursor = btr_cur_get_page_cur(cursor);
	cursor->index = index;

	space = dict_tree_get_space(tree);
	page_no = dict_tree_get_page(tree);

	height = ULINT_UNDEFINED;
	for(;;){
		page = buf_page_get_gen(space, page_no, RW_NO_LATCH, NULL, BUF_GET, IB__FILE__, __LINE__, mtr);
		ut_ad(0 == ut_dulint_cmp(tree->id, btr_page_get_index_id(page)));

		if(height == ULINT_UNDEFINED)
			height = btr_page_get_level(page, mtr);

		if(height == 0)
			btr_cur_latch_leaves(tree, page, space, page_no, latch_mode, cursor, mtr);
		/*�����λһ����¼������page cursorָ����*/
		page_cur_open_on_rnd_user_rec(page, page_cursor);	
		if(height == 0)
			break;

		ut_ad(height > 0);
		height --;

		node_ptr = page_cur_get_rec(page_cursor);
		page_no = btr_node_ptr_get_child_page_no(node_ptr);
	}
}



