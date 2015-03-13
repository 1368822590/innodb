#ifndef __trx0undo_h_
#define __trx0undo_h_

#include "univ.h"
#include "trx0types.h"
#include "mtr0mtr.h"
#include "trx0sys.h"
#include "page0types.h"

struct trx_undo_struct
{
	ulint				id;				/*undo log segment��rollback segment��slot id*/
	ulint				type;			/*undo�����ͣ�insert����update*/
	ulint				state;
	ibool				del_marks;		/*ɾ����ʾ��TRUEʱ���������delete mark�����������������extern������*/
	dulint				trx_id;			/*����ID*/
	ibool				dict_operation;	/*�Ƿ���DDL����*/
	dulint				table_id;		/*DDLD��Ӧ��table id*/
	trx_rseg_t*			rseg;			/*��Ӧ�Ļع��ξ��*/
	ulint				space;			/*undo log��Ӧ�ı�ռ�ID*/
	ulint				hdr_page_no;	/*undo log header���ڵ�ҳ���*/
	ulint				hdr_offset;		/*undo log header����ҳ�е�ƫ����*/
	ulint				last_page_no;	/*undo log�����µĵ�undo pageҳ�����*/
	ulint				size;			/*��ǰundo logռ�õ�ҳ����*/
	ulint				empty;			/*undo log�Ƿ�Ϊ�գ��������ȫ��select�����������ʾ����Ч*/
	ulint				top_page_no;	/*���һ��undo log��־���ڵ�page���*/
	ulint				top_offset;		/*���һ��undo log��־���ڵ�pageҳ�е�ƫ��*/
	dulint				top_undo_no;	/*���һ��undo log��־��undo no*/
	page_t*				guess_page;		/*���һ��undo log��buffer pool�е�ҳָ��*/
	UT_LIST_NODE_T(trx_undo_t) undo_list;
};

/*undo log page�Ľṹ����*/
#define TRX_UNDO_PAGE_HDR				FSEG_PAGE_DATA
#define TRX_UNDO_PAGE_TYPE				0
#define TRX_UNDO_PAGE_START				2
#define TRX_UNDO_PAGE_FREE				4
#define TRX_UNDO_PAGE_NODE				6
#define TRX_UNDO_PAGE_HDR_SIZE			(6 + FLST_NODE_SIZE)
#define TRX_UNDO_PAGE_REUSE_LIMIT		(3 * UNIV_PAGE_SIZE / 4)

#define	TRX_UNDO_SEG_HDR				(TRX_UNDO_PAGE_HDR + TRX_UNDO_PAGE_HDR_SIZE)
#define	TRX_UNDO_STATE					0
#define	TRX_UNDO_LAST_LOG				2
#define	TRX_UNDO_FSEG_HEADER			4
#define	TRX_UNDO_PAGE_LIST				(4 + FSEG_HEADER_SIZE)
#define TRX_UNDO_SEG_HDR_SIZE			(4 + FSEG_HEADER_SIZE + FLST_BASE_NODE_SIZE)
#define	TRX_UNDO_TRX_ID					0
#define TRX_UNDO_TRX_NO					8
#define TRX_UNDO_DEL_MARKS				16
#define	TRX_UNDO_LOG_START				18
#define	TRX_UNDO_DICT_OPERATION			20
#define TRX_UNDO_TABLE_ID				22
#define	TRX_UNDO_NEXT_LOG				30
#define TRX_UNDO_PREV_LOG				32
#define TRX_UNDO_HISTORY_NODE			34
#define TRX_UNDO_LOG_HDR_SIZE			(34 + FLST_NODE_SIZE)

/*undo log segment������*/
#define TRX_UNDO_INSERT					1
#define TRX_UNDO_UPDATE					2
/*undo log segment��״̬*/
#define TRX_UNDO_ACTIVE					1
#define TRX_UNDO_CACHED					2
#define TRX_UNDO_TO_FREE				3
#define TRX_UNDO_TO_PURGE				4

UNIV_INLINE dulint						trx_undo_build_roll_ptr(ibool is_insert, ulint rseg_id, ulint page_no, ulint offset);

UNIV_INLINE void						trx_undo_decode_roll_ptr(dulint roll_ptr, ibool* is_insert, ulint* rseg_id, ulint* page_no, ulint* offset);

UNIV_INLINE void						trx_write_roll_ptr(byte* ptr, dulint roll_ptr);

UNIV_INLINE dulint						trx_read_roll_ptr(byte* ptr);

UNIV_INLINE page_t*						trx_undo_page_get(ulint space, ulint page_no, mtr_t* mtr);

UNIV_INLINE page_t*						trx_undo_page_get_s_latched(ulint space, ulint page_no, mtr_t* mtr);

UNIV_INLINE trx_undo_rec_t*				trx_undo_page_get_prev_rec(trx_undo_rec_t* rec, ulint page_no, ulint offset);

UNIV_INLINE trx_undo_rec_t*				trx_undo_page_get_next_rec(trx_undo_rec_t* rec, ulint page_no, ulint offset);

UNIV_INLINE trx_undo_rec_t*				trx_undo_page_get_last_rec(page_t* undo_page, ulint page_no, ulint offset);

UNIV_INLINE trx_undo_rec_t*				trx_undo_page_get_first_rec(page_t* undo_page, ulint page_no, ulint offset);

trx_undo_rec_t*							trx_undo_get_prev_rec(trx_undo_rec_t* rec, ulint page_no, ulint offset, mtr_t* mtr);

trx_undo_rec_t*							trx_undo_get_next_rec(trx_undo_rec_t* rec, ulint page_no, ulint offset, mtr_t* mtr);

trx_undo_rec_t*							trx_undo_get_first_rec(ulint space, ulint page_no, ulint offset, ulint mode, mtr_t* mtr);

ulint									trx_undo_add_page(trx_t* trx, trx_undo_t* undo, mtr_t* mtr);

void									trx_undo_truncate_end(trx_t* trx, trx_undo_t* undo, dulint limit);

void									trx_undo_truncate_start(trx_rseg_t* rseg, ulint space, ulint hdr_page_no, ulint hdr_offset, dulint limit);

ulint									trx_undo_lists_init(trx_rseg_t* rseg);

trx_undo_t*								trx_undo_assign_undo(trx_t* trx, ulint type);

page_t*									trx_undo_set_state_at_finish(trx_t* trx, trx_undo_t* undo, mtr_t* mtr);

void									trx_undo_update_cleanup(trx_t* trx, page_t* undo_page, mtr_t* mtr);

dulint									trx_undo_update_cleanup_by_discard(trx_t* trx, mtr_t* mtr);

void									trx_undo_insert_cleanup(trx_t* trx);

byte*									trx_undo_parse_page_init(byte* ptr, byte* end_ptr, page_t* page, mtr_t* mtr);

byte*									trx_undo_parse_page_header(ulint type, byte* ptr, byte* end_ptr, page_t* page, mtr_t* mtr);

byte*									trx_undo_parse_discard_latest(byte* ptr, byte* end_ptr, page_t* page, mtr_t* mtr);

#include "trx0undo.inl"

#endif




