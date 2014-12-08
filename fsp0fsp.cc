#include "fsp0fsp.h"
#include "buf0buf.h"
#include "fil0fil.h"
#include "sync0sync.h"
#include "mtr0log.h"
#include "fut0fut.h"
#include "ut0byte.h"
#include "srv0srv.h"
#include "page0types.h"
#include "ibuf0ibuf.h"
#include "btr0btr.h"
#include "btr0sea.h"
#include "dict0boot.h"
#include "dict0mem.h"
#include "log0log.h"

typedef	byte	fsp_header_t;
typedef byte	fseg_inode_t;

typedef byte	xdes_t;

#define FSP_HEADER_OFFSET			FIL_PAGE_DATA

#define FSP_NOT_USED				0
#define FSP_SIZE					8
#define FSP_FREE_LIMIT				12
#define FSP_LOWEST_NO_WRITE			16
#define FSP_FRAG_N_USED				20
#define FSP_FREE					24
/*FLST_BASE_NODE_SIZE = 16*/
#define FSP_FREE_FRAG				(24 + FLST_BASE_NODE_SIZE)
#define FSP_FULL_FRAG				(24 + 2 * FLST_BASE_NODE_SIZE)
#define FSP_SEG_ID					(24 + 3 * FLST_BASE_NODE_SIZE)
#define FSP_SEG_INODES_FULL			(32 + 3 * FLST_BASE_NODE_SIZE)
#define FSP_SEG_INODES_FREE			(32 + 4 * FLST_BASE_NODE_SIZE)
/*file space header size*/
#define FSP_HEADER_SIZE				(32 + 5 * FLST_BASE_NODE_SIZE)
#define FSP_FREE_ADD				4

/*segment inode*/
#define FSEG_INODE_PAGE_NODE		FSEG_PAGE_DATA
/*FLST_NODE_SIZE = 12*/
#define FSEG_ARR_OFFSET				(FSEG_PAGE_DATA + FLST_NODE_SIZE)

#define FSEG_ID						0 /*8�ֽ�*/
#define FSEG_NOT_FULL_N_USED		8
#define FSEG_FREE					12
#define FSEG_NOT_FULL				(12 + FLST_NODE_SIZE)
#define FSEG_FULL					(12 + 2 * FLST_NODE_SIZE)
#define FSEG_MAGIC_N				(12 + 3 * FLST_NODE_SIZE)
#define FSEG_FRAG_ARR				(16 + 3 * FLST_NODE_SIZE)
#define FSEG_FRAG_ARR_N_SLOTS		(FSP_EXTENT_SIZE / 2)
#define FSEG_FRAG_SLOT_SIZE			4

#define FSEG_INODE_SIZE				(16 + 3 * FLST_BASE_NODE_SIZE + FSEG_FRAG_ARR_N_SLOTS * FSEG_FRAG_SLOT_SIZE)

#define FSP_SEG_INODES_PER_PAGE		((UNIV_PAGE_SIZE - FSEG_ARR_OFFSET - 10) / FSEG_INODE_SIZE)

#define FSEG_MAGIC_N_VALUE			97937874

#define	FSEG_FILLFACTOR				8
#define FSEG_FRAG_LIMIT				FSEG_FRAG_ARR_N_SLOTS
#define FSEG_FREE_LIST_LIMIT		40
#define	FSEG_FREE_LIST_MAX_LEN		4

/*��(extent)*/
#define	XDES_ID						0
#define XDES_FLST_NODE				8
#define	XDES_STATE					(FLST_NODE_SIZE + 8)
#define XDES_BITMAP					(FLST_NODE_SIZE + 12)

#define XDES_BITS_PER_PAGE			2
#define XDES_FREE_BIT				0
#define XDES_CLEAN_BIT				1

#define XDES_FREE					1
#define XDES_FREE_FRAG				2
#define XDES_FULL_FRAG				3
#define XDES_FSEG					4

#define XDES_SIZE					(XDES_BITMAP + (FSP_EXTENT_SIZE * XDES_BITS_PER_PAGE + 7) / 8)	/*40*/
#define XDES_ARR_OFFSET				(FSP_HEADER_OFFSET + FSP_HEADER_SIZE)							/*150*/

/********************************************************************/
/*�ͷ�һ��extent��space��free list��*/
static void		fsp_free_extent(ulint space, ulint page, mtr_t* mtr);
/*�ͷ�segment�е�һ��extent��space free list��*/
static void		fseg_free_extent(fseg_inode_t* seg_inode, ulint space, ulint page, mtr_t* mtr);

static ulint	fseg_n_reserved_pages_low(fseg_inode_t* header, ulint* used, mtr_t* mtr);

static void		fseg_mark_page_used(fseg_inode_t* seg_inde, ulint space, ulint page, mtr_t* mtr);

static xdes_t*	fseg_get_first_extent(fseg_inode_t* inode, mtr_t* mtr);

static void		fsp_fill_free_list(ulint space, fsp_header_t* header, mtr_t* mtr);

static ulint	fseg_alloc_free_page_low(ulint space, fseg_inode_t* seg_inode, ulint hint, byte	direction, mtr_t* mtr);

/*********************************************************************/
/*��ȡspace header�Ļ�����ָ��*/
UNIV_INLINE fsp_header_t* fsp_get_space_header(ulint space_id, mtr_t* mtr)
{
	fsp_header_t* header;
	
	ut_ad(mtr);
	/*���space header������*/
	header = buf_page_get(id, 0, RW_X_LATCH, mtr) + FSP_HEADER_OFFSET;
	buf_page_dbg_add_level(header, SYNC_FSP_PAGE);

	return header;
}

/*�ж�xdesָ����bitmap�ϵ�ֵ�Ƿ���1*/
UNIV_INLINE ibool xdes_get_bit(xdes_t* descr, ulint bit, ulint offset, mtr_t* mtr)
{
	ulint index;
	ulint byte_index;
	ulint bit_index;

	/*mtrһ������X LATCH*/
	ut_ad(mtr_memo_contains(mtr, buf_block_align(descr), MTR_MEMO_PAGE_X_FIX));
	ut_ad((bit == XDES_FREE_BIT) || (bit == XDES_CLEAN_BIT));
	ut_ad(offset < FSP_EXTENT_SIZE);

	index = bit + XDES_BITS_PER_PAGE * offset;
	byte_index = index / 8;
	bit_index = index % 8;
	/*��ȡxdes�ж�Ӧ��bit���ж��Ƿ�Ϊ1*/
	return ut_bit_get_nth(mtr_read_ulint(descr + XDES_BITMAP + byte_index, MLOG_1BYTE, mtr), bit_index);
}

UNIV_INLINE void xdes_set_bit(xdes_t* descr, ulint bit, ulint offset, ibool val, mtr_t* mtr)
{
	ulint	index;
	ulint	byte_index;
	ulint	bit_index;
	ulint	descr_byte;

	ut_ad(mtr_memo_contains(mtr, buf_block_align(descr), MTR_MEMO_PAGE_X_FIX));
	ut_ad((bit == XDES_FREE_BIT) || (bit == XDES_CLEAN_BIT));
	ut_ad(offset < FSP_EXTENT_SIZE);

	index = bit + XDES_BITS_PER_PAGE * offset;
	byte_index = index / 8;
	bit_index = index % 8;

	descr_byte = mtr_read_ulint(descr + XDES_BITMAP + byte_index, MLOG_1BYTE, mtr);
	descr_byte = ut_bit_set_nth(descr_byte, bit_index, val);

	mlog_write_ulint(descr + XDES_BITMAP + byte_index, descr_byte, MLOG_1BYTE, mtr);
}

/*��hint���ɵ͵��߽��в���*/
UNIV_INLINE ulint xdes_find_bit(xdes_t* descr, ulint bit,ibool val, ulint hint, mtr_t* mtr)
{
	ulint i;

	ut_ad(descr && mtr);
	ut_ad(val <= TRUE);
	ut_ad(hint < FSP_EXTENT_SIZE);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(descr), MTR_MEMO_PAGE_X_FIX));

	/*hint��bit�ƶϵ�λ�ã����ܾ�bit���ڵ�λ��*/
	for(i = hint; i < FSP_EXTENT_SIZE; i ++){
		if(val == xdes_get_bit(descr, bit, i, mtr))
			return i;
	}

	for(i = 0; i < hint; i ++){
		if(val == xdes_get_bit(descr, bit, i, mtr))
			return i;
	}

	return ULINT_UNDEFINED;
}

/*��hint���ɸߵ��ͽ��в���*/
UNIV_INLINE ulint xdes_find_bit_downward(xdes_t* descr, ulint bit, ibool val, ulint hint, mtr_t* mtr)
{
	ulint	i;

	ut_ad(descr && mtr);
	ut_ad(val <= TRUE);
	ut_ad(hint < FSP_EXTENT_SIZE);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(descr), MTR_MEMO_PAGE_X_FIX));

	for(i = hint + 1; i > 0; i --){
		if(val == xdes_get_bit(descr, bit, i - 1, mtr))
			return i - 1;
	}

	for(i = FSP_EXTENT_SIZE - 1; i > hint; i --){
		if(val = xdes_get_bit(descr, bit, i, mtr))
			return i;
	}

	return ULINT_UNDEFINED;
}

/*�����Ѿ�������ʹ�õ�page����*/
UNIV_INLINE ulint xdes_get_n_used(xdes_t* descr, mtr_t* mtr)
{
	ulint i;
	ulint count = 0;

	ut_ad(descr && mtr);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(descr), MTR_MEMO_PAGE_X_FIX));

	for(i = 0; i < FSP_EXTENT_SIZE; i ++){
		if(xdes_get_bit(descr, XDES_FREE_BIT, i, mtr) == FALSE)
			count ++;
	}

	return count;
}

/*�ж�һ�����Ƿ����*/
UNIV_INLINE ibool xdes_is_free(xdes_t* descr, mtr_t* mtr)
{
	if(0 == xdes_get_n_used(descr, mtr))
		return TRUE;
	else
		return FALSE;
}

/*�ж�һ�����Ƿ�����*/
UNIV_INLINE ibool xdes_is_full(xdes_t* descr, mtr_t* mtr)
{
	if(xdes_get_n_used(descr, mtr) == FSP_EXTENT_SIZE)
		return TRUE;
	else
		return FALSE;
}

UNIV_INLINE void xdes_set_state(xdes_t* descr, ulint state, mtr_t* mtr)
{
	ut_ad(descr && mtr);
	ut_ad(state >= XDES_FREE);
	ut_ad(state <= XDES_FSEG);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(descr), MTR_MEMO_PAGE_X_FIX));

	mlog_write_ulint(descr + XDES_STATE, state, MLOG_4BYTES, mtr);
}

UNIV_INLINE ulint xdes_get_state(xdes_t* descr, mtr_t* mtr)
{
	ut_ad(descr && mtr);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(descr), MTR_MEMO_PAGE_X_FIX));

	return mtr_read_ulint(descr + XDES_STATE, MLOG_4BYTES, mtr);
}

UNIV_INLINE void xdes_init(xdes_t* descr, mtr_t* mtr)
{
	ulint	i;

	ut_ad(descr && mtr);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(descr), MTR_MEMO_PAGE_X_FIX));
	ut_ad((XDES_SIZE - XDES_BITMAP) % 4 == 0);

	/*��ʼ��bitmap*/
	for(i = XDES_BITMAP, i < XDES_SIZE; i += 4){
		mlog_write_ulint(descr + i, 0xFFFFFFFF, MLOG_4BYTES, mtr);
	}

	/*������״̬*/
	xdes_set_state(descr, XDES_FREE, mtr);
}

/*offset��page size����*/
UNIV_INLINE ulint xdes_calc_descriptor_page(ulint offset)
{
	ut_ad(UNIV_PAGE_SIZE > XDES_ARR_OFFSET + (XDES_DESCRIBED_PER_PAGE / FSP_EXTENT_SIZE) * XDES_SIZE);

	return ut_2pow_round(offset, XDES_DESCRIBED_PER_PAGE);
}

/*����offset��Ӧ��page ��������*/
UNIV_INLINE ulint xdes_calc_descriptor_index(ulint offset)
{
	ut_2pow_remainder(offset, XDES_DESCRIBED_PER_PAGE) / FSP_EXTENT_SIZE;
}

UNIV_INLINE xdes_t* xdes_get_descriptor_with_space_hdr(fsp_header_t* sp_header, ulint space, ulint offset, mtr_t* mtr)
{
	ulint limit;
	ulint size;
	ulint descr_page_no;
	ulint descr_page;

	ut_ad(mtr);
	ut_ad(mtr_memo_contains(mtr, fil_space_get_latch(space), MTR_MEMO_X_LOCK));

	limit = mtr_read_ulint(sp_header + FSP_FREE_LIMIT, MLOG_4BYTES, mtr);
	size = mtr_read_ulint(sp_header + FSP_SIZE, MLOG_4BYTES, mtr);

	if(offset >= size || offset > limit)
		return NULL;

	/*�Ѿ�����limit���ޣ����п����µ�extent*/
	if(offset == limit)
		fsp_fill_free_list(space, sp_header, mtr);
	
	/*�����Ӧ��page no*/
	descr_page_no = xdes_calc_descriptor_page(offset);

	if(descr_page_no == 0)
		descr_page = buf_frame_align(sp_header);
	else{
		descr_page = buf_page_get(space, descr_page_no, RW_X_LATCH, mtr); /*���descr_page_no��Ӧ��ָ���ַ*/
		buf_page_dbg_add_level(descr_page, SYNC_FSP_PAGE);
	}

	return (descr_page + XDES_ARR_OFFSET + XDES_SIZE * xdes_calc_descriptor_index(offset));
}

/*ͨ��offset��ȡspace��Ӧ��������ָ��*/
static xdes_t* xdes_get_descriptor(ulint space, ulint offset, mtr_t* mtr)
{
	fsp_header_t* sp_header;
	sp_header = FSP_HEADER_OFFSET + buf_page_get(space, 0, RW_X_LATCH, mtr);
	buf_page_dbg_add_level(sp_header, SYNC_FSP_PAGE);

	return xdes_get_descriptor_with_space_hdr(sp_header, space, offset, mtr);
}

/*ͨ��fil_addr���extentָ��*/
UNIV_INLINE xdes_t* xdes_lst_get_descriptor(ulint space, fil_addr_t lst_node, mtr_t* mtr)
{
	xdes_t* descr;

	ut_ad(mtr);
	ut_ad(mtr_memo_contains(mtr, fil_space_get_latch(space), MTR_MEMO_X_LOCK));

	descr = fut_get_ptr(space, lst_node, RW_X_LATCH, mtr) - XDES_FLST_NODE;

	return descr;
}

UNIV_INLINE xdes_t* xdes_ls_get_next(xdes_t* descr, mtr_t* mtr)
{
	ulint	space;

	ut_ad(mtr & descr);

	space = buf_frame_get_space_id(descr);

	return xdes_lst_get_descriptor(space, flst_get_next_addr(descr + XDES_FLST_NODE, mtr), mtr);
}

/*����descr����extent��һ��ҳ��ƫ����*/
UNIV_INLINE ulint xdes_get_offset(xdes_t* descr)
{
	ut_ad(descr);
	return buf_frame_get_page_no(descr) + ((descr - buf_frame_align(descr) - XDES_ARR_OFFSET) / XDES_SIZE) * FSP_EXTENT_SIZE;
}

/*��ʼ��һ��pageҳ*/
static void fsp_init_file_page_low(byte* ptr)
{
	page_t* page;
	page = buf_frame_align(ptr);

	/*��ʼ��page trailer lsn*/
	mach_write_to_8(page + UNIV_PAGE_SIZE - FIL_PAGE_END_LSN, ut_dulint_zero);
	/*��ʼ��page lsn*/
	mach_write_to_8(page + FIL_PAGE_LSN, ut_dulint_zero);
}

void fsp_init_file_page(page_t* page, mtr_t* mtr)
{
	/*��ʼ��һ��ҳ*/
	fsp_init_file_page_low(page);
	/*��¼һ����ʼ��ҳ��mtr��־*/
	mlog_write_initial_log_record(page, MLOG_INIT_FILE_PAGE, mtr);
}

/*������ִ��һ����ʼ��ҳ��redo log,��������־ʱ����*/
byte* fsp_parse_init_file_page(byte* ptr, byte* end_ptr, page_t* page)
{
	ut_ad(ptr && end_ptr);
	if(page)
		fsp_init_file_page_low(page);

	return ptr;
}

void fsp_init()
{

}

/*��ʼ��space header*/
void fsp_header_init(ulint space, ulint size, mtr_t* mtr)
{
	fsp_header_t* header;
	page_t* page;

	ut_ad(mtr);
	
	/*���space mtr��x-latch����Ȩ*/
	mtr_x_lock(fil_space_get_latch(space), mtr);

	/*����һ����ʼ��ҳ*/
	page = buf_page_create(space, 0, mtr);
	buf_page_dbg_add_level(page, SYNC_FSP_PAGE);

	/*���page��X_LATCH*/
	buf_page_get(space, 0, RW_X_LATCH, mtr);
	buf_page_dbg_add_level(page, SYNC_FSP_PAGE);
	
	/*��ҳ���г�ʼ��*/
	fsp_init_file_page(page, mtr);

	/*���space header��ָ��*/
	header = FSP_HEADER_OFFSET + page;
	/*��ͷ��Ϣ���г�ʼ��*/
	mlog_write_ulint(header + FSP_SIZE, size, MLOG_4BYTES, mtr); 
	mlog_write_ulint(header + FSP_FREE_LIMIT, 0, MLOG_4BYTES, mtr); 
	mlog_write_ulint(header + FSP_LOWEST_NO_WRITE, 0, MLOG_4BYTES, mtr); 
	mlog_write_ulint(header + FSP_FRAG_N_USED, 0, MLOG_4BYTES, mtr);

	/*��ʼ����������б�*/
	flst_init(header + FSP_FREE, mtr);
	flst_init(header + FSP_FREE_FRAG, mtr);
	flst_init(header + FSP_FULL_FRAG, mtr);
	flst_init(header + FSP_SEG_INODES_FULL, mtr);
	flst_init(header + FSP_SEG_INODES_FREE, mtr);

	/*��ֵseg_id*/
	mlog_write_dulint(header + FSP_SEG_ID, ut_dulint_create(0, 1), MLOG_8BYTES, mtr);
	/*�����µ�һЩ���õ�extent*/
	fsp_fill_free_list(space, header, mtr);

	/*����һ��btree*/
	btr_create(DICT_CLUSTERED | DICT_UNIVERSAL | DICT_IBUF, space, ut_dulint_add(DICT_IBUF_ID_MIN, space), mtr);
}

/*����space�ĳ���*/
void fsp_header_inc_size(ulint space, ulint size_inc, mtr_t* mtr)
{
	fsp_header_t* header;
	ulint size;

	ut_ad(mtr);

	mtr_x_lock(fil_space_get_latch(space), mtr);

	header = fsp_get_space_header(space, mtr);
	size = mtr_read_ulint(header + FSP_SIZE, MLOG_4BYTES, mtr);
	mlog_write_ulint(header + FSP_SIZE, size + size_inc, MLOG_4BYTES, mtr);
}

ulint fsp_header_get_free_limit(ulint space)
{
	fsp_header_t* header;
	ulint limit;
	mtr_t mtr;

	ut_a(space == 0);
	mtr_start(&mtr);

	mtr_x_lock(fil_space_get_latch(space), &mtr);
	header = mtr_read_ulint(header + FSP_FREE_LIMIT, MLOG_4BYTES, &mtr);

	limit = limit / ((1024 * 1024) / UNIV_PAGE_SIZE);

	/*����log��checkpoint limit*/
	log_fsp_current_free_limit_set_and_checkpoint(limit);
	
	mtr_commit(&mtr);

	return limit;
}

/*��ñ�ռ��size*/
ulint fsp_header_get_tablespace_size(ulint space)
{
	fsp_header_t*	header;
	ulint		size;
	mtr_t		mtr;

	mtr_start(&mtr);

	mtr_x_lock(fil_space_get_latch(space), &mtr);
	header = fsp_get_space_header(space, mtr);
	size = mtr_read_ulint(header + FSP_SIZE, MLOG_4BYTES, &mtr);

	mtr_commit(&mtr);

	return size;
}

/*��������last data file�������ļ���������Զ�����*/
static ibool fsp_try_extend_last_file(ulint* actual_increase, ulint space, fsp_header_t* header, mtr_t* mtr)
{
	ulint	size;
	ulint	size_increase;
	ibool	success;

	ut_a(space == 0);

	*actual_increase = 0;
	/*�����ļ��Զ��Ŵ��*/
	if(!srv_auto_extend_last_data_file)
		return FALSE;

	/*ȷ������Ĵ�С*/
	size = mtr_read_ulint(header + FSP_SIZE, MLOG_4BYTES, mtr);

	if(srv_last_file_size_max != 0){
		if(srv_last_file_size_max < srv_data_file_sizes[srv_n_data_files - 1]){
			fprintf(stderr, "InnoDB: Error: Last data file size is %lu, max size allowed %lu\n",
				srv_data_file_sizes[srv_n_data_files - 1], srv_last_file_size_max);
		}

		size_increase = srv_last_file_size_max - srv_data_file_sizes[srv_n_data_files - 1];
		if(size_increase > SRV_AUTO_EXTEND_INCREMENT)
			size_increase = SRV_AUTO_EXTEND_INCREMENT;
	}
	else
		size_increase = SRV_AUTO_EXTEND_INCREMENT;

	if(size_increase == 0)
		return TRUE;

	/*�Ŵ����data file,���󲿷���0�������*/
	success = fil_extend_last_data_file(actual_increase, size_increase);
	if(success) /*��������space size*/
		mlog_write_ulint(header + FSP_SIZE, size + *actual_increase, MLOG_4BYTES, mtr);

	return TRUE;
}

static void fsp_fill_free_list(ulint space, fsp_header_t* header, mtr_t* mtr)
{
	ulint	limit;
	ulint	size;
	xdes_t*	descr;
	ulint	count = 0;
	ulint	frag_n_used;
	page_t*	descr_page;
	page_t*	ibuf_page;
	ulint	actual_increase;
	ulint	i;
	mtr_t	ibuf_mtr;

	ut_ad(header && mtr);
	/*���space size*/
	size = mtr_read_ulint(header + FSP_SIZE, MLOG_4BYTES, mtr);
	/*���space limit*/
	limit = mtr_read_ulint(header + FSP_FREE_LIMIT, MLOG_4BYTES, mtr);

	/*�ж��Ƿ�Ҫ�Ŵ�ռ�*/
	if(srv_auto_extend_last_data_file && size < limit + FSP_EXTENT_SIZE * FSP_FREE_ADD){
		fsp_try_extend_last_file(&actual_increase, space, header, mtr);
		size = mtr_read_ulint(header + FSP_SIZE, MLOG_4BYTES, mtr);
	}

	i = limit;
	/*һ��������64ҳ*/
	while((i + FSP_EXTENT_SIZE <= size) && count < FSP_FREE_ADD){
		mlog_write_ulint(header + FSP_FREE_LIMIT, i + FSP_EXTENT_SIZE, MLOG_4BYTES, mtr);

		log_fsp_current_free_limit_set_and_checkpoint((i + FSP_EXTENT_SIZE)/ ((1024 * 1024) / UNIV_PAGE_SIZE));
		if(0 == i % XDES_DESCRIBED_PER_PAGE){ /*��ʼ��һ������page*/
			if(i > 0){
				descr_page = buf_page_create(space, i, mtr);
				buf_page_dbg_add_level(descr_page,SYNC_FSP_PAGE);
				buf_page_get(space, i, RW_X_LATCH, mtr);
				buf_page_dbg_add_level(descr_page,SYNC_FSP_PAGE);

				fsp_init_file_page(descr_page, mtr);
			}

			mtr_start(&ibuf_mtr);
			/*����ibuf page�ĳ�ʼ��*/
			ibuf_page = buf_page_create(space, i + FSP_IBUF_BITMAP_OFFSET, &ibuf_mtr);
			buf_page_dbg_add_level(ibuf_page, SYNC_IBUF_BITMAP);
			buf_page_get(space, i + FSP_IBUF_BITMAP_OFFSET, RW_X_LATCH, &ibuf_mtr);
			buf_page_dbg_add_level(ibuf_page, SYNC_FSP_PAGE);
			
			fsp_init_file_page(ibuf_page, &ibuf_mtr);
			ibuf_bitmap_page_init(ibuf_page, &ibuf_mtr);

			mtr_commit(&ibuf_mtr);
		}

		/*�������������ʼ����*/
		descr = xdes_get_descriptor_with_space_hdr(header, space, i, mtr);
		xdes_init(descr, mtr);

		ut_ad(XDES_DESCRIBED_PER_PAGE % FSP_EXTENT_SIZE == 0);

		/*����page��Ӧbitmap�ϵ�״̬*/
		if(0 == i % XDES_DESCRIBED_PER_PAGE){
			xdes_set_bit(descr, XDES_FREE_BIT, 0, FALSE, mtr);
			xdes_set_bit(descr, XDES_FREE_BIT, FSP_IBUF_BITMAP_OFFSET, FALSE, mtr);
			xdes_set_state(descr, XDES_FREE_FRAG, mtr);

			flst_add_last(header + FSP_FREE_FRAG, descr + XDES_FLST_NODE, mtr);

			frag_n_used = mtr_read_ulint(header + FSP_FRAG_N_USED, MLOG_4BYTES, mtr);

			mlog_write_ulint(header + FSP_FRAG_N_USED, frag_n_used + 2, MLOG_4BYTES, mtr);
		}
		else{ /*���ӿ���extent*/
			flst_add_last(header + FSP_FREE, descr + XDES_FLST_NODE, mtr);
			count ++;
		}
		/*������һ��extent*/
		i += FSP_EXTENT_SIZE;
	}
}

/*���һ���µ�extent*/
static xdes_t* fsp_alloc_free_extent(ulint space, ulint hint, mtr_t* mtr)
{
	fsp_header_t*	header;
	fil_addr_t		first;
	xdes_t*			descr;

	ut_ad(mtr);

	/*���ͷ��Ϣ��ͷ��Ϣ�п��õ�extent*/
	header = fsp_get_space_header(space, mtr);
	descr = xdes_get_descriptor_with_space_hdr(header, space, hint, mtr);
	if(descr && (xdes_get_state(descr, mtr) == XDES_FREE)){

	}
	else{
		first = flst_get_first(header + FSP_FREE, mtr);
		if(fil_addr_is_null(first)){ /*free list�ǿյģ������¿��õ�extent*/
			fsp_fill_free_list(space, header, mtr);
			first = flst_get_first(header + FSP_FREE, mtr);
		}
		
		if(fil_addr_is_null(first))
			return NULL;

		/*���des������*/
		descr = xdes_lst_get_descriptor(space, first, mtr);
	}

	/*�ӿ��õĶ�����ɾ�����Ѿ������node*/
	flst_remove(header + FSP_FREE, descr + XDES_FLST_NODE, mtr);

	return descr;
}

static ulint fsp_alloc_free_page(ulint space, ulint hint, mtr_t* mtr)
{
	fsp_header_t*	header;
	fil_addr_t	first;
	xdes_t*		descr;
	page_t*		page;
	ulint		free;
	ulint		frag_n_used;
	ulint		page_no;

	ut_ad(mtr);

	header = fsp_get_space_header(space, mtr);
	descr = xdes_get_descriptor_with_space_hdr(header, space, hint, mtr);
	if(descr && (xdes_get_state(descr, mtr) == XDES_FREE_FRAG)){

	}
	else{
		first = flst_get_first(header + FSP_FREE_FRAG, mtr);
		if(fil_addr_is_null(first)){ /*��һ��extent����NULL,��FSP_FREE��ȡһ���µ�*/
			descr = fsp_alloc_free_extent(space, hint, mtr);
			if(descr == NULL)
				return FIL_NULL;
			
			xdes_set_state(descr, XDES_FREE_FRAG, mtr);
			flst_add_last(header + FSP_FREE_FRAG, descr + XDES_FLST_NODE, mtr);
		}
		else
			descr = xdes_lst_get_descriptor(space, first, mtr);

		hint = 0;
	}

	/*���ҿ����õ�ҳ*/
	free = xdes_find_bit(descr, XDES_FREE_BIT, TRUE, hint % FSP_EXTENT_SIZE, mtr);
	ut_a(free != NULL);
	/*����bitmapռ�ñ�־*/
	xdes_set_bit(descr, XDES_FREE_BIT, free, FALSE, mtr);

	frag_n_used = mtr_read_ulint(header + FSP_FRAG_N_USED, MLOG_4BYTES, mtr);
	frag_n_used ++;

	/*�޸��Ѿ�ʹ�õ�ҳ����*/
	mlog_write_ulint(header + FSP_FRAG_N_USED, frag_n_used, MLOG_4BYTES, mtr);
	if(xdes_is_full(descr, mtr)){/*����ռ����*/
		flst_remove(header + FSP_FREE_FRAG, descr + XDES_FLST_NODE, mtr);
		xdes_set_state(header + FSP_FULL_FRAG, descr + XDES_FLST_NODE, mtr);

		flst_add_last(header + FSP_FULL_FRAG, descr + XDES_FLST_NODE, mtr);

		mlog_write_ulint(header + FSP_FRAG_N_USED, frag_n_used - FSP_EXTENT_SIZE, MLOG_4BYTES, mtr);
	}

	page_no = xdes_get_offset(descr) + free;
	/*��ʼ��һ��ҳ*/
	buf_page_create(space, page_no, mtr);
	page = buf_page_get(space, page_no, RW_X_LATCH, mtr);
	buf_page_dbg_add_level(page, SYNC_FSP_PAGE);

	fsp_init_file_page(page, mtr);

	return page_no;
}

static void fsp_free_page(ulint space, ulint page, mtr_t* mtr)
{
	fsp_header_t*	header;
	xdes_t*			descr;
	ulint			state;
	ulint			frag_n_used;
	char			buf[1000];

	ut_ad(mtr);

	header = fsp_get_space_header(space, mtr);
	descr = xdes_get_descriptor_with_space_hdr(header, space, page, mtr);
	state = xdes_get_state(descr, mtr);

	if(state != XDES_FREE_FRAG && state != XDES_FULL_FRAG){
		fprintf(stderr,"InnoDB: Error: File space extent descriptor of page %lu has state %lu\n", page, state);
		ut_sprintf_buf(buf, ((byte*)descr) - 50, 200);
		fprintf(stderr, "InnoDB: Dump of descriptor: %s\n", buf);

		if(state == XDES_FREE)
			return;

		ut_a(0);
	}

	/*bitmap�еı�־�Ѿ��ǿ���״̬*/
	if(xdes_get_bit(descr, XDES_FREE_BIT, page % FSP_EXTENT_SIZE, mtr)){
		fprintf(stderr, "InnoDB: Error: File space extent descriptor of page %lu says it is free\n", page);
		ut_sprintf_buf(buf, ((byte*)descr) - 50, 200);

		fprintf(stderr, "InnoDB: Dump of descriptor: %s\n", buf);

		return;
	}

	xdes_set_bit(descr, XDES_FREE_BIT, page % FSP_EXTENT_SIZE, TRUE, mtr);
	xdes_set_bit(descr, XDES_CLEAN_BIT, page % FSP_EXTENT_SIZE, TRUE, mtr);

	frag_n_used = mtr_read_ulint(header + FSP_FRAG_N_USED, MLOG_4BYTES, mtr);
	if(state == XDES_FULL_FRAG){
		flst_remove(header + FSP_FULL_FRAG, descr + XDES_FLST_NODE, mtr);
		xdes_set_state(descr, FSP_FREE_FRAG, mtr);
		
		flst_add_last(header + FSP_FREE_FRAG, descr + XDES_FLST_NODE, mtr);
		/*�޸ĵ�ǰ��ռ�õ�ҳ���������һ��ҳ�ͷ�����һ�����������棬��ôӦ���������PAGE���� -1�������ۼӵģ��ǰ���Ϊ��λ��ռ��*/
		mlog_write_ulint(header + FSP_FRAG_N_USED, frag_n_used + FSP_EXTENT_SIZE - 1, MLOG_4BYTES, mtr);
	}
	else{
		ut_a(frag_n_used > 0);
		mlog_write_ulint(header + FSP_FRAG_N_USED, frag_n_used - 1, MLOG_4BYTES, mtr);
	}

	if(xdes_is_free(descr, mtr)){ /*������ȫ�����ˣ���FSP_FREE_FRAG��ɾ��������FSP_FREE����*/
		flst_remove(header + FSP_FREE_FRAG, descr + XDES_FLST_NODE, mtr);

		fsp_free_extent(space, page, mtr);
	}
}

static void fsp_free_extent(ulint space, ulint page, mtr_t* mtr)
{
	fsp_header_t* header;
	xdes_t*	descr;

	ut_ad(mtr);
	header = fsp_get_space_header(space, mtr);
	descr = xdes_get_descriptor_with_space_hdr(header, space, page, mtr);
	ut_a(xdes_get_state(descr, mtr) != XDES_FREE);
	/*���xdes*/
	xdes_init(descr, mtr);
	flst_add_last(header + FSP_FREE, descr + XDES_FLST_NODE, mtr);
}

UNIV_INLINE fseg_inode_t* fsp_seg_inode_page_get_nth_inode(page_t* page, ulint i, mtr_t* mtr)
{
	ut_ad(i < FSP_SEG_INODES_PER_PAGE);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(page), MTR_MEMO_PAGE_X_FIX));

	return(page + FSEG_ARR_OFFSET + FSEG_INODE_SIZE * i);
}

static ulint fsp_seg_inode_page_find_used(page_t* page, mtr_t* mtr)
{
	ulint		i;
	fseg_inode_t*	inode;

	for(i = 0; i < FSP_SEG_INODES_PER_PAGE; i ++){
		inode = fsp_seg_inode_page_get_nth_inode(page, i, mtr);
		if(ut_dulint_cmp(mach_read_from_8(inode + FSEG_ID), ut_dulint_zero) != 0)
			return i;
	}

	return ULINT_UNDEFINED;
}

static ulint fsp_seg_inode_page_find_free(page_t* page, ulint j, mtr_t* mtr)
{
	ulint i;
	fseg_inode_t*	inode;

	for(i = j; i < FSP_SEG_INODES_PER_PAGE; i ++){
		inode = fsp_seg_inode_page_get_nth_inode(page, i, mtr);
		if(ut_dulint_cmp(mach_read_from_8(inode + FSEG_ID), ut_dulint_zero) == 0)
			return i;
	}

	return ULINT_UNDEFINED;
}

/*����һ�����е�inodeҳ*/
static bool fsp_alloc_seg_inode_page(fsp_header_t* space_header, mtr_t* mtr)
{
	fseg_inode_t*	inode;
	page_t*		page;
	ulint		page_no;
	ulint		space;
	ulint		i;

	space = buf_frame_get_sapce_id(space_header);
	page_no = fsp_alloc_free_page(space, 0, mtr); /*���һ�����е�page*/
	if(page_no == FIL_NULL)
		return FALSE;

	page = buf_get_get(space, page_no, RW_X_LATCH, mtr);
	buf_page_dbg_add_level(page, SYNC_FSP_PAGE);

	/*��ʼ��inodeҳ*/
	for(i = 0; i < FSP_SEG_INODES_PER_PAGE; i ++){
		inode = fsp_seg_inode_page_get_nth_inode(page, i, mtr);
		mlog_write_dulint(inode + FSEG_ID, ut_dulint_zero, MLOG_8BYTES, mtr);
	}
	/*��ҳ���뵽FSP_SEG_INODES_FREE�����*/
	flst_add_last(space_header + FSP_SEG_INODES_FREE, page + FSEG_INODE_PAGE_NODE, mtr);

	return TRUE;
}

/*����һ���µ�segment inode*/
static fseg_inode_t* fsp_alloc_seg_inode(fsp_header_t* space_header, mtr_t* mtr)
{
	ulint		page_no;
	page_t*		page;
	fseg_inode_t*	inode;
	ibool		success;
	ulint		n;

	/*û�п��е�inodeҳ*/
	if(flst_get_len(space_header + FSP_SEG_INODES_FREE, mtr) == 0){
		if(fsp_alloc_seg_inode_page(space_header, mtr) == FALSE)
			return NULL;
	}

	page_no = flst_get_first(space_header + FSEP_SEG_INODES_FREE, mtr).page;
	page = buf_page_get(buf_frame_get_space_id(space_header), page, RW_X_LATCH, mtr);
	buf_page_dbg_add_level(page, SYNC_FSP_PAGE);

	n = fsp_seg_inode_page_find_free(pge, 0, mtr);
	ut_a(n != ULINT_UNDEFINED);

	inode = fsp_seg_inode_page_get_nth_inode(page, n, mtr);

	if(ULINT32_UNDEFINED == fsp_seg_inode_page_find_free(page, n + 1, mtr)){ /*���inode page�Ѿ�����,�����FSP_SEG_INODES_FREEɾ��*/
		flst_remove(space_header + FSP_SEG_INODES_FREE, page + FSEG_INODE_PAGE_NODE, mtr);
		flst_add_last(space_header + FSP_SEG_INODES_FULL, page + FSEG_INODE_PAGE_NODE, mtr);
	}

	return inode;
}

/*�ͷ�һ��segment inode*/
static void fsp_free_seg_inode(ulint space, fseg_inode_t* inode, mtr_t* mtr)
{
	page_t*		page;
	fsp_header_t*	space_header;

	page = buf_frame_align(inode);

	space_header = fsp_get_space_header(space, mtr);
	page = buf_frame_align(inode);

	/*ħ����У��*/
	ut_ad(mach_read_from_4(inode + FSEG_MAGIC_N) == FSEG_MAGIC_N_VALUE);
	if(ULINT_UNDEFINED == fsp_seg_inode_page_find_free(page, 0, mtr)){ /*���page���ڿ��е�inode*/
		flst_remove(space_header + FSP_SEG_INODES_FULL, page + FSEG_INODE_PAGE_NODE, mtr);
		flst_add_last(space_header + FSP_SEG_INODES_FREE, page + FSEG_INODE_PAGE_NODE, mtr);
	}
	/*����seg_id��ħ����*/
	mlog_write_dulint(inode + FSEG_ID, ut_dulint_zero, MLOG_8BYTES, mtr); 
	mlog_write_ulint(inode + FSEG_MAGIC_N, 0, MLOG_4BYTES, mtr);

	/*���ҳ�е�inodeȫ�����У��ͷŶ�ҳ��ռ��*/
	if(ULINT_UNDEFINED == fsp_seg_inode_page_find_used(page, mtr)){
		flst_remove(space_header + FSP_SEG_INODES_FREE, page + FSEG_INODE_PAGE_NODE, mtr);
		fsp_free_page(space, buf_frame_get_page_no(page), mtr);		
	}
}

static fseg_inode_t* fseg_inode_get(fseg_header_t* header, mtr_t* mtr)
{
	fil_addr_t	inode_addr;
	fseg_inode_t*	inode;

	/*��õ�ǰsegment inode������fil_addr_t��ַ*/
	inode_addr.page = mach_read_from_4(header + FSEG_HDR_PAGE_NO);
	inode_addr.boffset = mach_read_from_2(header + FSEG_HDR_OFFSET);
	/*��ö�Ӧinode��ָ���ַ*/
	inode = fut_get_ptr(mach_read_from_4(header + FSEG_HDR_SPACE), inode_addr, RW_X_LATCH, mtr);
	/*ħ����У��*/
	ut_ad(mach_read_from_4(inode + FSEG_MAGIC_N) == FSEG_MAGIC_N_VALUE);

	return inode;
}

/*���inodeָ��λ�õ�page no*/
UNIV_INLINE ulint fseg_get_nth_frag_page_no(fseg_inode_t* inode, ulint n, mtr_t* mtr)
{
	ut_ad(inode && mtr);
	ut_ad(n < FSEG_FRAG_ARR_N_SLOTS);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(inode), MTR_MEMO_PAGE_X_FIX));

	return(mach_read_from_4(inode + FSEG_FRAG_ARR + n * FSEG_FRAG_SLOT_SIZE));
}

UNIV_INLINE void fseg_set_nth_frag_page_no(fseg_inode_t* inode, ulint n, ulint page_no, mtr_t* mtr)
{
	ut_ad(inode && mtr);
	ut_ad(n < FSEG_FRAG_ARR_N_SLOTS);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(inode), MTR_MEMO_PAGE_X_FIX));
	/*��page noд�뵽��Ӧ��slotλ����*/
	mlog_write_ulint(inode + FSEG_FRAG_ARR + n * FSEG_FRAG_SLOT_SIZE, page_no, MLOG_4BYTES, mtr);
}

/*��segment��inode���л��һ�����е�page��slot*/
static ulint fseg_find_free_frag_page_slot(fseg_inode_t* inode, mtr_t* mtr)
{
	ulint i;
	ulint page_no;

	ut_ad(inode & mtr);
	for(i = 0; i < FSEG_FRAG_ARR_N_SLOTS; i ++){
		page_no = fseg_get_nth_frag_page_no(inode, i, mtr); /*���*/
		if(page_no == FIL_NULL)
			return i;
	}

	return ULINT_UNDEFINED;
}

/*��inode���в������һ����ʹ�õ�ҳ��slot*/
static ulint fseg_find_last_used_frag_page_slot(fseg_inode_t* inode, mtr_t* mtr)
{
	ulint i;
	ulint page_no;

	ut_ad(inode && mtr);

	/*�Ӻ�����ҵ�ǰ�棬Ӧ��Ϊ����Ч������*/
	for(i = 0; i < FSEG_FRAG_ARR_N_SLOTS; i ++){
		page_no = fseg_get_nth_frag_page_no(inode, FSEG_FRAG_ARR_N_SLOTS - i -1, mtr);
		if(page_no != FIL_NULL)
			return FSEG_FRAG_ARR_N_SLOTS - i - 1;
	}

	return ULINT_UNDEFINED;
}

/*��ȡinode���Ѿ�ʹ�õ�page��*/
static ulint fseg_get_n_frag_pages(fseg_inode_t* inode, mtr_t* mtr)
{
	ulint	i;
	ulint	count = 0;

	for(i = 0; i < FSEG_FRAG_ARR_N_SLOTS; i ++)
		if(FIL_NULL != fseg_get_nth_frag_page_no(inode, i, mtr))
			count ++;

	return count;
}

/*����һ��segment������*/
page_t* fseg_create_general(ulint space, ulint page, ulint byte_offset, ibool has_done_reservation, mtr_t* mtr)
{
	fsp_header_t*	space_header;
	fseg_inode_t*	inode;
	dulint		seg_id;
	fseg_header_t*	header;
	rw_lock_t*	latch;
	ibool		success;
	page_t*		ret	= NULL;
	ulint		i;

	ut_ad(mtr);

	/*���fseg_header*/
	if(page != 0)
		header = byte_offset + buf_page_get(space, page, RW_X_LATCH, mtr);

	ut_ad(!mutex_own(&kernel_mutex) || mtr_memo_contains(mtr, fil_space_get_latch(space), MTR_MEMO_X_LOCK));

	/*���space��x-latch����Ȩ*/
	latch = fil_space_get_latch(space);
	mtr_x_lock(latch, mtr); /*MTR_MEMO_X_LOCK*/

	if(rw_lock_get_reader_count(latch) == 1)
		ibuf_free_excess_pages(space);

	if(!has_done_reservation){
		success = fsp_reserve_free_extents(space, 2, FSP_NORMAL, mtr);
		if(!success)
			return NULL;
	}

	space_header = fsp_get_space_header(space, mtr);
	/*���һ�����е�inode*/
	inode = fsp_alloc_seg_inode(space_header, mtr);
	if(inode == NULL)
		goto funct_exit;

	seg_id = mtr_read_dulint(space_header + FSP_SEG_ID, MLOG_8BYTES, mtr);
	/*seg id  + 1*/
	mlog_write_dulint(space_header + FSP_SEG_ID, ut_dulint_add(seg_id, 1), MLOG_8BYTES, mtr);

	/*��inode��Ϣ���г�ʼ��*/
	mlog_write_dulint(inode + FSEG_ID, seg_id, MLOG_8BYTES, mtr);
	mlog_write_ulint(inode + FSEG_NOT_FULL_N_USED, 0, MLOG_4BYTES, mtr); 
	mlog_write_ulint(inode + FSEG_MAGIC_N, FSEG_MAGIC_N_VALUE, MLOG_4BYTES, mtr);

	for(i = 0; i < FSEG_FRAG_ARR_N_SLOTS; i ++)
		fseg_set_nth_frag_page_no(inode, i, FIL_NULL, mtr);

	if(page == 0){
		/*��ȡһ��ҳ*/
		page = fseg_alloc_free_page_low(space, inode, 0, FSP_UP, mtr);
		if(page == FIL_NULL){
			fsp_free_seg_inode(space, inode, mtr);
			goto funct_exit;
		}
		/*���header��λ��*/
		header = byte_offset + buf_page_get(space, page, RW_X_LATCH, mtr);
	}

	/*����segment����Ϣ*/
	mlog_write_ulint(header + FSEG_HDR_OFFSET, inode - buf_frame_align(inode), MLOG_2BYTES, mtr);
	mlog_write_ulint(header + FSEG_HDR_PAGE_NO, buf_frame_get_page_no(inode), MLOG_4BYTES, mtr);
	mlog_write_ulint(header + FSEG_HDR_SPACE, space, MLOG_4BYTES, mtr);

	ret = buf_frame_align(header);

funct_exit:
	if (!has_done_reservation)
		fil_space_release_free_extents(space, 2);

	return ret;
}

page_t* fseg_create(ulint space, ulint page, ulint byte_offset, mtr_t* mtr)
{
	return fseg_create_general(space, page, byte_offset, FALSE, mtr);
}

/*����fseg_inode�������е�page�����Ѿ�ʹ�õ�page����*/
static ulint fseg_n_reserved_pages_low(fseg_inode_t* node, ulint* used, mtr_t* mtr)
{
	ulint	ret;

	ut_ad(inode && used && mtr);
	ut_ad(mtr_memo_contains(mtr, buf_block_align(inode), MTR_MEMO_PAGE_X_FIX));

	*used = mtr_read_ulint(inode + FSEG_NOT_FULL_N_USED, MLOG_4BYTES, mtr) 
		+ FSP_EXTENT_SIZE * flst_get_len(inode + FSEG_FULL, mtr) + fseg_get_n_frag_pages(inode, mtr);

	/*����segment������page��*/
	ret = fseg_get_n_frag_pages(inode, mtr) + FSP_EXTENT_SIZE * flst_get_len(inode + FSEG_FREE, mtr)
		+ FSP_EXTENT_SIZE * flst_get_len(inode + FSEG_NOT_FULL, mtr)
		+ FSP_EXTENT_SIZE * flst_get_len(inode + FSEG_FULL, mtr);
	
	return ret;
}

ulint fseg_n_reserved_pages(fseg_header_t* node, ulint* used, mtr_t* mtr)
{
	ulint			ret;
	fseg_header_t*	inode;
	ulint			space;

	space = buf_frame_get_space_id(header);
	ut_ad(!mutex_own(&kernel_mutex) || mtr_memo_contains(mtr, fil_space_get_latch(space), MTR_MEMO_X_LOCK));

	mtr_x_lock(fil_space_get_latch(space), mtr);
	/*��ȡ��ǰsegment��inode*/
	inode = fseg_inode_get(header, mtr);

	return fseg_n_reserved_pages_low(inode, used, mtr);
}

static void fseg_fill_free_list(fseg_inode_t* inode, ulint space, ulint hint, mtr_t* mtr)
{
	xdes_t*	descr;
	ulint	i;
	dulint	seg_id;
	ulint	reserved;
	ulint	used;

	ut_ad(inode && mtr);

	reserved = fseg_n_reserved_pages_low(inode, &used, mtr);
	if(reserved < FSEG_FREE_LIST_LIMIT * FSP_EXTENT_SIZE) /*�ﵽһ��segment������page��*/
		return;

	/*inode����free��page*/
	if(flst_get_len(inode + FSEG_FREE, mtr) > 0)
		return 0;

	for(i = 0; i < FSEG_FREE_LIST_MAX_LEN; i ++){
		descr = xdes_get_descriptor(space, hint, mtr);
		if(descr == NULL || (XDES_FREE != xdes_get_state(descr, mtr)))
			return ;

		/*���һ���µ�extent*/
		descr = fsp_alloc_free_extent(space, hint, mtr);
		xdes_set_state(descr, XDES_FSEG, mtr);
		/*����seg id*/
		seg_id = mtr_read_dulint(inode + FSEG_ID, MLOG_8BYTES, mtr);
		mlog_write_dulint(descr + XDES_ID, seg_id, MLOG_8BYTES, mtr);

		/*���뵽segment�Ŀ���extent�б���*/
		flst_add_last(inode + FSEG_FREE, descr + XDES_FLST_NODE, mtr);

		hint += FSP_EXTENT_SIZE;
	}
}

static xdes_t* fseg_alloc_free_extent(fseg_inode_t* inode, ulint space, mtr_t* mtr)
{
	xdes_t*		descr;
	dulint		seg_id;
	fil_addr_t 	first;

	/*inode���п��е�*/
	if(flst_get_len(inode + FSEG_FREE, mtr) > 0){
		first = flst_get_first(inode + FSEG_FREE, mtr);
		descr = xdes_lst_get_descriptor(space, first, mtr);
	}
	else{
		descr = fsp_alloc_free_extent(space, 0, mtr);
		if(descr == NULL)
			return NULL;

		seg_id = mtr_read_dulint(inode + FSEG_ID, MLOG_8BYTES, mtr);
		/*����descr��״̬��Ϣ*/
		xdes_set_state(descr, XDES_FSEG, mtr);
		mlog_write_dulint(descr + XDES_ID, seg_id, MLOG_8BYTES, mtr);

		flst_add_last(inode + FSEG_FREE, descr + XDES_FLST_NODE, mtr);
		/*�ж���������ǲ����Ѿ�ȫ��������page,��������ˣ�����һ���µ�extent*/
		fseg_fill_free_list(inode, space, xdes_get_offset(descr) + FSP_EXTENT_SIZE, mtr);
	}

	return descr;
}

static ulint fseg_alloc_free_page_low(ulint space, fseg_inode_t* seg_inode, ulint hint, byte direction, mtr_t* mtr)
{ 
	dulint		seg_id;
	ulint		used;
	ulint		reserved;
	fil_addr_t	first;
	xdes_t*		descr;		/* extent of the hinted page */
	ulint		ret_page;	/* the allocated page offset, FIL_NULL if could not be allocated */
	xdes_t*		ret_descr;	/* the extent of the allocated page */
	page_t*		page;
	ibool		frag_page_allocated = FALSE;
	ulint		n;

	ut_ad(mtr);
	ut_ad((direction >= FSP_UP) && (direction <= FSP_NO_DIR));
	ut_ad(mach_read_from_4(seg_inode + FSEG_MAGIC_N) == FSEG_MAGIC_N_VALUE);

	seg_id = mtr_read_dulint(seg_inode + FSEG_ID, MLOG_8BYTES, mtr);
}

