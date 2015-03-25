#ifndef dict0mem_h_
#define dict0mem_h_

#include "univ.h"
#include "dict0types.h"
#include "data0type.h"
#include "data0data.h"
#include "mem0mem.h"
#include "rem0types.h"
#include "btr0types.h"
#include "ut0mem.h"
#include "ut0lst.h"
#include "ut0rnd.h"
#include "ut0byte.h"
#include "sync0rw.h"
#include "lock0types.h"
#include "hash0hash.h"
#include "que0types.h"

/*��������*/

#define DICT_CLUSTERED			1			/*�ۼ�����*/
#define DICT_UNIQUE				2			/*Ψһ����*/
#define DICT_UNIVERSAL			4			/*��ͨ����*/
#define DICT_IBUF				8			/*IBUF B tree*/

#define DICT_DESCEND			1

/* Types for a table object */
#define DICT_TABLE_ORDINARY		1
#define	DICT_TABLE_CLUSTER_MEMBER	2
#define	DICT_TABLE_CLUSTER		3

#define	DICT_TREE_MAGIC_N	7545676

struct dict_col_struct
{
	hash_node_t					hash;				/*��ϣ�ڵ�*/
	ulint						ind;				/*�����ڱ��е�ƫ����*/
	ulint						clust_pos;			/*�����ھۼ������е�λ��*/
	ulint						ord_part;			/*������Ϊ��������Ĵ���*/
	char*						name;				/*����*/
	dtype_t						type;				/*�е���������*/
	dict_table_t*				table;				/*���������*/
	ulint						aux;				/*�����ֶ�*/
};

struct dict_field_struct
{
	dict_col_t*					col;				/*�������������У�*/
	char*						name;				/*����*/
	ulint						order;				/*�����ʶ*/
};

/*�����������ݽṹ����*/
struct dict_tree_struct
{
	ulint						type;				/*��������*/
	dulint						id;
	ulint						space;
	ulint						page;
	byte						pad[64];
	rw_lock_t					lock;
	ulint						mem_fix;
	UT_LIST_BASE_NODE_T(dict_index_t)	tree_indexs;
	ulint						magic_n;
};

/*������������ݽṹ*/
struct dict_index_struct
{
	dulint						id;					/*����ID*/
	mem_heap_t*					heap;				
	ulint						type;				/*��������*/

	char*						name;				/*��������*/
	char*						table_name;			/*���ڱ���*/
	dict_table_t*				table;				/*���ڱ����*/
	ulint						space;				/*��������ŵı�ռ�ID*/
	ulint						page_no;			/*������root�ڵ��Ӧ��page no*/
	ulint						trx_id_offset;		/**/
	ulint						n_user_defined_cols;/*���������������*/
	ulint						n_uniq;				/*�ܹ�ȷ��Ψһ���������������*/
	ulint						n_def;
	ulint						n_fields;			/*���������鳤��*/
	dict_field_t*				fields;				/*����������*/
	UT_LIST_NODE_T(dict_index_t) indexs;			/*���ڱ���������������Ľڵ��б�*/
	UT_LIST_NODE_T(dict_index_t) tree_indexs;		/*������Ӧ�������б���������������Ľڵ��б�*/

	dict_tree_t*				tree;				/*��Ӧ��������*/
	ibool						cached;
	btr_search_t*				search_info;		/*����ӦHASH������Ϣ*/
	ib_longlong*				stat_n_diff_key_vals; /*��������ͬ��ֵ����������ţ�*/
	ulint						stat_index_size;	/*������ռ�õ�page��*/
	ulint						stat_n_leaf_pages;	/*����������������Ҷ�ӽڵ����Ŀ*/
	ulint						magic_n;
};

/*���Լ�������ݽṹ*/
struct dict_foreign_struct
{
	mem_heap_t*					heap;				/*���Լ��heap*/
	char*						id;					/*���Լ����ID�ַ���*/
	char*						type;				/* 0 or DICT_FOREIGN_ON_DELETE_CASCADE or DICT_FOREIGN_ON_DELETE_SET_NULL */
	char*						foreign_table_name;	/*����ӱ���*/
	dict_table_t*				foreign_table;		/*����ӱ����*/
	char**						foreign_col_names;	/*����еĴӱ�����*/
	char*						referenced_table_name;/*���������*/
	dict_table_t*				referenced_table;	/*����������*/
	char**						referenced_col_names;/*����е���������*/
	ulint						n_fields;			/*���ڶ�������Լ�����������и���*/
	dict_index_t*				foreign_index;		/*������еĴӱ������ڱ��ж�Ӧ����������*/
	dict_index_t*				referenced_index;	/*������е����������ڱ��ж�Ӧ����������*/
	UT_LIST_NODE_T(dict_foreign_t) foreign_list;
	UT_LIST_NODE_T(dict_foreign_t) referenced_list;
};

#define DICT_FOREIGN_ON_DELETE_CASCADE	1
#define DICT_FOREIGN_ON_DELETE_SET_NULL	2

#define	DICT_INDEX_MAGIC_N	76789786
/*���ݿ��ṹ����*/
struct dict_table_struct
{
	dulint		id;					/*��ID*/
	ulint		type;				/*������*/
	mem_heap_t*	heap;				/* memory heap */
	char*		name;				/*����*/
	ulint		space;				/* �ۼ������洢�ı�ռ�ID */
	hash_node_t	name_hash;			/* hash chain node */
	hash_node_t	id_hash;			/* hash chain node */
	ulint		n_def;				/* number of columns defined so far */
	ulint		n_cols;				/* number of columns */
	dict_col_t*	cols;				/* array of column descriptions */
	UT_LIST_BASE_NODE_T(dict_index_t) indexes; /*�ñ���������������б�*/
	UT_LIST_BASE_NODE_T(dict_foreign_t) foreign_list; /*�ñ����ڷֱ����Ӹñ�ӱ����������б�*/
	UT_LIST_BASE_NODE_T(dict_foreign_t) referenced_list;/*�ñ����ڷֱ����Ӹñ��������������б� */
	UT_LIST_NODE_T(dict_table_t) table_LRU;		/* node of the LRU list of tables */
	ulint		mem_fix;			/*���ڼ�¼fix�ļ�����*/
	ulint		n_mysql_handles_opened; /*���ڼ����ñ�����MYSQL�����*/
	ulint		n_foreign_key_checks_running; /**/
	ibool		cached;				/*������Ƿ��Ѿ��������ֵ仺����*/
	lock_t*		auto_inc_lock;		/*��������mutex*/
	UT_LIST_BASE_NODE_T(lock_t) locks; /*���ڸñ�ǰ�������б�*/

	dulint		mix_id;					/* */
	ulint		mix_len;				/**/
	ulint		mix_id_len;
	byte		mix_id_buf[12];			/**/
	char*		cluster_name;			/**/

	ibool		does_not_fit_in_memory; /**/

	ib_longlong	stat_n_rows;			/*�ñ�Ĵ������*/
	ulint		stat_clustered_index_size; /*�ñ�ۼ�����ռ�õ�ҳ��*/
	ulint		stat_sum_of_other_index_sizes;	/*�ñ�Ǿۼ�����ռ�õ�ҳ��*/
	ibool           stat_initialized;	/*ͳ���ֶ��Ƿ񱻳�ʼ��*/
	ulint		stat_modified_counter;	/**/

	mutex_t		autoinc_mutex;			/**/
	ibool		autoinc_inited;			/*������ID�Ƿ��ʼ����*/
	ib_longlong	autoinc;				/*������ID*/	
	ulint		magic_n;				/* magic number */
};

#define	DICT_TABLE_MAGIC_N	76333786

/*�洢���̽ṹ����*/
struct dict_proc_struct
{
	mem_heap_t*			heap;
	char*				name;
	char*				sql_string;
	hash_node_t			name_hash;
	UT_LIST_BASE_NODE_T(que_fork_t) graphs;
	ulint				mem_fix;
};

dict_table_t*			dict_mem_table_create(char*	name, ulint space, ulint n_cols);

dict_cluster_t*			dict_mem_cluster_create(char* name, ulint space, ulint n_cols, ulint mix_len);

void					dict_mem_table_make_cluster_member(dict_table_t* table, char* cluster_name);

void					dict_mem_table_add_col(dict_table_t* table, char* name, ulint mtype, ulint prtype, ulint len, ulint prec);

dict_index_t*			dict_mem_index_create(char* table_name, char* index_name, ulint space, ulint type, ulint n_fields);

void					dict_mem_index_add_field(dict_index_t* index, char* name, ulint order);

void					dict_mem_index_free(dict_index_t* index);

dict_foreign_t*			dict_mem_foreign_create();

dict_proc_t*			dict_mem_procedure_create(char* name, char* sql_string, que_fork_t* graph);

#endif




