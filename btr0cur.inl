#include "btr0btr.h"

/*ͨ��btr cur���page cursor*/
UNIV_INLINE page_cur_t* btr_cur_get_page_cur(btr_cur_t* cursor)
{
	return &(cursor->page_cur);
}

/*ͨ��btree cursor��ö�Ӧ�ļ�¼*/
UNIV_INLINE rec_t* btr_cur_get_rec(btr_cur_t* cursor)
{
	return page_cur_get_rec(&(cursor->page_cur));
}

UNIV_INLINE void  btr_cur_invalidate(btr_cur_t* cursor)
{
	page_cur_invalidate(&(cursor->page_cur));
}

/*ͨ��btree cursor��ö�Ӧ��pageҳ*/
UNIV_INLINE page_t* btr_cur_get_page(btr_cur_t* cursor)
{
	return buf_frame_align(page_cur_get_rec(&(cursor->page_cur)));
}

/*ͨ��btree cursor��ö�Ӧ����������*/
UNIV_INLINE dict_tree_t* btr_cur_get_tree(btr_cur_t* cursor)
{
	return cursor->index->tree;
}

/*����btree cursor��Ӧ��page��¼��¼*/
UNIV_INLINE void btr_cur_position(dict_index_t* index, rec_t* rec, btr_cur_t* cursor)
{
	page_cur_position(rec, btr_cur_get_page_cur(cursor));
}

/*��btree cursor��Ӧ��page�ж��Ƿ���Ժϲ�*/
UNIV_INLINE ibool btr_cur_compress_recommendation(btr_cur_t* cursor, mtr_t* mtr)
{
	page_t* page;

	ut_ad(mtr_memo_contains(mtr, buf_block_align(btr_cur_get_page(cursor)), MTR_MEMO_PAGE_X_FIX));

	page = btr_cur_get_page(cursor);

	/*page��������ݵ���������ӻ���û���ֵ�ҳ*/
	if((page_get_data_size(page) < BTR_CUR_PAGE_COMPRESS_LIMIT) || 
		(btr_page_get_next(page, mtr) == FIL_NULL && btr_page_get_prev(page, mtr) == FIL_NULL)){
			/*�Ѿ���root page�ˣ����ܽ��кϲ�*/
			if(dict_tree_get_page(cursor->index->tree) == buf_frame_get_page_no(page))
				return FALSE;

			return TRUE;
	}

	return FALSE;
}

/*���ɾ��cursor��Ӧ�ļ�¼�Ƿ�ᴥ��cursor��Ӧҳ�ĺϲ���������᷵��TRUE*/
UNIV_INLINE ibool btr_cur_can_delete_without_compress(btr_cur_t* cursor, mtr_t* mtr)
{
	ulint rec_size;
	page_t* page;

	ut_ad(mtr_memo_contains(mtr, buf_block_align(btr_cur_get_page(cursor)), MTR_MEMO_PAGE_X_FIX));

	rec_size = rec_get_size(btr_cur_get_rec(cursor));
	page = btr_cur_get_page(cursor);

	/*���ɾ����ǰcursorָ��ļ�¼����Դ����ϲ���������ռ����С��������� ���� ��ǰpage���ֵ� ���� pageֻʣ��1����¼��*/
	if((page_get_data_size(page) - rec_size < BTR_CUR_PAGE_COMPRESS_LIMIT)
		|| ((btr_page_get_next(page, mtr) == FIL_NULL) && (btr_page_get_prev(page, mtr) == FIL_NULL))
		|| page_get_n_rec(page) < 2){
			if(dict_tree_get_page(cursor->index->tree) == buf_frame_get_page_no(page))
				return TRUE;

			return FALSE;
	}

	return TRUE;
}

