#include "btr0pcur.h"
#include "ut0byte.h"
#include "rem0cmp.h"


btr_pcur_t* btr_pcur_create_for_mysql()
{
	btr_pcur_t*	pcur;

	pcur = mem_alloc(sizeof(btr_pcur_t));

	pcur->btr_cur.index = NULL;
	btr_pcur_init(pcur);

	return pcur;
}

void btr_pcur_free_for_mysql(btr_pcur_t* cursor)
{
	if(cursor->old_rec_buf != NULL){
		mem_free(cursor->old_rec_buf);

		cursor->old_rec = NULL;
		cursor->old_rec_buf = NULL;
	}

	cursor->btr_cur.page_cur.rec = NULL;
	cursor->old_rec = NULL;
	cursor->old_stored = BTR_PCUR_OLD_NOT_STORED;

	cursor->latch_mode = BTR_NO_LATCHES;
	cursor->pos_state = BTR_PCUR_NOT_POSITIONED;

	mem_free(cursor);
}

/*��pcursorָ��ļ�¼���޸�ʱ����б��浽old_stored��old_rec����*/
void btr_pcur_store_position(btr_pcur_t* cursor, mtr_t* mtr)
{
	page_cur_t*		page_cursor;
	rec_t*			rec;
	dict_tree_t*	tree;
	page_t*			page;

	ut_a(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);

	tree = btr_cur_get_tree(btr_pcur_get_btr_cur(cursor));
	/*���page cursor*/
	page_cursor = btr_pcur_get_page_cur(cursor);
	/*���pcursorָ��ļ�¼*/
	rec = page_cur_get_rec(page_cursor);
	/*��ö�Ӧ��page*/
	page = buf_frame_align(rec);

	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_S_FIX) 
		|| mtr_memo_contains(mtr, buf_block_align(page),MTR_MEMO_PAGE_X_FIX));

	ut_a(cursor->latch_mode != BTR_NO_LATCHES);

	/*û���û���Ч��¼,Ϊempty tree*/
	if(page_get_n_recs(page) == 0){
		ut_a(btr_page_get_next(page, mtr) == FIL_NULL && btr_page_get_prev(page, mtr) == FIL_NULL);
		
		/*rec��¼Ϊsupremum*/
		if(rec == page_get_supremum_rec(page)){
			cursor->rel_pos = BTR_PCUR_AFTER_LAST_IN_TREE;
			cursor->old_stored = BTR_PCUR_OLD_STORED;
		}
		else{ /*recΪinfimum*/
			cursor->rel_pos = BTR_PCUR_BEFORE_FIRST_IN_TREE;
			cursor->old_stored = BTR_PCUR_OLD_STORED;
		}
		return;
	}

	/*��������û���¼*/
	if(rec == page_get_supremum_rec(page)){ 
		/*�Ѿ���ҳĩβ��ָ�����һ����Ч�ļ�¼*/
		rec = page_rec_get_prev(rec);
		cursor->rel_pos = BTR_PCUR_AFTER;
	}
	else if(rec == page_get_infimum_rec(page)){
		/*�Ѿ���ҳͷ��ָ���һ����Ч�ļ�¼*/
		rec = page_rec_get_next(rec);
		cursor->rel_pos = BTR_PCUR_BEFORE;
	}
	else
		cursor->rel_pos = BTR_PCUR_ON;

	/*����¼�Ͳ���ʱ�̼�¼����*/
	cursor->old_stored = BTR_PCUR_OLD_STORED;
	cursor->old_rec = dict_tree_copy_rec_order_prefix(tree, rec, &(cursor->old_rec_buf), &(cursor->buf_size));
	cursor->modify_clock = buf_frame_get_modify_clock(page);
}

/*��pcur_donate�е�position������pcur_receive����*/
void btr_pcur_copy_stored_position(btr_pcur_t* pcur_receive, btr_pcur_t* pcur_donate)
{
	if(pcur_receive->old_rec_buf != NULL)
		mem_free(pcur_receive->old_rec_buf);

	/*����pcursor�е�����*/
	ut_memcpy((byte*)pcur_receive, (byte*)pcur_donate, sizeof(btr_pcur_t));

	/*����old_rec_buf*/
	if(pcur_donate->old_rec_buf != NULL){
		pcur_receive->old_rec_buf = mem_alloc(pcur_donate->buf_size);
		ut_memcpy(pcur_receive->old_rec_buf, pcur_donate->old_rec_buf, pcur_donate->buf_size);
		/*�������һ�������ļ�¼����*/
		pcur_receive->old_rec = pcur_receive->old_rec_buf + (pcur_donate->old_rec - pcur_donate->old_rec_buf);
	}
}

/*���±���pcursor position*/
ibool btr_pcur_restore_position(ulint latch_mode, btr_pcur_t* cursor, mtr_t* mtr)
{
	dict_tree_t*	tree;
	page_t*		page;
	dtuple_t*	tuple;
	ulint		mode;
	ulint		old_mode;
	ibool		from_left;
	mem_heap_t*	heap;

	ut_a(cursor->pos_state == BTR_PCUR_WAS_POSITIONED || cursor->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_a(cursor->old_stored == BTR_PCUR_OLD_STORED);

	/*�Ѿ�����btree��������߻������ұ�*/
	if(cursor->rel_pos == BTR_PCUR_AFTER_LAST_IN_TREE || cursor->rel_pos == BTR_PCUR_BEFORE_FIRST_IN_TREE){
		if(cursor->rel_pos == BTR_PCUR_BEFORE_FIRST_IN_TREE) /*cursorָ�������λ��ֻ�ܴ����������*/
			from_left = TRUE;
		else /*cursor���ұߣ�ֻ�ܴ����ұ������*/
			from_left = FALSE;

		/*��λ����Ӧ��λ��*/
		btr_cur_open_at_index_side(from_left, btr_pcur_get_btr_cur(cursor)->index, latch_mode, btr_pcur_get_btr_cur(cursor), mtr);

		return FALSE;
	}

	ut_a(cursor->old_rec);

	page = btr_cur_get_page(btr_pcur_get_btr_cur(cursor));
	/*ֻ�����������޸�btree��Ҷ�ӽڵ�*/
	if(latch_mode == BTR_SEARCH_LEAF || latch_mode == BTR_MODIFY_LEAF){
		/*�����ֹ۷�ʽ����*/
		if(buf_page_optimistic_get(latch_mode, page, cursor->modify_clock, mtr)){
			cursor->pos_state = BTR_PCUR_IS_POSITIONED;
			buf_page_dbg_add_level(page, SYNC_TREE_NODE);

			if(cursor->rel_pos == BTR_PCUR_ON){
				cursor->latch_mode = latch_mode;

				/*�ϴβ����ļ�¼����cursorָ��ļ�¼*/
				ut_ad(cmp_rec_rec(cursor->old_rec, btr_pcur_get_rec(cursor),
					dict_tree_find_index(btr_cur_get_tree(btr_pcur_get_btr_cur(cursor)), btr_pcur_get_rec(cursor))) == 0); 

				return TRUE;
			}

			return FALSE;
		}
	}

	heap = mem_heap_create(256);

	tree = btr_cur_get_tree(btr_pcur_get_btr_cur(cursor));
	/*ͨ��old_rec����һ��tuple��¼*/
	tuple = dict_tree_build_data_tuple(tree, cursor->old_rec, heap);

	old_mode = cursor->search_mode;
	/*ȷ����¼ƥ��ķ�ʽ*/
	if(cursor->rel_pos == BTR_PCUR_ON)
		mode = PAGE_CUR_LE; /*С�ڵ���*/
	else if(cursor->rel_pos == BTR_PCUR_AFTER)
		mode = PAGE_CUR_G;  /*����*/
	else 
		mode = PAGE_CUR_L; /*С��*/

	/*ͨ��tuple��λ��btree��Ӧ��λ��*/
	btr_pcur_open_with_no_init(btr_pcur_get_btr_cur(cursor)->index, tuple, mode, latch_mode, cursor, 0, mtr);

	cursor->old_stored = BTR_PCUR_OLD_STORED;

	cursor->search_mode = old_mode;
	/*tuple��cursorִ�еļ�¼��һ�µ�����,ֻ��Ҫ������modify clock*/
	if (cursor->rel_pos == BTR_PCUR_ON && btr_pcur_is_on_user_rec(cursor, mtr)
		&& 0 == cmp_dtuple_rec(tuple, btr_pcur_get_rec(cursor))){
			cursor->modify_clock = buf_frame_get_modify_clock(buf_frame_buf(btr_pcur_get_rec(cursor)));
			mem_heap_free(heap);

			return TRUE;
	}

	mem_heap_free(heap);
	/*���¸���position*/
	btr_pcur_store_position(cursor, mtr);

	return FALSE;
}

/*�ͷ�cursorָ��page��latch(BTR_LEAF_SEARCH��BTR_LEAF_MODIFY)*/
void btr_pcur_release_leaf(btr_pcur_t* cursor, ulint latch_mode, mtr_t* mtr)
{
	page_t* page;
	
	ut_a(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);

	/*���cursor��Ӧpage*/
	page = btr_cur_get_page(btr_pcur_get_btr_cur(cursor));
	/*�ͷų��е�latch*/
	btr_leaf_page_release(page, cursor->latch_mode, mtr);

	cursor->latch_mode = BTR_NO_LATCHES;

	cursor->pos_state = BTR_PCUR_WAS_POSITIONED;
}

/*cursor������һ��page*/
void btr_pcur_move_to_next_page(btr_pcur_t* cursor, mtr_t* mtr)
{
	ulint	next_page_no;
	ulint	space;
	page_t*	page;
	page_t*	next_page;

	ut_a(cursor->pos_state == BTR_PCUR_IS_POSITIONED);	
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);
	ut_ad(btr_pcur_is_after_last_on_page(cursor, mtr));	

	cursor->old_stored = BTR_PCUR_OLD_NOT_STORED;
	page = btr_pcur_get_page(cursor);

	/*���ͬһ�����¸�page no*/
	next_page_no = btr_page_get_next(page, mtr);
	space = buf_frame_get_space_id(page);

	ut_ad(next_page_no != FIL_NULL);

	/*�����һ��page��ָ��*/
	next_page = btr_page_get(space, next_page_no, cursor->latch_mode, mtr);
	/*�ͷű�page���е�latch*/
	btr_leaf_page_release(page, cursor->latch_mode, mtr);

	/*��λ��infimum�ϣ���Ϊ������ƶ�������cursor�ᶨλ�ں�һҳ��infimum��*/
	page_cur_set_before_first(next_page, btr_pcur_get_page_cur(cursor));
}

void btr_pcur_move_backward_from_page(btr_pcur_t* cursor, mtr_t* mtr)
{
	ulint	prev_page_no;
	ulint	space;
	page_t*	page;
	page_t*	prev_page;
	ulint	latch_mode;
	ulint	latch_mode2;

	ut_a(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);

	ut_ad(btr_pcur_is_before_first_on_page(cursor, mtr));	
	ut_ad(!btr_pcur_is_before_first_in_tree(cursor, mtr));	

	latch_mode = cursor->latch_mode;
	/*ȷ��mode2*/
	if(latch_mode == BTR_SEARCH_LEAF)
		latch_mode2 = BTR_SEARCH_PREV;
	else if(latch_mode == BTR_MODIFY_LEAF)
		latch_mode2 = BTR_MODIFY_PREV;
	else{ /*���������ȫ��Ϊ���ų�����������*/
		latch_mode2 = 0;
		ut_error;
	}

	btr_pcur_store_position(cursor, mtr);

	mtr_commit(mtr);

	mtr_start(mtr);

	btr_pcur_restore_position(latch_mode2, cursor, mtr);
	page = btr_pcur_get_page(cursor);

	prev_page_no = btr_page_get_prev(page, mtr);
	space = buf_frame_get_space_id(page);

	/*�Ѿ���ҳ��ͷһ����¼�ˣ����ƶ���ָ����һҳ�����һ����¼*/
	if(btr_pcur_is_before_first_on_page(cursor, mtr) && prev_page_no != FIL_NULL){
		prev_page = btr_pcur_get_btr_cur(cursor)->left_page;
		btr_leaf_page_release(page, latch_mode, mtr);
		page_cur_set_after_last(prev_page, btr_pcur_get_page_cur(cursor));
	}
	else if(prev_page_no != FIL_NULL){
		prev_page = btr_pcur_get_btr_cur(cursor)->left_page;
		btr_leaf_page_release(prev_page, latch_mode, mtr);
	}

	cursor->latch_mode = latch_mode;
	cursor->old_stored = BTR_PCUR_OLD_NOT_STORED;
}

/*����pcursorָ���ǰһ����¼*/
ibool btr_pcur_move_to_prev(btr_pcur_t*	cursor,mtr_t* mtr)	
{
	ut_ad(cursor->pos_state == BTR_PCUR_IS_POSITIONED);
	ut_ad(cursor->latch_mode != BTR_NO_LATCHES);
	
	cursor->old_stored = BTR_PCUR_OLD_NOT_STORED;

	/*cursor�Ѿ�����ҳ����ǰ��*/
	if (btr_pcur_is_before_first_on_page(cursor, mtr)){
		if (btr_pcur_is_before_first_in_tree(cursor, mtr)) /*�Ѿ�������ǰ�����ǰ�棬�޷�����ǰ�ƶ�*/
			return FALSE;

		btr_pcur_move_backward_from_page(cursor, mtr);
		return TRUE;
	}

	btr_pcur_move_to_prev_on_page(cursor, mtr);

	return(TRUE);
}

/*����tuple�򿪲���λһ��pcursor,���mode =PAGE_CUR_GE��PAGE_CUR_G����Ҫ����Ƿ��ҳ*/
void btr_pcur_open_on_user_rec(
	dict_index_t*	index,	/* in: index */
	dtuple_t*	tuple,		/* in: tuple on which search done */
	ulint		mode,		/* in: PAGE_CUR_L, ... */
	ulint		latch_mode,	/* in: BTR_SEARCH_LEAF or BTR_MODIFY_LEAF */
	btr_pcur_t*	cursor, 	/* in: memory buffer for persistent cursor */
	mtr_t*		mtr)		/* in: mtr */
{
	btr_pcur_open(index, tuple, mode, latch_mode, cursor, mtr);

	if ((mode == PAGE_CUR_GE) || (mode == PAGE_CUR_G)){
		if (btr_pcur_is_after_last_on_page(cursor, mtr)) 
			btr_pcur_move_to_next_user_rec(cursor, mtr);/*pcursorָ��page��supremum�ϣ����ƶ�����һ����¼��*/
	} 
	else{
		ut_ad((mode == PAGE_CUR_LE) || (mode == PAGE_CUR_L));
		ut_error;
	}
}

