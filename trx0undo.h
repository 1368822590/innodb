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
	ulint				trx_id;			/*����ID*/
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

UNIV_INLINE dulint trx_undo_build_roll_ptr(ibool is_insert, ulint rseg_id, ulint page_no, ulint offset);

#include "trx0undo.inl"

#endif




