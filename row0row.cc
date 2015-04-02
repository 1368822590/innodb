#include "row0row.h"
#include "dict0dict.h"
#include "btr0btr.h"
#include "mach0data.h"
#include "trx0rseg.h"
#include "trx0trx.h"
#include "trx0roll.h"
#include "trx0undo.h"
#include "trx0purge.h"
#include "trx0rec.h"
#include "que0que.h"
#include "row0row.h"
#include "row0upd.h"
#include "rem0cmp.h"
#include "read0read.h"

/*�Ӽ�¼�ж�ȡϵͳĬ�ϵ���(TRX ID/ROLL PTR)*/
dulint row_get_rec_sys_field(ulint type, rec_t* rec, dict_index_t* index)
{
	ulint	pos;
	byte*	field;
	ulint	len;

	ut_ad(index->type & DICT_CLUSTERED);

	/*�����������ж�ȡtype�����е�ƫ��*/
	pos = dict_index_get_sys_col_pos(index, type);
	field = rec_get_nth_field(rec, pos, &len);
	if(type == DATA_TRX_ID)
		return trx_read_trx_id(field);
	else{
		ut_ad(type == DATA_ROLL_PTR);
		return trx_read_roll_ptr(field);
	}
}

/*��ϵͳ��TRX ID/ROLL PTR��Ĭ�ϵ�����ֵ*/
void row_set_rec_sys_field(ulint type, rec_t* rec, dict_index_t* index, dulint val)
{
	ulint	pos;
	byte*	field;
	ulint	len;

	ut_ad(index->type & DICT_CLUSTERED);

	pos = dict_index_get_sys_col_pos(index, type);
	field = rec_get_nth_field(rec, pos, &len);
	if(type == DATA_TRX_ID)
		trx_write_trx_id(field, val);
	else{
		ut_ad(type == DATA_ROLL_PTR);
		trx_write_roll_ptr(field, val);
	}
}

/*����������Ҫ����Ҫ�󣬴�row����һ����Ӧ��������¼����*/
dtuple_t* row_build_index_entry(dtuple_t* row, dict_index_t* index, mem_heap_t* heap)
{
	dtuple_t*		entry;
	ulint			entry_len;
	dict_field_t*	ind_field;
	dfield_t*		dfield;
	dfield_t*		dfield2;
	dict_col_t*		col;
	ulint			i;

	ut_ad(row && index && heap);
	ut_ad(dtuple_check_typed(row));

	/*���index��������洢��Ӧ������*/
	entry_len = dict_index_get_n_fields(index);
	entry = dtuple_create(heap, entry_len);
	/*������������ȷ����¼������*/
	if(index->type & DICT_UNIVERSAL)
		dtuple_set_n_fields_cmp(entry, entry_len);
	else
		dtuple_set_n_fields_cmp(entry, dict_index_get_n_unique_in_tree(index));
	/*�п���*/
	for(i = 0; i < entry_len; i ++){
		ind_field = dict_index_get_nth_field(index, i);
		col = ind_field->col;

		dfield = dtuple_get_nth_field(entry, i);
		dfield2 = dtuple_get_nth_field(row, dict_col_get_no(col));

		dfield_copy(dfield, dfield2);
		dfield->col_no = dict_col_get_no(col);
	}

	ut_ad(dtuple_check_typed(entry));

	return entry;
}

/*��rec�й���һ�����Ͼۼ�������������¼��ȫ�п���*/
dtuple_t* row_build(ulint type, rec_t* rec ,dict_index_t* index, mem_heap_t* heap)
{
	dtuple_t*	row;
	dict_table_t*	table;
	dict_col_t*	col;
	dfield_t*	dfield;
	ulint		n_fields;
	byte*		field;
	ulint		len;
	ulint		row_len;
	byte*		buf; 
	ulint		i;

	ut_ad(index && rec && heap);
	ut_ad(index->type & DICT_CLUSTERED);

	if(type != ROW_COPY_POINTERS){
		buf = mem_heap_alloc(heap, rec_get_size(rec));
		rec = rec_copy(buf, rec);
	}

	/*����һ��table��Ӧ��tuple����*/
	table = index->table;
	row_len = dict_table_get_n_cols(table);
	row = dtuple_create(heap, row_len);

	dtuple_set_info_bits(row, rec_get_info_bits(rec));
	n_fields = dict_index_get_n_fields(index);

	ut_ad(n_fields == rec_get_n_fields(rec));

	for(i = 0; i < n_fields; i ++){
		col = dict_field_get_col(dict_index_get_nth_field(index, i));
		dfield = dtuple_get_nth_field(row, dict_col_get_no(col));
		field = rec_get_nth_field(rec, i, &len);
		/*BLOB�еĿ���*/
		if (type == ROW_COPY_ALSO_EXTERNALS && rec_get_nth_field_extern_bit(rec, i))
			field = btr_rec_copy_externally_stored_field(rec, i, &len, heap);

		dfield_set_data(dfield, field, len);
	}

	ut_ad(dtuple_check_typed(row));

	return row;
}

/*�������¼��rec��ת��������Ҫ���tuple�߼��ṹ�Ķ���,
rec ->tuple*/
dtuple_t* row_rec_to_index_entry(ulint type, dict_index_t* index, rec_t* rec, mem_heap_t* heap)
{
	dtuple_t*	entry;
	dfield_t*	dfield;
	ulint		i;
	byte*		field;
	ulint		len;
	ulint		rec_len;
	byte*		buf;

	ut_ad(rec && heap && index);

	/* Take a copy of rec to heap */
	if (type == ROW_COPY_DATA) {
		buf = mem_heap_alloc(heap, rec_get_size(rec));
		rec = rec_copy(buf, rec);
	}

	rec_len = rec_get_n_fields(rec);
	entry = dtuple_create(heap, rec_len);

	dtuple_set_n_fields_cmp(entry, dict_index_get_n_unique_in_tree(index));
	ut_ad(rec_len == dict_index_get_n_fields(index));

	dict_index_copy_types(entry, index, rec_len);
	dtuple_set_info_bits(entry, rec_get_info_bits(rec));

	for (i = 0; i < rec_len; i++) {
		dfield = dtuple_get_nth_field(entry, i);
		field = rec_get_nth_field(rec, i, &len);

		dfield_set_data(dfield, field, len);
	}

	ut_ad(dtuple_check_typed(entry));

	return entry;
}

/*ͨ��������������һ���ۼ�������Ӧ�ļ�¼����(dtuple)������ȡ���������������ݣ�
  �������������¼û�ж�Ӧ�У���mix_id_buf���,���¼����������ھۼ������Ķ�λ
  (rec_t -> tuple)*/
dtuple_t* row_build_row_ref(ulint type, dict_index_t* index, rec_t* rec, mem_heap_t* heap)
{
	dict_table_t*	table;
	dict_index_t*	clust_index;
	dfield_t*	dfield;
	dict_col_t*	col;
	dtuple_t*	ref;
	byte*		field;
	ulint		len;
	ulint		ref_len;
	ulint		pos;
	byte*		buf;
	ulint		i;

	/* Take a copy of rec to heap */
	if (type == ROW_COPY_DATA) {
		buf = mem_heap_alloc(heap, rec_get_size(rec));
		rec = rec_copy(buf, rec);
	}

	/*��ö�Ӧ��ľۼ�����*/
	table = index->table;
	clust_index = dict_table_get_first_index(table);
	ref_len = dict_index_get_n_unique(clust_index);
	/*�������������б�*/
	ref = dtuple_create(heap, ref_len);
	dict_index_copy_types(ref, clust_index, ref_len);
	for(i = 0; i < ref_len; i ++){
		dfield = dtuple_get_nth_field(ref, i);

		col = dict_field_get_col(dict_index_get_nth_field(clust_index, i));
		pos = dict_index_get_nth_col_pos(index, dict_col_get_no(col));

		/*�������������¼���ж�Ӧ���У��Ӹ���������¼�п���������*/
		if (pos != ULINT_UNDEFINED) {	
			field = rec_get_nth_field(rec, pos, &len);
			dfield_set_data(dfield, field, len);
		} 
		else {
			ut_ad(table->type == DICT_TABLE_CLUSTER_MEMBER);
			ut_ad(i == table->mix_len);

			dfield_set_data(dfield, mem_heap_alloc(heap, table->mix_id_len), table->mix_id_len);
			ut_memcpy(dfield_get_data(dfield), table->mix_id_buf, table->mix_id_len);
		}
	}
}

/*������row_build_row_ref���ƣ�����������ڲ���Ϊdtuple����ռ�
(rec_t -> tuple)*/
void row_build_row_ref_in_tuple(dtuple_t* ref, dict_index_t* index, rec_t* rec)
{
	dict_table_t*	table;
	dict_index_t*	clust_index;
	dfield_t*	dfield;
	dict_col_t*	col;
	byte*		field;
	ulint		len;
	ulint		ref_len;
	ulint		pos;
	ulint		i;

	ut_a(ref && index && rec);
	table = index->table;

	if (!table) {
		fprintf(stderr, "InnoDB: table %s for index %s not found\n", index->table_name, index->name);
		ut_a(0);
	}

	/*��ñ�ľۼ���������*/
	clust_index = dict_table_get_first_index(table);
	if (!clust_index) {
		fprintf(stderr, "InnoDB: clust index for table %s for index %s not found\n", index->table_name, index->name);
		ut_a(0);
	}

	ref_len = dict_index_get_n_unique(clust_index);
	ut_ad(ref_len == dtuple_get_n_fields(ref));
	dict_index_copy_types(ref, clust_index, ref_len);

	/*�п���*/
	for (i = 0; i < ref_len; i++) {
		dfield = dtuple_get_nth_field(ref, i);

		col = dict_field_get_col(dict_index_get_nth_field(clust_index, i));
		pos = dict_index_get_nth_col_pos(index, dict_col_get_no(col));

		if (pos != ULINT_UNDEFINED) {	
			field = rec_get_nth_field(rec, pos, &len);
			dfield_set_data(dfield, field, len);
		} 
		else {
			ut_ad(table->type == DICT_TABLE_CLUSTER_MEMBER);
			ut_ad(i == table->mix_len);
			ut_a(0);
		}	
	}

	ut_ad(dtuple_check_typed(ref));
}

/*�ο�����������¼����(row)����һ��tuple�����tuple�������ھۼ������ϵĲ���
tuple -> tuple*/
void row_build_row_ref_from_row(dtuple_t* ref, dict_table_t* table, dtuple_t* row)
{
	dict_index_t*	clust_index;
	dfield_t*	dfield;
	dfield_t*	dfield2;
	dict_col_t*	col;
	ulint		ref_len;
	ulint		i;

	ut_ad(ref && table && row);

	/*���Ҷ�Ӧ��ľۼ���������*/
	clust_index = dict_table_get_first_index(table);
	ref_len = dict_index_get_n_unique(clust_index);
	ut_ad(ref_len == dtuple_get_n_fields(ref));

	for(i = 0; i < ref_len; i ++){
		dfield = dtuple_get_nth_field(ref, i);
		col = dict_field_get_col(dict_index_get_nth_field(clust_index, i));
		dfield2 = dtuple_get_nth_field(row, dict_col_get_no(col));

		dfield_copy(dfield, dfield2);
	}

	ut_ad(dtuple_check_typed(ref));
}

/*ͨ�������Ĳο���¼�Ծۼ��������в���,�����ȫƥ�䵽�������TRUE*/
ibool row_search_on_row_ref(btr_pcur_t* pcur, ulint mode, dict_table_t* table, dtuple_t* ref, mtr_t* mtr)
{
	ulint		low_match;	
	rec_t*		rec;
	dict_index_t*	index;
	page_t*		page;	

	ut_ad(dtuple_check_typed(ref));

	/*��þۼ���������*/
	index = dict_table_get_first_index(table);
	ut_a(dtuple_get_n_fields(ref) == dict_index_get_n_unique(index));

	btr_pcur_open(index, ref, PAGE_CUR_LE, mode, pcur, mtr);
	low_match = btr_pcur_get_low_match(pcur);
	rec = btr_pcur_get_rec(pcur);
	page = buf_frame_align(rec);
	/*�ۼ��������Ҳ����ο���¼��Ӧ�ļ�¼*/
	if(rec == page_get_infimum_rec(page))
		return FALSE;
	/*ƥ����в���,Ҳ����˵û����ȫƥ��ļ�¼*/
	if (low_match != dtuple_get_n_fields(ref))
		return FALSE;

	return TRUE;
}

rec_t* row_get_clust_rec(ulint mode, rec_t* rec, dict_index_t* index, dict_index_t** clust_index, mtr_t* mtr)
{
	mem_heap_t*	heap;
	dtuple_t*	ref;
	dict_table_t*	table;
	btr_pcur_t	pcur;
	ibool		found;
	rec_t*		clust_rec;

	ut_ad((index->type & DICT_CLUSTERED) == 0);

	table = index->table;

	heap = mem_heap_create(256);
	/*�Ӹ���������¼rec����һ���ۼ����������õĲο���¼*/
	ref = row_build_row_ref(ROW_COPY_POINTERS, index, rec, heap);
	/*�Ծۼ��������в�ѯ����*/
	found = row_search_on_row_ref(&pcur, mode, table, ref, mtr);
	clust_rec = btr_pcur_get_rec(&pcur);

	mem_heap_free(heap);

	btr_pcur_close(&pcur);

	*clust_index = dict_table_get_first_index(table);
	if(!found)
		return NULL;

	return clust_rec;
}

/*ͨ��index����������entryƥ��ļ�¼������У�����TRUE*/
ibool row_search_index_entry(dict_index_t* index, dtuple_t* entry, ulint mode, btr_pcur_t* pcur, mtr_t* mtr)
{
	ulint	n_fields;
	ulint	low_match;
	page_t*	page;
	rec_t*	rec;

	ut_ad(dtuple_check_typed(entry));

	btr_pcur_open(index, entry, PAGE_CUR_LE, mode, pcur, mtr);
	low_match = btr_pcur_get_low_match(pcur);

	rec = btr_pcur_get_rec(pcur);
	page = buf_frame_align(rec);

	n_fields = dtuple_get_n_fields(entry);
	if(rec == page_get_infimum_rec(page))
		return FALSE;

	if(low_match != n_fields)
		return FALSE;

	return TRUE;
}





