#include "mach0data.h"
#include "mtr0mtr.h"
#include "mtr0log.h"

#define BTR_MAX_NODE_LEVEL	50


UNIV_INLINE page_t* btr_page_get(ulint space, ulint page_no, ulint mode, mtr_t* mtr)
{
	page_t* page;
	
	page = buf_page_get(space, page_no, mode, mtr);

#ifdef UNIV_SYNC_DEBUG
	if (mode != RW_NO_LATCH)
		buf_page_dbg_add_level(page, SYNC_TREE_NODE);
#endif

	return page;
}

/*����ҳ��B-TREE������ID,���¼��redo log��*/
UNIV_INLINE void btr_page_set_index_id(page_t* page, dulint id, mtr_t* mtr)
{
	mlog_write_dulint(page + PAGE_HEADER + PAGE_INDEX_ID, id, MLOG_8BYTES, mtr);
}

UNIV_INLINE ulint btr_page_get_level_low(page_t* page)
{
	ulint level;

	ut_ad(page);

	level = mach_read_from_2(page + PAGE_HEADER + PAGE_LEVEL);
	ut_ad(level <= BTR_MAX_NODE_LEVEL);

	return level;
}

UNIV_INLINE ulint btr_page_get_level(page_t* page, mtr_t* mtr)
{
	ut_ad(page && mtr);

	return btr_page_get_level_low(page);
}

/*����page���ڵ�B-TREE�������Ĳ��*/
UNIV_INLINE void btr_page_set_level(page_t* page, ulint level, mtr_t* mtr)
{
	ut_ad(page && mtr);
	ut_ad(level <= BTR_MAX_NODE_LEVEL);

	mlog_write_ulint(page + PAGE_HEADER + PAGE_LEVEL, level, MLOG_2BYTES, mtr);
}

/*��ȡ��һ��pageָ�����һ��PAGE no,��ΪBTREE��Ҷ�ӽڵ���һ��˫������*/
UNIV_INLINE ulint btr_page_get_next(page_t* page, mtr_t* mtr)
{
	ut_ad(page && mtr);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page),MTR_MEMO_PAGE_X_FIX)
		|| mtr_memo_contains(mtr, buf_block_align(page),MTR_MEMO_PAGE_S_FIX));

	return mach_read_from_4(page + FIL_PAGE_NEXT);
}

/*����page��һ��ҳ��page no*/
UNIV_INLINE void btr_page_set_next(page_t* page, ulint next, mtr_t* mtr)
{
	ut_ad(page && mtr);

	mlog_write_ulint(page + FIL_PAGE_NEXT, next, MLOG_4BYTES, mtr);
}

UNIV_INLINE ulint btr_page_get_prev(page_t* page, mtr_t* mtr)
{
	ut_ad(page && mtr);

	return(mach_read_from_4(page + FIL_PAGE_PREV));
}

UNIV_INLINE void btr_page_set_prev(page_t* page, ulint prev, mtr_t* mtr)
{
	ut_ad(page && mtr);

	mlog_write_ulint(page + FIL_PAGE_PREV, next, MLOG_4BYTES, mtr);
}

/*��node ptr�л�ȡ��Ӧ���ӵ�PAGE NO*/
UNIV_INLINE ulint btr_node_ptr_get_child_page_no(rec_t* rec)
{
	ulint	n_fields;
	byte*	field;
	ulint	len;

	n_fields = rec_get_n_fields(rec);
	field = rec_get_nth_field(rec, n_fields - 1, &len);

	ut_ad(len == 4);

	return mach_read_from_4(field);
}

/*�ͷ���Ҷ�ӽڵ������е�latch*/
UNIV_INLINE void btr_leaf_page_release(page_t* page, ulint latch_mode)
{
	ut_ad(!mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_MODIFY));

	/*��ѯģʽ*/
	if(latch_mode == BTR_SEARCH_LEAF)
		mtr_memo_release(mtr, buf_block_align(page), MTR_MEMO_PAGE_S_FIX); /*�ͷ�mtr�е�MTR_MEMO_PAGE_S_FIX latch*/
	else{/*�޸�ģʽ*/
		ut_ad(latch_mode == BTR_MODIFY_LEAF);
		mtr_memo_release(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX); /*�ͷ�mtr�е�MTR_MEMO_PAGE_X_FIX latch*/
	}
}

