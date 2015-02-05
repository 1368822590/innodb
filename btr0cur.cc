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

void btr_cur_search_to_nth_level(dict_index_t* index, ulint level, dtuple_t* tuple, ulint mode, ulint latch_mode,
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
		if(estimate) /*����row��*/
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

static rec_t* btr_cur_insert_if_possible(btr_cur_t* cursor, dtuple_t* tuple, ibool* reorg, mtr_t* mtr)
{
	page_cur_t*	page_cursor;
	page_t*		page;
	rec_t*		rec;

	ut_ad(dtuple_check_typed(tuple));

	*reorg = FALSE;

	page = btr_cur_get_page(cursor);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	page_cursor = btr_cur_get_page_cur(cursor);

	/*��tuple���뵽page��*/
	rec = page_cur_tuple_insert(page_cursor, tuple, mtr);
	if(rec == NULL){
		/*���ܿռ䲻���������򲻶ԣ�����ҳ����,�п��ܲ����¼�Ŀռ��޷���page�Ϸ���(û�п��е�rec�ռ䣬ɾ���ĵ�����¼ < tuple����Ҫ�Ŀռ�)*/
		btr_page_reorganize(page, mtr);
		*reorg = TRUE;
		/*���¶�λpage�α�*/
		page_cur_search(page, tuple, PAGE_CUR_GE, page_cursor);

		rec = page_cur_tuple_insert(page_cursor, tuple, mtr);
	}

	return rec;
}

/*Ϊ����ļ�¼���һ���������ͻع���־*/
UNIV_INLINE ulint btr_cur_ins_lock_and_undo(ulint flags, btr_cur_t* cursor, dtuple_t* entry, que_thr_t* thr, ibool* inherit)
{
	dict_index_t*	index;
	ulint		err;
	rec_t*		rec;
	dulint		roll_ptr;

	rec = btr_cur_get_rec(cursor);
	index = cursor->index;

	/*���һ��������������ж�Ӧ���������ظ�����*/
	err = lock_rec_insert_check_and_lock(flags, rec, index, thr, inherit);
	if(err != DB_SUCCESS) /*�����ʧ��*/
		return err;

	if(index->type & DICT_CLUSTERED && !(index->type & DICT_IBUF)){
		err = trx_undo_report_row_operation(flags, TRX_UNDO_INSERT_OP, thr, index, entry,
							NULL, 0, NULL, &roll_ptr);

		if(err != DB_SUCCESS)
			return err;

		if(!(flags & BTR_KEEP_SYS_FLAG))
			row_upd_index_entry_sys_field(entry, index, DATA_ROLL_PTR, roll_ptr);
	}

	return DB_SUCCESS;
}

/*�������ֹ�ʽ�����¼*/
ulint btr_cur_optimistic_insert(ulint flags, btr_cur_t* cursor, dtuple_t* entry, rec_t** rec, big_rec_t** big_rec, que_thr_t* thr, mtr_t* mtr)
{
	big_rec_t*	big_rec_vec	= NULL;
	dict_index_t*	index;
	page_cur_t*	page_cursor;
	page_t*		page;
	ulint		max_size;
	rec_t*		dummy_rec;
	ulint		level;
	ibool		reorg;
	ibool		inherit;
	ulint		rec_size;
	ulint		data_size;
	ulint		extra_size;
	ulint		type;
	ulint		err;

	*big_rec = NULL;

	page = btr_cur_get_page(cursor);
	index = cursor->index;

	if(!dtuple_check_typed_no_assert(entry))
		fprintf(stderr, "InnoDB: Error in a tuple to insert into table %lu index %s\n",
			index->table_name, index->name);

	if (btr_cur_print_record_ops && thr) {
		printf("Trx with id %lu %lu going to insert to table %s index %s\n",
			ut_dulint_get_high(thr_get_trx(thr)->id),
			ut_dulint_get_low(thr_get_trx(thr)->id),
			index->table_name, index->name);

		dtuple_print(entry);
	}

	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	/*page�������ɵ�����¼�ռ�*/
	max_size = page_get_max_insert_size_after_reorganize(page, 1);
	level = btr_page_get_level(page, mtr); /*���page�����Ĳ�*/

calculate_sizes_again:
	/*���tuple�洢��Ҫ�Ŀռ��С*/
	data_size = dtuple_get_data_size(entry);
	/*���rec header��Ҫ�ĳ���*/
	extra_size = rec_get_converted_extra_size(data_size, dtuple_get_n_fields(entry));
	rec_size = data_size + extra_size;

	/*��¼�������Ĵ洢��Χ������ת��Ϊbig_rec*/
	if((rec_size >= page_get_free_space_of_empty() / 2) || rec_size >= REC_MAX_DATA_SIZE){
		big_rec_vec = dtuple_convert_big_rec(index, entry, NULL, 0);

		/*ת��ʧ��,��¼ʵ��̫��,������̫��Ķ��У�*/
		if(big_rec_vec == NULL)
			return DB_TOO_BIG_RECORD;

		goto calculate_sizes_again;
	}

	type = index->type;

	/*�ۼ���������Ҷ�ӽڵ���Խ������ҷ���,����BTREE���Ŀռ䲻���������ܴ��룬��ת��Ϊbig_rec�Ĳ����ع�*/
	if ((type & DICT_CLUSTERED)
		&& (dict_tree_get_space_reserve(index->tree) + rec_size > max_size)
		&& (page_get_n_recs(page) >= 2)
		&& (0 == level)
		&& (btr_page_get_split_rec_to_right(cursor, &dummy_rec)
		|| btr_page_get_split_rec_to_left(cursor, &dummy_rec))){
			if(big_rec_vec) /*��tupleת����big_rec*/
				dtuple_convert_back_big_rec(index, entry, big_rec_vec);

			return DB_FAIL;
	}

	if (!(((max_size >= rec_size) && (max_size >= BTR_CUR_PAGE_REORGANIZE_LIMIT))
		|| page_get_max_insert_size(page, 1) >= rec_size || page_get_n_recs(page) <= 1)) {
			if(big_rec_vec)
				dtuple_convert_back_big_rec(index, entry, big_rec_vec);

			return DB_FAIL;
	}

	err = btr_cur_ins_lock_and_undo(flags, cursor, entry, thr, &inherit);
	if(err != DB_SUCCESS){ /*����������ɹ����ع�big_rec*/
		if(big_rec_vec)
			dtuple_convert_back_big_rec(index, entry, big_rec_vec);

		return err;
	}

	page_cursor = btr_cur_get_page_cur(cursor);
	reorg = FALSE;

	/*��tuple���뵽page����*/
	*rec = page_cur_insert_rec_low(page_cursor, entry, data_size, NULL, mtr);
	if(*rec == NULL){ /*����ʧ�ܣ�����page����*/
		btr_page_reorganize(page, mtr);

		ut_ad(page_get_max_insert_size(page, 1) == max_size);
		reorg = TRUE;

		page_cur_search(page, entry, PAGE_CUR_LE, page_cursor);
		*rec = page_cur_tuple_insert(page_cursor, entry, mtr);
		if(*rec == NULL){ /*�������ʧ�ܣ���ӡ������Ϣ*/
			char* err_buf = mem_alloc(1000);

			dtuple_sprintf(err_buf, 900, entry);

			fprintf(stderr, "InnoDB: Error: cannot insert tuple %s to index %s of table %s\n"
				"InnoDB: max insert size %lu\n", err_buf, index->name, index->table->name, max_size);

			mem_free(err_buf);
		}

		ut_a(*rec);
	}

	/*����HASH����*/
	if(!reorg && level == 0 && cursor->flag == BTR_CUR_HASH)
		btr_search_update_hash_node_on_insert(cursor);
	else
		btr_search_update_hash_on_insert(cursor);

	/*�̳к�һ��������(GAP��ʽ)*/
	if(!(flags & BTR_NO_LOCKING_FLAG) && inherit)
		lock_update_insert(*rec);

	/*�Ǿۼ���������*/
	if(!(type & DICT_CLUSTERED))
		ibuf_update_free_bits_if_full(cursor->index, page, max_size,
		rec_size + PAGE_DIR_SLOT_SIZE);

	*big_rec = big_rec_vec;

	return DB_SUCCESS;
}

/*�Ա���ִ�м�¼�Ĳ���,���۷�ʽ�Ǳ�ռ䲻������Ҫ�����ռ�*/
ulint btr_cur_pessimistic_insert(ulint flags, btr_cur_t* cursor, dtuple_t* entry, rec_t** rec, big_rec_t** big_rec, que_thr_t* thr, mtr_t* mtr)
{
	dict_index_t*	index = cursor->index;
	big_rec_t*	big_rec_vec	= NULL;
	page_t*		page;
	ulint		err;
	ibool		dummy_inh;
	ibool		success;
	ulint		n_extents = 0;

	*big_rec = NULL;
	page = btr_cur_get_page(cursor);

	ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(btr_cur_get_tree(cursor)), MTR_MEMO_X_LOCK));
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	cursor->flag = BTR_CUR_BINARY;

	err = btr_cur_optimistic_insert(flags, cursor, entry, rec, big_rec, thr, mtr);
	if(err != DB_FAIL) /*�ֹ�����ʽ����ɹ���ֱ�ӷ���*/
		return err;

	err = btr_cur_ins_lock_and_undo(flags, cursor, entry, thr, &dummy_inh);
	if(err != DB_SUCCESS)
		return err;

	if(!(flags && BTR_NO_UNDO_LOG_FLAG)){
		n_extents = cursor->tree_height / 16 + 3;

		/*����file space��ռ�*/
		success = fsp_reserve_free_extents(index->space, n_extents, FSP_NORMAL, mtr);
		if(!success){ /*������ռ䷶Χ*/
			err =  DB_OUT_OF_FILE_SPACE;
			return err;
		}
	}

	/*����ҳ�޷��洢entry��¼,��Ϊ���¼�洢*/
	if(rec_get_converted_size(entry) >= page_get_free_space_of_empty() / 2 || rec_get_converted_size(entry) >= REC_MAX_DATA_SIZE){
		 big_rec_vec = dtuple_convert_big_rec(index, entry, NULL, 0);
		 if(big_rec_vec == NULL)
			 return DB_TOO_BIG_RECORD;
	}

	if (dict_tree_get_page(index->tree) == buf_frame_get_page_no(page))
			*rec = btr_root_raise_and_insert(cursor, entry, mtr);
	else
		*rec = btr_page_split_and_insert(cursor, entry, mtr);
	
	btr_cur_position(index, page_rec_get_prev(*rec), cursor);

	/*��������ӦHASH*/
	btr_search_update_hash_on_insert(cursor);

	/*�²����ж�gap�����ļ̳�*/
	if(!(flags & BTR_NO_LOCKING_FLAG))
		lock_update_insert(*rec);

	err = DB_SUCCESS;

	if(n_extents > 0)
		fil_space_release_free_extents(index->space, n_extents);

	*big_rec = big_rec_vec;

	return err;
}

/*���ۼ������޸ļ�¼����������������*/
UNIV_INLINE ulint btr_cur_upd_lock_and_undo(ulint flags, btr_cur_t* cursor, upd_t* update, ulint cmpl_info, que_thr_t* thr, dulint roll_ptr)
{
	dict_index_t*	index;
	rec_t*			rec;
	ulint			err;

	ut_ad(cursor && update && thr && roll_ptr);
	ut_ad(cursor->index->type & DICT_CLUSTERED);

	rec = btr_cur_get_rec(cursor);
	index = cursor->index;

	err =DB_SUCCESS;

	if(!(flags & BTR_NO_LOCKING_FLAG)){
		/*����޸ļ�¼ʱ����¼�ۼ��������Ƿ�������������ʽ������ʽ����*/
		err = lock_clust_rec_modify_check_and_lock(flags, rec, index, thr);
		if(err != DB_SUCCESS)
			return err;
	}

	err = trx_undo_report_row_operation(flags, TRX_UNDO_MODIFY_OP, thr, index, NULL, update,
		cmpl_info, rec, roll_ptr);

	return err;
}

/*Ϊupdate record����һ��redo log*/
UNIV_INLINE void btr_cur_update_in_place_log(ulint flags, rec_t* rec, dict_index_t* index, 
	upd_t* update, trx_t* trx, dulint roll_ptr, mtr_t* mtr)
{
	byte* log_ptr;

	log_ptr = mlog_open(mtr, 30 + MLOG_BUF_MARGIN);
	log_ptr = mlog_write_initial_log_record_fast(rec, MLOG_REC_UPDATE_IN_PLACE, log_ptr, mtr);
	
	mach_write_to_1(log_ptr, flags);
	log_ptr++;
	
	log_ptr = row_upd_write_sys_vals_to_log(index, trx, roll_ptr, log_ptr, mtr);

	mach_write_to_2(log_ptr, rec - buf_frame_align(rec));
	log_ptr += 2;

	row_upd_index_write_log(update, log_ptr, mtr);
}

/*����һ���޸ļ�¼��redo log����������*/
byte* btr_cur_parse_update_in_place(byte* ptr, byte* end_ptr, page_t* page, mtr_t* mtr)
{
	ulint	flags;
	rec_t*	rec;
	upd_t*	update;
	ulint	pos;
	dulint	trx_id;
	dulint	roll_ptr;
	ulint	rec_offset;
	mem_heap_t* heap;

	if(end_ptr < ptr + 1)
		return NULL;

	flags = mach_read_from_1(ptr);
	ptr++;

	ptr = row_upd_parse_sys_vals(ptr, end_ptr, &pos, &trx_id, &roll_ptr);

	if(ptr == NULL)
		return NULL;

	if(end_ptr < ptr + 2)
		return NULL;

	/*��redo log�ж�ȡ�޸ļ�¼��ƫ��*/
	rec_offset = mach_read_from_2(ptr);
	ptr += 2;

	/*��redo log�ж�ȡ�޸ĵ�����*/
	heap = mem_heap_create(256);
	ptr = row_upd_index_parse(ptr, end_ptr, heap, &update);
	if(ptr == NULL){
		mem_heap_free(heap);
		return NULL;
	}

	if(page == NULL){
		mem_heap_free(heap);
		return NULL;
	}

	rec = page + rec_offset;
	if(!(flags & BTR_KEEP_SYS_FLAG))
		row_upd_rec_sys_fields_in_recovery(rec, pos, trx_id, roll_ptr);

	/*���м�¼�޸�*/
	row_upd_rec_in_place(heap, update);

	mem_heap_free(heap);

	return ptr;
}
/*ͨ�����������޸Ķ�Ӧ�ļ�¼*/
ulint btr_cur_update_sec_rec_in_place(btr_cur_t* cursor, upd_t* update, que_thr_t* thr, mtr_t* mtr)
{
	dict_index_t*	index = cursor->index;
	dict_index_t*	clust_index;
	ulint		err;
	rec_t*		rec;
	dulint		roll_ptr = ut_dulint_zero;
	trx_t*		trx	= thr_get_trx(thr);

	ut_ad(0 == (index->type & DICT_CLUSTERED));

	rec = btr_cur_get_rec(cursor);

	if(btr_cur_print_record_ops && thr){
		printf("Trx with id %lu %lu going to update table %s index %s\n",
			ut_dulint_get_high(thr_get_trx(thr)->id),ut_dulint_get_low(thr_get_trx(thr)->id), index->table_name, index->name);

		rec_print(rec);
	}

	/*ͨ����������������ϵ���*/
	err = lock_sec_rec_modify_check_and_lock(0, rec, index, thr);
	if(err != DB_SUCCESS)
		return err;

	/*ɾ������ӦHASH��Ӧ��ϵ*/
	btr_search_update_hash_on_delete(cursor);
	/*�Լ�¼�����޸�*/
	row_upd_rec_in_place(rec, update);

	/*ͨ�����������ҵ��ۼ�����*/
	clust_index = dict_table_get_first_index(index->table);
	/*ͨ���ۼ��������һ���޸ļ�¼��redo log*/
	btr_cur_update_in_place_log(BTR_KEEP_SYS_FLAG, rec, clust_index, update, trx, roll_ptr, mtr);
	
	return DB_SUCCESS;
}

ulint btr_cur_update_in_place(ulint flags, btr_cur_t* cursor, upd_t* update, ulint cmpl_info, que_thr_t* thr, mtr_t* mtr)
{
	dict_index_t*	index;
	buf_block_t*	block;
	ulint		err;
	rec_t*		rec;
	dulint		roll_ptr;
	trx_t*		trx;
	ibool		was_delete_marked;

	ut_ad(cursor->index->type & DICT_CLUSTERED);

	/*��ö�Ӧ�ļ�¼������������*/
	rec = btr_cur_get_rec(cursor);
	index = cursor->index;
	trx = thr_get_trx(thr);

	if(btr_cur_print_record_ops && thr){
		printf("Trx with id %lu %lu going to update table %s index %s\n",
			ut_dulint_get_high(thr_get_trx(thr)->id),
			ut_dulint_get_low(thr_get_trx(thr)->id),
			index->table_name, index->name);

		rec_print(rec);
	}

	/*������������*/
	err = btr_cur_upd_lock_and_undo(flags, cursor, update, cmpl_info, thr, &roll_ptr);
	if(err != DB_SUCCESS)
		return err;

	block = buf_block_align(rec);
	if(block->is_hashed){
		if (row_upd_changes_ord_field_binary(NULL, index, update)) 
			btr_search_update_hash_on_delete(cursor);

		rw_lock_x_lock(&btr_search_latch);
	}

	if(!(flags & BTR_KEEP_SYS_FLAG))
		row_upd_rec_sys_fields(rec, index, trx, roll_ptr);

	was_delete_marked = rec_get_deleted_flag(rec);
	/*���м�¼����*/
	row_upd_rec_in_place(rec, update);

	if(block->is_hashed)
		rw_lock_x_unlock(&btr_search_latch);

	btr_cur_update_in_place_log(flags, rec, index, update, trx, roll_ptr, mtr);

	/*��ɾ��״̬���δɾ��״̬*/
	if(was_delete_marked && !rec_get_deleted_flag(rec))
		btr_cur_unmark_extern_fields(rec, mtr);

	return DB_SUCCESS;
}

/*�ֹ۷�ʽ����һ����¼���ȳ�����ԭ���ļ�¼λ��ֱ�Ӹ��£�������ܾͻὫԭ���ļ�¼ɾ���������µ��������һ����¼���뵽��Ӧ��page��*/
ulint btr_cur_optimistic_update(ulint flags, btr_cur_t* cursor, upd_t* update, ulint cmpl_info, que_thr_t* thr, mtr_t* mtr)
{
	dict_index_t*	index;
	page_cur_t*	page_cursor;
	ulint		err;
	page_t*		page;
	rec_t*		rec;
	ulint		max_size;
	ulint		new_rec_size;
	ulint		old_rec_size;
	dtuple_t*	new_entry;
	dulint		roll_ptr;
	trx_t*		trx;
	mem_heap_t*	heap;
	ibool		reorganized	= FALSE;
	ulint		i;

	ut_ad((cursor->index)->type & DICT_CLUSTERED);

	page = btr_cur_get_page(cursor);
	rec = btr_cur_get_rec(cursor);
	index = cursor->index;

	if (btr_cur_print_record_ops && thr) {
		printf("Trx with id %lu %lu going to update table %s index %s\n",
			ut_dulint_get_high(thr_get_trx(thr)->id),
			ut_dulint_get_low(thr_get_trx(thr)->id),
			index->table_name, index->name);

		rec_print(rec);
	}

	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_BUF_FIX));

	/*���ܸı��еĴ�С��ֱ����ԭ���ļ�¼λ��������*/
	if(!row_upd_changes_field_size(rec, index, update))
		return btr_cur_update_in_place(flags, cursor, update, cmpl_info, thr, mtr);

	/*�ж����Ƿ����*/
	for(i = 0; i < upd_get_n_fields(update); i++){
		if(upd_get_nth_field(update, i)->extern_storage)
			return DB_OVERFLOW;
	}

	/*�����������ֽڱ�ʾ����*/
	if(rec_contains_externally_stored_field(btr_cur_get_rec(cursor)))
		return DB_OVERFLOW;

	page_cursor = btr_cur_get_page_cur(cursor);
	heap = mem_heap_create(1024);

	/*���ڴ��й���һ���¼�¼�Ĵ洢�����¼*/
	new_entry = row_rec_to_index_entry(ROW_COPY_DATA, index, rec, heap);
	row_upd_clust_index_replace_new_col_vals(new_entry, update);

	old_rec_size = rec_get_size(rec);
	new_rec_size = rec_get_converted_size(new_entry);

	/*���µļ�¼��С�Ѿ�����ҳ�ܴ洢�Ĵ�С*/
	if(new_rec_size >= page_get_free_space_of_empty() / 2){
		mem_heap_free(heap);
		return DB_OVERFLOW;
	}
	/*����ҳ�������Դ洢�����ռ�*/
	max_size = old_rec_size + page_get_max_insert_size_after_reorganize(page, 1);
	if(page_get_data_size(page) - old_rec_size + new_rec_size < BTR_CUR_PAGE_COMPRESS_LIMIT){
		/*��¼���º�ᴥ��btree�ĺϲ���䣬����ֱ�Ӹ���*/
		mem_heap_free(heap);
		return DB_UNDERFLOW;
	}

	/*�ܴ洢�Ŀռ�С�ںϲ��Ŀռ���ֵ���߲��ܴ����µļ�¼,����page�Ŀռ䲻ֻ1����¼*/
	if(!((max_size >= BTR_CUR_PAGE_REORGANIZE_LIMIT && max_size >= new_rec_size) || page_get_n_recs(page) <= 1)){
		mem_heap_free(heap);
		return DB_OVERFLOW;
	}

	/*���ܶԼ�¼��������*/
	err = btr_cur_upd_lock_and_undo(flags, cursor, update, cmpl_info, thr, &roll_ptr);
	if(err != DB_SUCCESS){
		mem_heap_free(heap);
		return err;
	}

	/*����¼������ȫ��ת�Ƶ�infimum,Ӧ������ʱ�洢������ط�*/
	lock_rec_store_on_page_infimum(rec);

	btr_search_update_hash_on_delete(cursor);
	/*��page�α��Ӧ�ļ�¼ɾ��*/
	page_cur_delete_rec(page_cursor, mtr);
	/*�α��Ƶ�ǰ��һ����¼��*/
	page_cur_move_to_prev(page_cursor);

	trx = thr_get_trx(thr);

	if(!(flags & BTR_KEEP_SYS_FLAG)){
		row_upd_index_entry_sys_field(new_entry, index, DATA_ROLL_PTR, roll_ptr);
		row_upd_index_entry_sys_field(new_entry, index, DATA_TRX_ID, trx->id);
	}
	/*���¹�����tuple��¼���뵽ҳ��*/
	rec = btr_cur_insert_if_possible(cursor, new_entry, &reorganized, mtr);
	ut_a(rec);

	if(!rec_get_deleted_flag(rec))
		btr_cur_unmark_extern_fields(rec, mtr);

	/*��������infimum�ϵ����ָ����²���ļ�¼��*/
	lock_rec_restore_from_page_infimum(rec, page);

	page_cur_move_to_next(page_cursor);

	mem_heap_free(heap);

	return DB_SUCCESS;
}

static void btr_cur_pess_upd_restore_supremum(rec_t* rec, mtr_t* mtr)
{
	page_t*	page;
	page_t*	prev_page;
	ulint	space;
	ulint	prev_page_no;

	page = buf_frame_align(rec);
	if(page_rec_get_next(page_get_infimum_rec(page) != rec))
		return;

	/*���recǰһҳ��page����*/
	space = buf_frame_get_space_id(page);
	prev_page_no = btr_page_get_prev(page, mtr);

	ut_ad(prev_page_no != FIL_NULL);
	prev_page = buf_page_get_with_no_latch(space, prev_page_no, mtr);

	/*ȷ���Ѿ�ӵ��x-latch*/
	ut_ad(mtr_memo_contains(mtr, buf_block_align(prev_page), MTR_MEMO_PAGE_X_FIX));
	/*ǰһ��page��supremum�̳�rec�ϵ���*/
	lock_rec_reset_and_inherit_gap_locks(page_get_supremum_rec(prev_page), rec);
}

/*��update�е��޸������ݸ��µ�tuple�߼���¼����*/
static void btr_cur_copy_new_col_vals(dtuple_t* entry, upd_t* update, mem_heap_t* heap)
{
	upd_field_t*	upd_field;
	dfield_t*	dfield;
	dfield_t*	new_val;
	ulint		field_no;
	byte*		data;
	ulint		i;

	dtuple_set_info_bits(entry, update->info_bits);

	/*��update�����и��ĵ������������滻��dtuple��Ӧ������*/
	for(i = 0; i < upd_get_n_fields(update); i ++){
		upd_field = upd_get_nth_field(update, i);
		field_no = upd_field->field_no;
		dfield = dtuple_get_nth_field(entry, field_no);

		new_val = &(upd_field->new_val);
		if(new_val->len = UNIV_SQL_NULL)
			data = NULL;
		else{
			data = mem_heap_alloc(heap, new_val->len);
			ut_memcpy(data, new_val->data, new_val->len);
		}

		/*�˴�Ϊ0����*/
		dfield_set_data(dfield, data, new_val->len);
	}
}

/*����ʽ���¼�¼*/
ulint btr_cur_pessimistic_update(ulint flags, btr_cur_t* cursor, big_rec_t** big_rec, upd_t* update, ulint cmpl_info, que_thr_t* thr, mtr_t* mtr)
{
	big_rec_t*	big_rec_vec	= NULL;
	big_rec_t*	dummy_big_rec;
	dict_index_t*	index;
	page_t*		page;
	dict_tree_t*	tree;
	rec_t*		rec;
	page_cur_t*	page_cursor;
	dtuple_t*	new_entry;
	mem_heap_t*	heap;
	ulint		err;
	ulint		optim_err;
	ibool		dummy_reorganized;
	dulint		roll_ptr;
	trx_t*		trx;
	ibool		was_first;
	ibool		success;
	ulint		n_extents	= 0;
	ulint*		ext_vect;
	ulint		n_ext_vect;
	ulint		reserve_flag;

	*big_rec = NULL;

	page = btr_cur_get_page(cursor);
	rec = btr_cur_get_rec(cursor);
	index = cursor->index;
	tree = index->tree;

	ut_ad(index->type & DICT_CLUSTERED);
	ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(tree), MTR_MEMO_X_LOCK));
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	/*�ȳ����ֹ�ʽ�ĸ���*/
	optim_err = btr_cur_optimistic_update(flags, cursor, update, cmpl_info, thr, mtr);
	if(optim_err != DB_UNDERFLOW && optim_err != DB_OVERFLOW)
		return optim_err;

	err = btr_cur_upd_lock_and_undo(flags, cursor, update, cmpl_info, thr, &roll_ptr);
	if(err != DB_SUCCESS)
		return err;

	if(optim_err == DB_OVERFLOW){
		n_extents = cursor->tree_height / 16 + 3;
		if(flags & BTR_NO_UNDO_LOG_FLAG)
			reserve_flag = FSP_CLEANING;
		else
			reserve_flag = FSP_NORMAL;

		/*���������ռ�*/
		success = fsp_reserve_free_extents(cursor->index->space, n_extents, reserve_flag, mtr);
		if(!success)
			err = DB_OUT_OF_FILE_SPACE;
		return err;
	}
	
	heap = mem_heap_create(1024);
	trx = thr_get_trx(thr);
	new_entry = row_rec_to_index_entry(ROW_COPY_DATA, index, rec, heap);

	/*������������ͬ����new_entry��*/
	btr_cur_copy_new_col_vals(new_entry, update, heap);

	/*��¼��ת��infimum����ʱ����*/
	lock_rec_store_on_page_infimum(rec);

	btr_search_update_hash_on_delete(cursor);

	if(flags & BTR_NO_UNDO_LOG_FLAG){
		ut_a(big_rec_vec == NULL);
		btr_rec_free_updated_extern_fields(index, rec, update, TRUE, mtr);
	}

	ext_vect = mem_heap_alloc(heap, Size(ulint) * rec_get_n_fields(rec));
	n_ext_vect = btr_push_update_extern_fields(ext_vect, rec, update);

	/*ɾ���α괦�ļ�¼*/
	page_cur_delete_rec(page_cursor, mtr);
	page_cur_move_to_prev(page_cursor);

	/*�Ǹ����¼,����big_recת��*/
	if((rec_get_converted_size(new_entry) >= page_get_free_space_of_empty() / 2)
		|| (rec_get_converted_size(new_entry) >= REC_MAX_DATA_SIZE)) {
			big_rec_vec = dtuple_convert_big_rec(index, new_entry, ext_vect, n_ext_vect);
			if(big_rec_vec == NULL){
				mem_heap_free(heap);
				goto return_after_reservations;
			}
	}

	/*��tuple���뵽page��*/
	rec = btr_cur_insert_if_possible(cursor, new_entry, &dummy_reorganized, mtr);
	if(rec != NULL){
		/*����ɹ������Ĵ���infimum�е�����ת�ƻ���*/
		lock_rec_restore_from_page_infimum(rec, page);
		rec_set_field_extern_bits(rec, ext_vect, n_ext_vect, mtr);
		
		if(!rec_get_deleted_flag(rec))
			btr_cur_unmark_extern_fields(rec, mtr);

		/*TODO:??*/
		btr_cur_compress_if_useful(cursor, mtr);

		err = DB_SUCCESS;
		mem_heap_free(heap);

		goto return_after_reservations;
	}

	/*�ж��α��ǲ���ָ���ʼ�ļ�¼infimum*/
	if(page_cur_is_before_first(page_cursor))
		was_first = TRUE;
	else
		was_first = FALSE;

	/*�������ֹ�ʽ����tuple,��������п����ճ�*/
	err = btr_cur_pessimistic_insert(BTR_NO_UNDO_LOG_FLAG
		| BTR_NO_LOCKING_FLAG
		| BTR_KEEP_SYS_FLAG, cursor, new_entry, &rec, &dummy_big_rec, NULL, mtr);
	ut_a(rec);
	ut_a(err == DB_SUCCESS);
	ut_a(dummy_big_rec == NULL);

	rec_set_field_extern_bits(rec, ext_vect, n_ext_vect, mtr);

	if(!rec_get_deleted_flag(rec))
		btr_cur_unmark_extern_fields(rec, mtr);

	lock_rec_restore_from_page_infimum(rec, page);

	/*���û��infimum����ô��������ȫ��ת�Ƶ���ǰһҳ��supremum��,�������ڷ��ѵ�ʱ����ܲ������*/
	if(!was_first)
		btr_cur_pess_upd_restore_supremum(rec, mtr);

	mem_heap_free(heap);

return_after_reservations:
	if(n_extents > 0) /*��¼ֻ�ǲ�����ibuffer�У���û��ˢ�����̣����Ի��ȱ��Ϊδռ��״̬*/
		fil_space_release_free_extents(cursor->index->space, n_extents);

	*big_rec = big_rec_vec;

	return err;
}

/*ͨ���ۼ�����ɾ����¼��redo log*/
UNIV_INLINE void btr_cur_del_mark_set_clust_rec_log(ulint flags, rec_t* rec, dict_index_t* index, ibool val, 
					trx_t* trx, dulint roll_ptr, mtr_t* mtr)
{
	byte* log_ptr;
	log_ptr = mlog_open(mtr, 30);
	/*����һ��CLUST DELETE MARK��redo log*/
	log_ptr = mlog_write_initial_log_record_fast(rec, MLOG_REC_CLUST_DELETE_MARK, log_ptr, mtr); 
	mach_write_to_1(log_ptr, flags);
	log_ptr ++;
	mach_write_to_1(log_ptr, val);
	log_ptr ++;

	/*�������ID�ͻع�λ��д�뵽redo log��*/
	log_ptr = row_upd_write_sys_vals_to_log(index, trx, roll_ptr, log_ptr, mtr);
	mach_write_to_2(log_ptr, rec - buf_frame_align(rec));
	log_ptr += 2;

	mlog_close(mtr, log_ptr);
}

/*redo ���̶�CLUST DELETE MARK��־������*/
byte* btr_cur_parse_del_mark_set_clust_rec(byte* ptr, byte* end_ptr, page_t* page)
{
	ulint	flags;
	ibool	val;
	ulint	pos;
	dulint	trx_id;
	dulint	roll_ptr;
	ulint	offset;
	rec_t*	rec;

	if(end_ptr < ptr + 2)
		return NULL;

	flags = mach_read_from_1(ptr);
	ptr++;
	val = mach_read_from_1(ptr);
	ptr++;

	ptr = row_upd_parse_sys_vals(ptr, end_ptr, &pos, &trx_id, &roll_ptr);
	if(ptr == NULL)
		return NULL;

	if(end_ptr < ptr + 2)
		return NULL;

	/*��ü�¼��ƫ��,��ͨ�����ƫ�Ƶõ���¼��λ��*/
	offset = mach_read_from_2(ptr);
	ptr += 2;

	if(page != NULL){
		rec = page + offset;
		if(!(flags & BTR_KEEP_SYS_FLAG))
			row_upd_rec_sys_fields_in_recovery(rec, pos, trx_id, roll_ptr);

		/*����¼����Ϊɾ��״̬*/
		rec_set_deleted_flag(rec, val);
	}

	return ptr;
}

ulint btr_cur_del_mark_set_clust_rec(ulint flags, btr_cur_t* cursor, ibool val, que_thr_t* thr, mtr_t* mtr)
{
	dict_index_t*	index;
	buf_block_t*	block;
	dulint		roll_ptr;
	ulint		err;
	rec_t*		rec;
	trx_t*		trx;

	rec = btr_cur_get_rec(cursor);
	index = cursor->index;

	if(btr_cur_print_record_ops && thr){
		printf("Trx with id %lu %lu going to del mark table %s index %s\n",
			ut_dulint_get_high(thr_get_trx(thr)->id),
			ut_dulint_get_low(thr_get_trx(thr)->id),
			index->table_name, index->name);

		rec_print(rec);
	}

	ut_ad(index->type & DICT_CLUSTERED);
	ut_ad(rec_get_deleted_flag(rec) == FALSE);

	/*����ͨ���ۼ��������rec��¼�е���ִ��Ȩ���������ʿ����ת������ʾ��*/
	err = lock_clust_rec_modify_check_and_lock(flags, rec, index, thr);
	if(err != DB_SUCCESS)
		return err;

	/*���������Ȩ��,���undo log��־*/
	err = trx_undo_report_row_operation(flags, TRX_UNDO_MODIFY_OP, thr,
		index, NULL, NULL, 0, rec, &roll_ptr);
	if(err != DB_SUCCESS)
		return err;

	block = buf_block_align(rec);

	if(block->is_hashed)
		rw_lock_x_lock(&btr_search_latch);

	/*�Լ�¼��ɾ����ʶ*/
	rec_set_deleted_flag(rec, val);

	trx = thr_get_trx(thr);
	if(!(flags & BTR_KEEP_SYS_FLAG)){
		row_upd_rec_sys_fields(&btr_search_latch);
	}

	if(block->is_hashed)
		rw_lock_x_unlock(&btr_search_latch);

	/*��¼redo log��־*/
	btr_cur_del_mark_set_clust_rec_log(flags, rec, index, val, trx, roll_ptr, mtr);

	return DB_SUCCESS;
}

/*д��ͨ����������ɾ����¼��redo log*/
UNIV_INLINE void btr_cur_del_mark_set_sec_rec_log(rec_t* rec, ibool val, mtr_t* mtr)
{
	byte* log_ptr;

	log_ptr = mlog_open(mtr, 30);
	/*����һ��SEC DELETE MARKɾ����¼��redo log*/
	log_ptr =  mlog_write_initial_log_record_fast(rec, MLOG_REC_SEC_DELETE_MARK, log_ptr, mtr);
	mach_write_to_1(log_ptr, val);
	log_ptr ++;

	mach_write_to_2(log_ptr, rec - buf_frame_align(rec));
	log_ptr += 2;

	mlog_close(mtr, log_ptr);
}

/*����SEC DELETE MARK��־*/
byte* btr_cur_parse_del_mark_set_sec_rec(byte* ptr, byte* end_ptr, page_t* page)
{
	ibool	val;
	ulint	offset;
	rec_t*	rec;

	if(end_ptr < ptr + 3)
		return NULL;

	val = mach_read_from_1(ptr);
	ptr ++;

	offset = mach_read_from_2(ptr);
	ptr += 2;

	ut_a(offset <= UNIV_PAGE_SIZE);
	if(page){
		rec = page + offset;
		rec_set_deleted_flag(rec, val);
	}

	return ptr;
}

/*ͨ����������ɾ����¼*/
ulint btr_cur_del_mark_set_sec_rec(ulint flags, btr_cur_t* cursor, ibool val, que_thr_t* thr, mtr_t* mtr)
{
	buf_block_t*	block;
	rec_t*		rec;
	ulint		err;

	rec = btr_cur_get_rec(cursor);

	if (btr_cur_print_record_ops && thr) {
		printf("Trx with id %lu %lu going to del mark table %s index %s\n",
			ut_dulint_get_high(thr_get_trx(thr)->id),
			ut_dulint_get_low(thr_get_trx(thr)->id),
			cursor->index->table_name, cursor->index->name);
		rec_print(rec);
	}

	err = lock_sec_rec_modify_check_and_lock(flags, rec, cursor->index, thr);
	if(err != DB_SUCCESS)
		return err;

	block = buf_block_align(rec);

	if(block->is_hashed)
		rw_lock_x_lock(&btr_search_latch);

	rec_set_deleted_flag(rec, val);

	if(block->is_hashed)
		rw_lock_x_unlock(&btr_search_latch);

	btr_cur_del_mark_set_sec_rec_log(rec, val, mtr);

	return DB_SUCCESS;
}

/*ֱ�Ӵ�IBUF��ɾ��*/
void btr_cur_del_unmark_for_ibuf(rec_t* rec, mtr_t* mtr)
{
	rec_set_deleted_flag(rec, FALSE);
	btr_cur_del_mark_set_sec_rec_log(rec, FALSE, mtr);
}

/*����ѹ���ϲ�һ��btree�ϵ�Ҷ�ӽڵ�page*/
void btr_cur_compress(btr_cur_t* cursor, mtr_t* mtr)
{
	ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(btr_cur_get_tree(cursor)), MTR_MEMO_X_LOCK));
	ut_ad(mtr_memo_contains(mtr, buf_block_align(btr_cur_get_page(cursor)), MTR_MEMO_PAGE_X_FIX));
	ut_ad(btr_page_get_level(btr_cur_get_page(cursor), mtr) == 0);

	btr_compress(cursor, mtr);
}

/*�ж��Ƿ���Խ���page compress,����ܣ�����btree page compress*/
ibool btr_cur_compress_if_useful(btr_cur_t* cursor, mtr_t* mtr)
{
	ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(btr_cur_get_tree(cursor)), MTR_MEMO_X_LOCK));
	ut_ad(mtr_memo_contains(mtr, buf_block_align( btr_cur_get_page(cursor)), MTR_MEMO_PAGE_X_FIX));
	/*������������ж��Ƿ���Ҫ��cursorָ���Ҷ�ӽڵ�*/
	if(btr_cur_compress_recommendation(cursor, mtr)){
		btr_compress(cursor, mtr);
		return TRUE;
	}

	return FALSE;
}

/*�ֹ�ʽɾ�������漰IO����*/
ibool btr_cur_optimistic_delete(btr_cur_t* cursor, mtr_t* mtr)
{
	page_t*	page;
	ulint	max_ins_size;

	ut_ad(mtr_memo_contains(mtr, buf_block_align(btr_cur_get_page(cursor)), MTR_MEMO_PAGE_X_FIX));

	page = btr_cur_get_page(cursor);
	ut_ad(btr_page_get_level(page, mtr) == 0);

	/*���һ���б����ڶ��ҳ�д洢�Ļ�����ʱ����ɾ������Ϊ�漰����ҳ�ϲ��ᴥ��IO����*/
	if(rec_contains_externally_stored_field(btr_cur_get_rec(cursor)))
		return FALSE;

	/*ɾ��cursor���ᴥ��page��compress*/
	if(btr_cur_can_delete_without_compress(cursor, mtr)){
		/*��¼ɾ����������ת�Ƶ�����һ����*/
		lock_update_delete(btr_cur_get_rec(cursor));
		btr_search_update_hash_on_delete(cursor);

		max_ins_size = page_get_max_insert_size_after_reorganize(page, 1);
		/*��¼ɾ��*/
		page_cur_delete_rec(btr_cur_get_page_cur(cursor), mtr);
		/*����ibuf��Ӧ��page״̬*/
		ibuf_update_free_bits_low(cursor->index, page, max_ins_size, mtr);
		
		return TRUE;
	}

	return FALSE;
}

/*����ʽɾ��,�漰��ռ�ĸı�*/
ibool btr_cur_pessimistic_delete(ulint* err, ibool has_reserved_extents, btr_cur_t* cursor, ibool in_roolback, mtr_t* mtr)
{
	page_t*		page;
	dict_tree_t*	tree;
	rec_t*		rec;
	dtuple_t*	node_ptr;
	ulint		n_extents	= 0;
	ibool		success;
	ibool		ret		= FALSE;
	mem_heap_t*	heap;

	page = btr_cur_get_page(cursor);
	tree = btr_cur_get_tree(cursor);

	ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(tree), MTR_MEMO_X_LOCK));
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	if(!has_reserved_extents){
		n_extents = cursor->tree_height / 32 + 1;
		success = fsp_reserve_free_extents(cursor->index->space, n_extents, FSP_CLEANING, mtr);
		if(!success){
			*err = DB_OUT_OF_FILE_SPACE;
			return FALSE;
		}
	}

	/*�Ƿ��п�ҳ�洢��ҳ*/
	btr_rec_free_externally_stored_fields(cursor->index, btr_cur_get_rec(cursor), in_roolback, mtr);

	/*page��ֻ��1����¼������cursor����ָ���ҳ����cursor��ǰָ���page,�п�������̫��ֿ��洢ռ�õ�ҳ,page���з���*/
	if(page_get_n_recs(page) < 2 && dict_tree_get_page(btr_cur_get_tree(cursor)) != buf_frame_get_page_no(page)){
		btr_discard_page(cursor, mtr);
		*err = DB_SUCCESS;
		ret = TRUE;

		goto return_after_reservations;
	}

	rec = btr_cur_get_rec(cursor);
	/*����ת�Ƶ�rec����һ����¼*/
	lock_update_delete(rec);

	/*page���Ǵ���Ҷ�ӽڵ��ϣ����Ҵ��ڵ�һ����¼��*/
	if(btr_page_get_level(page, mtr) > 0 && page_rec_get_next(page_get_infimum_rec(page)) == rec){
		if(btr_page_get_prev(page, mtr) == FIL_NULL) /*ǰ��û���ֵ�ҳ����ô��һ����¼����Ϊmin rec*/
			btr_set_min_rec_mark(page_rec_get_next(rec), mtr);
		else{
			/*�Ƚ�page��node ptr�Ӹ��׽ڵ���ɾ��*/
			btr_node_ptr_delete(tree, page, mtr);

			heap = mem_heap_create(256);
			/*��rec����һ����¼��key��Ϊnode ptr����µ�ֵ���뵽���׽ڵ���*/
			node_ptr = dict_tree_build_node_ptr(tree, page_rec_get_next(rec), buf_frame_get_page_no(page), heap, btr_page_get_level(page, mtr));
			btr_insert_on_non_leaf_level(tree, btr_page_get_level(page, mtr) + 1, node_ptr, mtr);

			mem_heap_free(heap);
		}
	}

	btr_search_update_hash_on_delete(cursor);
	/*ɾ����¼*/
	page_cur_delete_rec(btr_cur_get_page_cur(cursor), mtr);

	ut_ad(btr_check_node_ptr(tree, page, mtr));
	*err = DB_SUCCESS;

return_after_reservations:
	if(!ret) /*���кϲ��ж�,�����������ͻᴥ���ϲ�*/
		ret = btr_cur_compress_if_useful(cursor, mtr);

	if(n_extents > 0)
		fil_space_release_free_extents(cursor->index->space, n_extents);

	return ret;
}

static void btr_cur_add_path_info(btr_cur_t* cursor, ulint height, ulint root_height)
{
	btr_path_t*	slot;
	rec_t*		rec;

	ut_a(cursor->path_arr);

	if(root_height >= BTR_PATH_ARRAY_N_SLOTS - 1){ /*root �Ĳ��*/
		slot = cursor->path_arr;
		slot->nth_rec = ULINT_UNDEFINED;
		return ;
	}

	if(height == 0){  
		slot = cursor->path_arr + root_height + 1;
		slot->nth_rec = ULINT_UNDEFINED;
	}

	rec = btr_cur_get_rec(cursor);
	slot = cursor->path_arr + (root_height - height);

	slot->nth_rec = page_rec_get_n_recs_before(rec);
	slot->n_recs = page_get_n_recs(buf_frame_align(rec));
}

/*����tuple1 tuple2֮��ļ�¼������ֻ�ǽ���ֵ�����Ǿ�ȷֵ*/
ib_longlong btr_estimate_n_rows_in_range(dict_index_t* index, dtuple_t* tuple1, ulint mode1, dtuple_t* tuple2, ulint mode2)
{
	btr_path_t	path1[BTR_PATH_ARRAY_N_SLOTS];
	btr_path_t	path2[BTR_PATH_ARRAY_N_SLOTS];
	btr_cur_t	cursor;
	btr_path_t*	slot1;
	btr_path_t*	slot2;
	ibool		diverged;
	ibool       diverged_lot;
	ulint       divergence_level;           
	ib_longlong	n_rows;
	ulint		i;
	mtr_t		mtr;

	mtr_start(&mtr);

	cursor.path_arr = path1;
	if(dtuple_get_n_fields(tuple1) > 0){
		/*��cursor��λ����tuple1������ͬ��page��¼��,����*/
		btr_cur_search_to_nth_level(index, 0, tuple1, mode1, BTR_SEARCH_LEAF | BTR_ESTIMATE, &cursor, 0, &mtr);
	}
	else{
		btr_cur_open_at_index_side(TRUE, index, BTR_SEARCH_LEAF | BTR_ESTIMATE, &cursor, &mtr);
	}

	mtr_commit(&mtr);

	mtr_start(&mtr);

	cursor.path_arr = path2;
	if(dtuple_get_n_fields(tuple2) > 0){
		btr_cur_search_to_nth_level(index, 0, tuple2, mode2, BTR_SEARCH_LEAF | BTR_ESTIMATE, &cursor, 0, &mtr);
	}
	else{
		btr_cur_open_at_index_side(FALSE, index, BTR_SEARCH_LEAF | BTR_ESTIMATE, &cursor, &mtr);
	}

	mtr_commit(&mtr);

	n_rows = 1;
	diverged = FALSE;
	diverged_lot = FALSE;

	divergence_level = 1000000;

	for(i = 0; ; i ++){
		ut_ad(i < BTR_PATH_ARRAY_N_SLOTS);

		slot1 = path1 + i;
		slot2 = path2 + i;

		/*�Ѿ����㵽Ҷ�ӽڵ���*/
		if(slot1->nth_rec == ULINT_UNDEFINED || slot2->nth_rec == ULINT_UNDEFINED){
			if(i > divergence_level + 1)
				n_rows = n_rows * 2;

			if(n_rows > index->table->stat_n_rows / 2){
				n_rows = index->table->stat_n_rows / 2;
				if(n_rows == 0)
					n_rows = index->table->stat_n_rows;
			}

			return n_rows;
		}

		if(!diverged && slot1->nth_rec != slot2->nth_rec){
			diverged = TRUE;
			if(slot1->nth_rec < slot2->nth_rec){ /*����root page�ϵ������ҳ��*/
				n_rows = slot2->nth_rec - slot1->nth_rec;
				if(n_rows > 1){
					diverged_lot = TRUE;
					divergence_level = i;
				}
			}
			else /*���tuple2��tuple1ǰ�棬��ô����һ�������Ե�ֵ10*/
				return 10;
		}
		else if(diverged && !diverged_lot){ /*��ͬһҳ��,ֻͳ�Ʋ��*/
			if (slot1->nth_rec < slot1->n_recs || slot2->nth_rec > 1) {

				diverged_lot = TRUE;
				divergence_level = i;

				n_rows = 0;

				if (slot1->nth_rec < slot1->n_recs) {
					n_rows += slot1->n_recs - slot1->nth_rec;
				}

				if (slot2->nth_rec > 1) {
					n_rows += slot2->nth_rec - 1;
				}
			}
		}
		else if(diverged_lot)/*�ڲ�ͬҳ�У�ͳ�����ҳ�����е�������Ϊ����ֵ*/
			n_rows = (n_rows * (slot1->n_recs + slot2->n_recs)) / 2;
	}
}

/*ͳ��keyֵ��ͬ�ĸ���������Cardinality��ֵ*/
void btr_estimate_number_of_different_key_vals(dict_index_t* index)
{
	btr_cur_t	cursor;
	page_t*		page;
	rec_t*		rec;
	ulint		n_cols;
	ulint		matched_fields;
	ulint		matched_bytes;
	ulint*		n_diff;
	ulint		not_empty_flag	= 0;
	ulint		total_external_size = 0;
	ulint		i;
	ulint		j;
	ulint		add_on;
	mtr_t		mtr;

	/*���������е�����*/
	n_cols = dict_index_get_n_unique(index);
	n_diff = mem_alloc((n_cols + 1) * sizeof(ib_longlong));
	for(j = 0; j <= n_cols; j ++)
		n_diff[j] = 0;

	/*�������ȡ8��ҳ��Ϊ����,ͳ�Ʋ�ͬ��¼�ĸ���*/
	for(i = 0; i < BTR_KEY_VAL_ESTIMATE_N_PAGES; i ++){
		mtr_start(&mtr);
		btr_cur_open_at_rnd_pos(index, BTR_SEARCH_LEAF, &cursor, &mtr);

		page = btr_cur_get_page(&cursor);
		rec = page_get_infimum_rec(page);
		rec = page_rec_get_next(rec);

		if(rec != page_get_supremum_rec(page))
			not_empty_flag = 1;

		while(rec != page_get_supremum_rec(page) && page_rec_get_next(rec) != page_get_supremum_rec(page)){
			matched_fields = 0;
			matched_bytes = 0;

			cmp_rec_rec_with_match(rec, page_rec_get_next(rec), index, &matched_fields, &matched_bytes);
			for (j = matched_fields + 1; j <= n_cols; j++)
				n_diff[j]++;

			total_external_size += btr_rec_get_externally_stored_len(rec);

			rec = page_rec_get_next(rec);
		}

		total_external_size += btr_rec_get_externally_stored_len(rec);
		mtr_commit(&mtr);
	}
	/*����ƽ������*/
	for(j = 0; j <= n_cols; j ++){
		index->stat_n_diff_key_vals[j] =
			(n_diff[j] * index->stat_n_leaf_pages + BTR_KEY_VAL_ESTIMATE_N_PAGES - 1 + total_external_size + not_empty_flag) / (BTR_KEY_VAL_ESTIMATE_N_PAGES + total_external_size);

		add_on = index->stat_n_leaf_pages / (10 * (BTR_KEY_VAL_ESTIMATE_N_PAGES + total_external_size));

		if(add_on > BTR_KEY_VAL_ESTIMATE_N_PAGES)
			add_on = BTR_KEY_VAL_ESTIMATE_N_PAGES;

		index->stat_n_diff_key_vals[j] += add_on;
	}

	mem_free(n_diff);
}

/*���rec����ռ�õ�ҳ��*/
static ulint btr_rec_get_externally_stored_len(rec_t* rec)
{
	ulint	n_fields;
	byte*	data;
	ulint	local_len;
	ulint	extern_len;
	ulint	total_extern_len = 0;
	ulint	i;

	if(rec_get_data_size(rec) <= REC_1BYTE_OFFS_LIMIT)
		return 0;

	n_fields = rec_get_n_fields(rec);
	for(i = 0; i < n_fields; i ++){
		data = rec_get_nth_field(rec, i, &local_len);
		local_len -= BTR_EXTERN_FIELD_REF_SIZE;
		extern_len = mach_read_from_4(data + local_len + BTR_EXTERN_LEN + 4); /*����е��ܳ��ȣ�*/

		total_extern_len += ut_calc_align(extern_len, UNIV_PAGE_SIZE);
	}

	return total_extern_len / UNIV_PAGE_SIZE;
}

/*�����ⲿ�洢�е�ownershipλ*/
static void btr_cur_set_ownership_of_extern_field(rec_t* rec, ulint i, ibool val, mtr_t* mtr)
{
	byte*	data;
	ulint	local_len;
	ulint	byte_val;

	data = rec_get_nth_field(rec, i, &local_len);
	ut_a(local_len >= BTR_EXTERN_FIELD_REF_SIZE);

	local_len -= BTR_EXTERN_FIELD_REF_SIZE;

	byte_val = mach_read_from_1(data + local_len + BTR_EXTERN_LEN);
	if(val)
		byte_val = byte_val & (~BTR_EXTERN_OWNER_FLAG);
	else
		byte_val = byte_val | BTR_EXTERN_OWNER_FLAG;

	mlog_write_ulint(data + local_len + BTR_EXTERN_LEN, byte_val, MLOG_1BYTE, mtr);
}

void btr_cur_mark_extern_inherited_fields(rec_t* rec, upd_t* update, mtr_t* mtr)
{
	ibool	is_updated;
	ulint	n;
	ulint	j;
	ulint	i;

	n = rec_get_n_fields(rec);

	for(i = 0; i < n; i++){
		if (rec_get_nth_field_extern_bit(rec, i)){
			is_updated = FALSE;

			if(update){
				for(j = 0; j < upd_get_n_fields(update); j ++){
					if(upd_get_nth_field(update, j)->field_no = i)
						is_updated = TRUE;
				}
			}

			/*ȡ��owershipλ*/
			if(!is_updated)
				btr_cur_set_ownership_of_extern_field(rec, i, FALSE, mtr);
		}
	}
}

void btr_cur_mark_dtuple_inherited_extern(dtuple_t* entry, ulint* ext_vec, ulint n_ext_vec, upd_t* update)
{
	dfield_t* dfield;
	ulint	byte_val;
	byte*	data;
	ulint	len;
	ibool	is_updated;
	ulint	j;
	ulint	i;

	if(ext_vec = NULL)
		return ;

	for(i = 0; i < n_ext_vec; i ++){
		is_updated = FALSE;

		for(j = 0; j < upd_get_n_fields(update); j ++){
			if(upd_get_nth_field(update, j)->field_no == ext_vec[i])
				is_updated = TRUE;
		}

		/*����extern�̳б�ʶ*/
		if(!is_updated){
			dfield = dtuple_get_nth_field(entry, ext_vec[i]);
			data = dfield_get_data(dfield);
			len = dfield_get_len(dfield);

			len -= BTR_EXTERN_FIELD_REF_SIZE;

			byte_val = mach_read_from_1(data + len + BTR_EXTERN_LEN);
			byte_val = byte_val | BTR_EXTERN_INHERITED_FLAG;

			mach_write_to_1(data + len + BTR_EXTERN_LEN, byte_val);
		}
	}
}

void btr_cur_unmark_extern_fields(rec_t* rec, mtr_t* mtr)
{
	ulint n, i;

	n = rec_get_n_fields(rec);
	for(i = 0; i < n; i ++){
		if(rec_get_nth_field_extern_bit(rec, i)) /*�����е�ownershipλ*/
			btr_cur_set_ownership_of_extern_field(rec, i, TRUE, mtr);
	}
}

/*ȡ��tuple��Ӧ�е�ownershipλ*/
void btr_cur_unmark_dtuple_extern_fields(dtuple_t* entry, ulint* ext_vec, ulint n_ext_vec)
{
	dfield_t* dfield;
	ulint	byte_val;
	byte*	data;
	ulint	len;
	ulint	i;

	for(i = 0; i < n_ext_vec; i ++){
		dfield = dtuple_get_nth_field(entry, ext_vec[i]);

		data = dfield_get_data(dfield);
		len = dfield_get_len(dfield);

		len -= BTR_EXTERN_FIELD_REF_SIZE;

		byte_val = mach_read_from_1(data + len + BTR_EXTERN_LEN);
		byte_val = byte_val & ~BTR_EXTERN_OWNER_FLAG;

		/*ȡ��ownershipλ*/
		mach_write_to_1(data + len + BTR_EXTERN_LEN, byte_val);
	}
}

ulint btr_push_update_extern_fields(ulint* ext_vect, rec_t* rec, upd_t* update)
{
	ulint	n_pushed	= 0;
	ibool	is_updated;
	ulint	n;
	ulint	j;
	ulint	i;

	/*����update�иı����ID*/
	if(update){
		n = upd_get_n_fields(update);

		for (i = 0; i < n; i++) {
			if (upd_get_nth_field(update, i)->extern_storage){
				ext_vect[n_pushed] =upd_get_nth_field(update, i)->field_no;
				n_pushed++;
			}
		}
	}

	/*����rec���ı����ID*/
	n = rec_get_n_fields(rec);
	for(i = 0; i < n; i ++){
		if(rec_get_nth_field_extern_bit(rec, i)){
			is_updated = FALSE;
			if (update) {
				for (j = 0; j < upd_get_n_fields(update); j++) {
						if (upd_get_nth_field(update, j)->field_no == i)
								is_updated = TRUE;
				}
			}

			if(!is_updated){
				ext_vect[n_pushed] = i;
				n_pushed ++;
			}
		}
	}

	return n_pushed;
}

/*���blob�еĳ���*/
static ulint btr_blob_get_part_len(byte* blob_header)
{
	return mach_read_from_4(blob_header + BTR_BLOB_HDR_PART_LEN);
}

/*��ô���blob���ݵ���һҳID*/
static ulint btr_blob_get_next_page_no(byte* blob_header)
{
	return mach_read_from_4(blob_header + BTR_BLOB_HDR_NEXT_PAGE_NO);
}

/*�洢big rec����*/
ulint btr_store_big_rec_extern_fields(dict_index_t* index, rec_t* rec, big_rec_t* big_rec_vec, mtr_t* mtr)
{
	byte*	data;
	ulint	local_len;
	ulint	extern_len;
	ulint	store_len;
	ulint	page_no;
	page_t*	page;
	ulint	space_id;
	page_t*	prev_page;
	page_t*	rec_page;
	ulint	prev_page_no;
	ulint	hint_page_no;
	ulint	i;
	mtr_t	mtr;

	ut_ad(mtr_memo_contains(local_mtr, dict_tree_get_lock(index->tree), MTR_MEMO_X_LOCK));
	ut_ad(mtr_memo_contains(local_mtr, buf_block_align(data), MTR_MEMO_PAGE_X_FIX));
	ut_a(index->type & DICT_CLUSTERED);

	space_id = buf_frame_get_space_id(rec);

	for(i = 0; i < big_rec_vec->n_fields; i ++){
		data = rec_get_nth_field(rec, big_rec_vec->fields[i].field_no, &local_len);

		ut_a(local_len >= BTR_EXTERN_FIELD_REF_SIZE);
		local_len -= BTR_EXTERN_FIELD_REF_SIZE;

		extern_len = big_rec_vec->fields[i].len;

		ut_a(extern_len > 0);
		prev_page_no = FIL_NULL;

		while(extern_len > 0){
			mtr_start(&mtr);

			/*��λ����ҳ��hint id*/
			if(prev_page_no = FIL_NULL)
				hint_page_no = buf_frame_get_page_no(rec) + 1;
			else
				hint_page_no = prev_page_no + 1;

			/*��btree ����һ��page*/
			page = btr_page_alloc(index->tree, hint_page_no, FSP_NO_DIR, 0, &mtr);
			if(page == NULL){
				mtr_commit(&mtr);
				return DB_OUT_OF_FILE_SPACE;
			}

			page_no = buf_frame_get_page_no(page);
			if(prev_page_no != FIL_NULL){
				prev_page = buf_page_get(space_id, prev_page_no, RW_X_LATCH, &mtr);

				buf_page_dbg_add_level(prev_page, SYNC_EXTERN_STORAGE);
				/*��page noд�뵽ǰһҳ��BTR_BLOB_HDR_NEXT_PAGE_NO��*/
				mlog_write_ulint(prev_page + FIL_PAGE_DATA + BTR_BLOB_HDR_NEXT_PAGE_NO, page_no, MLOG_4BYTES, &mtr);
			}

			/*ȷ���洢�����ݳ���*/
			if(extern_len > (UNIV_PAGE_SIZE - FIL_PAGE_DATA - BTR_BLOB_HDR_SIZE - FIL_PAGE_DATA_END))
				store_len = UNIV_PAGE_SIZE - FIL_PAGE_DATA - BTR_BLOB_HDR_SIZE - FIL_PAGE_DATA_END;
			else
				store_len = extern_len;
			/*������д�뵽page��*/
			mlog_write_string(page + FIL_PAGE_DATA + BTR_BLOB_HDR_SIZE, big_rec_vec->fields[i].data + big_rec_vec->fields[i].len - extern_len,
				store_len, &mtr);

			/*д��blob�д洢�ڱ�ҳ�����ݳ���*/
			mlog_write_ulint(page + FIL_PAGE_DATA + BTR_BLOB_HDR_PART_LEN, store_len, MLOG_4BYTES, &mtr);
			/*��Ϊ������ȷ����Ҫ�����ҳ�洢���У�����������ʱ��дFIL_NULL,����һ��ҳ����ʱ������´�ֵ*/
			mlog_write_ulint(page + FIL_PAGE_DATA+ BTR_BLOB_HDR_NEXT_PAGE_NO,FIL_NULL, MLOG_4BYTES, &mtr);

			extern_len -= store_len;
			rec_page = buf_page_get(space_id, buf_frame_get_page_no(data), RW_X_LATCH, &mtr);

			buf_page_dbg_add_level(rec_page, SYNC_NO_ORDER_CHECK);

			mlog_write_ulint(data + local_len + BTR_EXTERN_LEN, 0, MLOG_4BYTES, &mtr);
			mlog_write_ulint(data + local_len + BTR_EXTERN_LEN + 4, big_rec_vec->fields[i].len - extern_len, MLOG_4BYTES, &mtr);
			
			/*д��field��ʼ��ҳλ����Ϣ(space id, page no, offset)����Ӧrec����*/
			if(prev_page_no == FIL_NULL){
				mlog_write_ulint(data + local_len
					+ BTR_EXTERN_SPACE_ID, space_id, MLOG_4BYTES, &mtr);

				mlog_write_ulint(data + local_len
					+ BTR_EXTERN_PAGE_NO, page_no, MLOG_4BYTES, &mtr);

				mlog_write_ulint(data + local_len
					+ BTR_EXTERN_OFFSET, FIL_PAGE_DATA, MLOG_4BYTES, &mtr);

				/*����һ���ж��ҳ�洢�ı�ʶ*/
				rec_set_nth_field_extern_bit(rec, big_rec_vec->fields[i].field_no,TRUE, &mtr);
			}

			prev_page_no = page_no;
			mtr_commit(&mtr);
		}
	}

	return DB_SUCCESS;
}

/*��blobռ�õ�page�ͷ�*/
void btr_free_externally_stored_field(dict_index_t* index, byte* data, ulint local_len, ibool do_not_free_inherited, mtr_t* local_mtr)
{
	page_t*	page;
	page_t*	rec_page;
	ulint	space_id;
	ulint	page_no;
	ulint	offset;
	ulint	extern_len;
	ulint	next_page_no;
	ulint	part_len;
	mtr_t	mtr;

	ut_a(local_len >= BTR_EXTERN_FIELD_REF_SIZE);
	ut_ad(mtr_memo_contains(local_mtr, dict_tree_get_lock(index->tree), MTR_MEMO_X_LOCK));
	ut_ad(mtr_memo_contains(local_mtr, buf_block_align(data), MTR_MEMO_PAGE_X_FIX));

	local_len -= BTR_EXTERN_FIELD_REF_SIZE;
	for(;;){
		mtr_start(&mtr);

		/*�ҵ���¼���ڵ�ҳ*/
		rec_page = buf_page_get(buf_frame_get_space_id(data), buf_frame_get_page_no(data), RW_X_LATCH, &mtr);
		buf_page_dbg_add_level(rec_page, SYNC_NO_ORDER_CHECK);

		space_id = mach_read_from_4(data + local_len + BTR_EXTERN_SPACE_ID);
		page_no = mach_read_from_4(data + local_len + BTR_EXTERN_PAGE_NO);
		offset = mach_read_from_4(data + local_len + BTR_EXTERN_OFFSET);

		/*����ⲿ���ݳ���*/
		extern_len = mach_read_from_4(data + local_len + BTR_EXTERN_LEN + 4);
		if(extern_len == 0){
			mtr_commit(&mtr);
			return ;
		}

		/*��¼��û�н����ݷֵ�����ҳ�ϴ洢*/
		if(mach_read_from_1(data + local_len + BTR_EXTERN_LEN) & BTR_EXTERN_OWNER_FLAG){
			mtr_commit(&mtr);
			return ;
		}

		/*�Ѿ����ع��ˣ�����Ҫfree*/
		if(do_not_free_inherited &&  mach_read_from_1(data + local_len + BTR_EXTERN_LEN) & BTR_EXTERN_INHERITED_FLAG){
			mtr_commit(&mtr);
			return;
		}

		page = buf_page_get(space_id, page_no, RW_X_LATCH, &mtr);
		buf_page_dbg_add_level(page, SYNC_EXTERN_STORAGE);

		/*�����һ��ҳID*/
		next_page_no = mach_read_from_4(page + FIL_PAGE_DATA + BTR_BLOB_HDR_NEXT_PAGE_NO);
		part_len = btr_blob_get_part_len(page + FIL_PAGE_DATA);

		ut_a(extern_len >= part_len);

		/*�ͷ�blob��ռ�õ�ҳ*/
		btr_page_free_low(index->tree, page, 0, &mtr);

		mlog_write_ulint(data + local_len + BTR_EXTERN_PAGE_NO, next_page_no, MLOG_4BYTES, &mtr);
		mlog_write_ulint(data + local_len + BTR_EXTERN_LEN + 4, extern_len - part_len, MLOG_4BYTES, &mtr);

		/*��blob�������Ե��ж�*/
		if(next_page_no == FIL_NULL)
			ut_a(extern_len - part_len == 0);

		if(extern_len - part_len == 0)
			ut_a(next_page_no == FIL_NULL);

		mtr_commit(&mtr);
	}
}

/*�ͷ�rec������blob��ռ�õ�ҳ�ռ�*/
void btr_rec_free_externally_stored_fields(dict_index_t* index, rec_t* rec, ibool do_not_free_inherited, mtr_t* mtr)
{
	ulint	n_fields;
	byte*	data;
	ulint	len;
	ulint	i;

	ut_ad(mtr_memo_contains(mtr, buf_block_align(rec), MTR_MEMO_PAGE_X_FIX));

	if(rec_get_data_size(rec) <= REC_1BYTE_OFFS_LIMIT)
		return;

	n_fields = rec_get_n_fields(rec);
	for(i = 0; i < n_fields; i ++){
		if (rec_get_nth_field_extern_bit(rec, i)){
			data = rec_get_nth_field(rec, i, &len);
			btr_free_externally_stored_field(index, data, len, do_not_free_inherited, mtr);
		}
	}
}

/*��update�е�blob fieldռ�õ�ҳ�����ͷ�*/
static void btr_rec_free_updated_extern_fields(dict_index_t* index, rec_t* rec, upd_t* update, ibool do_not_free_inherited, mtr_t* mtr)
{
	upd_field_t*	ufield;
	ulint		n_fields;
	byte*		data;
	ulint		len;
	ulint		i;

	ut_ad(mtr_memo_contains(mtr, buf_block_align(rec), MTR_MEMO_PAGE_X_FIX));

	if(rec_get_data_size(rec) < REC_1BYTE_OFFS_LIMIT)
		return ;

	n_fields = upd_get_n_fields(update);

	for (i = 0; i < n_fields; i++) {
		ufield = upd_get_nth_field(update, i);

		if (rec_get_nth_field_extern_bit(rec, ufield->field_no)) {
			data = rec_get_nth_field(rec, ufield->field_no, &len);
			btr_free_externally_stored_field(index, data, len, do_not_free_inherited, mtr);
		}
	}
}

/*�Դ��н��п���*/
byte* btr_copy_externally_stored_field(ulint* len, byte* data, ulint local_len, mem_heap_t* heap)
{
	page_t*	page;
	ulint	space_id;
	ulint	page_no;
	ulint	offset;
	ulint	extern_len;
	byte*	blob_header;
	ulint	part_len;
	byte*	buf;
	ulint	copied_len;
	mtr_t	mtr;

	ut_a(local_len >= BTR_EXTERN_FIELD_REF_SIZE);

	local_len -= BTR_EXTERN_FIELD_REF_SIZE;
	/*���blobռ��ҳ��λ��*/
	space_id = mach_read_from_4(data + local_len + BTR_EXTERN_SPACE_ID);
	page_no = mach_read_from_4(data + local_len + BTR_EXTERN_PAGE_NO);
	offset = mach_read_from_4(data + local_len + BTR_EXTERN_OFFSET);

	extern_len = mach_read_from_4(data + local_len + BTR_EXTERN_LEN + 4);
	buf = mem_heap_alloc(heap, local_len + extern_len);
	/*�ȿ���field index*/
	ut_memcpy(buf, data, local_len);
	copied_len = local_len;
	if (extern_len == 0){
		*len = copied_len;
		return(buf);
	}

	for(;;){
		mtr_start(&mtr);

		page = buf_page_get(space_id, page_no, RW_X_LATCH, &mtr);
		buf_page_dbg_add_level(page, SYNC_EXTERN_STORAGE);

		blob_header = page + offset;
		part_len = btr_blob_get_part_len(blob_header);
		/*����field data*/
		ut_memcpy(buf + copied_len, blob_header + BTR_BLOB_HDR_SIZE, part_len);
		copied_len += part_len;

		/*�����һ��page��ID*/
		page_no = btr_blob_get_next_page_no(blob_header);

		offset = FIL_PAGE_DATA;

		mtr_commit(&mtr);
		/*�Ѿ�û�и������blob�����ݵ�ҳ�ˣ��������*/
		if (page_no == FIL_NULL) {
			ut_a(copied_len == local_len + extern_len);
			*len = copied_len;
			return(buf);
		}

		ut_a(copied_len < local_len + extern_len);
	}
}

/*����rec�е�no�е����ݣ����һ����BLOB��*/
byte* btr_rec_copy_externally_stored_field(rec_t* rec, ulint no, ulint* len, mem_heap_t* heap)
{
	ulint	local_len;
	byte*	data;

	ut_a(rec_get_nth_field_extern_bit(rec, no));

	data = rec_get_nth_field(rec, no, &local_len);
	return btr_copy_externally_stored_field(len, data, local_len, heap);
}


