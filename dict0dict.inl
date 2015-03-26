
#include "dict0load.h"
#include "trx0undo.h"
#include "trx0sys.h"
#include "rem0rec.h"

/*���column������*/
UNIV_INLINE dtype_t* dict_col_get_type(dict_col_t* col)
{
	ut_ad(col);
	return &(col->type);
}

/*���column���ڱ��е�ƫ��*/
UNIV_INLINE ulint dict_col_get_no(dict_col_t* col)
{
	ut_ad(col);
	return col->ind;
}
/*��ø����ھۼ������е�ƫ��*/
UNIV_INLINE ulint dict_col_get_clust_pos(dict_col_t* col)
{
	ut_ad(col);
	return col->clust_pos;
}

/*��ñ��е�һ����������*/
UNIV_INLINE dict_index_t* dict_table_get_first_index(dict_table_t* table)
{
	ut_ad(table);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	return UT_LIST_GET_FIRST(table->indexes);
}

/*���index����һ����������*/
UNIV_INLINE dict_index_t* dict_table_get_next_index(dict_index_t* index)
{
	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	return UT_LIST_GET_NEXT(indexes, index);
}

/*��ñ����û����������*/
UNIV_INLINE ulint dict_table_get_n_user_cols(dict_table_t* table)
{
	ut_ad(table);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(table->cached);

	return table->n_cols - DATA_N_SYS_COLS;
}

/*��ñ��ϵͳ����*/
UNIV_INLINE dict_table_get_n_sys_cols(dict_table_t* table)
{
	ut_ad(table);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(table->cached);

	return DATA_N_SYS_COLS;
}
/*��ñ��������������ϵͳ��*/
UNIV_INLINE ulint dict_table_get_n_cols(dict_table_t* table)
{
	ut_ad(table);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(table->cached);

	return table->n_cols;
}

/*��ñ�ĵ�N�ж���*/
UNIV_INLINE dict_col_t* dict_table_get_nth_col(dict_table_t* table, ulint pos)
{
	ut_ad(table);
	ut_ad(pos < table->n_def);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	return table->cols + pos;
}

/*��ñ��е�Nϵͳ�ж���*/
UNIV_INLINE dict_col_t* dict_table_get_sys_col(dict_table_t* table, ulint sys)
{
	dict_col_t*	col;

	ut_ad(table);
	ut_ad(sys < DATA_N_SYS_COLS);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	col = dict_table_get_nth_col(table, table->n_cols - DATA_N_SYS_COLS + sys);

	ut_ad(col->type.mtype == DATA_SYS);
	ut_ad(col->type.prtype == sys);

	return col;
}
/*���sysϵͳ��λ���������е�����λ��*/
UNIV_INLINE ulint dict_table_get_sys_col_no(dict_table_t* table, ulint sys)
{
	ut_ad(table);
	ut_ad(sys < DATA_N_SYS_COLS);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	return(table->n_cols - DATA_N_SYS_COLS + sys);
}

/*������������Ӧ��������*/
UNIV_INLINE ulint dict_index_get_n_fields(dict_index_t* index)
{
	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);
	ut_ad(index->cached);

	return index->n_fields;
}

/*��������������ȷ��Ψһ�Ե�����*/
UNIV_INLINE ulint dict_index_get_n_unique(dict_index_t* index)
{	
	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);
	ut_ad(index->cached);

	return index->n_uniq;
}

/*ȷ������Ψһ�Ե�����*/
UNIV_INLINE ulint dict_index_get_n_unique_in_tree(dict_index_t* index)
{
	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);
	ut_ad(index->cached);

	if(index->type & DICT_CLUSTERED)
		return dict_index_get_n_unique(index);

	return dict_index_get_n_fields(index);
}

/************************************************************************
Gets the number of user-defined ordering fields in the index. In the internal
representation of clustered indexes we add the row id to the ordering fields
to make a clustered index unique, but this function returns the number of
fields the user defined in the index as ordering fields. */
UNIV_INLINE ulint dict_index_get_n_ordering_defined_by_user(dict_index_t* index)
{
	return index->n_user_defined_cols;
}

/*������������N��λ���ϵ���*/
UNIV_INLINE dict_field_t* dict_index_get_nth_field(dict_index_t* index, ulint pos)
{
	ut_ad(index);
	ut_ad(pos < index->n_def);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	return index->fields + pos;
}

/*����һ��ϵͳ�������������λ��*/
UNIV_INLINE ulint dict_index_get_sys_col_pos(dict_index_t* index, ulint type)
{
	dict_col_t*	col;

	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);
	ut_ad(!(index->type & DICT_UNIVERSAL));

	col = dict_table_get_sys_col(index->table, type);
	if(index->type & DICT_CLUSTERED)
		return col->clust_pos;

	return dict_index_get_nth_col_pos(index, dict_table_get_sys_col_no(index->table, type));
}
/*��ü�¼rec��ϵͳ�е�ֵ��row_id/roll_ptr/trx_id/mix_id*/
UNIV_INLINE dulint dict_index_rec_get_sys_col(dict_index_t* index, ulint type, rec_t* rec)
{
	ulint	pos;
	byte*	field;
	ulint	len;

	ut_ad(index);
	ut_ad(index->type & DICT_CLUSTERED);

	pos = dict_index_get_sys_col_pos(index, type); 	/*ȷ��ϵͳ��λ��*/
	field = rec_get_nth_field(rec, pos, &len);

	if (type == DATA_ROLL_PTR) {
		ut_ad(len == 7);
		return(trx_read_roll_ptr(field));
	} 
	else if ((type == DATA_ROW_ID) || (type == DATA_MIX_ID)){
		return(mach_dulint_read_compressed(field));
	}
	else{
		ut_ad(type == DATA_TRX_ID);
		return(trx_read_trx_id(field));
	}
}

/*������������Ӧ������������*/
UNIV_INLINE dict_tree_t* dict_index_get_tree(dict_index_t* index)
{
	ut_ad(index);
	ut_ad(index->magic_n == DICT_INDEX_MAGIC_N);

	return index->tree;
}
/*����е������ʶ*/
UNIV_INLINE ulint dict_field_get_order(dict_field_t* field)
{
	ut_ad(field);
	return field->order;
}

/*���field��Ӧ��column����*/
UNIV_INLINE dict_col_t* dict_field_get_col(dict_field_t* field)
{
	ut_ad(field);
	return field->col;
}

/*������������е�N���������е���������*/
UNIV_INLINE dtype_t* dict_index_get_nth_type(dict_index_t* index, ulint pos)
{
	return dict_col_get_type(dict_field_get_col(dict_index_get_nth_field(index, pos)));
}

/*���������root page��Ӧ�ı�ռ�ID*/
UNIV_INLINE ulint dict_tree_get_space(dict_tree_t* tree)
{
	ut_ad(tree);
	ut_ad(tree->magic_n == DICT_TREE_MAGIC_N);

	return tree->space;
}
/*���������������root page��Ӧ�ı�ռ�ID*/
UNIV_INLINE void dict_tree_set_space(dict_tree_t* tree, ulint space)
{
	ut_ad(tree);
	ut_ad(tree->magic_n == DICT_TREE_MAGIC_N);

	tree->space = space;
}

/*�����������root page��Ӧ��page no*/
UNIV_INLINE ulint dict_tree_get_page(dict_tree_t* tree)
{
	ut_ad(tree);
	ut_ad(tree->magic_n == DICT_TREE_MAGIC_N);

	return tree->page;
}

/*�����������type*/
UNIV_INLINE ulint dict_tree_get_type(dict_tree_t* tree)
{
	ut_ad(tree);
	ut_ad(tree->magic_n == DICT_TREE_MAGIC_N);

	return tree->type;
}

/*�����������rw_lock*/
UNIV_INLINE rw_lock_t* dict_tree_get_lock(dict_tree_t* tree)
{
	ut_ad(tree);
	ut_ad(tree->magic_n == DICT_TREE_MAGIC_N);

	return &(tree->lock);
}

/*���tree��Ӧ�ı�ռ�Ԥ�������¼�¼�õĿռ��С, 1/16 page size*/
UNIV_INLINE ulint dict_tree_get_space_reserve(dict_tree_t* tree)
{
	ut_ad(tree);

	UT_NOT_USED(tree);

	return (UNIV_PAGE_SIZE / 16);
}

/*�жϱ��Ƿ��������ֵ��cache���У�����ڣ����ض�Ӧ�������ֵ����*/
UNIV_INLINE dict_table_t* dict_table_check_if_in_cache_low(char* name)
{
	dict_table_t*	table;
	ulint			table_fold;

	ut_ad(table_name);
	ut_ad(mutex_own(&(dict_sys->mutex)));

	table_fold = ut_fold_string(name);

	/*��dict_sys->table_hash�в���*/
	HASH_SEARCH(name_hash, dict_sys->table_hash, table_fold, table, ut_strcmp(table->name, table_name) == 0);

	return table;
}

/*ͨ��������ñ�������ֵ����*/
UNIV_INLINE dict_table_t* dict_table_get_low(char* table_name)
{
	dict_table_t* table;

	ut_ad(table_name);
	ut_ad(mutex_own(&(dict_sys->mutex)));

	table = dict_table_check_if_in_cache_low(table_name);
	if(table == NULL)
		table = dict_load_table(table_name); /*�Ӵ����ϵ���table_name�ı������ֵ䵽�ڴ���*/

	return table;
}

UNIV_INLINE dict_proc_t* dict_procedure_get(char* proc_name, trx_t* trx)
{
	dict_proc_t*	proc;
	ulint		name_fold;

	UT_NOT_USED(trx);

	mutex_enter(&(dict_sys->mutex));

	name_fold = ut_fold_string(proc_name);

	HASH_SEARCH(name_hash, dict_sys->procedure_hash, name_fold, proc, ut_strcmp(proc->name, proc_name) == 0);
	if (proc != NULL) 
		proc->mem_fix++;

	mutex_exit(&(dict_sys->mutex));

	return(proc);
}

/*ͨ��table id���Ҷ�Ӧ��������ֵ����*/
UNIV_INLINE dict_table_t* dict_table_get_on_id_low(dulint table_id, trx_t* trx)
{
	dict_table_t* table;
	ulint fold;

	ut_ad(mutex_own(&(dict_sys->mutex)));
	UT_NOT_USED(trx);

	fold = ut_fold_dulint(table_id);
	HASH_SEARCH(id_hash, dict_sys->table_id_hash, fold, table, ut_dulint_cmp(table->id, table_id) == 0);
	if(table == NULL)
		table = dict_load_table_on_id(table_id); /*�Ӵ����ϵ����Ӧ��������ֵ�*/

	if(table != NULL)
		table->mem_fix ++;

	return table;
}

UNIV_INLINE void dict_table_release(dict_table_t* table)
{
	mutex_enter(&(dict_sys->mutex));
	table->mem_fix --;
	mutex_exit(&(dict_sys->mutex));
}

/*ͨ����������ñ��Ӧ����������*/
UNIV_INLINE dict_index_t* dict_table_get_index(dict_table_t* table, char* name)
{
	dict_index_t* index = NULL;
	index = dict_table_get_first_index(table);
	while(index != NULL){
		if(ut_strcmp(name, index->name) == 0)
			break;

		index = dict_table_get_next_index(index);
	}

	return index;
}

/*�����mix_id_buf�Ƿ���rec������*/
UNIV_INLINE ibool dict_is_mixed_table_rec(dict_table_t* table, rec_t* rec)
{
	byte* mix_id_field;
	ulint len;

	/*���rec��mix_len��Ӧ����*/
	mix_id_field = rec_get_nth_field(rec, table->mix_len, &len);
	if(len != table->mix_id_len || (0 != ut_memcmp(table->mix_id_buf, mix_id_field, len)))
		return FALSE;

	return TRUE;
}

