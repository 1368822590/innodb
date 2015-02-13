#include "ibuf0ibuf.h"

#include "buf0buf.h"
#include "buf0rea.h"
#include "fsp0fsp.h"
#include "trx0sys.h"
#include "fil0fil.h"
#include "thr0loc.h"
#include "rem0rec.h"
#include "btr0cur.h"
#include "btr0pcur.h"
#include "btr0btr.h"
#include "sync0sync.h"
#include "dict0boot.h"
#include "fut0lst.h"
#include "lock0lock.h"
#include "log0recv.h"
#include "que0que.h"


/* Buffer pool size per the maximum insert buffer size */
#define IBUF_POOL_SIZE_PER_MAX_SIZE	2

/*ibuf��ȫ�ֶ���*/
ibuf_t* ibuf			= NULL;

ulint ibuf_rnd			= 986058871;
ulint ibuf_flush_count	= 0;

#define IBUF_COUNT_N_SPACES 10
#define IBUF_COUNT_N_PAGES	10000

/*���ڵ��Ե�ibuf��page��¼ͳ����*/
ulint*	ibuf_counts[IBUF_COUNT_N_SPACES];

ibool ibuf_counts_inited = FALSE;

#define IBUF_BITMAP			PAGE_DATA;

#define IBUF_BITMAP_FREE		0
#define IBUF_BITMAP_BUFFERED	2
#define IBUF_BITMAP_IBUF		3

#define IBUF_BITS_PER_PAGE		4

/* The mutex used to block pessimistic inserts to ibuf trees */
mutex_t	ibuf_pessimistic_insert_mutex;
/*insert buffer�ṹ����latch*/
mutex_t ibuf_mutex;
/*insert buffer bitmap�ṹ����latch*/
mutex_t ibuf_bitmap_mutex;

#define IBUF_MERGE_AREA			8

#define IBUF_MERGE_THRESHOLD	4

#define IBUF_MAX_N_PAGES_MERGED IBUF_MERGE_AREA

#define IBUF_CONTRACT_ON_INSERT_NON_SYNC	0
#define IBUF_CONTRACT_ON_INSERT_SYNC		5
#define IBUF_CONTRACT_DO_NOT_INSERT			10

/*insert buffer�ṹ�Ϸ����ж�*/
static ibool ibuf_validate_low(void);

/*���õ�ǰos thread��ibuf��ʶ*/
UNIV_INLINE void ibuf_enter()
{
	ibool* ptr;
	ptr = thr_local_get_in_ibuf_field();
	ut_ad(*ptr == FALSE);
	*ptr = TRUE;
}

UNIV_INLINE void ibuf_exit()
{
	ibool* ptr;
	ptr = thr_local_get_in_ibuf_field();
	ut_ad(*ptr = TRUE);
	*ptr = FALSE;
}

/*�жϵ�ǰos thread�Ƿ��ڲ���ibuf*/
ibool ibuf_inside()
{
	return *thr_local_get_in_ibuf_field();
}

/*���ibuf header page,���Ҷ������rw_x_latch*/
static page_t* ibuf_header_page_get(ulint space, mtr_t* mtr)
{
	page_t* page;

	ut_ad(!ibuf_inside());
	/*���ibufͷҳ*/
	page = buf_page_get(space, FSP_IBUF_HEADER_PAGE_NO, RW_X_LATCH, mtr);
	buf_page_dbg_add_level(page, SYNC_IBUF_HEADER);

	return page;
}

/*���ibuf btree��root page,���Ҷ������rw_x_latch*/
static page_t* ibuf_tree_root_get(ibuf_data_t* data, ulint space, mtr_t* mtr)
{
	page_t* page;
	ut_ad(ibuf_inside());

	mtr_x_lock(dict_tree_get_lock((data->index)->tree), mtr);

	page = buf_page_get(space, FSP_IBUF_TREE_ROOT_PAGE_NO, RW_X_LATCH, mtr);
	buf_page_dbg_add_level(page, SYNC_TREE_NODE);

	return page;
}

/*���ָ��page��ibuf count*/
ulint ibuf_count_get(ulint space, ulint page_no)
{
	ut_ad(space < IBUF_COUNT_N_SPACES);
	ut_ad(page_no < IBUF_COUNT_N_PAGES);

	if(!ibuf_counts_inited)
		return 0;

	return *(ibuf_counts[space] + page_no);
}

/*����ָ��page��ibuf count*/
static void ibuf_count_set(ulint space, ulint page_no, ulint val)
{
	ut_ad(space < IBUF_COUNT_N_SPACES);
	ut_ad(page_no < IBUF_COUNT_N_PAGES);
	ut_ad(val < UNIV_PAGE_SIZE);

	*(ibuf_counts[space] + page_no) = val;
}

/*����ibuf ȫ�ֶ��󲢳�ʼ�������ҳ�ʼ����ռ�*/
void ibuf_init_at_db_start()
{
	ibuf = mem_alloc(sizeof(ibuf_t));

	/*ibuf���ռ�õ��ڴ�ռ�Ϊ����ص�1/2, max_size��ָҳ���������ֽ���*/
	ibuf->max_size = buf_pool_get_curr_size() / (UNIV_PAGE_SIZE * IBUF_POOL_SIZE_PER_MAX_SIZE);
	ibuf->meter = IBUF_THRESHOLD + 1;

	UT_LIST_INIT(ibuf->data_list);
	ibuf->size = 0;

	/*��ibuf_counts�ĳ�ʼ��*/
	{
		ulint	i, j;

		for (i = 0; i < IBUF_COUNT_N_SPACES; i++) {

			ibuf_counts[i] = mem_alloc(sizeof(ulint)
				* IBUF_COUNT_N_PAGES);
			for (j = 0; j < IBUF_COUNT_N_PAGES; j++) {
				ibuf_count_set(i, j, 0);
			}
		}
	}

	/*�����̲߳�����latch����*/
	mutex_create(&ibuf_pessimistic_insert_mutex);
	mutex_set_level(ibuf_pessimistic_insert_mutex, SYNC_IBUF_PESS_INSERT_MUTEX);

	mutex_create(&ibuf_mutex);
	mutex_set_level(&ibuf_mutex, SYNC_IBUF_MUTEX);

	mutex_create(&ibuf_bitmap_mutex);
	mutex_set_level(&ibuf_bitmap_mutex, SYNC_IBUF_BITMAP_MUTEX);

	/*��ʼ��ibuf��Ӧ�ı�ռ��ļ�,�Ὠ��ibuf_data��space֮��Ĺ�ϵ*/
	fil_ibuf_init_at_db_start();

	ibuf_counts_inited = TRUE;
}

/*����ibuf data�Ŀռ��С��Ϣ������segment�Ĵ�Сû���ı������¼���*/
static void ibuf_data_sizes_update(ibuf_data_t* data, page_t* root, mtr_t* mtr)
{
	ulint old_size;

	ut_ad(mutex_own(&ibuf_mutex));
	
	old_size = data->size;

	/*���root pageͷ�е�ibuf free list*/
	data->free_list_len = flst_get_len(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST, mtr);
	/*���ibuf btree�Ĳ��*/
	data->height = btr_page_get_level(root, mtr) + 1;
	/*���dataռ�õ�ҳ��,һ����root page�ı���*/
	data->size = data->seg_size - (1 + data->free_list_len);

	ut_ad(data->size < data->seg_size);
	/*�ж�����ibuf btree���Ƿ����û���¼*/
	if(page_get_n_recs(root) > 0)
		data->empty = TRUE;
	else
		data->empty = FALSE;

	ut_ad(ibuf->size + data->size >= old_size);
	/*����ibuf size����˼���Ǽ���data->size ��old_size֮��Ĳ�ֵ*/
	ibuf->size = ibuf->size + data->size - old_size;
}

/*����һ����������ibuf�ı�ռ䣬�����������fil_ibuf_init_at_db_start֮�е���*/
ibuf_data_t* ibuf_data_init_for_space(ulint space)
{
	ibuf_data_t*	data;
	page_t*			root;
	page_t*			header_page;
	mtr_t			mtr;
	char			buf[50];
	dict_table_t*	table;
	dict_index_t*	index;
	ulint			n_used;

	data = mem_alloc(sizeof(ibuf_data_t));
	data->space = space;

	mtr_start(&mtr);

	mutex_enter(&ibuf_mutex);
	mtr_x_lock(fil_space_get_latch(space), &mtr);

	/*���ibufͷҳ*/
	header_page = ibuf_header_page_get(space, &mtr);
	/*����ibuf segment֮���ж���ҳ��ռ����*/
	fseg_n_reserved_pages(header_page + IBUF_HEADER + IBUF_TREE_SEG_HEADER, &n_used, &mtr);

	ibuf_enter();
	/*��һ��pageΪheaderҳ���ڶ���pageΪinsert bitmap*/
	ut_ad(n_used >= 2);

	data->seg_size = n_used;

	root = buf_page_get(space, FSP_IBUF_TREE_ROOT_PAGE_NO, RW_X_LATCH, &mtr);
	buf_page_dbg_add_level(root, SYNC_TREE_NODE);

	data->size = 0;
	data->n_inserts = 0;
	data->n_merges = 0;
	data->n_merged_recs = 0;

	/*����ibuf->size*/
	ibuf_data_sizes_update(data, root, &mtr);

	mutex_exit(&ibuf_mutex);
	mtr_commit(&mtr);
	ibuf_exit();

	/*�ڱ��ֵ��н���һ�Ŷ�Ӧ��ibuf��*/
	sprintf(buf, "SYS_IBUF_TABLE_%lu", space);

	table = dict_mem_table_create(buf, space, 2);
	dict_mem_table_add_col(table, "PAGE_NO", DATA_BINARY, 0, 0, 0);
	dict_mem_table_add_col(table, "TYPES", DATA_BINARY, 0, 0, 0);

	table->id = ut_dulint_add(DICT_IBUF_ID_MIN, space);

	dict_table_add_to_cache(table);

	index = dict_mem_index_create(buf, "CLUST_IND", space, DICT_CLUSTERED | DICT_UNIVERSAL | DICT_IBUF, 2);

	dict_mem_index_add_field(index, "PAGE_NO", 0);
	dict_mem_index_add_field(index, "TYPES", 0);

	index->page_no = FSP_IBUF_TREE_ROOT_PAGE_NO;
	index->id = ut_dulint_add(DICT_IBUF_ID_MIN, space);

	dict_index_add_to_cache(table, index);

	data->index = dict_table_get_first_index(table);

	/*��ibuf_data���뵽ibuf��*/
	mutex_enter(&ibuf_mutex);
	UT_LIST_ADD_LAST(data_list, ibuf->data_list, data);
	mutex_exit(&ibuf_mutex);

	return data;
}

/*��ʼ��ibuf bitamp age*/
void ibuf_bitmap_page_init(page_t* page, mtr_t* mtr)
{
	ulint	bit_offset;
	ulint	byte_offset;
	ulint	i;

	bit_offset = XDES_DESCRIBED_PER_PAGE * IBUF_BITS_PER_PAGE;
	byte_offset = bit_offset / 8 + 1;

	/*��ʼ��Ϊ0*/
	for(i = IBUF_BITMAP; i < IBUF_BITMAP + byte_offset; i ++)
		*(page + 1) = 0;

	/*д��һ����ʼ��ibuf bitmap��mtr log*/
	mlog_write_initial_log_record(page, MLOG_IBUF_BITMAP_INIT, mtr);
}

/*�Գ�ʼ��bitmap mtr log������*/
byte* ibuf_parse_bitmap_init(byte* ptr, byte* end_ptr, page_t* page, mtr_t* mtr)
{
	ut_ad(ptr && end_ptr);

	if(page)
		ibuf_bitmap_page_init(page, mtr);

	return ptr;
}

/*��ibuf bitmap�л�ȡָ��ҳ��bitmap��Ϣ��һ��4bit����������*/
UNIV_INLINE ulint ibuf_bitmap_page_get_bits(page_t* page, ulint page_no, ulint bit, mtr_t* mtr)
{
	ulint	byte_offset;
	ulint	bit_offset;
	ulint	map_byte;
	ulint	value;

	ut_ad(bit < IBUF_BITS_PER_PAGE);
	ut_ad(IBUF_BITS_PER_PAGE % 2 == 0);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	/*һ��page��bit����ռ4λ*/
	bit_offset = (page_no % XDES_DESCRIBED_PER_PAGE) * IBUF_BITS_PER_PAGE + bit;

	byte_offset = bit_offset / 8;
	bit_offset = bit_offset % 8;

	ut_ad(byte_offset + IBUF_BITMAP < UNIV_PAGE_SIZE);

	map_type = mach_read_from_1(page + IBUF_BITMAP + byte_offset);
	value = ut_bit_get_nth(map_byte, bit_offset);

	/*�����ȡpageʣ��ռ�Ļ�����ô��ȡǰ��2λ*/
	if(bit == IBUF_BITMAP_FREE){
		ut_ad(bit_offset + 1 < 0);
		value = (value << 1) + ut_bit_get_nth(map_byte, bit_offset + 1);
	}

	return value;
}

/*����һ��page��ibuf bitmap��Ϣ*/
static void ibuf_bitmap_page_set_bits(page_t* page, ulint page_no, ulint bit, ulint val, mtr_t* mtr)
{
	ulint	byte_offset;
	ulint	bit_offset;
	ulint	map_byte;

	ut_ad(bit < IBUF_BITS_PER_PAGE);
	ut_ad(IBUF_BITS_PER_PAGE % 2 == 0);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	/*����page��Ӧ��bitmapλ��*/
	bit_offset = (page_no % XDES_DESCRIBED_PER_PAGE) * IBUF_BITS_PER_PAGE + bit;
	byte_offset = bit_offset / 8;
	bit_offset = bit_offset % 8;

	ut_ad(byte_offset + IBUF_BITMAP < UNIV_PAGE_SIZE);

	map_byte = mach_read_from_1(page + IBUF_BITMAP + byte_offset);

	/*����ҳ��ʣ��ռ�*/
	if (bit == IBUF_BITMAP_FREE){
		ut_ad(bit_offset + 1 < 8);
		ut_ad(val <= 3);

		map_byte = ut_bit_set_nth(map_byte, bit_offset, val / 2);
		map_byte = ut_bit_set_nth(map_byte, bit_offset + 1, val % 2);
	} 
	else{
		ut_ad(val <= 1);
		map_byte = ut_bit_set_nth(map_byte, bit_offset, val);
	}

	/*��¼һ������page bitmap��Ϣ��mtr log*/
	mlog_write_ulint(page + IBUF_BITMAP + byte_offset, map_byte, MLOG_1BYTE, mtr);
}

/*����page_no��ibuf bitmap��Ϣ���ڵ�bitmap page�����λ��*/
UNIV_INLINE ulint ibuf_bitmap_page_no_calc(ulint page_no)
{
	return FSP_IBUF_BITMAP_OFFSET + XDES_DESCRIBED_PER_PAGE * (page_no / XDES_DESCRIBED_PER_PAGE);
}

/*ͨ��space ��page no��ö�Ӧpage��bitmap���ڵ�bitmap page����*/
static page_t* ibuf_bitmap_get_map_page(ulint space, ulint page_no, mtr_t* mtr)
{
	page_t*	page;

	page = buf_get_get(space, ibuf_bitmap_page_no_calc(page_no), RW_X_LATCH, mtr);
	buf_page_dbg_add_level(page, SYNC_IBUF_BITMAP);

	return page;
}

/*����page��Ӧ��ibuf bitmap��Ϣ*/
UNIV_INLINE void ibuf_set_free_bits_low(ulint type, page_t* page, ulint val, mtr_t* mtr)
{
	page_t*	bitmap_page;

	/*�Ǿۼ�����*/
	if(type & DICT_CLUSTERED)
		return ;

	/*����btree��Ҷ�ӽڵ�*/
	if (btr_page_get_level_low(page) != 0)
		return;

	bitmap_page = ibuf_bitmap_get_map_page(buf_frame_get_space_id(page), buf_frame_get_page_no(page), mtr);

	ibuf_bitmap_page_set_bits(bitmap_page, buf_frame_get_page_no(page), IBUF_BITMAP_FREE, val, mtr);
}

/*����ָ��page��ibuf bitmap(page�Ŀ���ʣ�೤��)��mtr�������ں������Խ���*/
void ibuf_set_free_bits(ulint type, page_t* page, ulint val, ulint max_val)
{
	mtr_t mtr;
	page_t* bitmap_page;

	/*�ۼ���������������ibuf bitmap*/
	if(type & DICT_CLUSTERD)
		return;

	if(btr_page_get_level_low(page) != 0)
		return;

	mtr_start(&mtr);
	
	bitmap_page = ibuf_bitmap_get_map_page(buf_frame_get_space_id(page), buf_frame_get_page_no(page), &mtr);
	if(max_val != ULINT_UNDEFINED){

	}

	ibuf_bitmap_page_set_bits(bitmap_page, buf_frame_get_page_no(page), IBUF_BITMAP_FREE, val, &mtr);

	mtr_commit(&mtr);
}

/*����ָ��page��ibuf bitmap(pageʣ��ռ�)*/
void ibuf_reset_free_bits_with_type(ulint type, page_t* page)
{
	ibuf_set_free_bits(type, page, 0, ULINT_UNDEFINED);
}
/*��ibuf_reset_free_bits_with_type������ͬ*/
void ibuf_reset_free_bits(dict_index_t* index, page_t* page)
{
	ibuf_set_free_bits(index->type, page, 0, ULINT_UNDEFINED);
}

/*����page��ʣ��ռ���Ϣ��ibuf bitmap��,�������ֻ���ڼ�¼�ֹ۷�ʽ��ʱ�����*/
void ibuf_update_free_bits_low(dict_index_t* index, page_t* page, ulint max_ins_size, mtr_t* mtr)
{
	ulint	before;
	ulint	after;

	/*���max_ins_size��Ӧʣ��ռ䷶Χ�����*/
	before = ibuf_index_page_calc_free_bits(max_ins_size);
	/*���pageʣ��ռ�ķ�Χ���*/
	after = ibuf_index_page_calc_free(page);

	/*����page��Ӧ��bitmap��ʣ��ռ����Ϣ*/
	if(after != before)
		ibuf_set_free_bits_low(index->type, page, after, mtr);
}

/*ͬʱ��������page��ibuf bitmap,�������ֻ����btree���ѵ�ʱ�����*/
UNIV_INLINE ibool ibuf_update_free_bits_for_two_pages_low(dict_index_t* index, page_t* page1, page_t* page2, mtr_t* mtr)
{
	ulint state;

	/* As we have to x-latch two random bitmap pages, we have to acquire
	the bitmap mutex to prevent a deadlock with a similar operation
	performed by another OS thread. ��ֹ�����߳����ƵĲ��������������*/
	mutex_enter(&ibuf_bitmap_mutex);

	state = ibuf_index_page_calc_free(page1);
	ibuf_set_free_bits_low(index->type, page1, state, mtr);

	state = ibuf_set_free_bits_low(page2);
	ibuf_set_free_bits_low(index->type, page2, state, mtr);

	mutex_exit(&ibuf_bitmap_mutex);
}

/*�ж�page_no��Ӧ��page�Ƿ���ibuf�̶���page*/
UNIV_INLINE ibool ibuf_fixed_addr_page(ulint page_no)
{
	/*page_no��Ӧ��page��ibuf bitmap page����ibuf root page*/
	if(ibuf_bitmap_page(page) || page_no == IBUF_TREE_ROOT_PAGE_NO)
		return TRUE;

	return FALSE;
}

/*���ָ����page�Ƿ���ibuf btree������ҳ������Ҷ�ӽڵ�*/
ibool ibuf_page(ulint space, ulint page_no)
{
	page_t*	bitmap_page;
	mtr_t	mtr;
	ibool	ret;

	/*����redo log����*/
	if(recv_no_ibuf_operations)
		return FALSE;

	if(ibuf_fixed_addr_page(page_no))
		return TRUE;

	ut_ad(fil_space_get_type(space) == FIL_TABLESPACE);

	mtr_start(&mtr);

	bitmap_page = ibuf_bitmap_get_map_page(space, page_no, &mtr);
	/*���bitmap�ĵ�4λ�ı�ʶ*/
	ret = ibuf_bitmap_page_get_bits(bitmap_page, page_no, IBUF_BITMAP_IBUF, &mtr);

	mtr_commit(&mtr);

	return ret;
}

/*���ָ����page�Ƿ���ibuf btree������ҳ������Ҷ�ӽڵ�,ָ����mtr*/
ibool ibuf_page_low(ulint space, ulint page_no, mtr_t* mtr)
{
	page_t*	bitmap_page;
	ibool	ret;

	if(ibuf_fixed_addr_page(page_no))
		return TRUE;

	bitmap_page = ibuf_bitmap_get_map_page(space, page_no, mtr);
	ret = ibuf_bitmap_page_get_bits(bitmap_page, page_no, IBUF_BITMAP_IBUF, mtr);

	return ret;
}

/*��ibuf rec��ȡ��page no,�ɶ���ibuf_data_init_for_space�Ľ������*/
static ulint ibuf_rec_get_page_no(rec_t* rec)
{
	byte*	field;
	ulint	len;

	ut_ad(ibuf_inside());
	ut_ad(rec_get_n_fields(rec) > 2);

	field = rec_get_nth_field(rec, 0, &len);

	ut_ad(len == 4);

	return mach_read_from_4(field);
}

/*����õ�һ��ibuf_rec��¼ռ�õĿռ��С*/
static ulint ibuf_rec_get_volume(rec_t* ibuf_rec)
{
	dtype_t	dtype;
	ulint	data_size	= 0;
	ulint	n_fields;
	byte*	types;
	byte*	data;
	ulint	len;
	ulint	i;

	ut_ad(ibuf_inside());
	ut_ad(rec_get_n_fields(ibuf_rec) > 2);

	n_fields = rec_get_n_fields(ibuf_rec) - 2;
	/*��ü�¼�е�types*/
	types = rec_get_nth_field(ibuf_rec, 1, &len);

	ut_ad(len == n_fields * DATA_ORDER_NULL_TYPE_BUF_SIZE);
	for(i = 0; i < n_fields; i ++){
		data = rec_get_nth_field(ibuf_rec, i + 2, &len);
		dtype_read_for_order_and_null_size(&dtype, types + i * DATA_ORDER_NULL_TYPE_BUF_SIZE);
		if(len == UNIV_SQL_NULL)
			data_size += dtype_get_sql_null_size(&dtype);
		else
			data_size += len;
	}

	return data_size + rec_get_converted_extra_size(data_size, n_fields) + page_dir_calc_reserved_space(1);
}

/*����tuple entry����һ��ibuf ���߼���¼tuple_t*/
static dtuple_t* ibuf_entry_build(dtuple_t* entry, ulint page_no, mem_heap_t* heap)
{
	dtuple_t*	tuple;
	dfield_t*	field;
	dfield_t*	entry_field;
	ulint		n_fields;
	byte*		buf;
	byte*		buf2;
	ulint		i;

	n_fields = dtuple_get_n_fields(entry);

	/*����һ���ڴ��߼���¼*/
	tuple = dtuple_create(heap, n_fields + 2);
	field = dtuple_get_nth_field(tuple, 0);

	/*д��page no����һ����*/
	buf = mem_heap_alloc(heap, 4);
	mach_write_to_4(buf, page_no);
	dfield_set_data(field, buf, 4);

	buf2 = mem_heap_alloc(heap, n_fields * DATA_ORDER_NULL_TYPE_BUF_SIZE);
	for (i = 0; i < n_fields; i++) {
		/*��entry �е�ֵȫ��������tuple��*/
		field = dtuple_get_nth_field(tuple, i + 2);

		entry_field = dtuple_get_nth_field(entry, i);

		dfield_copy(field, entry_field);

		dtype_store_for_order_and_null_size(buf2 + i * DATA_ORDER_NULL_TYPE_BUF_SIZE, dfield_get_type(entry_field));
	}
	
	/*д��types*/
	field = dtuple_get_nth_field(tuple, 1);
	dfield_set_data(field, buf2, n_fields * DATA_ORDER_NULL_TYPE_BUF_SIZE);
	/*ȫ�����ó�DATA_BINARY����*/
	dtuple_set_types_binary(tuple, n_fields + 2);

	return tuple;
}

/*����ibuf rec�������¼ת����һ��dtuple*/
static dtuple_t* ibuf_build_entry_from_ibuf_rec(rec_t* ibuf_rec, mem_heap_t* heap)
{
	dtuple_t*	tuple;
	dfield_t*	field;
	ulint		n_fields;
	byte*		types;
	byte*		data;
	ulint		len;
	ulint		i;

	n_fields = rec_get_n_fields(ibuf_rec) - 2;

	tuple = dtuple_create(heap, n_fields);

	types = rec_get_nth_field(ibuf_rec, 1, &len);

	ut_ad(len == n_fields * DATA_ORDER_NULL_TYPE_BUF_SIZE);
	for(i = 0; i < n_fields; i ++){
		field = dtuple_get_nth_field(tuple, i);
		data = rec_get_nth_field(ibuf_rec, i + 2, &len);

		dfield_set_data(field, data, len);

		dtype_read_for_order_and_null_size(dfield_get_type(field), types + i * DATA_ORDER_NULL_TYPE_BUF_SIZE);
	}

	return tuple;
}

/*����һ���洢��page no����search��tuple*/
static dtuple_t* ibuf_search_tuple_build(ulint page_no, mem_heap_t* heap)
{
	dtuple_t*	tuple;
	dfield_t*	field;
	byte*		buf;

	tuple = dtuple_create(heap, 1);
	field = dtuple_get_nth_field(tuple, 0);

	buf = mem_heap_alloc(heap, 4);
	mach_write_to_4(buf, page_no);
	dfield_set_data(field, buf, 4);

	dtuple_set_types_binary(tuple, 1);
}

/*���data->free_list_len�Ƿ����㹻��page*/
UNIV_INLINE ibool ibuf_data_enough_free_for_insert(ibuf_data_t* data)
{
	ut_ad(mutex_own(&ibuf_mutex));

	if(data->free_list_len >= data->size / 2 + 3 * data->height)
		return TRUE;

	return FALSE;
}

/*���data->free_list_len��������е�page̫�࣬ɾ��һЩpage����tablespace���޳�*/
UNIV_INLINE ibool ibuf_data_too_much_free(ibuf_data_t* data)
{
	ut_ad(mutex_own(&ibuf_mutex));

	if (data->free_list_len >= 3 + data->size / 2 + 3 * data->height)
		return TRUE;

	return FALSE;
}

/*Ϊibuf_data����һ���µ�page�ռ�*/
static ulint ibuf_add_free_page(ulint space, ibuf_data_t* ibuf_data)
{
	mtr_t	mtr;
	page_t*	header_page;
	ulint	page_no;
	page_t*	page;
	page_t*	root;
	page_t*	bitmap_page;

	mtr_start(&mtr);

	mtr_x_lock(fil_space_get_latch(space), &mtr);

	/*��fil space�з���һ��page*/
	header_page = ibuf_header_page_get(space, &mtr);
	page_no = fseg_alloc_free_page(header_page + IBUF_HEADER + IBUF_TREE_SEG_HEADER, 0, FSP_UP, &mtr);
	if(page_no == FIL_NULL){
		mtr_commit(&mtr);
		return DB_STRONG_FAIL;
	}
	/*�ڻ�����л��page���ڴ�ָ��*/
	page = buf_page_get(space, page_no, RW_X_LATCH, &mtr);
	buf_page_dbg_add_level(page, SYNC_TREE_NODE_NEW);

	ibuf_enter();

	mutex_enter();

	root = ibuf_tree_root_get(ibuf_data, space, &mtr);
	/*���·����page���뵽root page free����*/
	flst_add_last(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST, page + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST_NODE, &mtr);
	
	ibuf_data->seg_size ++;
	ibuf_data->free_list_len ++;

	/*����page��Ӧibuf bitmap�ϵ�״̬��Ϣ*/
	bitmap_page = ibuf_bitmap_get_map_page(space, page_no, &mtr);
	ibuf_bitmap_page_set_bits(bitmap_page, page_no, IBUF_BITMAP_IBUF, TRUE, &mtr);

	mtr_commit(&mtr);
	mutex_exit(&ibuf_mutex);

	ibuf_exit();

	return DB_SUCCESS;
}

/*��һ������page��ibuf_data��ɾ�������ն�Ӧ�ı�ռ�*/
static void ibuf_remove_free_page(ulint space, ibuf_data_t* ibuf_data)
{
	mtr_t	mtr;
	mtr_t	mtr2;
	page_t*	header_page;
	ulint	page_no;
	page_t*	page;
	page_t*	root;
	page_t*	bitmap_page;

	mtr_start(&mtr);
	mtr_x_lock(fil_space_get_latch(space), &mtr);

	header_page = ibuf_header_page_get(space, &mtr);
	
	mutex_enter(&ibuf_pessimistic_insert_mutex);
	ibuf_enter();
	mutex_enter(&ibuf_mutex);

	/*ibuf_data��û�й����page���У����Բ����ͷŻ���page*/
	if(!ibuf_data_too_much_free(ibuf_data)){
		mutex_exit(&ibuf_mutex);
		ibuf_exit();
		mutex_exit(&ibuf_pessimistic_insert_mutex);
		mtr_commit(&mtr);

		return;
	}

	mtr_start(&mtr2);

	root = ibuf_tree_root_get(ibuf_data, space, &mtr2);
	/*���root���һ������ҳ��page no*/
	page_no = flst_get_last(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST, &mtr2).page;

	mtr_commit(&mtr2);
	mutex_exit(&ibuf_mutex);

	ibuf_exit();

	/*��page���б�ռ����*/
	fseg_free_page(header_page + IBUF_HEADER + IBUF_TREE_SEG_HEADER, space, page_no, &mtr);

	ibuf_enter();
	mutex_enter(&ibuf_mutex);

	/*��root��free list����ɾ��*/
	root = ibuf_tree_root_get(ibuf_data, space, &mtr);
	ut_ad(page_no == flst_get_last(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST, &mtr).page);

	page = buf_page_get(space, page_no, RW_X_LATCH, &mtr);
	buf_page_dbg_add_level(page, SYNC_TREE_NODE);

	flst_remove(root + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST, page + PAGE_HEADER + PAGE_BTR_IBUF_FREE_LIST_NODE, &mtr);

	ibuf_data->seg_size --;
	ibuf_data->free_list_len --;

	mutex_exit(&ibuf_pessimistic_insert_mutex);

	/*��ibuf bitmap�е�״̬����Ϊpageû�б�ibuf����*/
	bitmap_page = ibuf_bitmap_get_map_page(space, page_no, &mtr);
	ibuf_bitmap_page_set_bits(bitmap_page, page_no, IBUF_BITMAP_IBUF, FALSE, &mtr);

	mtr_commit(&mtr);
	mutex_exit(&ibuf_mutex);
	ibuf_exit();
}

/*�ͷ�space��Ӧ��ibuf_data�й�ʣ��page,ÿ�ε�������������ͷ�4��page,�ڱ�ռ����ҳ��ʱ�����ô˺���*/
void ibuf_free_excess_pages(ulint space)
{
	ibuf_data_t*	ibuf_data;
	ulint			i;

	ut_ad(rw_lock_own(fil_space_get_latch(space), RW_LOCK_EX));
	ut_ad(rw_lock_get_x_lock_count(fil_space_get_latch(space)) == 1);
	ut_ad(!ibuf_inside());

	/*��Ӧ��ռ���ibuf_data*/
	ibuf_data = fil_space_get_ibuf_data(space);
	if(ibuf_data == NULL)
		return ;

	/*�����ͷ�4��page*/
	for(i = 0; i < 4; i ++){
		mutex_enter(&ibuf_mutex);

		if (!ibuf_data_too_much_free(ibuf_data)){
			mutex_exit(&ibuf_mutex);

			return;
		}

		mutex_exit(&ibuf_mutex);

		ibuf_remove_free_page(space, ibuf_data);
	}
}

/*��ibuf tree��Ҷ��page�ļ�¼�л�úϲ���page nos*/
static ulint ibuf_get_merge_page_nos(ibool contract, rec_t* first_rec, ulint* page_nos, ulint* n_stored)
{
	ulint	prev_page_no;
	ulint	first_page_no;
	ulint	rec_page_no;
	rec_t*	rec;
	ulint	sum_volumes;
	ulint	volume_for_page;
	ulint	rec_volume;
	ulint	limit;
	page_t*	page;
	ulint	n_pages;

	*n_stored = 0;

	limit = ut_min(IBUF_MAX_N_PAGES_MERGED, buf_pool->curr_size / 4);

	page = uf_frame_align(first_rec);

	if(first_rec == page_get_supremum_rec(page))
		first_rec = page_rec_get_prev(first_rec);

	if(first_rec == page_get_infimum_rec(page))
		first_rec = page_rec_get_next(first_rec);
	
	/*��һҳ�޼�¼*/
	if (first_rec == page_get_supremum_rec(page))
		return(0);

	rec = first_rec;
	first_page_no = ibuf_rec_get_page_no(first_rec);
	n_pages = 0;
	prev_page_no = 0;

	/*��λ����8��pageΪ��λ�ķ�Χ��ǰ��һ����¼*/
	while(rec != page_get_infimum_rec(page) && n_pages < limit){
		rec_page_no = ibuf_rec_get_page_no(rec);
		ut_ad(rec_page_no != 0);

		/*��first pageͬʱ����8��pageΪ��λ�ķ�Χ֮��*/
		if(rec_page_no / IBUF_MERGE_AREA != first_page_no / IBUF_MERGE_AREA)
			break;

		if(rec_page_no != prev_page_no)
			n_pages ++;

		prev_page_no = rec_page_no;
		rec = page_rec_get_prev(rec);
	}

	rec = page_rec_get_next(rec);

	prev_page_no = 0;
	sum_volumes = 0;
	volume_for_page = 0;

	while(*n_stored < limit){
		if(rec == page_get_supremum_rec(page))
			rec_page_no = 1;
		else{
			rec_page_no = ibuf_rec_get_page_no(rec);
			ut_ad(rec_page_no > IBUF_TREE_ROOT_PAGE_NO);
		}

		if(rec_page_no != prev_page_no){
			/*���rec��Ӧ��page��first_page_no���߼�¼�ռ�ﵽ��merge����ֵ,��ô���page����Ϊ�ϲ���Ŀ��page??*/
			if(prev_page_no == first_page_no || contract || 
				volume_for_page > (((IBUF_MERGE_THRESHOLD - 1) * 4 * UNIV_PAGE_SIZE / IBUF_PAGE_SIZE_PER_FREE_SPACE)/ IBUF_MERGE_THRESHOLD)){
				page_nos[*n_stored] = prev_page_no;
				(*n_stored) ++;
				sum_volumes += volume_for_page;
			}

			if(rec_page_no / IBUF_MERGE_AREA != first_page_no / IBUF_MERGE_AREA)
				break;

			volume_for_page = 0;
		}

		if(rec_page_no == 1) /*ֱ�ӵ���supremum��¼���˳�ѭ��*/
			break;

		rec_volume = ibuf_rec_get_volume(rec);
		volume_for_page += rec_volume;

		prev_page_no = rec_page_no;
		rec = page_rec_get_next(rec);
	}

	return sum_volumes;
}

/*����buffer pool�ڴ����޵�������ibuf btree*/
static ulint ibuf_contract_ext(ulint* n_pages, ibool sync)
{
	ulint		rnd_pos;
	ibuf_data_t*	data;
	btr_pcur_t	pcur;
	ulint		space;
	ibool		all_trees_empty;
	ulint		page_nos[IBUF_MAX_N_PAGES_MERGED];
	ulint		n_stored;
	ulint		sum_sizes;
	mtr_t		mtr;

	*n_pages = 0;

loop:
	ut_ad(!ibuf_inside());
	mutex_enter(&ibuf_mutex);

	ut_ad(ibuf_validate_low());

	/*������һ��λ��*/
	ibuf_rnd = 865558671;
	rnd_pos = ibuf_rnd % ibuf->size;

	all_trees_empty = TRUE;

	/*��λcontract��ibuf_dataλ��*/
	data = UT_LIST_GET_FIRST(ibuf->data_list);
	for(;;){
		if(!data->empty){
			all_trees_empty = FALSE;
			if(rnd_pos < data->size)
				break;

			rnd_pos -= data->size;
		}

		data = UT_LIST_GET_NEXT(data_list, data);
		if(data == NULL){ /*û�����λ��data size�ϲ���*/
			if(all_trees_empty){
				mutex_exit(&ibuf_mutex);
				return 0;
			}

			data = UT_LIST_GET_FIRST(ibuf->data_list);
		}
	}

	ut_ad(data);

	space = data->index->space;
	mtr_start(&mtr);
	ibuf_enter();

	/*��btree�������λ����BTREE �����α�*/
	btr_pcur_open_at_rnd_pos(data->index, BTR_SEARCH_LEAF, &pcur, &mtr);
	if(0 == page_get_n_recs(btr_pcur_get_page(&pcur))){ /*��λ����pageû���κε��û���¼,�޷�merge page*/
		data->empty = TRUE;

		ibuf_exit();
		mtr_commit(&mtr);

		btr_pcur_close(&pcur);

		mutex_exit(&ibuf_mutex);

		goto loop;
	}

	mutex_exit(&ibuf_mutex);

	/*ѡȡ�ϲ���pageλ��*/
	sum_sizes = ibuf_get_merge_page_nos(TRUE, btr_pcur_get_rec(&pcur), page_nos, &n_stored);

	ibuf_exit();
	mtr_commit(&mtr);
	btr_pcur_close(&pcur);

	buf_read_ibuf_merge_pages(sync, space, page_nos, n_stored);
	*n_pages = n_stored;

	return sum_sizes + 1;
}

ulint ibuf_contract(ibool sync)
{
	ulint n_pages;
	return ibuf_contract_ext(&n_pages, sync);
}

/*��ibuf page��merge,ֱ��n_pages��page��merage���,master thread����*/
ulint ibuf_contract_for_n_pages(ibool sync, ulint n_pages)
{
	ulint	sum_bytes	= 0;
	ulint	sum_pages 	= 0;
	ulint	n_bytes;
	ulint	n_pag2;

	while(sum_pages < n_pages){
		n_bytes = ibuf_contract_ext(&n_pag2, sync);

		if(n_bytes == 0)
			return sum_bytes;

		sum_bytes += n_bytes;
		sum_pages += n_pag2;
	}

	return sum_bytes;
}

/*�ڲ���һ��ibuf ������ռ�ÿռ����,���ʱ���������insert buffer�ļ�¼�ϲ�������������*/
UNIV_INLINE void ibuf_contract_after_insert(ulint entry_size)
{
	ibool	sync;
	ulint	sum_sizes;
	ulint	size;

	mutex_enter(&ibuf_mutex);

	/*ibufռ�õ�ҳ������û�г�����������̵�ҳ��*/
	if(ibuf->size < ibuf->max_size + IBUF_CONTRACT_ON_INSERT_NON_SYNC){
		mutex_exit(&ibuf_mutex);
		return ;
	}

	sync = FALSE;
	/*����̫���ˣ�����ͬ���ϲ�*/
	if(ibuf->size >= ibuf->max_size + IBUF_CONTRACT_ON_INSERT_SYNC)
		sync = TRUE;

	mutex_exit(&ibuf_mutex);

	sum_sizes = 0;
	size = 1;
	/*���кϲ�*/
	while(size > 0 && sum_sizes < entry_size){
		size = ibuf_contract(sync);
		sum_sizes += size;
	}
}

/*ͳ��pcur��Ӧ��ibuf btree����¼��ӳ��ҳռ��ibuf buffer�ļ�¼�ռ��ܺ�*/
ulint ibuf_get_volume_buffered(btr_pcur_t* pcur, ulint space, ulint page_no, mtr_t* mtr)
{
	ulint	volume;
	rec_t*	rec;
	page_t*	page;
	ulint	prev_page_no;
	page_t*	prev_page;
	ulint	next_page_no;
	page_t*	next_page;

	ut_ad((pcur->latch_mode == BTR_MODIFY_PREV) || (pcur->latch_mode == BTR_MODIFY_TREE));

	volume = 0;

	rec = btr_pcur_get_rec(pcur);
	page = buf_frame_align(rec);

	if(rec == page_get_supremum_rec(page))
		rec = page_rec_get_prev(rec);

	/*ͳ��rec����ǰ��ļ�¼�ռ��ܺ�*/
	for(;;){
		if(rec == page_get_infimum_rec(page))
			break;

		if(page_no != ibuf_rec_get_page_no(rec))
			goto count_later;

		volume += ibuf_rec_get_volume(rec);
		rec = page_rec_get_prev(rec);
	}

	/*��ǰҳͳ��*/
	prev_page_no = btr_page_get_prev(page, mtr);
	if(prev_page_no == FIL_NULL)
		goto count_later;

	prev_page = buf_page_get(space, prev_page_no, RW_X_LATCH, mtr);
	buf_page_dbg_add_level(prev_page, SYNC_TREE_NODE);

	rec = page_get_supremum_rec(prev_page);
	rec = page_rec_get_prev(rec);
	for(;;){
		if (rec == page_get_infimum_rec(prev_page)) {
			/* We cannot go to yet a previous page, because we
			do not have the x-latch on it, and cannot acquire one
			because of the latching order: we have to give up */
		
			return(UNIV_PAGE_SIZE);
		}
		
		if (page_no != ibuf_rec_get_page_no(rec)) 
			goto count_later;


		volume += ibuf_rec_get_volume(rec);
		rec = page_rec_get_prev(rec);
	}

count_later:
	/*���ҳͳ��*/
	rec = btr_pcur_get_rec(pcur);

	if (rec != page_get_supremum_rec(page))
		rec = page_rec_get_next(rec);

	for (;;) {
		if (rec == page_get_supremum_rec(page))
			break;

		if (page_no != ibuf_rec_get_page_no(rec)) 
			return(volume);

		volume += ibuf_rec_get_volume(rec);
		rec = page_rec_get_next(rec);
	}

	next_page_no = btr_page_get_next(page, mtr);
	if (next_page_no == FIL_NULL)
		return(volume);

	next_page = buf_page_get(space, next_page_no, RW_X_LATCH, mtr);

	buf_page_dbg_add_level(next_page, SYNC_TREE_NODE);

	rec = page_get_infimum_rec(next_page);
	rec = page_rec_get_next(rec);

	for (;;) {
		if (rec == page_get_supremum_rec(next_page)) 
			return(UNIV_PAGE_SIZE);

		if (page_no != ibuf_rec_get_page_no(rec))
			return(volume);

		volume += ibuf_rec_get_volume(rec);
		rec = page_rec_get_next(rec);
	}
}

/*����һ�����������ļ�¼��ibuf����*/
static ulint ibuf_insert_low(ulint mode, dtuple_t* entry, dict_index_t* index, ulint space, ulint page_no, que_thr_t* thr)
{
	big_rec_t*	dummy_big_rec;
	ulint		entry_size;
	btr_pcur_t	pcur;
	btr_cur_t*	cursor;
	mtr_t		mtr;
	mtr_t		bitmap_mtr;
	dtuple_t*	ibuf_entry;
	mem_heap_t*	heap;
	ulint		buffered;
	rec_t*		ins_rec;
	ibool		old_bit_value;
	page_t*		bitmap_page;
	ibuf_data_t*	ibuf_data;
	dict_index_t*	ibuf_index;
	page_t*		root;
	ulint		err;
	ibool		do_merge;
	ulint		page_nos[IBUF_MAX_N_PAGES_MERGED];
	ulint		n_stored;
	ulint		bits;

	ut_a(!(index->type & DICT_CLUSTERED));
	ut_ad(dtuple_check_typed(entry));

	do_merge = FALSE;

	ibuf_data = fil_space_get_ibuf_data(space);
	ibuf_index = ibuf_data->index;

	mutex_enter(&ibuf_mutex);

	/*ibufռ�õĿռ䳬����ibuf����Ŀռ�����*/
	if(ibuf->size >= ibuf->max_size + IBUF_CONTRACT_DO_NOT_INSERT){
		mutex_exit(&ibuf_mutex);
		/*��ibuf�ϵļ�¼�����ϲ���������������*/
		ibuf_contract(TRUE);

		return DB_STRONG_FAIL;
	}

	mutex_exit(&ibuf_mutex);

	if(mode == BTR_MODIFY_TREE){
		mutex_enter(&ibuf_pessimistic_insert_mutex);

		ibuf_enter();
		mutex_enter(&ibuf_mutex);

		/*�ж�ibuf_data��page����ҳ�Ƿ�*/
		while(!ibuf_data_enough_free_for_insert(ibuf_data)){
			mutex_exit(&ibuf_mutex);
			ibuf_exit();
			mutex_exit(&ibuf_pessimistic_insert_mutex);

			/*Ϊibuf_data����һ��page�ռ�*/
			err = ibuf_add_free_page(space, ibuf_data);
			if(err == DB_STRONG_FAIL)
				return err;

			mutex_enter(&ibuf_pessimistic_insert_mutex);
			ibuf_enter();
			mutex_enter(&ibuf_mutex);
		}
	}
	else
		ibuf_enter();

	/*��������¼�ĳ���*/
	entry_size = rec_get_converted_size(entry);
	/*����һ��ibuf��Ӧ�ļ�¼��ʽ*/
	ibuf_entry = ibuf_entry_build(entry, page_no, heap);

	mtr_start(&mtr);
	/*����ibuf_entry��row id��page no����ibuf_index btree�϶�Ϊ��һ��cursorλ��*/
	btr_pcur_open(ibuf_index, ibuf_entry, PAGE_CUR_LE, mode, &pcur, &mtr);
	/*�����Ӧpageռ�õ�ibuf�ռ��С*/
	buffered = ibuf_get_volume_buffered(&pcur, space, page_no, &mtr);

	mtr_start(&bitmap_mtr);
	bitmap_page = ibuf_bitmap_get_map_page(space, page_no, &bitmap_mtr);
	if(buf_page_peek(space, page_no) || lock_rec_expl_exist_on_page(space, page_no)){ /*�޷���page no��Ӧ��page���޸�*/
		err = DB_STRONG_FAIL;
		mtr_commit(&bitmap_mtr);
		goto function_exit;
	}

	bits = ibuf_bitmap_page_get_bits(bitmap_page, page_no, IBUF_BITMAP_FREE, &bitmap_mtr);
	/*��bitmap�л�ö�Ӧ��ibuf pageʣ��Ŀռ䣬����ռ䲻�㣬��Ҫ����¼�ϲ�*/
	if(buffered + entry_size + page_dir_calc_reserved_space(1) > ibuf_index_page_calc_free_from_bits(bits)){
		mtr_commit(&bitmap_mtr);
		err = DB_STRONG_FAIL;
		do_merge = TRUE;

		ibuf_get_merge_page_nos(FALSE, btr_pcur_get_rec(&pcur), page_nos, &n_stored);

		goto function_exit;
	}

	/*����page no��Ӧ��page�м�¼��ibuf�л���*/
	old_bit_value = ibuf_bitmap_page_get_bits(bitmap_page, page_no, IBUF_BITMAP_BUFFERED, &bitmap_mtr);
	if(!old_bit_value)
		ibuf_bitmap_page_set_bits(bitmap_page, page_no, IBUF_BITMAP_BUFFERED, TRUE, &bitmap_mtr);

	mtr_commit(&bitmap_mtr);

	/*������������¼���뵽ibuf btree��*/
	cursor = btr_pcur_get_btr_cur(&pcur);
	if(mode == BTR_MODIFY_PREV){
		err = btr_cur_optimistic_insert(BTR_NO_LOCKING_FLAG, cursor, ibuf_entry, &ins_rec,
			&dummy_big_rec, thr, &mtr);

		if (err == DB_SUCCESS)
			page_update_max_trx_id(buf_frame_align(ins_rec), thr_get_trx(thr)->id);
	}
	else{
		ut_ad(mode == BTR_MODIFY_TREE);

		root = ibuf_tree_root_get(ibuf_data, space, &mtr);
		err = btr_cur_pessimistic_insert(BTR_NO_LOCKING_FLAG | BTR_NO_UNDO_LOG_FLAG, cursor,
			ibuf_entry, &ins_rec,
			&dummy_big_rec, thr, &mtr);

		if(err == DB_SUCCESS)
			page_update_max_trx_id(buf_frame_align(ins_rec), thr_get_trx(thr)->id);

		/*����ibuf_data->size*/
		ibuf_data_sizes_update(ibuf_data, root, &mtr);
	}

function_exit:
	if(mode == BTR_MODIFY_PREV){
		ut_ad(ibuf_validate_low());
		mutex_exit(&ibuf_mutex);
		mutex_exit(&ibuf_pessimistic_insert_mutex);
	}

	mtr_commit(&mtr);
	btr_pcur_close(&pcur);
	ibuf_exit();

	if(err == DB_SUCCESS){
		ibuf_data->empty = FALSE;
		ibuf_data->n_inserts ++;
	}

	mutex_exit(&ibuf_mutex);

	/*�жϲ����¼���Ƿ�Ҫ��ibuf�ļ�¼��merge*/
	if(mode == BTR_MODIFY_TREE && err == DB_SUCCESS)
		ibuf_contract_after_insert(entry_size);

	/*ǰ���жϳ���Ҫmerge������ibuf ��¼�ϲ�����������ҳ��*/
	if(do_merge)
		buf_read_ibuf_merge_pages(FALSE, space, page_nos, n_stored);

	return err;
}

/*��ibuf�в���һ����¼(tuple)*/
ibool ibuf_insert(dtuple_t* entry, dict_index_t* index, ulint space, ulint page_no, que_thr_t* thr)
{
	ulint err;

	ut_ad(dtuple_check_typed(entry));
	ut_a(!(index->type & DICT_CLUSTERED));

	/*��¼̫�󣬲��ʺϷŵ�ibuf��*/
	if(rec_get_converted_size(entry) >= page_get_free_space_of_empty() / 2)
		return FALSE;

	/*���ֹ۷�ʽ���룬���ʧ�ܣ��Ա��۷�ʽ����*/
	err = ibuf_insert_low(BTR_MODIFY_PREV, entry, index, space, page_no, thr);
	if(err == DB_FAIL)
		err = ibuf_insert_low(BTR_MODIFY_TREE, entry, index, space, page_no, thr);

	if(err == DB_SUCCESS)
		return TRUE;
	else{
		ut_a(err == DB_STRONG_FAIL);
		return FALSE;
	}
}

/*��һ��ibuf�ļ�¼�ϲ�����������ҳ��*/
static void ibuf_insert_to_index_page(dtuple_t* entry, page_t* page, mtr_t* mtr)
{
	page_cur_t	page_cur;
	ulint		low_match;
	rec_t*		rec;
	page_t*		bitmap_page;
	ulint		old_bits;
	char		errbuf[1000];

	ut_ad(ibuf_inside());
	ut_ad(dtuple_check_typed(entry));

	low_match = page_cur_search(page, entry, PAGE_CUR_LE, &page_cur);
	if(low_match == dtuple_get_n_fields(entry)){ /*ƥ�䵽��¼�����һ��*/
		rec = page_cur_get_rec(&page_cur);
		btr_cur_del_unmark_for_ibuf(rec, mtr);
	}
	else{
		rec = page_cur_tuple_insert(&page_cur, entry, mtr);
		if(rec == NULL){ /*����ʧ�ܣ�����ҳ�ռ�����*/
			btr_page_reorganize(page, mtr);
			page_cur_search(page, entry, PAGE_CUR_LE, &page_cur);
			
			/*���ǲ���ʧ�ܣ����д�����Ϣ��ӡ*/
			if(!page_cur_tuple_insert(&page_cur, entry, mtr)){
				ut_print_timestamp(stderr);

				fprintf(stderr,"InnoDB: Error: Insert buffer insert fails; page free %lu, dtuple size %lu\n",
					page_get_max_insert_size(page, 1),rec_get_converted_size(entry));

				dtuple_sprintf(errbuf, 900, entry);

				fprintf(stderr, "InnoDB: Cannot insert index record %s\n", errbuf);

				fprintf(stderr,
					"InnoDB: The table where where this index record belongs\n"
					"InnoDB: is now probably corrupt. Please run CHECK TABLE on\n"
					"InnoDB: that table.\n");

				bitmap_page = ibuf_bitmap_get_map_page(buf_frame_get_space_id(page), buf_frame_get_page_no(page), mtr);
				old_bits = ibuf_bitmap_page_get_bits(bitmap_page, buf_frame_get_page_no(page), IBUF_BITMAP_FREE, mtr);

				fprintf(stderr, "Bitmap bits %lu\n", old_bits);

				fprintf(stderr, "InnoDB: Send a detailed bug report to mysql@lists.mysql.com!\n");
			}
		}
	}
}

/*��ibuf ��ɾ��ָ����¼*/
static ibool ibuf_delete_rec(ulint space, ulint page_no, btr_pcur_t* pcur, dtuple_t* search_tuple, mtr_t* mtr)
{
	ibool		success;
	ibuf_data_t*	ibuf_data;
	page_t*		root;
	ulint		err;

	ut_ad(ibuf_inside());

	/*�ֹ�ʽɾ��*/
	success = btr_cur_optimistic_delete(btr_pcur_get_btr_cur(pcur), mtr);
	if(success)
		return FALSE;

	/*û�����ɹ������˵�ɾ����¼����ǰ��pcurλ��*/
	btr_pcur_store_position(pcur, mtr);
	btr_pcur_commit_specify_mtr(pcur, mtr);

	/*���space��Ӧ��ibuf����*/
	ibuf_data = fil_space_get_ibuf_data(space);

	mutex_enter(&ibuf_mutex);
	mtr_start(mtr);

	success = btr_pcur_restore_position(BTR_MODIFY_TREE, pcur, mtr);
	if(!success){
		fprintf(stderr,"InnoDB: ERROR: Send the output to heikki.tuuri@innodb.com\n");
		fprintf(stderr, "InnoDB: ibuf cursor restoration fails!\n");
		fprintf(stderr, "InnoDB: ibuf record inserted to page %lu\n", page_no);

		rec_print(btr_pcur_get_rec(pcur));
		rec_print(pcur->old_rec);
		dtuple_print(search_tuple);

		rec_print(page_rec_get_next(btr_pcur_get_rec(pcur)));

		mtr_commit(mtr);

		fprintf(stderr, "InnoDB: Validating insert buffer tree:\n");
		ut_a(btr_validate_tree(ibuf_data->index->tree));		
		fprintf(stderr, "InnoDB: Ibuf tree ok\n");
	}

	ut_a(success);

	/*����ʽɾ��*/
	root = ibuf_tree_root_get(ibuf_data, space, mtr);
	btr_cur_pessimistic_delete(&err, TRUE, btr_pcur_get_btr_cur(pcur), FALSE, mtr);

	ut_a(err == DB_SUCCESS);
	/*����ȷ��ibuf_data��ռ�ÿռ��С*/
	ibuf_data_sizes_update(ibuf_data, root, mtr);

	btr_pcur_commit_specify_mtr(pcur, mtr);

	btr_pcur_close(pcur);
	mutex_exit(&ibuf_mutex);

	return TRUE;
}

/*��page_no�еļ�¼��ibuf btree��ɾ���������뵽page����*/
void ibuf_merge_or_delete_for_page(page_t* page, ulint space, ulint page_no)
{
	mem_heap_t*	heap;
	btr_pcur_t	pcur;
	dtuple_t*	entry;
	dtuple_t*	search_tuple;
	rec_t*		ibuf_rec;
	ibool		closed;
	buf_block_t*	block;
	page_t*		bitmap_page;
	ibuf_data_t*	ibuf_data;
	ibool		success;
	ulint		n_inserts;
	ulint		volume;
	ulint		old_bits;
	ulint		new_bits;
	dulint		max_trx_id;
	ibool		corruption_noticed	= FALSE;
	mtr_t		mtr;
	char		err_buf[500];

	/*server����־����*/
	if(srv_force_recovery >= SRV_FORCE_NO_IBUF_MERGE)
		return ;

	if(ibuf_fixed_addr_page(page_no) || fsp_descr_page(page_no) || trx_sys_hdr_page(space, page_no))
		return ;

	mtr_start(&mtr);
	
	bitmap_page = ibuf_bitmap_get_map_page(space, page_no, &mtr);
	if(!ibuf_bitmap_page_get_bits(bitmap_page, page_no, IBUF_BITMAP_BUFFERED, &mtr)){ /*page no��Ӧ��page�ļ�¼����ibuf��*/
		mtr_commit(&mtr);
		return ;
	}

	mtr_commit(&mtr);

	ibuf_data = fil_space_get_ibuf_data(space);

	ibuf_enter();

	heap = mem_heap_create(512);
	/*����һ�����������������߼���¼tuple*/
	search_tuple = ibuf_search_tuple_build(page_no, heap);
	if(page){
		block = buf_block_align(page);
		rw_lock_x_lock_move_ownership(&(block->lock));

		/*������������ҳ*/
		if(fil_page_get_type(page) != FIL_PAGE_INDEX){
			corruption_noticed = TRUE;
			ut_print_timestamp(stderr);

			mtr_start(&mtr);

			fprintf(stderr, "  InnoDB: Dump of the ibuf bitmap page:\n");
			bitmap_page = ibuf_bitmap_get_map_page(space, page_no, &mtr);

			buf_page_print(bitmap_page);
			mtr_commit(&mtr);

			fprintf(stderr, "\nInnoDB: Dump of the page:\n");
			buf_page_print(page);

			fprintf(stderr,
				"InnoDB: Error: corruption in the tablespace. Bitmap shows insert\n"
				"InnoDB: buffer records to page n:o %lu though the page\n"
				"InnoDB: type is %lu, which is not an index page!\n"
				"InnoDB: We try to resolve the problem by skipping the insert buffer\n"
				"InnoDB: merge for this page. Please run CHECK TABLE on your tables\n"
				"InnoDB: to determine if they are corrupt after this.\n\n"
				"InnoDB: Please make a detailed bug report and send it to\n"
				"InnoDB: mysql@lists.mysql.com\n\n",
				page_no, fil_page_get_type(page));
		}
	}

	n_inserts = 0;
	volume = 0;

loop:
	mtr_start(&mtr);
	if(page){
		success = buf_page_get_known_nowait(RW_X_LATCH, page, BUF_KEEP_OLD, IB_FILE_, __LINE__, &mtr);
		ut_a(success);

		buf_page_dbg_add_level(page, SYNC_TREE_NODE);
	}

	/*��search_tuple��ibuf tree������*/
	btr_pcur_open_on_user_rec(ibuf_data->index, search_tuple, PAGE_CUR_GE, BTR_MODIFY_LEAF, &pcur, &mtr);
	if(!btr_pcur_is_on_user_rec(&pcur, &mtr)){
		ut_ad(btr_pcur_is_after_last_in_tree(&pcur, &mtr));
		goto reset_bit;
	}

	for(;;){
		ut_ad(btr_pcur_is_on_user_rec(&pcur, &mtr));
		ibuf_rec = btr_cur_get_rec(&pcur);

		/*ibuf_rec����page no�ļ�¼*/
		if(ibuf_rec_get_page_no(ibuf_rec) != page_no){ 
			if(page) /*��page���һ�������ļ�¼��λΪ0*/
				page_header_reset_last_insert(page, &mtr);
			goto reset_bit;
		}

		if(corruption_noticed){
			rec_sprintf(err_buf, 450, ibuf_rec);
			fprintf(stderr, "InnoDB: Discarding record\n %s\n from the insert buffer!\n\n", err_buf);
		}
		else if(page){
			max_trx_id = page_get_max_trx_id(buf_frame_align(ibuf_rec));
			page_update_max_trx_id(page, max_trx_id);

			/*��ibuf recת��Ϊtuple�߼���¼(����ҳ�ļ�¼��ʽ)*/
			entry = ibuf_build_entry_from_ibuf_rec(ibuf_rec, heap);
			/*����¼���뵽����ҳpage����*/
			ibuf_insert_to_index_page(entry, page, &mtr);
		}

		n_inserts ++;

		/*����¼��ibuf btree��ɾ��*/
		closed = ibuf_delete_rec(space, page_no, &pcur, search_tuple, &mtr);
		if(closed)
			goto loop;

		/*pcur�Ѿ�ָ��page��supremum��¼*/
		if(btr_pcur_is_after_last_on_page(&pcur, &mtr)){
			mtr_commit(&mtr);
			btr_pcur_close(&pcur);
			goto loop;
		}
	}

reset_bit:
	bitmap_page = ibuf_bitmap_get_map_page(space, page_no, &mtr);
	/*ȡ����page no��Ӧҳ��ibuf�����ʶ*/
	ibuf_bitmap_page_set_bits(bitmap_page, page_no, IBUF_BITMAP_BUFFERED, FALSE, &mtr);
	if(page){
		old_bits = ibuf_bitmap_page_get_bits(bitmap_page, page_no, IBUF_BITMAP_FREE, &mtr);
		new_bits = ibuf_index_page_calc_free(page); /*��������page��ʣ������ó���*/

		if(old_bits != new_bits)
			ibuf_bitmap_page_set_bits(bitmap_page, page_no, IBUF_BITMAP_FREE, new_bits, &mtr);
	}

	ibuf_data->n_merges++;	
	ibuf_data->n_merged_recs += n_inserts;

	mtr_commit(&mtr);
	btr_pcur_close(&pcur);

	mem_heap_free(heap);

	ibuf_exit();
}

/*���ibuf�ĺϷ���*/
static ibool ibuf_validate_low()
{
	ibuf_data_t*	data;
	ulint			sum_sizes;

	ut_ad(&ibuf_mutex);

	sum_sizes = 0;

	data = UT_LIST_GET_FIRST(ibuf->data_list);
	while(data){
		sum_sizes += data->size;
		data = UT_LIST_GET_NEXT(data_list, data);
	}

	ut_a(sum_sizes == ibuf->size);

	return TRUE;
}

void ibuf_print(char* buf, char* buf_end)
{
	ibuf_data_t*	data;

	if(buf_end - buf < 500)
		return;

	mutex_enter(&ibuf_mutex);

	data = UT_LIST_GET_FIRST(ibuf->data_list);
	while(data){
		buf += sprintf(buf,
			"Ibuf for space %lu: size %lu, free list len %lu, seg size %lu,\n",
			data->space, data->size, data->free_list_len, data->seg_size);

		buf += sprintf(buf,
			"%lu inserts, %lu merged recs, %lu merges\n",
			data->n_inserts, data->n_merged_recs, data->n_merges);

		data = UT_LIST_GET_NEXT(data_list, data);
	}

	mutex_exit(&ibuf_mutex);
}

