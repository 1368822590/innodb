#include "btr0btr.h"
#include "fsp0fsp.h"
#include "page0page.h"
#include "btr0cur.h"
#include "btr0sea.h"
#include "btr0pcur.h"
#include "rem0cmp.h"
#include "lock0lock.h"
#include "ibuf0ibuf.h"

/*�����˳����룬���ҳ���ѵ�һ����¼������*/
#define BTR_PAGE_SEQ_INSERT_LIMIT	5

static void			btr_page_create(page_t* page, dict_tree_t* tree, mtr_t* mtr);

UNIV_INLINE void	btr_node_ptr_set_child_page_no(rec_t* rec, ulint page_no, mtr_t* mtr);

static rec_t*		btr_page_get_father_node_ptr(dict_tree_t* tree, page_t* page, mtr_t* mtr); 

static void			btr_page_empty(page_t* page, mtr_t* mtr);

static ibool		btr_page_insert_fits(btr_cur_t* cursor, rec_t* split_rec, dtuple_t* tuple);

/**************************************************************************/
/*���root node���ڵ�page,���һ����������x-latch*/
page_t* btr_root_get(dict_tree_t* tree, mtr_t* mtr)
{
	ulint	space;
	ulint	root_page_no;
	page_t*	root;

	ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(tree), MTR_MEMO_X_LOCK)
		|| mtr_memo_contains(mtr, dict_tree_get_lock(tree), MTR_MEMO_S_LOCK));

	space = dict_tree_get_space(tree);
	root_page_no = dict_tree_get_page(tree);

	root = btr_page_get(space, root_page_no, RW_X_LATCH, mtr);

	return root;
}

/*���B-TREE��recλ�õ���һ����¼*/
rec_t* btr_get_prev_user_rec(rec_t* rec, mtr_t* mtr)
{
	page_t*	page;
	page_t*	prev_page;
	ulint	prev_page_no;
	rec_t*	prev_rec;
	ulint	space;

	page = buf_frame_align(rec);

	if(page_get_infimum_rec(page) != rec){
		prev_rec = page_rec_get_prev(rec);

		if(page_get_infimum_rec(page) != prev_rec)
			return prev_rec;
	}

	prev_page_no = btr_page_get_prev(page, mtr);
	space = buf_frame_get_space_id(page);

	if(prev_page_no != FIL_NULL){
		prev_page = buf_page_get_with_no_latch(space, prev_page_no, mtr);

		/*�Գ���latch���ж�,һ�������latch��*/
		ut_ad((mtr_memo_contains(mtr, buf_block_align(prev_page), MTR_MEMO_PAGE_S_FIX))
			|| (mtr_memo_contains(mtr, buf_block_align(prev_page), MTR_MEMO_PAGE_X_FIX)));

		prev_rec = page_rec_get_prev(page_get_supremum_rec(prev_page));

		return prev_rec;
	}

	return NULL;
}

/*���B-TREE��recλ�õ���һ����¼*/
rec_t* btr_get_next_user_rec(rec_t* rec, mtr_t* mtr)
{
	page_t*	page;
	page_t*	next_page;
	ulint	next_page_no;
	rec_t*	next_rec;
	ulint	space;

	page = buf_frame_align(rec);
	if(page_get_supremum_rec(page) != rec){
		next_rec = page_rec_get_next(rec);
		if(page_get_supremum_rec(page) != next_rec)
			return next_rec;
	}

	next_page_no = btr_page_get_next(page, mtr);
	space = buf_frame_get_space_id(page);

	if(next_page_no != FIL_NULL){
		next_page = buf_page_get_with_no_latch(space, next_page_no, mtr);

		ut_ad((mtr_memo_contains(mtr, buf_block_align(next_page), MTR_MEMO_PAGE_S_FIX))
			|| (mtr_memo_contains(mtr, buf_block_align(next_page), MTR_MEMO_PAGE_X_FIX)));

		next_rec = page_rec_get_next(page_get_infimum_rec(page));

		return next_rec;
	}

	return NULL;
}

static void btr_page_create(page_t* page, dict_tree_t* tree, mtr_t* mtr)
{
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	/*���ڴ��й���һ��ҳ�ṹ,���ҹ����߼��ṹ��Ĭ�ϼ�¼*/
	page_create(page, mtr);

	btr_page_set_index_id(page, tree->id, mtr);
}

/*��insert buffer�Ͽ���һ��ҳ�ռ�*/
static page_t* btr_page_alloc_for_ibuf(dict_tree_t* tree, mtr_t* mtr)
{
	fil_addr_t	node_addr;
	page_t*		root;
	page_t*		new_page;

	root = btr_root_get(tree, mtr);
	/*���һ��page��Ҫ�洢��fil addr�ռ�*/
	node_addr = flst_get_first(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST, mtr);
	ut_a(node_addr.page != FIL_NULL);

	/*��buf�л��һ����ҳ�Ŀռ䣬���BUF�ʹ���һһ��Ӧ��*/
	new_page = buf_page_get(dict_tree_get_space(tree), node_addr.page, RW_X_LATCH, mtr);
	buf_page_dbg_add_level(new_page, SYNC_TREE_NODE_NEW);

	/*�ӿ��еĴ��̶�����ɾ����Ӧpage��node addr*/
	flst_remove(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST, new_page + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST_NODE, mtr);

	ut_ad(flst_validate(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST, mtr));

	return new_page;
}

/*B-TREE�Ϸ���ҳ�ռ�*/
page_t* btr_page_alloc(dict_index_t* tree, ulint hint_page_no, byte file_direction, ulint level, mtr_t* mtr)
{
	fseg_header_t*	seg_header;
	page_t*		root;
	page_t*		new_page;
	ulint		new_page_no;

	ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(tree), MTR_MEMO_X_LOCK));

	/*ֱ����ibuf�Ϸ���һ��ҳ*/
	if(tree->type & DICT_IBUF)
		return btr_page_alloc_for_ibuf(tree, mtr);

	/*���root�ڵ��Ӧ��ҳ*/
	root = btr_root_get(tree, mtr);
	if(level == 0)
		seg_header = root + PAGE_HEADER + PAGE_BTR_SEG_LEAF;
	else
		seg_header = root + PAGE_HEADER + PAGE_BTR_SEG_TOP;

	/*�ڶ�Ӧ�ı�ռ��з���һ��ҳID*/
	new_page_no = fseg_alloc_free_page_general(seg_header, hint_page_no, file_direction, TRUE, mtr);
	if(new_page_no == FIL_NULL) /*Ϊ��úϷ���ҳID*/
		return NULL;

	/*��IBUF�ϻ��һ��ҳ�ռ�,���ҳ��δ������ʼ��,��Ҫ����btr_page_create�����ʼ��*/
	new_page = buf_page_get(dict_tree_get_space(tree), new_page_no, RW_X_LATCH, mtr);

	return new_page;
}

/*���index��Ӧ��B-TREE��page������*/
ulint btr_get_size(dict_index_t* index, ulint flag)
{
	fseg_header_t*	seg_header;
	page_t*		root;
	ulint		n;
	ulint		dummy;
	mtr_t		mtr;

	mtr_start(&mtr);
	/*��������һ��s-latch*/
	mtr_s_lock(dict_tree_get_lock(index->tree), &mtr);
	
	root = btr_root_get(index->tree, &mtr);

	if(flag == BTR_N_LEAF_PAGES){ /*��ȡҶ�ӽڵ������ֱ�ӷ��ض�Ӧsegment��ҳ����������*/
		seg_header = root + PAGE_HEADER + PAGE_BTR_SEG_LEAF;
		fseg_n_reserved_pages(seg_header, &n, &mtr);
	}
	else if(flag == BTR_TOTAL_SIZE){ /*������еĵ���*/
		seg_header = root + PAGE_HEADER + PAGE_BTR_SEG_TOP;
		n = fseg_n_reserved_pages(seg_header, &dummy, &mtr);

		seg_header = root + PAGE_HEADER + PAGE_BTR_SEG_LEAF;
		n += fseg_n_reserved_pages(seg_header, &dummy, &mtr);
	}
	else{
		ut_a(0);
	}

	mtr_commit(&mtr);

	return n;
}

/*ibuf����page�ռ�*/
static void btr_page_free_for_ibuf(dict_tree_t* tree, page_t* page, mtr_t* mtr)
{
	page_t* root;

	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	root = btr_root_get(tree, mtr);
	/*ֱ����ӵ�root��ibuf free list���о���*/
	flst_add_first(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST, page + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST_NODE, mtr);

	ut_a(flst_validate(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST, mtr));
}

/*����page�ռ�*/
void btr_page_free_low(dict_tree_t* tree, page_t* page, ulint level, mtr_t* mtr)
{
	fseg_header_t*	seg_header;
	page_t*		root;
	ulint		space;
	ulint		page_no;

	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	buf_frame_modify_clock_inc(page);

	if(tree->type & DICT_IBUF){
		btr_page_free_for_ibuf(tree, page, mtr);
		return;
	}

	/*��ö�Ӧ��ռ��segmentλ����Ϣ*/
	root = btr_root_get(tree, mtr);
	if(level == 0)
		seg_header = root + PAGE_HEADER + PAGE_BTR_SEG_LEAF;
	else
		seg_header = root + PAGE_HEADER + PAGE_BTR_SEG_TOP;

	space = buf_frame_get_space_id(page);
	page_no = buf_frame_get_page_no(page);

	/*�ڱ�ռ����ͷ�ҳ*/
	fseg_free_page(seg_header, space, page_no, mtr);
}

/*�ͷ�page�ռ�*/
void btr_page_free(dict_index_t* tree, page_t* page, mtr_t* mtr)
{
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));
	level = btr_page_get_level(page, mtr);

	btr_page_free_low(tree, page, level, mtr);
}

/*����child node��fil addr*/
UNIV_INLINE void btr_node_ptr_set_child_page_no(rec_t* rec, ulint page_no, mtr_t* mtr)
{
	ulint	n_fields;
	byte*	field;
	ulint	len;

	ut_ad(0 < btr_page_get_level(buf_frame_align(rec), mtr));

	n_fields = rec_get_n_fields(rec);
	field = rec_get_nth_field(rec, n_fields - 1, &len);

	ut_ad(len == 4);

	/*��page noд�뵽rec�е����һ����*/
	mlog_write_ulint(field, page_no, MLOG_4BYTES, mtr);
}

/*ͨ��node ptr��¼��ö�Ӧ��pageҳ*/
static page_t* btr_node_ptr_get_child(rec_t* node_ptr, mtr_t* mtr)
{
	ulint	page_no;
	ulint	space;
	page_t*	page;

	space = buf_frame_get_space_id(node_ptr);
	page_no = btr_node_ptr_get_child_page_no(node_ptr);

	page = btr_page_get(space, page_no, RW_X_LATCH, mtr);
	
	return page;
}

/*����pageҲ���ڽڵ����һ��ڵ��node ptr ��¼*/
static rec_t* btr_page_get_father_for_rec(dict_tree_t* tree, page_t* page, rec_t* user_rec, mtr_t* mtr)
{
	mem_heap_t*	heap;
	dtuple_t*	tuple;
	btr_cur_t	cursor;
	rec_t*		node_ptr;

	ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(tree), MTR_MEMO_X_LOCK));
	ut_a(user_rec != page_get_supremum_rec(page));
	ut_a(user_rec != page_get_infimum_rec(page));

	ut_ad(dict_tree_get_page(tree) != buf_frame_get_page_no(page));

	heap = mem_heap_create(100);
	
	/*����һ��node ptr ��¼,��page noд��tuple��*/
	tuple = dict_tree_build_node_ptr(tree, user_sec, 0, heap, btr_page_get_level(page, mtr));

	btr_cur_search_to_nth_level(UT_LIST_GET_FIRST(tree->tree_indexes), btr_page_get_level(page, mtr) + 1,
		tuple, PAGE_CUR_LE, BTR_CONT_MODIFY_TREE, 
		&cursor, 0, mtr);

	node_ptr = btr_cur_get_rec(&cursor);

	/*node ptr�еĺ��ӽڵ�page no ��page�е�page no��ͬ��˵���д���,���������Ϣ*/
	if(btr_node_ptr_get_child_page_no(node_ptr) != buf_frame_get_page_no(page)){
		fprintf(stderr, "InnoDB: Dump of the child page:\n");
		buf_page_print(buf_frame_align(page));

		fprintf(stderr,"InnoDB: Dump of the parent page:\n");
		buf_page_print(buf_frame_align(node_ptr));

		fprintf(stderr,
			"InnoDB: Corruption of an index tree: table %s, index %s,\n"
			"InnoDB: father ptr page no %lu, child page no %lu\n",
			(UT_LIST_GET_FIRST(tree->tree_indexes))->table_name,
			(UT_LIST_GET_FIRST(tree->tree_indexes))->name,
			btr_node_ptr_get_child_page_no(node_ptr),
			buf_frame_get_page_no(page));

		page_rec_print(page_rec_get_next(page_get_infimum_rec(page)));
		page_rec_print(node_ptr);

		fprintf(stderr,
			"InnoDB: You should dump + drop + reimport the table to fix the\n"
			"InnoDB: corruption. If the crash happens at the database startup, see\n"
			"InnoDB: section 6.1 of http://www.innodb.com/ibman.html about forcing\n"
			"InnoDB: recovery. Then dump + drop + reimport.\n");
	}

	ut_a(btr_node_ptr_get_child_page_no(node_ptr) == buf_frame_get_page_no(page));

	mem_heap_free(heap);
}
/*���page��һ��ڵ�node ptr*/
static rec_t* btr_page_get_father_node_ptr(dict_tree_t* tree, page_t* page, mtr_t* mtr)
{
	return btr_page_get_father_for_rec(tree, page, page_rec_get_next(page_get_infimum_rec(page)), mtr);
}

/*����һ��btree��������root node ��page ID*/
ulint btr_create(ulint type, ulint space, dulint index_id, mtr_t* mtr)
{
	ulint			page_no;
	buf_frame_t*	ibuf_hdr_frame;
	buf_frame_t*	frame;
	page_t*			page;

	if(type & DICT_IBUF){
		/*����һ��fil segment*/
		ibuf_hdr_frame = fseg_create(space, 0, IBUF_HEADER + IBUF_TREE_SEG_HEADER, mtr);
		buf_page_dbg_add_level(ibuf_hdr_frame, SYNC_TREE_NODE_NEW);

		ut_ad(buf_frame_get_page_no(ibuf_hdr_frame) == IBUF_HEADER_PAGE_NO);

		/*��ibuf_hdr_frame�Ϸ���һ��ҳ*/
		page_no = fseg_alloc_free_page(ibuf_hdr_frame + IBUF_HEADER + IBUF_TREE_SEG_HEADER, 
			IBUF_TREE_ROOT_PAGE_NO, FSP_UP, mtr);

		frame = buf_page_get(space, page_no, RW_X_LATCH, mtr);
	}
	else{
		/*�ڱ�ռ��ϴ���һ��file segment����λ��root��PAGE_BTR_SEG_TOP��*/
		frame = fseg_create(space, 0, PAGE_HEADER + PAGE_BTR_SEG_TOP, mtr);
	}

	if(frame == NULL)
		return FIL_NULL;

	page_no = buf_frame_get_page_no(frame);
	buf_page_dbg_add_level(frame, SYNC_TREE_NODE_NEW);
	if(type & DICT_IBUF){
		ut_ad(page_no == IBUF_TREE_ROOT_PAGE_NO);
		/*��ʼ�����е�ibuf �����б����ڴ洢�ͷŵ�PAGE*/
		flst_init(frame + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST, mtr);
	}
	else{ /*�������ibuf��b tree,����һ��fil segment���ڴ洢leaf page*/
		fseg_create(space, page_no, PAGE_HEADER + PAGE_BTR_SEG_LEAF, mtr);
		buf_page_dbg_add_level(frame, SYNC_TREE_NODE_NEW);
	}

	/*��fil segment�ϴ���һ��page��ҳ�߼��ṹ*/
	page = page_create(frame, mtr);

	/*����page��index id*/
	btr_page_set_index_id(page, index_id, mtr);
	/*����LEVEL*/
	btr_page_set_level(page, 0, mtr);

	/*����Ҷ��page������ϵ*/
	btr_page_set_next(page, FIL_NULL, mtr);
	btr_page_set_prev(page, FIL_NULL, mtr);

	/*��ʲô�õģ���*/
	ibuf_reset_free_bits_with_type(type, page);

	ut_ad(page_get_max_insert_size(page, 2) > 2 * BTR_PAGE_MAX_REC_SIZE);

	return page_no;
}

/*�ͷ�����btree��page,root page�����ͷ�*/
void btr_free_but_not_root(ulint space, ulint root_page_no)
{
	ibool	finished;
	page_t*	root;
	mtr_t	mtr;

/*Ҷ�ӽڵ��ͷ�*/
leaf_loop:
	mtr_start(&mtr);
	/*�ͷ����root��Ӧ��segment��page*/
	root = btr_page_get(space, root_page_no, RW_X_LATCH, &mtr);
	finished = fseg_free_step(root + PAGE_HEADER + PAGE_BTR_SEG_LEAF, &mtr);
	mtr_commit(&mtr);

	if(!finished)
		goto leaf_loop;

/*֦�ɽڵ��ͷ�,�����ͷ�root��Ӧ��ͷҳ��ͷҳ����fsegment����Ϣ*/
top_loop:
	mtr_start(&mtr);

	root = btr_page_get(space, root_page_no, RW_X_LATCH, &mtr);	
	finished = fseg_free_step_not_header(root + PAGE_HEADER + PAGE_BTR_SEG_TOP, &mtr);

	mtr_commit(&mtr);
	if(!finished)
		goto top_loop;
}

/*�ͷ�btree��Ӧ��root page*/
void btr_free_root(ulint space, ulint root_page_no, mtr_t* mtr)
{
	ibool	finished;
	page_t*	root;

	root = btr_page_get(space, root_page_no, RW_X_LATCH, mtr);
	/*ɾ������Ӧ������Ӧhash����*/
	btr_search_drop_hash_index(root);

top_loop:
	finished = fseg_free_step(root + PAGE_HEADER + PAGE_BTR_SEG_TOP, mtr);
	if(!finished)
		goto top_loop;
}

static void btr_page_reorganize_low(ibool recovery, page_t* page, mtr_t* mtr)
{
	page_t*	new_page;
	ulint	log_mode;

	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));
	/*д��һ��page reorganize����־*/
	mlog_write_initial_log_record(page, MLOG_PAGE_REORGANIZE, mtr);

	/*�ر�mini transcation logģʽ*/
	log_mode = mtr_set_log_mode(mtr, MTR_LOG_NONE);

	new_page = buf_frame_alloc();
	buf_frame_copy(new_page, page);

	/*��������ڻָ�redo log�Ĺ��̣���ɾ������Ӧ�Ĺ�ϣ����*/
	if(!recovery)
		btr_search_drop_page_hash_index(page);

	/*������page�ռ��Ϲ���һ���µ�page�߼��ṹ*/
	page_create(page, mtr);

	/*������new page�еļ�¼ȫ��ת�Ƶ�page�ϣ���buf_frame_copy��ͬ�����Ӧ�����߼�����*/
	page_copy_rec_list_end_no_locks(page, new_page, page_get_infimum_rec(new_page), mtr);

	/*���ö���������Ӧ����������ID*/
	page_set_max_trx_id(page, page_get_max_trx_id(new_page));

	/*�������page��Ӧ��������*/
	if(!recovery)
		lock_move_reorganize_page(page, new_page);

	/*�ͷŵ���ʱ��ҳ*/
	buf_frame_free(new_page);

	/*�ָ�mini transcation logģʽ*/
	mtr_set_log_mode(mtr, log_mode);
}

/*�ڷ�redo����������page*/
void btr_page_reorganize(page_t* page, mtr_t* mtr)
{
	btr_page_reorganize_low(FALSE, page, mtr);
}

/*��redo log�ָ�����������page*/
byte* btr_parse_page_reorganize(byte* ptr, byte* end_ptr, page_t* page, mtr_t* mtr)
{
	ut_ad(ptr && end_ptr);

	if(page)
		btr_page_reorganize_low(TRUE, page, mtr);

	return ptr;
}

/*btree����page���*/
static void btr_page_empty(page_t* page, mtr_t* mtr)
{
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	btr_search_drop_page_hash_index(page);

	page_create(page, mtr);
}

rec_t* btr_root_raise_and_insert(btr_cur_t* cursor, dtuple_t* tuple, mtr_t* mtr)
{
	dict_tree_t*	tree;
	page_t*			root;
	page_t*			new_page;
	ulint			new_page_no;
	rec_t*			rec;
	mem_heap_t*		heap;
	dtuple_t*		node_ptr;
	ulint			level;	
	rec_t*			node_ptr_rec;
	page_cur_t*		page_cursor;

	root = btr_cur_get_page(cursor);
	tree = btr_cur_get_tree(cursor);

	ut_ad(dict_tree_get_page(tree) == buf_frame_get_page_no(root));
	ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(tree), MTR_MEMO_X_LOCK));
	ut_ad(mtr_memo_contains(mtr, buf_block_align(root), MTR_MEMO_PAGE_X_FIX));

	btr_search_drop_page_hash_index(root);
	/*�ڱ�ռ��з���һ��ҳ�ռ�*/
	new_page = btr_page_alloc(tree, 0, FSP_NO_DIR, btr_page_get_level(root, mtr), mtr);

	/*��btree�ϴ���һ����ҳ*/
	btr_page_create(new_page, tree, mtr);

	level = btr_page_get_level(root, mtr);

	/*���ò��*/
	btr_page_set_level(new_page, level, mtr);
	btr_page_set_level(root, level + 1, mtr);
	/*��root�����еļ�¼�Ƶ�new_page��*/
	page_move_rec_list_end(new_page, root, page_get_infimum_rec(root), mtr);
	/*������ת��*/
	lock_update_root_raise(new_page, root);

	heap = mem_heap_create(100);

	/*���new page�ĵ�һ����Ч��¼*/
	rec = page_rec_get_next(page_get_infimum_rec(new_page));
	new_page_no = buf_frame_get_page_no(new_page);

	node_ptr = dict_tree_build_node_ptr(tree, rec, new_page_no, heap, level);

	/*��root page���·���ռ�*/
	btr_page_reorganize(root, mtr);	

	page_cursor = btr_cur_get_page_cur(cursor);

	/*��new page�ĵ�һ����¼(node_ptr)���뵽root��*/
	page_cur_set_before_first(root, page_cursor);
	node_ptr_rec = page_cur_tuple_insert(page_cursor, node_ptr, mtr);
	ut_ad(node_ptr_rec);

	btr_set_min_rec_mark(node_ptr_rec, mtr);

	mem_heap_free(heap);

	ibuf_reset_free_bits(UT_LIST_GET_FIRST(tree->tree_indexes), new_page);
	/*���¶�λpage cursor��ָ���λ�ã�Ҳ��ı�btree cursor��btree_cursor����ָ��new page�ϵĶ�Ӧ��¼*/
	page_cur_search(new_page, tuple, PAGE_CUR_LE, page_cursor);

	return btr_page_split_and_insert(cursor, tuple, mtr);
}

/*�ж�ҳ�Ƿ��������ۼ����з��ѣ���ȷ�����ѵ�λ��,Ҳ����˵�������ļ�¼��Χ���ܶ������*/
ibool btr_page_get_split_rec_to_left(btr_cur_t* cursor, rec_t** split_rec)
{
	page_t*	page;
	rec_t*	insert_point;
	rec_t*	infimum;

	page = btr_cur_get_page(cursor);
	insert_point = btr_cur_get_rec(cursor);

	if ((page_header_get_ptr(page, PAGE_LAST_INSERT) == page_rec_get_next(insert_point))
		&& (page_header_get_field(page, PAGE_DIRECTION) == PAGE_LEFT)
		&& ((page_header_get_field(page, PAGE_N_DIRECTION) >= BTR_PAGE_SEQ_INSERT_LIMIT)
		|| (page_header_get_field(page, PAGE_N_DIRECTION) + 1>= page_get_n_recs(page)))) {

			infimum = page_get_infimum_rec(page);

			/*ֱ�Ӵ�insert point�����ѣ����insert point��infimum̫������������һ����¼������*/
			if ((infimum != insert_point) && (page_rec_get_next(infimum) != insert_point))
				*split_rec = insert_point;
			else
				*split_rec = page_rec_get_next(insert_point);

			return TRUE;
	}

	return FALSE;
}

/*�ж�ҳ�Ƿ�������Ҿۼ����з��ѣ���ȷ�����ѵ�λ��,Ҳ����˵�������ļ�¼��Χ���ܶ����ұ�*/
ibool btr_page_get_split_rec_to_right(btr_cur_t* cursor, rec_t** split_rec)
{
	page_t*	page;
	rec_t*	insert_point;
	rec_t*	supremum;

	page = btr_cur_get_page(cursor);
	insert_point = btr_cur_get_rec(cursor);

		if ((page_header_get_ptr(page, PAGE_LAST_INSERT) == insert_point)
	    && (page_header_get_field(page, PAGE_DIRECTION) == PAGE_RIGHT)
	    && ((page_header_get_field(page, PAGE_N_DIRECTION) >= BTR_PAGE_SEQ_INSERT_LIMIT)
	     	|| (page_header_get_field(page, PAGE_N_DIRECTION) + 1 >= page_get_n_recs(page)))) {

	     	supremum = page_get_supremum_rec(page);
	    
			/*��insert_point�����3����¼��ʼ����,����ֱ�Ӵ�insert point������*/
		if ((page_rec_get_next(insert_point) != supremum) && (page_rec_get_next(page_rec_get_next(insert_point)) != supremum)
		    && (page_rec_get_next(page_rec_get_next(page_rec_get_next(insert_point))) != supremum)) {

			/* If there are >= 3 user records up from the insert point, split all but 2 off */
			*split_rec = page_rec_get_next(page_rec_get_next(page_rec_get_next(insert_point)));
		} 
		else 
	     	*split_rec = NULL;

		return TRUE;
	}

	return FALSE;
}

/*ȷ����page �м���з���ִ��ʱ�ļ�¼λ�ã�һ�����������ʱ������ж�*/
static rec_t* btr_page_get_sure_split_rec(btr_cur_t* cursor, dtuple_t* tuple)
{
	page_t*	page;
	ulint	insert_size;
	ulint	free_space;
	ulint	total_data;
	ulint	total_n_recs;
	ulint	total_space;
	ulint	incl_data;
	rec_t*	ins_rec;
	rec_t*	rec;
	rec_t*	next_rec;
	ulint	n;

	page = btr_cur_get_page(cursor);
	/*��ò����¼���ڴ�����Ӧ��ռ�õĿռ�*/
	insert_size = rec_get_converted_size(tuple);
	/*���ҳ�ļ�¼���ÿռ�*/
	free_space = page_get_free_space_of_empty();

	total_data = page_get_data_size(page) + insert_size;
	total_n_recs = page_get_n_recs(page) + 1;
	ut_ad(total_n_recs >= 2);

	/*��������page�б�ռ�õĿռ���,��¼�ռ� + ��¼������slots*/
	total_space = total_data + page_dir_calc_reserved_space(total_n_recs);

	n = 0;
	incl_data = 0;
	/*tupleӦ�ò����λ��*/
	ins_rec = btr_cur_get_rec(cursor);
	rec = page_get_infimum_rec(page);

	for(;;){
		if(rec == ins_rec) /*����Ҫ���ѣ�tuple��¼����ֱ�Ӳ��뵽page*/
			rec = NULL;
		else if(rec == NULL)
			rec = page_rec_get_next(ins_rec);
		else
			rec = page_rec_get_next(rec);
	}

	/*�������tuple��ͳ�Ʋ��������ݳ��ȣ�Ȼ�����������Ƚ��з����ж�*/
	if(rec == NULL)
		incl_data += insert_size;
	else
		incl_data += rec_get_size(rec);

	n ++;

	/*��infimum��recλ�õ�ռ�ÿռ��ܺʹ�����ʹ�ÿռ�һ��,������rec��¼������*/
	if(incl_data + page_dir_calc_reserved_space(n) >= total_space / 2){
		/*ռ�ÿռ���ܺ�С��ҳ�Ŀ��ÿռ�*/
		if(incl_data + page_dir_calc_reserved_space(n) <= free_space){
			if(rec == ins_rec) /*��insert rec�����ѣ����ǲ���Ҫ����*/
				next_rec = NULL;
			else if(rec == NULL) /*��ins_rec����һ����¼������*/
				next_rec = page_rec_get_next(ins_rec);
			else
				next_rec = page_rec_get_next(rec);

			if(next_rec != page_get_supremum_rec(page))
				return next_rec;
		}

		return rec;
	}

	return NULL;
}

/*pageҳ�м���ѣ�split rec�Ƿ������Ϊ���ѵ�*/
static ibool btr_page_insert_fits(btr_cur_t* cursor, rec_t* split_rec, dtuple_t* tuple)
{
	page_t*	page;
	ulint	insert_size;
	ulint	free_space;
	ulint	total_data;
	ulint	total_n_recs;
	rec_t*	rec;
	rec_t*	end_rec;

	page = btr_cur_get_page(cursor);

	insert_size = rec_get_converted_size(tuple);
	free_space = page_get_free_space_of_empty();

	total_data   = page_get_data_size(page) + insert_size;
	total_n_recs = page_get_n_recs(page) + 1;

	/*δָ�����ѵ㣬��ҳ��һ����¼��cursorָ��ļ�¼����,ȷ�����ѵ�����*/
	if(split_rec == NULL){
		rec = page_rec_get_next(page_get_infimum_rec(page));
		end_rec = page_rec_get_next(btr_cur_get_rec(cursor));
	}
	else if(cmp_dtuple_rec(tuple, split_rec) >= 0){ /*ָ������λ�ã��ӿ�ʼ��ָ����¼λ�ã���tuple����split_rec֮���λ��*/
		rec = page_rec_get_next(page_get_infimum_rec(page));
		end_rec = split_rec;
	}
	else{ /*tuple ��split_rec֮ǰ*/
		rec = split_rec;
		end_rec = page_get_supremum_rec(page);
	}

	if (total_data + page_dir_calc_reserved_space(total_n_recs) <= free_space) 
		return TRUE;

	while(rec != end_rec){
		total_data -= rec_get_size(rec);
		total_n_recs --;

		/*�ܱ�֤ҳ�������Ӧ������*/
		if(total_data + page_dir_calc_reserved_space(total_n_recs) <= free_space)
			return TRUE;

		rec = page_rec_get_next(rec);
	}

	return FALSE;
}

/*��һ��tuple���뵽btree�еķ�Ҷ�ӽڵ�*/
void btr_insert_on_non_leaf_level(dict_tree_t* tree, ulint level, dtuple_t* tuple, mtr_t* mtr)
{
	big_rec_t*	dummy_big_rec;
	btr_cur_t	cursor;		
	ulint		err;
	rec_t*		rec;

	ut_ad(level > 0);

	btr_cur_search_to_nth_level(UT_LIST_GET_FIRST(tree->tree_indexes), level, tuple, 
		PAGE_CUR_LE, BTR_CONT_MODIFY_TREE, &cursor, 0, mtr);

	err = btr_cur_pessimistic_insert(BTR_NO_LOCKING_FLAG | BTR_KEEP_SYS_FLAG | BTR_NO_UNDO_LOG_FLAG, 
		&cursor, tuple, &rec, &dummy_big_rec, NULL, mtr);

	ut_a(err == DB_SUCCESS);
}

/*ҳ���м���Ѻ����޸ķ��Ѻ��ڸ���ҳ�ϵ�node ptr��¼��Ȼ������ֵ�ҳ֮���ǰ�������ϵ*/
static void btr_attach_half_pages(dict_tree_t* tree, page_t* page, rec_t* split_rec, page_t* new_page, ulint direction, mtr_t* mtr)
{
	ulint		space;
	rec_t*		node_ptr;
	page_t*		prev_page;
	page_t*		next_page;
	ulint		prev_page_no;
	ulint		next_page_no;
	ulint		level;
	page_t*		lower_page;
	page_t*		upper_page;
	ulint		lower_page_no;
	ulint		upper_page_no;
	dtuple_t*	node_ptr_upper;
	mem_heap_t* 	heap;

	/*��page��mtr log���ж�*/
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));
	ut_ad(mtr_memo_contains(mtr, buf_block_align(new_page), MTR_MEMO_PAGE_X_FIX));

	/*���ѷ���Ӹߵ���*/
	if(direction == FSP_DOWN){
		lower_page_no = buf_frame_get_page_no(new_page);
		upper_page_no = buf_frame_get_page_no(page);
		lower_page = new_page;
		upper_page = page;
		/*���ָ��page��node ptr*/
		node_ptr = btr_page_get_father_node_ptr(tree, page, mtr);
		/*��lower_page_no����Ϣ�滻ԭ����node ptr,ԭ����node ptr��ֵ����Ϊ��node ptr���뵽level + 1���ҳ��*/
		btr_node_ptr_set_child_page_no(node_ptr, lower_page_no, mtr);
	}
	else{ /*�ӵ͵���*/
		/*ԭ����node ptr���䣬�ٺ������һ�����ѳ�����ҳ��node ptr��level+1����*/
		lower_page_no = buf_frame_get_page_no(page);
		upper_page_no = buf_frame_get_page_no(new_page);
		lower_page = page;
		upper_page = new_page;
	}

	heap = mem_heap_create(100);
	level = btr_page_get_level(page, mtr);

	/*����һ�����ѳ�����node ptr*/
	node_ptr_upper = dict_tree_build_node_ptr(tree, split_rec, upper_page_no, heap, level);
	/*����¼���뵽����level + 1��*/
	btr_insert_on_non_leaf_level(tree, level + 1, node_ptr_upper, mtr);

	/*��÷���ǰpage��ǰ���ϵ*/
	prev_page_no = btr_page_get_prev(page, mtr);
	next_page_no = btr_page_get_next(page, mtr);
	space = buf_frame_get_space_id(page);

	/*�޸�prev page�е������ϵ*/
	if(prev_page_no != FIL_NULL){
		prev_page = btr_page_get(space, prev_page_no, RW_X_LATCH, mtr); /*���ǰһ��page*/
		btr_page_set_next(prev_page, lower_page_no, mtr);
	}

	/*�޸�next page�е������ϵ*/
	if(next_page_no != FIL_NULL){
		next_page = btr_page_get(space, next_page_no, RW_X_LATCH, mtr);
		btr_page_set_prev(next_page, upper_page_no, mtr);
	}

	/*�޸�lower page��ǰ���ϵ*/
	btr_page_set_prev(lower_page, prev_page_no, mtr);
	btr_page_set_next(lower_page, upper_page_no, mtr);
	btr_page_set_level(lower_page, level, mtr);
	/*�޸�page��ǰ���ϵ*/
	btr_page_set_prev(upper_page, lower_page_no, mtr);
	btr_page_set_next(upper_page, next_page_no, mtr);
	btr_page_set_level(upper_page, level, mtr);
}

rec_t* btr_page_split_and_insert(btr_cur_t* cursor, dtuple_t* tuple, mtr_t* mtr)
{
	dict_tree_t*	tree;
	page_t*		page;
	ulint		page_no;
	byte		direction;
	ulint		hint_page_no;
	page_t*		new_page;
	rec_t*		split_rec;
	page_t*		left_page;
	page_t*		right_page;
	page_t*		insert_page;
	page_cur_t*	page_cursor;
	rec_t*		first_rec;
	byte*		buf;
	rec_t*		move_limit;
	ibool		insert_will_fit;
	ulint		n_iterations = 0;
	rec_t*		rec;

func_start:
	tree = btr_cur_get_tree(cursor);

	ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(tree), MTR_MEMO_X_LOCK));
	ut_ad(rw_lock_own(dict_tree_get_lock(tree), RW_LOCK_EX));

	page = btr_cur_get_page(cursor);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));
	ut_ad(page_get_n_recs(page) >= 2);

	page_no = buf_frame_get_page_no(page);

	/*���split_rec == NULL,��ζ��tuple�ǲ��뵽upper page�ĵ�һ����¼��,half-page*/
	if(n_iterations > 0){
		direction = FSP_UP;
		hint_page_no = page_no + 1;
		/*ȷ�����ѵ�λ��*/
		split_rec = btr_page_get_sure_split_rec(cursor, tuple);
	}
	else if(btr_page_get_split_rec_to_right(cursor, &split_rec)){ /*���Ҿۼ�����*/
		direction = FSP_UP;
		hint_page_no = page_no + 1;
	}
	else if(btr_page_get_split_rec_to_left(cursor, &split_rec)){ /*����ۼ�����*/
		direction = FSP_DOWN;
		hint_page_no = page_no - 1;
	}
	else{
		direction = FSP_UP;
		hint_page_no = page_no + 1; /*��ռ�����ҳ����ʼҳ��,��Ϊ����ҳ��һ��Ԥ������*/
		/*���м����,Ԥ���м��¼��Ϊ���ѵ�*/
		split_rec = page_get_middle_rec(page);
	}

	/*����һ����page�ռ䲢��ʼ��page*/
	new_page = btr_page_alloc(tree, hint_page_no, direction, btr_page_get_level(page, mtr), mtr);
	btr_page_create(new_page, tree, mtr);

	/*ȷ����λpage�ĵ�һ����¼*/
	if(split_rec != NULL){
		first_rec = split_rec;
		move_limit = split_rec;
	}
	else{
		buf = mem_alloc(rec_get_converted_size(tuple));
		/*��tupleת����upper page�ĵ�һ����¼*/
		first_rec = rec_convert_dtuple_to_rec(buf, tuple);
		move_limit = page_rec_get_next(btr_cur_get_rec(cursor));
	}

	/*����page������ϵ���޸�*/
	btr_attach_half_pages(tree, page, first_rec, new_page, direction, mtr);
	if(split_rec == NULL)
		mem_free(buf);

	/*�ٴ�ȷ���Ƿ������split rec�ϲ���tuple��������,ȷ�������Ƿ��ʺ�*/
	insert_will_fit = btr_page_insert_fits(cursor, split_rec, tuple);
	if(insert_will_fit && (btr_page_get_level(page, mtr) == 0)){ /*leaf page��*/
		mtr_memo_release(mtr, dict_tree_get_lock(tree), MTR_MEMO_X_LOCK);
	}

	/*��¼��������ת��*/
	if(direction == FSP_DOWN){ /*��¼ת��,����ۼ�*/
		page_move_rec_list_start(new_page, page, move_limit, mtr);
		left_page = new_page;
		right_page = page;
		/*�������ļ̳к�ת��*/
		lock_update_split_left(right_page, left_page);
	}
	else{ /*���Ҿۼ�����*/
		page_move_rec_list_end(new_page, page, move_limit, mtr);
		left_page = page;
		right_page = new_page;

		lock_update_split_right(right_page, left_page);
	}

	/*ȷ���¼�¼�����ҳ,��Ϊҳ������*/
	if(split_rec == NULL)
		insert_page = right_page;
	else if(cmp_tuple_rec(tuple, first_rec) >= 0)
		insert_page = right_page;
	else
		insert_page = left_page;

	/*����tuple����*/
	page_cursor = btr_cur_get_page_cur(cursor);
	page_cur_search(insert_page, tuple, PAGE_CUR_LE, page_cursor);
	rec = page_cur_tuple_insert(page_cursor, tuple, mtr);
	if(rec != NULL){
		ibuf_update_free_bits_for_two_pages_low(cursor->index, left_page, right_page, mtr);
		return rec;
	}

	/*���tuple�����ǲ��ʺϵģ�����reorganization*/
	btr_page_reorganize(insert_page, mtr);
	page_cur_search(insert_page, tuple, PAGE_CUR_LE, page_cursor);
	rec = page_cur_tuple_insert(page_cursor, tuple, mtr);
	if(rec == NULL){ /*������ٴγ��Բ��룬���ǲ��ʺϣ�����Ҫ�����ٴη���*/
		ibuf_reset_free_bits(cursor->index, new_page);
		n_iterations++;
		ut_ad(n_iterations < 2);
		ut_ad(!insert_will_fit);

		goto func_start;
	}

	ibuf_update_free_bits_for_two_pages_low(cursor->index, left_page, right_page, mtr);

	ut_ad(page_validate(left_page, UT_LIST_GET_FIRST(tree->tree_indexes)));
	ut_ad(page_validate(right_page, UT_LIST_GET_FIRST(tree->tree_indexes)));

	return rec;
}

/*���page��ǰ��ҳ������ϵ*/
static void btr_level_list_remove(dict_tree_t* tree, page_t* page, mtr_t* mtr)
{
	ulint	space;
	ulint	prev_page_no;
	page_t*	prev_page;
	ulint	next_page_no;
	page_t*	next_page;

	ut_ad(tree && page && mtr);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	prev_page_no = btr_page_get_prev(page, mtr);
	next_page_no = btr_page_get_next(page, mtr);
	space = buf_frame_get_space_id(page);

	/*����prev page�Ĺ�����ϵ*/
	if(prev_page_no != FIL_NULL){
		prev_page = btr_page_get(space, prev_page_no, RW_X_LATCH, mtr);
		btr_page_set_next(prev_page, next_page_no, mtr);
	}
	/*����next page�Ĺ�����ϵ*/
	if(next_page_no != FIL_NULL){
		prev_page = btr_page_get(space, prev_page_no, RW_X_LATCH, mtr);
		btr_page_set_next(prev_page, next_page_no, mtr);
	}
}

/*д��һ����¼ƫ��λ�õ�redo log��*/
UNIV_INLINE void btr_set_min_rec_mark_log(rec_t* rec, mtr_t* mtr)
{
	mlog_write_initial_log_record(rec, MLOG_REC_MIN_MARK, mtr);
	/*д��һ����¼���page��ƫ��*/
	mlog_catenate_ulint(mtr, rec - buf_frame_align(rec), MLOG_2BYTES);
}

/*����һ��MLOG_REC_MIN_MARK���͵�redo log*/
byte* btr_parse_set_min_rec_mark(byte* ptr, byte* end_ptr, page_t* page, mtr_t* mtr)
{
	rec_t* rec;
	if(end_ptr < ptr + 2)
		return NULL;

	if(page){
		rec = page + mach_read_from_2(ptr);
		btr_set_min_rec_mark(rec, mtr);
	}

	return ptr + 2;
}

/*����һ��minimum��¼��ʶ*/
void btr_set_min_rec_mark(rec_t* rec, mtr_t* mtr)
{
	ulint	info_bits;

	info_bits = rec_get_info_bits(rec);

	rec_set_info_bits(rec, info_bits | REC_INFO_MIN_REC_FLAG);

	btr_set_min_rec_mark_log(rec, mtr);
}

/*ɾ��һ��page��node ptr��¼*/
void btr_node_ptr_delete(dict_tree_t* tree, page_t* page, mtr_t* mtr)
{
	rec_t*		node_ptr;
	btr_cur_t	cursor;
	ibool		compressed;
	ulint		err;

	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	node_ptr = btr_page_get_father_node_ptr(tree, page, mtr);

	btr_cur_position(UT_LIST_GET_FIRST(tree->tree_indexes), node_ptr, &cursor);

	compressed = btr_cur_pessimistic_delete(&err, TRUE, &cursor, FALSE, mtr);
	
	ut_a(err == DB_SUCCESS);
	if(!compressed)
		btr_cur_compress_if_useful(&cursor, mtr);
}

/*��page�еļ�¼ȫ���ϲ���father page�ϣ�����btree �Ĳ��*/
static void btr_lift_page_up(dict_tree_t* tree, page_t* page, mtr_t* mtr)
{
	rec_t*	node_ptr;
	page_t*	father_page;
	ulint	page_level;

	ut_ad(btr_page_get_prev(page, mtr) == FIL_NULL);
	ut_ad(btr_page_get_next(page, mtr) == FIL_NULL);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	/*���page�ĸ��׽ڵ��Ӧ��PAGE*/
	node_ptr = btr_page_get_father_node_ptr(tree, page, mtr);
	father_page = buf_frame_algin(node_ptr);

	page_level = btr_page_get_level(page, mtr);
	/*ȡ��page��hash����*/
	btr_search_drop_page_hash_index(page);

	/*��ո���page*/
	btr_page_empty(father_page, mtr);
	/*��¼ת��*/
	page_copy_rec_list_end(father_page, page, page_get_infimum_rec(page), mtr);
	/*������ת��*/
	lock_update_copy_and_discard(father_page, page);

	btr_page_set_level(father_page, page_level, mtr);

	btr_page_free(tree, page, mtr);

	ibuf_reset_free_bits(UT_LIST_GET_FIRST(tree->tree_indexes), father_page);

	ut_ad(page_validate(father_page, UT_LIST_GET_FIRST(tree->tree_indexes)));
	ut_ad(btr_check_node_ptr(tree, father_page, mtr));
}

void btr_compress(btr_cur_t* cursor, mtr_t* mtr)
{
	dict_tree_t*	tree;
	ulint		space;
	ulint		left_page_no;
	ulint		right_page_no;
	page_t*		merge_page;
	page_t*		father_page;
	ibool		is_left;
	page_t*		page;
	rec_t*		orig_pred;
	rec_t*		orig_succ;
	rec_t*		node_ptr;
	ulint		data_size;
	ulint		n_recs;
	ulint		max_ins_size;
	ulint		max_ins_size_reorg;
	ulint		level;

	ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(tree), MTR_MEMO_X_LOCK));
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	level = btr_page_get_level(page, mtr);
	space = dict_tree_get_space(tree);

	left_page_no = btr_page_get_prev(page, mtr);
	right_page_no = btr_page_get_next(page, mtr);

	node_ptr = btr_page_get_father_node_ptr(tree, page, mtr);
	father_page = buf_frame_align(node_ptr);

	if(left_page_no != FIL_NULL){
		is_left = TRUE;
		merge_page = btr_page_get(space, left_page_no, RW_X_LATCH, mtr);
	}
	else if(right_page_no != FIL_NULL){
		is_left = FALSE;
		merge_page = btr_page_get(space, right_page_no, RW_X_LATCH, mtr);
	}
	else{ /*��һ��ֻ��1��page,ֱ�Ӻϲ�����һ����*/
		btr_lift_page_up(tree, page, mtr);
		return ;
	}

	n_recs = page_get_n_recs(page);
	data_size = page_get_data_size(page);

	/*������ɲ������ݵĿռ�*/
	max_ins_size_reorg = page_get_max_insert_size_after_reorganize(merge_page, n_recs);
	if(data_size > max_ins_size_reorg) /*���ܺϲ�,merge page�ռ䲻��*/
		return ;

	ut_ad(page_validate(merge_page, cursor->index));

	max_ins_size = page_get_max_insert_size(merge_page, n_recs);
	if (data_size > max_ins_size) { /*�������������ܺϲ�,������ɾ�������õ��кͼ�¼*/
		btr_page_reorganize(merge_page, mtr);

		ut_ad(page_validate(merge_page, cursor->index));
		ut_ad(page_get_max_insert_size(merge_page, n_recs) == max_ins_size_reorg);
	}

	btr_search_drop_page_hash_index(page);

	btr_level_list_remove(tree, page, mtr);
	if(is_left)
		btr_node_ptr_delete(tree, page, mtr);
	else{
		/*ʵ������right_page_no������page��λ��*/
		btr_node_ptr_set_child_page_no(node_ptr, right_page_no, mtr); /*����node ptr��Ӧ��page no*/
		btr_node_ptr_delete(tree, merge_page, mtr); /*ɾ��merge page��Ӧ��node ptr��¼*/
	}

	/*��¼�������ĺϲ�*/
	if(is_left){
		orig_pred = page_rec_get_prev( page_get_supremum_rec(merge_page));
		page_copy_rec_list_start(merge_page, page, page_get_supremum_rec(page), mtr);

		lock_update_merge_left(merge_page, orig_pred, page);
	}
	else{
		orig_succ = page_rec_get_next( page_get_infimum_rec(merge_page));
		page_copy_rec_list_end(merge_page, page, page_get_infimum_rec(page), mtr);

		lock_update_merge_right(orig_succ, page);
	}

	ibuf_update_free_bits_if_full(cursor->index, merge_page, UNIV_PAGE_SIZE, ULINT_UNDEFINED);

	btr_page_free(tree, page, mtr);

	ut_ad(btr_check_node_ptr(tree, merge_page, mtr));
}

/*����һ��page,���������õ���һ��ڵ���*/
static void btr_discard_only_page_on_level(dict_tree_t* tree, page_t* page, mtr_t* mtr)
{
	rec_t*	node_ptr;
	page_t*	father_page;
	ulint	page_level;

	ut_ad(btr_page_get_prev(page, mtr) == FIL_NULL);
	ut_ad(btr_page_get_next(page, mtr) == FIL_NULL);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	btr_search_drop_page_hash_index(page);

	node_ptr = btr_page_get_father_node_ptr(tree, page, mtr);
	father_page = buf_frame_align(node_ptr);

	page_level = btr_page_get_level(page, mtr);

	/*��page������ȫ��ת�Ƶ�father page��*/
	lock_update_discard(page_get_supremum_rec(father_page), page);

	btr_page_set_level(father_page, mtr);

	btr_page_free(tree, page, mtr);

	if(buf_frame_get_page_no(father_page) == dict_tree_get_page(tree)){ /*�Ѿ���root��*/
		btr_page_empty(father_page, mtr);
		ibuf_reset_free_bits(UT_LIST_GET_FIRST(tree->tree_indexes), father_page);
	}
	else{
		ut_ad(page_get_n_recs(father_page) == 1);
		btr_discard_only_page_on_level(tree, father_page, mtr);
	}
}

void btr_discard_page(btr_cur_t* cursor, mtr_t* mtr)
{
	dict_tree_t*	tree;
	ulint		space;
	ulint		left_page_no;
	ulint		right_page_no;
	page_t*		merge_page;
	ibool		is_left;
	page_t*		page;
	rec_t*		node_ptr;

	page = btr_cur_get_page(cursor);
	tree = btr_cur_get_tree(cursor);

	ut_ad(dict_tree_get_page(tree) != buf_frame_get_page_no(page));
	ut_ad(mtr_memo_contains(mtr, dict_tree_get_lock(tree), MTR_MEMO_X_LOCK));
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	space = dict_tree_get_space(tree);

	left_page_no = btr_page_get_prev(page, mtr);
	right_page_no = btr_page_get_next(page, mtr);

	if(left_page_no != FIL_NULL){
		is_left = TRUE;
		merge_page = btr_page_get(space, left_page_no, RW_X_LATCH, mtr);
	}
	else if(right_page_no != FIL_NULL){
		is_left = FALSE;
		merge_page = btr_page_get(space, right_page_no, RW_X_LATCH, mtr);
	}
	else{/*ֻ��һ��ҳ��ֱ�ӻغϲ���root��*/
		btr_discard_only_page_on_level(tree, page, mtr);
		return;
	}

	btr_search_drop_page_hash_index(page);

	if(left_page_no == FIL_NULL && btr_page_get_level(page, mtr) > 0){
		node_ptr = page_rec_get_next(page_get_infimum_rec(merge_page));
		ut_ad(node_ptr != page_get_supremum_rec(merge_page));
		/*��ʶ��Сrow key�ļ�¼ҳ*/
		btr_set_min_rec_mark(node_ptr, mtr);
	}
	/*��btree��ɾ��page*/
	btr_level_list_remove(tree, page, mtr);

	if(is_left)
		lock_update_discard(page_get_supremum_rec(merge_page), page);
	else
		lock_update_discard(page_rec_get_next(page_get_infimum_rec(merge_page)), page);

	btr_page_free(tree, page, mtr);

	ut_ad(btr_check_node_ptr(tree, merge_page, mtr));
}

void
btr_print_size(
/*===========*/
	dict_tree_t*	tree)	/* in: index tree */
{
	page_t*		root;
	fseg_header_t*	seg;
	mtr_t		mtr;

	if (tree->type & DICT_IBUF) {
		printf(
	"Sorry, cannot print info of an ibuf tree: use ibuf functions\n");

		return;
	}

	mtr_start(&mtr);
	
	root = btr_root_get(tree, &mtr);

	seg = root + PAGE_HEADER + PAGE_BTR_SEG_TOP;

	printf("INFO OF THE NON-LEAF PAGE SEGMENT\n");
	fseg_print(seg, &mtr);

	if (!(tree->type & DICT_UNIVERSAL)) {

		seg = root + PAGE_HEADER + PAGE_BTR_SEG_LEAF;

		printf("INFO OF THE LEAF PAGE SEGMENT\n");
		fseg_print(seg, &mtr);
	}

	mtr_commit(&mtr); 	
}

/****************************************************************
Prints recursively index tree pages. */
static
void
btr_print_recursive(
/*================*/
	dict_tree_t*	tree,	/* in: index tree */
	page_t*		page,	/* in: index page */
	ulint		width,	/* in: print this many entries from start
				and end */
	mtr_t*		mtr)	/* in: mtr */
{
	page_cur_t	cursor;
	ulint		n_recs;
	ulint		i	= 0;
	mtr_t		mtr2;
	rec_t*		node_ptr;
	page_t*		child;
	
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page),
							MTR_MEMO_PAGE_X_FIX));
	printf("NODE ON LEVEL %lu page number %lu\n",
		btr_page_get_level(page, mtr), buf_frame_get_page_no(page));
	
	page_print(page, width, width);
	
	n_recs = page_get_n_recs(page);
	
	page_cur_set_before_first(page, &cursor);
	page_cur_move_to_next(&cursor);

	while (!page_cur_is_after_last(&cursor)) {

		if (0 == btr_page_get_level(page, mtr)) {

			/* If this is the leaf level, do nothing */

		} else if ((i <= width) || (i >= n_recs - width)) {

			mtr_start(&mtr2);

			node_ptr = page_cur_get_rec(&cursor);

			child = btr_node_ptr_get_child(node_ptr, &mtr2);

			btr_print_recursive(tree, child, width, &mtr2);
			mtr_commit(&mtr2);
		}

		page_cur_move_to_next(&cursor);
		i++;
	}
}

/******************************************************************
Prints directories and other info of all nodes in the tree. */

void
btr_print_tree(
	dict_tree_t*	tree,	/* in: tree */
	ulint		width)	/* in: print this many entries from start
				and end */
{
	mtr_t	mtr;
	page_t*	root;

	printf("--------------------------\n");
	printf("INDEX TREE PRINT\n");

	mtr_start(&mtr);

	root = btr_root_get(tree, &mtr);

	btr_print_recursive(tree, root, width, &mtr);

	mtr_commit(&mtr);

	btr_validate_tree(tree);
}

/****************************************************************
Checks that the node pointer to a page is appropriate. */

ibool
btr_check_node_ptr(
	dict_tree_t*	tree,	/* in: index tree */
	page_t*		page,	/* in: index page */
	mtr_t*		mtr)	/* in: mtr */
{
	mem_heap_t*	heap;
	rec_t*		node_ptr;
	dtuple_t*	node_ptr_tuple;

	ut_ad(mtr_memo_contains(mtr, buf_block_align(page),
							MTR_MEMO_PAGE_X_FIX));
	if (dict_tree_get_page(tree) == buf_frame_get_page_no(page)) {

		return(TRUE);
	}

	node_ptr = btr_page_get_father_node_ptr(tree, page, mtr);
 
	if (btr_page_get_level(page, mtr) == 0) {

		return(TRUE);
	}
	
	heap = mem_heap_create(256);
		
	node_ptr_tuple = dict_tree_build_node_ptr(
				tree,
				page_rec_get_next(page_get_infimum_rec(page)),
				0, heap, btr_page_get_level(page, mtr));
				
	ut_a(cmp_dtuple_rec(node_ptr_tuple, node_ptr) == 0);

	mem_heap_free(heap);

	return(TRUE);
}

/****************************************************************
Checks the size and number of fields in a record based on the definition of
the index. */
static
ibool
btr_index_rec_validate(
/*====================*/
				/* out: TRUE if ok */
	rec_t*		rec,	/* in: index record */
	dict_index_t*	index)	/* in: index */
{
	dtype_t* type;
	byte*	data;
	ulint	len;
	ulint	n;
	ulint	i;
	char	err_buf[1000];
	
	n = dict_index_get_n_fields(index);

	if (rec_get_n_fields(rec) != n) {
		fprintf(stderr, "Record has %lu fields, should have %lu\n",
				rec_get_n_fields(rec), n);

		rec_sprintf(err_buf, 900, rec);
	  	fprintf(stderr, "InnoDB: record %s\n", err_buf);

		return(FALSE);
	}

	for (i = 0; i < n; i++) {
		data = rec_get_nth_field(rec, i, &len);

		type = dict_index_get_nth_type(index, i);
		
		if (len != UNIV_SQL_NULL && dtype_is_fixed_size(type)
		    && len != dtype_get_fixed_size(type)) {
			fprintf(stderr,
			"Record field %lu len is %lu, should be %lu\n",
				i, len, dtype_get_fixed_size(type));

			rec_sprintf(err_buf, 900, rec);
	  		fprintf(stderr, "InnoDB: record %s\n", err_buf);

			return(FALSE);
		}
	}

	return(TRUE);			
}

/****************************************************************
Checks the size and number of fields in records based on the definition of
the index. */
static ibool btr_index_page_validate(
/*====================*/
				/* out: TRUE if ok */
	page_t*		page,	/* in: index page */
	dict_index_t*	index)	/* in: index */
{
	rec_t*		rec;
	page_cur_t 	cur;
	ibool		ret	= TRUE;
	
	page_cur_set_before_first(page, &cur);
	page_cur_move_to_next(&cur);

	for (;;) {
		rec = (&cur)->rec;

		if (page_cur_is_after_last(&cur)) {
			break;
		}

		if (!btr_index_rec_validate(rec, index)) {

			ret = FALSE;
		}

		page_cur_move_to_next(&cur);
	}

	return(ret);	
}

/****************************************************************
Validates index tree level. */
static ibool btr_validate_level(
	dict_tree_t*	tree,	/* in: index tree */
	ulint		level)	/* in: level number */
{
	ulint		space;
	page_t*		page;
	page_t*		right_page;
	page_t*		father_page;
	page_t*		right_father_page;
	rec_t*		node_ptr;
	rec_t*		right_node_ptr;
	ulint		right_page_no;
	ulint		left_page_no;
	page_cur_t	cursor;
	mem_heap_t*	heap;
	dtuple_t*	node_ptr_tuple;
	ibool		ret	= TRUE;
	dict_index_t*	index;
	mtr_t		mtr;
	char		err_buf[1000];
	
	mtr_start(&mtr);

	mtr_x_lock(dict_tree_get_lock(tree), &mtr);
	
	page = btr_root_get(tree, &mtr);

	space = buf_frame_get_space_id(page);

	while (level != btr_page_get_level(page, &mtr)) {

		ut_a(btr_page_get_level(page, &mtr) > 0);

		page_cur_set_before_first(page, &cursor);
		page_cur_move_to_next(&cursor);

		node_ptr = page_cur_get_rec(&cursor);
		page = btr_node_ptr_get_child(node_ptr, &mtr);
	}

	index = UT_LIST_GET_FIRST(tree->tree_indexes);
	
	/* Now we are on the desired level */
loop:
	mtr_x_lock(dict_tree_get_lock(tree), &mtr);

	/* Check ordering etc. of records */

	if (!page_validate(page, index)) {
		fprintf(stderr, "Error in page %lu in index %s\n",
			buf_frame_get_page_no(page), index->name);

		ret = FALSE;
	}

	if (level == 0) {
		if (!btr_index_page_validate(page, index)) {
 			fprintf(stderr,
				"Error in page %lu in index %s, level %lu\n",
				buf_frame_get_page_no(page), index->name,
								level);
			ret = FALSE;
		}
	}
	
	ut_a(btr_page_get_level(page, &mtr) == level);

	right_page_no = btr_page_get_next(page, &mtr);
	left_page_no = btr_page_get_prev(page, &mtr);

	ut_a((page_get_n_recs(page) > 0)
	     || ((level == 0) &&
		  (buf_frame_get_page_no(page) == dict_tree_get_page(tree))));

	if (right_page_no != FIL_NULL) {

		right_page = btr_page_get(space, right_page_no, RW_X_LATCH,
									&mtr);
		if (cmp_rec_rec(page_rec_get_prev(page_get_supremum_rec(page)),
			page_rec_get_next(page_get_infimum_rec(right_page)),
			UT_LIST_GET_FIRST(tree->tree_indexes)) >= 0) {

 			fprintf(stderr,
			"InnoDB: Error on pages %lu and %lu in index %s\n",
				buf_frame_get_page_no(page),
				right_page_no,
				index->name);

			fprintf(stderr,
			"InnoDB: records in wrong order on adjacent pages\n");

			rec_sprintf(err_buf, 900,
				page_rec_get_prev(page_get_supremum_rec(page)));
	  		fprintf(stderr, "InnoDB: record %s\n", err_buf);

			rec_sprintf(err_buf, 900,
			page_rec_get_next(page_get_infimum_rec(right_page)));
	  		fprintf(stderr, "InnoDB: record %s\n", err_buf);

	  		ret = FALSE;
	  	}
	}
	
	if (level > 0 && left_page_no == FIL_NULL) {
		ut_a(REC_INFO_MIN_REC_FLAG & rec_get_info_bits(
			page_rec_get_next(page_get_infimum_rec(page))));
	}

	if (buf_frame_get_page_no(page) != dict_tree_get_page(tree)) {

		/* Check father node pointers */
	
		node_ptr = btr_page_get_father_node_ptr(tree, page, &mtr);

		if (btr_node_ptr_get_child_page_no(node_ptr) !=
						buf_frame_get_page_no(page)
		   || node_ptr != btr_page_get_father_for_rec(tree, page,
		   	page_rec_get_prev(page_get_supremum_rec(page)),
								&mtr)) {
 			fprintf(stderr,
			"InnoDB: Error on page %lu in index %s\n",
				buf_frame_get_page_no(page),
				index->name);

			fprintf(stderr,
			"InnoDB: node pointer to the page is wrong\n");

			rec_sprintf(err_buf, 900, node_ptr);
				
	  		fprintf(stderr, "InnoDB: node ptr %s\n", err_buf);

			fprintf(stderr,
				"InnoDB: node ptr child page n:o %lu\n",
				btr_node_ptr_get_child_page_no(node_ptr));

			rec_sprintf(err_buf, 900,
			 	btr_page_get_father_for_rec(tree, page,
		   	 	page_rec_get_prev(page_get_supremum_rec(page)),
					&mtr));

	  		fprintf(stderr, "InnoDB: record on page %s\n",
								err_buf);
		   	ret = FALSE;

		   	goto node_ptr_fails;
		}

		father_page = buf_frame_align(node_ptr);

		if (btr_page_get_level(page, &mtr) > 0) {
			heap = mem_heap_create(256);
		
			node_ptr_tuple = dict_tree_build_node_ptr(
					tree,
					page_rec_get_next(
						page_get_infimum_rec(page)),
						0, heap,
       					btr_page_get_level(page, &mtr));

			if (cmp_dtuple_rec(node_ptr_tuple, node_ptr) != 0) {

	 			fprintf(stderr,
				  "InnoDB: Error on page %lu in index %s\n",
					buf_frame_get_page_no(page),
					index->name);

	  			fprintf(stderr,
                	"InnoDB: Error: node ptrs differ on levels > 0\n");
							
				rec_sprintf(err_buf, 900, node_ptr);
				
	  			fprintf(stderr, "InnoDB: node ptr %s\n",
								err_buf);
				rec_sprintf(err_buf, 900,
				  page_rec_get_next(
					page_get_infimum_rec(page)));
				
	  			fprintf(stderr, "InnoDB: first rec %s\n",
								err_buf);
		   		ret = FALSE;
				mem_heap_free(heap);

		   		goto node_ptr_fails;
			}

			mem_heap_free(heap);
		}

		if (left_page_no == FIL_NULL) {
			ut_a(node_ptr == page_rec_get_next(
					page_get_infimum_rec(father_page)));
			ut_a(btr_page_get_prev(father_page, &mtr) == FIL_NULL);
		}

		if (right_page_no == FIL_NULL) {
			ut_a(node_ptr == page_rec_get_prev(
					page_get_supremum_rec(father_page)));
			ut_a(btr_page_get_next(father_page, &mtr) == FIL_NULL);
		}

		if (right_page_no != FIL_NULL) {

			right_node_ptr = btr_page_get_father_node_ptr(tree,
							right_page, &mtr);
			if (page_rec_get_next(node_ptr) !=
					page_get_supremum_rec(father_page)) {

				if (right_node_ptr !=
						page_rec_get_next(node_ptr)) {
					ret = FALSE;
					fprintf(stderr,
			"InnoDB: node pointer to the right page is wrong\n");

	 				fprintf(stderr,
				  "InnoDB: Error on page %lu in index %s\n",
					buf_frame_get_page_no(page),
					index->name);
				}
			} else {
				right_father_page = buf_frame_align(
							right_node_ptr);
							
				if (right_node_ptr != page_rec_get_next(
					   		page_get_infimum_rec(
							right_father_page))) {
					ret = FALSE;
					fprintf(stderr,
			"InnoDB: node pointer 2 to the right page is wrong\n");

	 				fprintf(stderr,
				  "InnoDB: Error on page %lu in index %s\n",
					buf_frame_get_page_no(page),
					index->name);
				}

				if (buf_frame_get_page_no(right_father_page)
				   != btr_page_get_next(father_page, &mtr)) {

					ret = FALSE;
					fprintf(stderr,
			"InnoDB: node pointer 3 to the right page is wrong\n");

	 				fprintf(stderr,
				  "InnoDB: Error on page %lu in index %s\n",
					buf_frame_get_page_no(page),
					index->name);
				}
			}					
		}
	}

node_ptr_fails:
	mtr_commit(&mtr);

	if (right_page_no != FIL_NULL) {
		mtr_start(&mtr);
	
		page = btr_page_get(space, right_page_no, RW_X_LATCH, &mtr);

		goto loop;
	}

	return(ret);
}

/******************************************************************
Checks the consistency of an index tree. */

ibool
btr_validate_tree(dict_tree_t*	tree)	/* in: tree */
{
	mtr_t	mtr;
	page_t*	root;
	ulint	i;
	ulint	n;

	mtr_start(&mtr);
	mtr_x_lock(dict_tree_get_lock(tree), &mtr);

	root = btr_root_get(tree, &mtr);
	n = btr_page_get_level(root, &mtr);

	for (i = 0; i <= n; i++) {
		
		if (!btr_validate_level(tree, n - i)) {

			mtr_commit(&mtr);

			return(FALSE);
		}
	}

	mtr_commit(&mtr);

	return(TRUE);
}
