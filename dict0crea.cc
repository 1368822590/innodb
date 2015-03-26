#include "dict0crea.h"

#include "btr0pcur.h"
#include "btr0btr.h"
#include "page0page.h"
#include "mach0data.h"
#include "dict0boot.h"
#include "dict0dict.h"
#include "que0que.h"
#include "row0ins.h"
#include "row0mysql.h"
#include "pars0pars.h"
#include "trx0roll.h"
#include "usr0sess.h"


static dtuple_t* dict_create_sys_tables_tuple(dict_table_t* table, mem_heap_t* heap);
static dtuple_t* dict_create_sys_columns_tuple(dict_table_t* table, ulint i, mem_heap_t* heap);
static dtuple_t* dict_create_sys_indexes_tuple(dict_index_t* index, mem_heap_t* heap, trx_t* trx);
static dtuple_t* dict_create_sys_fields_tuple(dict_index_t* index, ulint i, mem_heap_t* heap);
static dtuple_t* dict_create_search_tuple(dict_table_t* table, mem_heap_t* heap);
/*************************************************************************************/

/*��table���ֵ���Ϣ����һ��sys table���ڴ��߼���¼����*/
static dtuple_t* dict_create_sys_tables_tuple(dict_table_t* table, mem_heap_t* heap)
{
	dict_table_t*	sys_tables;
	dtuple_t*		entry;
	dfield_t*		dfield;
	byte*			ptr;

	ut_ad(table && heap);

	sys_tables = dict_sys->sys_tables;

	/*����һ��sys table���е��߼���¼����*/
	entry = dtuple_create(heap, 8 + DATA_N_SYS_COLS);
	/*NAME*/
	dfield = dtuple_get_nth_field(entry, 0);
	dfield_set_data(dfield, table->name, ut_strlen(table->name));
	/*ID*/
	dfield = dtuple_get_nth_field(entry, 1);
	ptr = mem_heap_alloc(heap, 8);
	mach_write_to_8(ptr, table->id);
	dfield_set_data(dfield, ptr, 8);
	/*N_COLS*/
	dfield = dtuple_get_nth_field(entry, 2);
	ptr = mem_heap_alloc(heap, 4);
	mach_write_to_4(ptr, table->n_def);
	dfield_set_data(dfield, ptr, 4);
	/*TYPE*/
	dfield = dtuple_get_nth_field(entry, 3);
	ptr = mem_heap_alloc(heap, 4);
	mach_write_to_4(ptr, table->type);
	dfield_set_data(dfield, ptr, 4);
	/*MIX ID*/
	dfield = dtuple_get_nth_field(entry, 4);
	ptr = mem_heap_alloc(heap, 8);
	mach_write_to_8(ptr, table->mix_id);
	dfield_set_data(dfield, ptr, 8);
	/*MIX_LEN*/
	dfield = dtuple_get_nth_field(entry, 5);
	ptr = mem_heap_alloc(heap, 4);
	mach_write_to_4(ptr, table->mix_len);
	dfield_set_data(dfield, ptr, 4);
	/*CLUSTER NAME*/
	dfield = dtuple_get_nth_field(entry, 6);

	if (table->type == DICT_TABLE_CLUSTER_MEMBER) {
		dfield_set_data(dfield, table->cluster_name, ut_strlen(table->cluster_name));
		ut_a(0);
	} 
	else
		dfield_set_data(dfield, NULL, UNIV_SQL_NULL);

	/*SPACE ID*/
	dfield = dtuple_get_nth_field(entry, 7);
	ptr = mem_heap_alloc(heap, 4);
	mach_write_to_4(ptr, table->space);
	dfield_set_data(dfield, ptr, 4);
	/*���ø����е���������*/
	dict_table_copy_types(entry, sys_tables);

	return entry;
}

/*����table���ֵ���Ϣ����һ��table��i�е�SYS_COLUMNS����ڴ��߼���¼����*/
static dtuple_t* dict_create_sys_columns_tuple(dict_table_t* table, ulint i, mem_heap_t* heap)
{
	dict_table_t*	sys_columns;
	dtuple_t*	entry;
	dict_col_t*	column;
	dfield_t*	dfield;
	byte*		ptr;

	ut_ad(table && heap);

	column = dict_table_get_nth_col(table, i);
	sys_columns = dict_sys->sys_columns;

	entry = dtuple_create(heap, 7 + DATA_N_SYS_COLS);

	/* 0: TABLE_ID -----------------------*/
	dfield = dtuple_get_nth_field(entry, 0);
	ptr = mem_heap_alloc(heap, 8);
	mach_write_to_8(ptr, table->id);
	dfield_set_data(dfield, ptr, 8);
	/* 1: POS ----------------------------*/
	dfield = dtuple_get_nth_field(entry, 1);
	ptr = mem_heap_alloc(heap, 4);
	mach_write_to_4(ptr, i);
	dfield_set_data(dfield, ptr, 4);
	/* 4: NAME ---------------------------*/
	dfield = dtuple_get_nth_field(entry, 2);
	dfield_set_data(dfield, column->name, ut_strlen(column->name));
	/* 5: MTYPE --------------------------*/
	dfield = dtuple_get_nth_field(entry, 3);
	ptr = mem_heap_alloc(heap, 4);
	mach_write_to_4(ptr, (column->type).mtype);
	dfield_set_data(dfield, ptr, 4);
	/* 6: PRTYPE -------------------------*/
	dfield = dtuple_get_nth_field(entry, 4);
	ptr = mem_heap_alloc(heap, 4);
	mach_write_to_4(ptr, (column->type).prtype);
	dfield_set_data(dfield, ptr, 4);
	/* 7: LEN ----------------------------*/
	dfield = dtuple_get_nth_field(entry, 5);
	ptr = mem_heap_alloc(heap, 4);
	mach_write_to_4(ptr, (column->type).len);

	dfield_set_data(dfield, ptr, 4);
	/* 8: PREC ---------------------------*/
	dfield = dtuple_get_nth_field(entry, 6);

	ptr = mem_heap_alloc(heap, 4);
	mach_write_to_4(ptr, (column->type).prec);

	dfield_set_data(dfield, ptr, 4);
	/*---------------------------------*/

	dict_table_copy_types(entry, sys_columns);
	return entry;
}

/*����MYSQL����ı���󣬽�Ԫ������ӵ�SYS TABLE��*/
static ulint dict_build_table_def_step(que_thr_t* thr, tab_node_t* node)
{
	dict_table_t*	table;
	dict_table_t*	cluster_table;
	dtuple_t*	row;

	UT_NOT_USED(thr);
	ut_ad(mutex_own(&(dict_sys->mutex)));

	table = node->table;
	table->id = dict_hdr_get_new_id(DICT_HDR_TABLE_ID);
	thr_get_trx(thr)->table_id = table->id;

	if(table->type == DICT_TABLE_CLUSTER_MEMBER){
		cluster_table = dict_table_get_low(table->cluster_name);
		if(cluster_table == NULL)
			return DB_CLUSTER_NOT_FOUND;
		/*���ñ�ռ䣬������cluster table*/
		table->space = cluster_table->space;
		table->mix_len = cluster_table->mix_len;
		/*��ȡһ�����ظ���mix id*/
		table->mix_id = dict_hdr_get_new_id(DICT_HDR_MIX_ID);
	}

	row = dict_create_sys_tables_tuple(table, node->heap);
	ins_node_set_new_row(node->table_def, row);

	return DB_SUCCESS;
}

/*��node->table�ĵ�i���ֵ���Ϣ���뵽SYS COLUMN��*/
static ulint dict_build_col_def_step(tab_node_t* node)
{
	dtuple_t*	row;

	row = dict_create_sys_columns_tuple(node->table, node->col_no, node->heap);
	ins_node_set_new_row(node->col_def, row);
	
	return DB_SUCCESS;
}

/*����������index�е��ֵ���Ϣ����SYS INDEX��ļ�¼��ʽ����һ���ڴ��¼����(tuple)*/
static dtuple_t* dict_create_sys_indexes_tuple(dict_index_t* index, mem_heap_t* heap, trx_t* trx)
{
	dict_table_t*	sys_indexes;
	dict_table_t*	table;
	dtuple_t*	entry;
	dfield_t*	dfield;
	byte*		ptr;

	UT_NOT_USED(trx);
	ut_ad(mutex_own(&(dict_sys->mutex)));
	ut_ad(index && heap);

	sys_indexes = dict_sys->sys_indexes;
	/*����tuple��¼����*/
	table = dict_table_get_low(index->table_name);
	entry = dtuple_create(heap, 7 + DATA_N_SYS_COLS);

	/* 0: TABLE_ID -----------------------*/
	dfield = dtuple_get_nth_field(entry, 0);
	ptr = mem_heap_alloc(heap, 8);
	mach_write_to_8(ptr, table->id);
	dfield_set_data(dfield, ptr, 8);
	/* 1: ID ----------------------------*/
	dfield = dtuple_get_nth_field(entry, 1);
	ptr = mem_heap_alloc(heap, 8);
	mach_write_to_8(ptr, index->id);
	dfield_set_data(dfield, ptr, 8);
	/* 4: NAME --------------------------*/
	dfield = dtuple_get_nth_field(entry, 2);
	dfield_set_data(dfield, index->name, ut_strlen(index->name));
	/* 5: N_FIELDS ----------------------*/
	dfield = dtuple_get_nth_field(entry, 3);
	ptr = mem_heap_alloc(heap, 4);
	mach_write_to_4(ptr, index->n_fields);
	dfield_set_data(dfield, ptr, 4);

	/* 6: TYPE --------------------------*/
	dfield = dtuple_get_nth_field(entry, 4);
	ptr = mem_heap_alloc(heap, 4);
	mach_write_to_4(ptr, index->type);
	dfield_set_data(dfield, ptr, 4);

	/* 7: SPACE --------------------------*/
	ut_a(DICT_SYS_INDEXES_SPACE_NO_FIELD == 7);
	dfield = dtuple_get_nth_field(entry, 5);
	ptr = mem_heap_alloc(heap, 4);
	mach_write_to_4(ptr, index->space);
	dfield_set_data(dfield, ptr, 4);

	/* 8: PAGE_NO --------------------------*/
	ut_a(DICT_SYS_INDEXES_PAGE_NO_FIELD == 8);
	dfield = dtuple_get_nth_field(entry, 6);
	ptr = mem_heap_alloc(heap, 4);
	mach_write_to_4(ptr, FIL_NULL);
	dfield_set_data(dfield, ptr, 4);
	/*--------------------------------*/

	dict_table_copy_types(entry, sys_indexes);

	return entry;
}

/*��index�е�i������������feild�ֵ���Ϣ����һ��SYS FIELD���ڴ��߼���¼����*/
static dtuple_t* dict_create_sys_fields_tuple(dict_index_t* index, ulint i, mem_heap_t* heap)
{
	dict_table_t*	sys_fields;
	dtuple_t*		entry;
	dict_field_t*	field;
	dfield_t*		dfield;
	byte*			ptr;

	ut_ad(index && heap);

	field = dict_index_get_nth_field(index, i);
	sys_fields = dict_sys->sys_fields;
	entry = dtuple_create(heap, 3 + DATA_N_SYS_COLS);

	/* 0: INDEX_ID -----------------------*/
	dfield = dtuple_get_nth_field(entry, 0);
	ptr = mem_heap_alloc(heap, 8);
	mach_write_to_8(ptr, index->id);
	dfield_set_data(dfield, ptr, 8);
	/* 1: POS ----------------------------*/
	dfield = dtuple_get_nth_field(entry, 1);
	ptr = mem_heap_alloc(heap, 4);
	mach_write_to_4(ptr, i);
	dfield_set_data(dfield, ptr, 4);
	/* 4: COL_NAME -------------------------*/
	dfield = dtuple_get_nth_field(entry, 2);
	dfield_set_data(dfield, field->name, ut_strlen(field->name));
	/*---------------------------------*/

	dict_table_copy_types(entry, sys_fields);

	return entry;
}

/*���һ�����Խ�������������λ��dtuple*/
static dtuple_t* dict_create_search_tuple(dtuple_t* tuple, mem_heap_t* heap)
{
	dtuple_t*	search_tuple;
	dfield_t*	field1;
	dfield_t*	field2;

	ut_ad(tuple && heap);

	search_tuple = dtuple_create(heap, 2);

	field1 = dtuple_get_nth_field(tuple, 0);	
	field2 = dtuple_get_nth_field(search_tuple, 0);	
	dfield_copy(field2, field1);

	field1 = dtuple_get_nth_field(tuple, 1);	
	field2 = dtuple_get_nth_field(search_tuple, 1);	
	dfield_copy(field2, field1);

	ut_ad(dtuple_validate(search_tuple));

	return search_tuple;
}

/*ͨ��node�е���Ϣ����һ��SYS INDEX���е����������¼,������SYS INDEX��*/
static ulint dict_build_index_def_step(que_thr_t* thr, ind_node_t* node)
{
	dict_table_t*	table;
	dict_index_t*	index;
	dtuple_t*	row;

	UT_NOT_USED(thr);
	ut_ad(mutex_own(&(dict_sys->mutex)));

	index = node->index;
	table = dict_table_get_low(index->table_name);
	if(table == NULL)
		return DB_TABLE_NOT_FOUND;
	/*����table id*/
	thr_get_trx(thr)->table->id = table->id;

	node->table = table;
	ut_ad((UT_LIST_GET_LEN(table->indexes) > 0) || (index->type & DICT_CLUSTERED));
	/*Ϊ����������һ��������ID*/
	index->id = dict_hdr_get_new_id(DICT_HDR_INDEX_ID);
	if(index->type & DICT_CLUSTERED)
		index->space = table->space;

	index->page_no = FIL_NULL;
	/*����һ��index��¼tuple*/
	row = dict_create_sys_indexes_tuple(index, node->heap, thr_get_trx(thr));
	node->ind_row = row;

	ins_node_set_new_row(node->ind_def, row);

	return DB_SUCCESS;
}

/*����һ��field��ʽΪSYS_FIELD����ڴ��м�¼tuple���󣬲���row���뵽SYS_FIELD��*/
static ulint dict_build_field_def_step(ind_node_t* node)
{
	dict_index_t*	index;
	dtuple_t*	row;

	index = node->index;
	row = dict_create_sys_fields_tuple(index, node->field_no, node->heap);
	ins_node_set_new_row(node->field_def, row);

	return DB_SUCCESS;
}

/*��������cluster member�����������������*/
static ulint dict_create_index_tree_step(que_thr_t* thr, ind_node_t* node)
{
	dict_index_t*	index;
	dict_table_t*	sys_indexes;
	dict_table_t*	table;
	dtuple_t*	search_tuple;
	btr_pcur_t	pcur;
	mtr_t		mtr;

	ut_ad(mutex_own(&(dict_sys->mutex)));
	UT_NOT_USED(thr);

	index = node->index;	
	table = node->table;

	sys_indexes = dict_sys->sys_indexes;
	if(index->type & DICT_CLUSTERED && table->type == DICT_TABLE_CLUSTER_MEMBER)
		return DB_SUCCESS;
	
	/*����һ��mini transaction*/
	mtr_start(&mtr);

	search_tuple = dict_create_search_tuple(node->ind_row, node->heap);
	/*��SYS INDEX��ۼ�����������λ��search_tuple��BTREEλ��*/
	btr_pcur_open(UT_LIST_GET_FIRST(sys_indexes->indexes), search_tuple, PAGE_CUR_L, BTR_MODIFY_LEAF, &pcur, &mtr);
	btr_pcur_move_to_next_user_rec(&pcur, &mtr);

	/*��index->space�ı�ռ���Ϊ�½���������Ӹ�btree*/
	index->page_no = btr_create(index->type, index->space, index->id, &mtr);
	/*���´�����page noд�뵽sys index����*/
	page_rec_write_index_page_no(btr_pcur_get_rec(&pcur), DICT_SYS_INDEXES_PAGE_NO_FIELD, index->page_no, &mtr);
	btr_pcur_close(&pcur);

	if(index->page_no == FIL_NULL)
		return DB_OUT_OF_FILE_SPACE;

	return DB_SUCCESS;
}

/*ɾ��һ������������*/
void dict_drop_index_tree(rec_t* rec, mtr_t* mtr)
{
	ulint	root_page_no;
	ulint	space;
	byte*	ptr;
	ulint	len;

	ut_ad(mutex_own(&(dict_sys->mutex)));
	/*���SYS INDEX���ж�Ӧ��root page no*/
	ptr = rec_get_nth_field(rec, DICT_SYS_INDEXES_PAGE_NO_FIELD, &len);
	ut_ad(len == 4);
	root_page_no = mtr_read_ulint(ptr, MLOG_4BYTES, mtr);
	if(root_page_no == FIL_NULL)
		return ;

	/*���space id*/
	ptr = rec_get_nth_field(rec, DICT_SYS_INDEXES_SPACE_NO_FIELD, &len);
	space = mtr_read_ulint(ptr, MLOG_4BYTES, mtr);
	/*�ͷ�����btree��page,root page�����ͷ�*/
	btr_free_but_not_root(space, root_page_no);
	/*��root page���ͷ�*/
	btr_free_root(space, root_page_no, mtr);

	/*����SYS INDEX��Ӧ��¼��root page no*/
	page_rec_write_index_page_no(rec, DICT_SYS_INDEXES_PAGE_NO_FIELD, FIL_NULL, mtr);
}











