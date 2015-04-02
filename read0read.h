#ifndef __read0read_h_
#define __read0read_h_

#include "univ.h"
#include "ut0bype.h"
#include "ut0lst.h"
#include "trx0trx.h"

typedef struct read_view_struct read_view_t;
/*����Ŀɼ������ṹ*/
typedef struct read_view_struct
{
	ibool			can_be_too_old;					/*TRUE��ʾ�������purge old version, read view�����ܼ����������ݣ����ֿ��ܻ�DB_MISSING_HISTORY����*/
	dulint			low_limit_no;					
	dulint			low_limit_id;					/*trx_ids������ֵ*/
	dulint			up_limit_id;					/*trx ids������ֵ*/
	ulint			n_trx_ids;
	dulint*			trx_ids;						
	trx_t*			creator;						/*������������*/

	UT_LIST_NODE_T(read_view_t) view_list;			/*Ϊ����������ʽ������trx_sys->view_list���ж��������ǰ���ϵ*/
}read_view_t;


read_view_t*				read_view_open_now(trx_t* cr_trx, mem_heap_t* heap);

read_view_t*				read_view_oldest_copy_or_open_new(trx_t* cr_trx, mem_heap_t* heap);

void						read_view_close(read_view_t* view);

UNIV_INLINE	ibool			read_view_sees_trx_id(read_view_t* view, dulint trx_id);

void						read_view_printf(read_view_t* view);

#include "read0read.inl"

#endif
