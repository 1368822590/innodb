#include "buf0lru.h"
#include "page0page.h"

extern ulint		ibuf_flush_count;

/*����pageʣ��ռ�ļ����������1/32��ҳ��СΪ��λ*/
#define IBUF_PAGE_SIZE_PER_FREE_SPACE 32

/*insert bufffer������ֵ,��ibuf_t->meter���*/
#define IBUF_THRESHOLD 50

struct ibuf_data_struct
{
	ulint			space;		/*��Ӧ��¼��table space id*/
	ulint			seg_size;	/*ibuf_data�����page����,��ռ�����ibuf_data��page����*/
	ulint			size;		/*ibuf btreeռ�õ�page����*/
	ibool			empty;		/*���κλ����¼��ʶ*/
	ulint			free_list_len; /*ibuf_data�����õĿ���page*/
	ulint			height;		/*ibuf btree�߶�*/
	dict_index_t*	index;		/*ibuf ��������*/
	UT_LIST_NODE_T(ibuf_data_t) data_list;

	ulint			n_inserts;	/*���뵽ibuf_data�Ĵ���*/
	ulint			n_merges;	/*ibuf��merge�Ĵ���*/
	ulint			n_merged_recs; /*��ibuf�б��ϲ�����������ҳ�еļ�¼����*/
};

struct ibuf_struct
{
	ulint			size;		/*��ǰibufռ��page�ռ���*/
	ulint			max_size;	/*������ռ�õ��ڴ�ռ���*/
	ulint			meter;
	UT_LIST_BASE_NODE_T(ibuf_data_t) data_list;
};

void				ibuf_set_free_bits(ulint type, page_t* page, ulint val, ulint max_val);

/*����һ��insert�ܲ��뵽ibuf���У����LRU��һ��buf_LRU_try_free_flushed_blocks*/
UNIV_INLINE ibool	ibuf_should_try(dict_index_t* index, ulint ignore_sec_unique)
{
	if(!(index->type & DICT_CLUSTERED) && (ignore_sec_unique || !(index->type & DICT_UNIQUE)) && ibuf->meter > IBUF_THRESHOLD){
		ibuf_flush_count ++;
		if(ibuf_flush_count % 8 == 0) /*���Խ�LRU���Ѿ����̵�blocks�����ͷŵ�free list����*/
			buf_LRU_try_free_flushed_blocks();

		return TRUE;
	}

	return FALSE;
}

/*���page_no��Ӧ��page�Ƿ���ibuf bitmap page*/
UNIV_INLINE ibool  ibuf_bitmap_page(ulint page_no)
{
	/*bitmap page��insert buffer bitmap�еĵڶ�ҳ, һ��insert buffer bitmap�ܹ���16K��page*/
	if(page % XDES_DESCRIBED_PER_PAGE == FSP_IBUF_BITMAP_OFFSET)
		return TRUE;

	return FALSE;
}

/*Translates the free space on a page to a value in the ibuf bitmap.������ռ�ת��ΪIBUF_BITMAP_FREE�ϵ�ֵ:
 0	-	ʣ��ҳ�ռ�С��512B,Ҳ�п���û���κοռ�
 1	-	ʣ��ռ����1/32 page_size
 2	-	ʣ��ռ����1/16 page_size
 3  -	ʣ��ռ����1/8 page_size*/
UNIV_INLINE ulint ibuf_index_page_calc_free_bits(ulint max_ins_size)
{
	ulint n;
	
	n = max_ins_size / (UNIV_PAGE_SIZE / IBUF_PAGE_SIZE_PER_FREE_SPACE);
	if(n == 3)
		n = 2;

	if(n > 3)
		n = 3;

	return n;
}

/*Translates the ibuf free bits to the free space on a page in bytes. */
UNIV_INLINE ulint ibuf_index_page_calc_free_from_bits(ulint bits)
{
	ut_ad(bits < 4);

	if(bit == 3)
		return (4 * UNIV_PAGE_SIZE / IBUF_PAGE_SIZE_PER_FREE_SPACE);

	return bits * UNIV_PAGE_SIZE / IBUF_PAGE_SIZE_PER_FREE_SPACE;
}

/*��page�ļ�¼�����ʣ����õĿռ���ibuf bitmap��ʾ*/
UNIV_INLINE ulint ibuf_index_page_calc_free(page_t* page)
{
	return ibuf_index_page_calc_free_bits(page_get_max_insert_size_after_reorganize(page, 1));
}

/****************************************************************************
Updates the free bits of the page in the ibuf bitmap if there is not enough
free on the page any more. This is done in a separate mini-transaction, hence
this operation does not restrict further work to only ibuf bitmap operations,
which would result if the latch to the bitmap page were kept. */
UNIV_INLINE void ibuf_update_free_bits_if_full(dict_index_t* index, page_t* page,
	ulint max_ins_size, ulint increase)
{
	ulint before;
	ulint after;

	before = ibuf_index_page_calc_free_bits(max_ins_size);

	if(max_ins_size >= increase){
		ut_ad(ULINT_UNDEFINED > UNIV_PAGE_SIZE);
		after = ibuf_index_page_calc_free_bits(max_ins_size - increase);
	}
	else /*���һ�β�����Ҫ�Ŀռ����max_ins_size,����page������ҳ���ÿռ�*/
		after = ibuf_index_page_calc_free(page);

	if(after == 0)
		buf_page_make_young(page);

	if(before > after)
		ibuf_set_free_bits(index->type, page, after, before);
}

