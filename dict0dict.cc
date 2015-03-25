#include "dict0dict.h"
#include "buf0buf.h"
#include "data0type.h"
#include "mach0data.h"
#include "dict0boot.h"
#include "dict0mem.h"
#include "dict0crea.h"
#include "trx0undo.h"
#include "btr0btr.h"
#include "btr0cur.h"
#include "btr0sea.h"
#include "pars0pars.h"
#include "pars0sym.h"
#include "que0que.h"
#include "rem0cmp.h"


dict_sys_t*		dict_sys = NULL; /*innodb�������ֵ�������*/

rw_lock_t		dict_foreign_key_check_lock;

#define DICT_HEAP_SIZE					100			/*memory heap size*/
#define DICT_POOL_PER_PROCEDURE_HASH	512			
#define DICT_POOL_PER_TABLE_HASH		512
#define DICT_POOL_PER_COL_HASH			128
#define DICT_POOL_PER_VARYING			4

/********************************************************************************************/
/*����һ��column���󵽶�Ӧ��table�������ֵ���*/
static void dict_col_add_to_cache(dict_table_t* table, dict_col_t* col);

static void dict_col_reposition_in_cache(dict_table_t* table, dict_col_t* col, char* new_name);

static void dict_col_remove_from_cache(dict_table_t* table, dict_col_t* col);

static void dict_index_remove_from_cache(dict_table_t* table, dict_index_t* index);

UNIV_INLINE void dict_index_add_col(dict_index_t* index, dict_col_t* col);

static void dict_index_copy(dict_index_t* index1, dict_index_t* index2, ulint start, ulint end);

static ibool dict_index_find_cols(dict_table_t* table, dict_index_t* index);

static dict_index_t* dict_index_build_internal_clust(dict_table_t* table, dict_index_t* index);

static dict_index_t* dict_index_build_internal_non_clust(dict_table_t* table, dict_index_t* index);

UNIV_INLINE dict_index_t* dict_tree_find_index_low(dict_tree_t* tree, rec_t* rec);

static void dict_foreign_remove_from_cache(dict_foreign_t* foreign);

static void dict_col_print_low(dict_col_t* col);

static void dict_index_print_low(dict_index_t* index);

static void dict_field_print_low(dict_field_t* field);

static void dict_foreign_free(dict_foreign_t* foreign);

/**********************************************************************************************/
#define LOCK_DICT() mutex_enter(&(dict_sys->mutex))
#define UNLOCK_DICT() mutex_exit(&(dict_sys->mutex));

/*��dict_sys->mutex����*/
void dict_mutex_enter_for_mysql()
{
	LOCK_DICT();
}

/*��dict_sys->mutex�ͷ���*/
void dict_mutex_exit_for_mysql()
{
	UNLOCK_DICT();
}

/*��table->n_mysql_handles_opened������-1*/
void dict_table_decrement_handle_count(dict_table_t* table)
{
	LOCK_DICT();

	ut_a(table->n_mysql_handles_opened > 0);
	table->n_mysql_handles_opened --;

	UNLOCK_DICT();
}

/*��ñ�ĵ�N�е�column����*/
dict_col_t* dict_table_get_nth_col_noninline(dict_table_t* table, ulint pos)
{
	return dict_table_get_nth_col(table, pos);
}

dict_index_t* dict_table_get_first_index_noninline(dict_table_t* table)
{
	return(dict_table_get_first_index(table));
}

dict_index_t* dict_table_get_next_index_noninline(dict_index_t* index)
{
	return dict_table_get_next_index(index);
}

dict_index_t* dict_table_get_index_noninline(dict_table_t* table, char* name)
{
	return dict_table_get_index(table, name);
}

/*��ʼ��autoinc�ļ�����*/
void dict_table_autoinc_initialize(dict_table_t* table, ib_longlong value)
{
	mutex_enter(&(table->autoinc_mutex));

	table->autoinc_inited = TRUE;
	table->autoinc = value;

	mutex_exit(&(table->autoinc_mutex));
}

/*���һ���µ�����IDֵ*/
ib_longlong dict_table_autoinc_get(dict_table_t* table)
{
	ib_longlong value;

	mutex_enter(&(table->autoinc_mutex));
	if(!table->autoinc_inited) /*autoincδ��ʼ��*/
		value = 0;
	else{
		value = table->autoinc;
		table->autoinc = table->autoinc + 1; 
	}
	mutex_exit(&(table->autoinc_mutex));

	return value;
}

/*�Ա��autoinc�Ķ�ȡ��������*/
ib_longlong dict_table_autoinc_read(dict_table_t* table)
{
	ib_longlong value;

	mutex_enter(&(table->autoinc_mutex));
	if(!table->autoinc_inited)
		value = 0;
	else
		value = table->autoinc;
	mutex_exit(&(table->autoinc_mutex));

	return value;
}

/*��table����ID�Ķ�ȡ������latch����*/
ib_longlong dict_table_autoinc_peek(dict_table_t* table)
{
	ib_longlong value;

	if(!table->autoinc_inited)
		value = 0;
	else
		value = table->autoinc;

	return value;
}

/*��table����ID�ĸ���*/
void dict_table_autoinc_update(dict_table_t* table, ib_longlong value)
{
	mutex_enter(&(table->autoinc_mutex));

	if(table->autoinc_inited){
		if(value >= table->autoinc)
			table->autoinc = value + 1;
	}

	mutex_exit(&(table->autoinc_mutex));
}

/*�������������Ӧ�ı��еĵ�N��column��pos*/
ulint dict_index_get_nth_col_pos(dict_index_t* index, ulint n)
{
	dict_field_t*	field;
	dict_col_t*	col;
	ulint		pos;
	ulint		n_fields;

	if(index->type & DICT_CLUSTERED){
		col = dict_table_get_nth_col(index->table, n);
		return col->clust_pos;
	}

	n_fields = dict_index_get_n_fields(index);
	for(pos = 0; pos < n_fields; pos ++){
		field = dict_index_get_nth_field(index, pos);
		col = field->col;
		if(dict_col_get_no(col) == n)
			return pos;
	}

	return ULINT_UNDEFINED;
}

/*ͨ��table id��trx��ñ�������ֵ�*/
dict_table_t* dict_table_get_on_id(dulint table_id, trx_t* trx)
{
	dict_table_t* table;

	if(ut_dulint_cmp(table_id, DICT_FIELDS_ID) <= 0 || trx->dict_operation){ /*DDL�����������ϵͳ���Ѿ�����dict_sys->mutex*/
		ut_ad(mutex_own(&(dict_sys->mutex)));
		return dict_table_get_on_id_low(table_id, trx);
	}

	LOCK_DICT();
	table = dict_table_get_on_id_low(table_id, trx);
	UNLOCK_DICT();

	return table;
}
/*���Ҿۼ������е�n��column��pos*/
ulint dict_table_get_nth_col_pos(dict_table_t* table, ulint n)
{
	/*��һ����������cluster index*/
	return dict_index_get_nth_col_pos(dict_table_get_first_index(table), n);
}

/*����dict_sys�������*/
void dict_init()
{
	dict_sys = mem_alloc(sizeof(dict_sys_t));

	mutex_create(&(dict_sys->mutex));
	mutex_set_level(&(dict_sys->mutex), SYNC_DICT);

	/*����buffer pool�����ֵ�������������ֵ��hash��*/
	dict_sys->table_hash = hash_create(buf_pool_get_max_size() / (DICT_POOL_PER_TABLE_HASH * UNIV_WORD_SIZE));
	dict_sys->table_id_hash = hash_create(buf_pool_get_max_size() / (DICT_POOL_PER_TABLE_HASH *UNIV_WORD_SIZE));
	dict_sys->col_hash = hash_create(buf_pool_get_max_size() / (DICT_POOL_PER_COL_HASH * UNIV_WORD_SIZE));
	dict_sys->procedure_hash = hash_create(buf_pool_get_max_size() / (DICT_POOL_PER_PROCEDURE_HASH * UNIV_WORD_SIZE));

	dict_sys->size = 0;
	UT_LIST_INIT(dict_sys->table_LRU);

	/*�������Լ������latch*/
	rw_lock_create(&dict_foreign_key_check_lock);
	rw_lock_set_level(&dict_foreign_key_check_lock, SYNC_FOREIGN_KEY_CHECK);
}

/*ͨ��������ñ�������ֵ�*/
dict_table_t* dict_table_get(char* table_name, trx_t* trx)
{
	dict_table_t*	table;

	UT_NOT_USED(trx);

	LOCK_DICT();
	table=  dict_table_get_low(table_name);
	UNLOCK_DICT();

	if(table != NULL && !table->stat_initialized){
		dict_update_statistics(table);
	}

	return table;
}

/*��MSYQL open handled��1�������ض�Ӧ�ı������ֵ����*/
dict_table_t* dict_table_get_and_increment_handle_count(char* table_name, trx_t* trx)
{
	dict_table_t* table;

	UT_NOT_USED(trx);
	LOCK_DICT();

	table = dict_table_get_low(table_name);
	if(table != NULL)
		table->n_mysql_handles_opened ++;

	UNLOCK_DICT();

	if(table != NULL && !table->stat_initialized)
		dict_update_statistics(table);

	return table;
}

/*��table�����ֵ���뵽�ֵ�cache����*/
void dict_table_add_to_cache(dict_table_t* table)
{
	ulint	fold;
	ulint	id_fold;
	ulint	i;

	ut_ad(table);
	ut_ad(mutex_own(&(dict_sys->mutex)));
	ut_ad(table->n_def == table->n_cols - DATA_N_SYS_COLS);
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);
	ut_ad(table->cached == FALSE);

	/*�������ֹ�ϣֵ��id��ϣֵ*/
	fold = ut_fold_string(table->name);
	id_fold = ut_fold_dulint(table->id);

	table->cached = TRUE;

	/*����Ĭ�ϵ�ϵͳcolumn(row id, trx id, roll_ptr, mix id)*/
	dict_mem_table_add_col(table, "DB_ROW_ID", DATA_SYS, DATA_ROW_ID, 0, 0);
	dict_mem_table_add_col(table, "DB_TRX_ID", DATA_SYS, DATA_TRX_ID, 0, 0);
	dict_mem_table_add_col(table, "DB_ROLL_PTR", DATA_SYS, DATA_ROLL_PTR, 0, 0);
	dict_mem_table_add_col(table, "DB_MIX_ID", DATA_SYS, DATA_MIX_ID, 0, 0);

	/*����������Ϊ��ϣ��table_hash�в��ң�һ����û�еģ�����У��ᷢ���ڴ�й¶��*/
	{
		dict_table_t*	table2;
		HASH_SEARCH(name_hash, dict_sys->table_hash, fold, table2, (ut_strcmp(table2->name, table->name) == 0));
		ut_ad(table2 == NULL);
	}

	{
		dict_table_t*	table2;
		HASH_SEARCH(id_hash, dict_sys->table_id_hash, id_fold, table2, (ut_dulint_cmp(table2->id, table->id) == 0));
		ut_a(table2 == NULL);
	}

	if(table->type == DICT_TABLE_CLUSTER_MEMBER){
		table->mix_id_len = mach_dulint_get_compressed_size(table->mix_id);
		mach_dulint_write_compressed(table->mix_id_buf, table->mix_id);
	}

	/*��table�����е�column����д�뵽cache��*/
	for(i = 0; i < table->n_cols; i ++)
		dict_col_add_to_cache(table, dict_table_get_nth_col(table, i));

	/*�����ֹ�ϣ�����ϵ����table��cache��*/
	HASH_INSERT(dict_table_t, name_hash, dict_sys->table_hash, fold, table);
	/*��ID��ϣ�����ϵ����table��cache��*/
	HASH_INSERT(dict_table_t, id_hash, dict_sys->table_id_hash, id_fold, table);

	/*��table���뵽dict_sys��LRU��̭�б��У����ʱ��̫��û�����ã�Ӧ�û��cache��ɾ��table*/
	UT_LIST_ADD_FIRST(table_LRU, dict_sys->table_LRU, table);

	dict_sys->size += mem_heap_get_size(table->heap);
}

/*ͨ��index id���ڴ�table lru cache���ҵ���Ӧ����������,����ɨ��table LRU���еı������ֵ�*/
dict_index_t* dict_index_find_on_id_low(dulint id)
{
	dict_table_t*	table;
	dict_index_t*	index;

	table = UT_LIST_GET_FIRST(dict_sys->table_LRU);
	while(table != NULL){
		index = dict_table_get_first_index(table);
		while(index != NULL){
			if(0 == ut_dulint_cmp(id, index->tree->id))
				return index;

			index = dict_table_get_next_index(index);
		}

		table = UT_LIST_GET_NEXT(table_LRU, table);
	}

	return index;
}

/*�Ա���и������������cache�У�cache�еı������ֵ������Ӧ�ĸ���*/
ibool dict_table_rename_in_cache(dict_table_t* table, char* new_name, ibool rename_also_foreigns)
{
	dict_foreign_t*	foreign;
	dict_index_t*	index;
	ulint			fold;
	ulint			old_size;
	char*			name_buf;
	ulint			i;

	ut_ad(table);
	ut_ad(mutex_own(&(dict_sys->mutex)));

	old_size = mem_heap_get_size(table->heap);
	fold = ut_fold_string(new_name);

	{
		dict_table_t*	table2;
		HASH_SEARCH(name_hash, dict_sys->table_hash, fold, table2,
			(ut_strcmp(table2->name, new_name) == 0));
		if (table2)
			return(FALSE);
	}

	/*��column�����������ָ���*/
	for(i = 0; i < table->n_cols; i ++)
		dict_col_reposition_in_cache(table, dict_table_get_nth_col(table, i), new_name);

	/*ɾ��ԭ���Ķ�Ӧ��ϵ*/
	HASH_DELETE(dict_table_t, name_hash, dict_sys->table_hash, ut_fold_string(table->name), table);
	name_buf = mem_heap_alloc(table->heap, ut_strlen(new_name) + 1);
	ut_memcpy(name_buf, new_name, ut_strlen(new_name) + 1);
	table->name = name_buf;
	/*���²��뵽table hash��*/
	HASH_INSERT(dict_table_t, name_hash, dict_sys->table_hash, fold, table);
	/*���������ֵ�ռ�õĿռ�*/
	dict_sys->size += (mem_heap_get_size(table->heap) - old_size);

	/*����������Ӧ�ı���*/
	index = dict_table_get_first_index(table);
	while(index != NULL){
		index->table_name = table->name;
		index = dict_table_get_next_index(index);
	}
	
	/*���������Լ����ϵ����ô��ȡ����Ӧ�����Լ��*/
	if(!rename_also_foreigns){
		foreign = UT_LIST_GET_LAST(table->foreign_list);
		while(foreign != NULL){
			dict_foreign_remove_from_cache(foreign);
			foreign = UT_LIST_GET_LAST(table->foreign_list);
		}

		foreign = UT_LIST_GET_FIRST(table->referenced_list);
		while(foreign != NULL){
			foreign->referenced_table = NULL;
			foreign->referenced_index = NULL;
			foreign = UT_LIST_GET_NEXT(referenced_list, foreign);
		}

		UT_LIST_INIT(table->referenced_list);
		return TRUE;
	}

	/*��Ҫ�����Լ������*/
	foreign = UT_LIST_GET_FIRST(table->foreign_list);

	while (foreign != NULL) {
		if (ut_strlen(foreign->foreign_table_name) < ut_strlen(table->name)) {
			foreign->foreign_table_name = mem_heap_alloc(foreign->heap, ut_strlen(table->name) + 1);
		}

		ut_memcpy(foreign->foreign_table_name, table->name, ut_strlen(table->name) + 1);
		foreign->foreign_table_name[ut_strlen(table->name)] = '\0';

		foreign = UT_LIST_GET_NEXT(foreign_list, foreign);
	}

	foreign = UT_LIST_GET_FIRST(table->referenced_list);

	while (foreign != NULL) {
		if(ut_strlen(foreign->referenced_table_name) < ut_strlen(table->name))
			foreign->referenced_table_name = mem_heap_alloc(foreign->heap,ut_strlen(table->name) + 1);

		ut_memcpy(foreign->referenced_table_name, table->name, ut_strlen(table->name) + 1);
		foreign->referenced_table_name[ut_strlen(table->name)] = '\0';

		foreign = UT_LIST_GET_NEXT(referenced_list, foreign);
	}

	return TRUE;
}

/*��ϵͳ�ֵ�cache��ɾ����ָ����table�����ֵ����*/
void dict_table_remove_from_cache(dict_table_t* table)
{
	dict_foreign_t*	foreign;
	dict_index_t*	index;
	ulint		size;
	ulint		i;

	ut_ad(table);
	ut_ad(mutex_own(&(dict_sys->mutex)));
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	/*��ɾ�����Լ��cache*/
	foreign = UT_LIST_GET_LAST(table->foreign_list);
	while(foreign != NULL){
		dict_foreign_remove_from_cache(foreign);
		foreign = UT_LIST_GET_LAST(table->foreign_list);
	}

	foreign = UT_LIST_GET_FIRST(table->referenced_list);
	while(foreign != NULL){
		foreign->referenced_table = NULL;
		foreign->referenced_index = NULL;
		foreign = UT_LIST_GET_NEXT(referenced_list, foreign);
	}

	/*ɾ�����������cache*/
	index = UT_LIST_GET_LAST(table->indexes);
	while(index != NULL){
		dict_index_remove_from_cache(table, index);
		index = UT_LIST_GET_LAST(table->indexes);
	}

	/*ɾ��column��cache*/
	for(i = 0; i < table->n_cols; i ++)
		dict_col_remove_from_cache(table, dict_table_get_nth_col(table, i));

	/*��hash����ɾ��table�Ķ�Ӧ��ϵ*/
	HASH_DELETE(dict_table_t, name_hash, dict_sys->table_hash, ut_fold_string(table->name), table);
	HASH_DELETE(dict_table_t, id_hash, dict_sys->table_id_hash, ut_fold_dulint(table->id), table);

	/*��table LRU��ɾ��ǰ���ϵ*/
	UT_LIST_REMOVE(table_LRU, dict_sys->table_LRU, table);
	/*free ��ID������������*/
	mutex_free(&(table->autoinc_mutex));

	/*����dict_sys�Ŀռ�ռ��ͳ��*/
	size = mem_heap_get_size(table->heap);
	ut_ad(dict_sys->size >= size);
	dict_sys->size -= size;
	/*�ͷŵ�table�Ķ�*/
	mem_heap_free(table->heap);
}

/*dict_sys cache��̫��ı������ֵ䣬��Ҫ����LRU��̭����table_LRU��ĩβ��ʼ��̭*/
void dict_table_LRU_trim()
{
	dict_table_t*	table;
	dict_table_t*	prev_table;

	ut_a(0);
	ut_ad(mutex_own(&(dict_sys->mutex)));

	table = UT_LIST_GET_LAST(dict_sys->table_LRU);
	while(table != NULL && dict_sys->size > buf_pool_get_max_size() / DICT_POOL_PER_VARYING){ /*ռ����buffer pool��1/4���Ͼͻ���̭*/
		prev_table = UT_LIST_GET_PREV(table_LRU, table);
		if(table->mem_fix == 0) /*ֻ��̭û�б��ⲿfix�ı������ֵ�*/
			dict_table_remove_from_cache(table);
		table = prev_table;
	}
}

/*����һ��column�������ֵ��cache*/
static void dict_col_add_to_cache(dict_table_t* table, dict_col_t* col)
{
	ulint	fold;

	ut_ad(table && col);
	ut_ad(mutex_own(&(dict_sys->mutex)));
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	fold = ut_fold_ulint_pair(ut_fold_string(table->name), ut_fold_string(col->name));
	{ /*column nameӦ�ò�������col hash����*/
		dict_col_t*	col2;
		HASH_SEARCH(hash, dict_sys->col_hash, fold, col2, (ut_strcmp(col->name, col2->name) == 0) && (ut_strcmp((col2->table)->name, table->name) == 0));  
		ut_a(col2 == NULL);
	}

	HASH_INSERT(dict_col_t, hash, dict_sys->col_hash, fold, col);
}

/*�������ֵ�cache��ɾ��һ��column����*/
static void dict_col_remove_from_cache(dict_table_t* table, dict_col_t* col)
{
	ulint		fold;

	ut_ad(table && col);
	ut_ad(mutex_own(&(dict_sys->mutex)));
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	fold = ut_fold_ulint_pair(ut_fold_string(table->name), ut_fold_string(col->name));
	HASH_DELETE(dict_col_t, hash, dict_sys->col_hash, fold, col);
}

/*�滻column��Ӧ�ı�����*/
static void dict_col_reposition_in_cache(dict_table_t* table, dict_col_t* col, char* new_name)
{
	ulint fold;

	ut_ad(table && col);
	ut_ad(mutex_own(&(dict_sys->mutex)));
	ut_ad(table->magic_n == DICT_TABLE_MAGIC_N);

	fold = ut_fold_ulint_pair(ut_fold_string(table->name), ut_fold_string(col->name));
	HASH_DELETE(dict_col_t, hash, dict_sys->col_hash, fold, col);

	/*�滻���µ������������ϵ*/
	fold = ut_fold_ulint_pair(ut_fold_string(new_name), ut_fold_string(col->name));
	HASH_INSERT(dict_col_t, hash, dict_sys->col_hash, fold, col);

}

