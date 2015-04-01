#include "fil0fil.h"

#include "mem0mem.h"
#include "sync0sync.h"
#include "hash0hash.h"
#include "os0file.h"
#include "os0sync.h"
#include "mach0data.h"
#include "ibuf0ibuf.h"
#include "buf0buf.h"
#include "log0log.h"
#include "log0recv.h"
#include "fsp0fsp.h"
#include "srv0srv.h"


ulint fil_n_pending_log_flushes = 0;
ulint fil_n_pending_tablespace_flushes = 0;

fil_addr_t fil_addr_null = {FIL_NULL, 0};

typedef struct fil_node_struct fil_node_t;

#define	FIL_NODE_MAGIC_N	89389
#define	FIL_SPACE_MAGIC_N	89472

#define FIL_SYSTEM_HASH_SIZE 500

struct fil_node_struct
{
	char*		name;				/*�ļ�·����*/
	ibool		open;				/*�ļ��Ƿ񱻴�*/
	os_file_t	handle;				/*�ļ����*/
	ulint		size;				/*�ļ�������ҳ������һ��ҳ��16K*/
	ulint		n_pending;			/*�ȴ���дIO�����ĸ���*/
	ibool		is_modified;		/*�Ƿ�����Ҳ���ڣ�Ҳ�����ڴ�cache��Ӳ�����ݲ�һ��*/
	ulint		magic_n;			/*ħ��У����*/
	UT_LIST_NODE_T(fil_node_t) chain;
	UT_LIST_NODE_T(fil_node_t) LRU;
};

struct fil_space_struct
{
	char*			name;				/*space����*/
	ulint			id;					/*space id*/
	ulint			purpose;			/*space�����ͣ���Ҫ��space table, log file��arch file*/
	ulint			size;				/*space������ҳ����*/
	ulint			n_reserved_extents; /*ռ�õ�ҳ����*/
	hash_node_t		hash;				/*chain node��HASH��*/
	rw_lock_t		latch;				/*space����������*/
	ibuf_data_t*	ibuf_data;			/*space ��Ӧ��insert buffer*/
	ulint			magic_n;			/*ħ��У����*/

	UT_LIST_BASE_NODE_T(fil_node_t) chain;
	UT_LIST_NODE_T(fil_space_t)		space_list;
};

typedef struct fil_system_struct
{
	mutex_t			mutex;				/*file system�ı�����*/
	hash_table_t*	spaces;				/*space�Ĺ�ϣ�����ڿ��ټ���space,һ����ͨ��space id����*/
	ulint			n_open_pending;		/*��ǰ�ж�дIO������fil_node����*/
	ulint			max_n_open;			/*�������򿪵��ļ�����*/
	os_event_t		can_open;			/*���Դ��µ��ļ����ź�*/
	
	UT_LIST_BASE_NODE_T(fil_node_t) LRU;			/*������򿪲��������ļ�,���ڿ��ٶ�λ�رյ�fil_node*/
	UT_LIST_BASE_NODE_T(fil_node_t) space_list;		/*file space�Ķ����б�*/
}fil_system_t;


fil_system_t* fil_system = NULL;


void fil_reserve_right_to_open()
{
loop:
	mutex_enter(&(fil_system->mutex));

	/*�ļ��������Ѿ������������*/
	if(fil_system->n_open_pending == fil_system->max_n_open){ /*�ȴ����ļ��ر�*/
		os_event_reset(fil_system->can_open);
		mutex_exit(&(fil_system->mutex));
		os_event_wait(fil_system->can_open);

		goto loop;
	}

	fil_system->max_n_open --;
	mutex_exit(&(fil_system->mutex));
}

void fil_release_right_to_open()
{
	mutex_enter(&(fil_system->mutex));
	/*�п��Դ򿪵��ļ�����*/
	if(fil_system->n_open_pending == fil_system->max_n_open)
		os_event_set(fil_system->can_open); /*���Ϳ��Դ��ļ����ź�*/

	fil_system->max_n_open ++;
	mutex_exit(&(fil_system->mutex));
}

rw_lock_t* fil_space_get_latch(ulint id)
{
	fil_space_t* space;
	fil_system_t* sys = fil_system;
	ut_ad(system);

	mutex_enter(&(sys->mutex));
	/*�ҵ���Ӧ��sapce*/
	HASH_SEARCH(hash, sys->spaces, id, space, space->id == id);
	mutex_exit(&(system->mutex));
	
	return &(space->latch);
}

ulint fil_space_get_type(ulint id)
{
	fil_space_t*	space;
	fil_system_t*	system	= fil_system;
	ut_ad(system);

	mutex_enter(&(system->mutex));
	HASH_SEARCH(hash, system->spaces, id, space, space->id == id);
	mutex_exit(&(system->mutex));

	return (space->purpose);
}

ibuf_data_t* fil_space_get_ibuf_data(ulint id)
{
	fil_space_t* space;
	fil_system_t* sys = fil_system;
	ut_ad(system);

	mutex_enter(&(system->mutex));
	HASH_SEARCH(hash, system->spaces, id, space, space->id == id);
	mutex_exit(&(system->mutex));

	return (space->ibuf_data);
}

void fil_node_create(char* name, ulint size, ulint id)
{
	fil_node_t* node;
	fil_space_t* space;
	char* name2;
	fil_system_t* sys = fil_system;

	ut_a(system);
	ut_a(name);
	ut_a(size > 0);

	mutex_enter(&(sys->mutex));
	node = mem_alloc(sizeof(fil_node_t));
	name2 = mem_alloc(ut_strlen(name) + 1);
	ut_strcpy(name2, name);

	node->name = name2;
	node->open = FALSE;
	node->size = size;
	node->magic_n = FIL_NODE_MAGIC_N;
	node->n_pending = 0;

	node->is_modified = FALSE;

	/*�ҵ���Ӧ��space*/
	HASH_SEARCH(hash, sys->spaces, id, space, space->id == id);
	space->size += size;

	UT_LIST_ADD_LAST(chain, space->chain, node);
	mutex_exit(&(sys->mutex));
}

/*�ر�һ���ļ�*/
static void fil_node_close(fil_node_t* node, fil_system_t* system)
{
	ibool ret;

	ut_ad(node && system);
	ut_ad(mutex_own(&(system->mutex)));
	ut_a(node->open);
	ut_a(node->n_pending == 0);

	ret = os_file_close(node->handle);
	ut_a(ret);
	node->open = FALSE;

	/*��ϵͳ��LRU�б��б���ɾ��*/
	UT_LIST_REMOVE(LRU, system->LRU, node);
}

static void fil_node_free(fil_node_t* node, fil_system_t* system, fil_space_t* space)
{
	ut_ad(node && system && space);
	ut_ad(mutex_own(&(system->mutex)));
	ut_a(node->magic_n == FIL_NODE_MAGIC_N);

	if(node->open)
		fil_node_close(node, system);

	space->size -= node->size;
	UT_LIST_REMOVE(chain, space->chain, node);
	
	mem_free(node->name);
	mem_free(node);
}

/*��space��ɾ��fil_node��ɾ���������ݳ���Ϊtrunc_len*/
void fil_space_truncate_start(ulint id, ulint trunc_len)
{
	fil_node_t*	node;
	fil_space_t*	space;
	fil_system_t*	system	= fil_system;

	mutex_enter(&(system->mutex));

	HASH_SEARCH(hash, system->spaces, id, space, space->id == id);
	ut_a(space);

	/*��ͷ��ʼɾ����֪��ɾ���ĳ��ȵ�trunc_len*/
	while(trunc_len > 0){
		node =  UT_LIST_GET_FIRST(space->chain);
		ut_a(node->size * UNIV_PAGE_SIZE >= trunc_len);
		trunc_len -= node->size * UNIV_PAGE_SIZE;

		fil_node_free(node, system, space);
	}

	mutex_exit(&(system->mutex));
}

/*����һ��fil_system*/
static fil_system_t* fil_system_create(ulint hash_size, ulint max_n_open)
{
	fil_system_t*	system;
	ut_a(hash_size > 0);
	ut_a(max_n_open > 0);

	system = mem_alloc(sizeof(fil_system_t));

	mutex_create(&(system->mutex));
	mutex_set_level(&(system->mutex), SYNC_ANY_LATCH);

	/*����space hash table*/
	system->spaces = hash_create(hash_size);
	UT_LIST_INIT(system->LRU);

	system->n_open_pending = 0;
	system->max_n_open = max_n_open;
	system->can_open = os_event_create(NULL);

	UT_LIST_INIT(system->spaces);

	return system;
}

/*��ʼ��filģ�飬����һ��ȫ�ֵ�fil_system*/
void fil_init(ulint max_n_open)
{
	ut_ad(fil_system);
	fil_system = fil_system_create(FIL_SYSTEM_HASH_SIZE, max_n_open);
}

void fil_ibuf_init_at_db_start()
{
	fil_space_t* space = UT_LIST_GET_FIRST(fil_system->space_list);
	while(space){
		if(space->purpose == FIL_TABLESPACE) /*�Ǳ��ļ�,����ibuf�ĳ�ʼ��*/
			space->ibuf_data = ibuf_data_init_for_space(space->id);

		space = UT_LIST_GET_NEXT(space_list, space);
	}
}

static ulint fil_write_lsn_and_arch_no_to_file(ulint space_id, ulint sum_of_sizes, dulint lsn, ulint arch_log_no)
{
	byte*	buf1;
	byte*	buf;

	buf1 = mem_alloc(2 * UNIV_PAGE_SIZE);
	buf = ut_align(buf1, UNIV_PAGE_SIZE);
	/*��sum_of_size��λ�ö�ȡһ��16k������ҳ*/
	fil_read(TRUE, space_id, sum_of_sizes, 0, UNIV_PAGE_SIZE, buf, NULL);

	/*��lsn��arch_log_noд�뵽buf���ļ�*/
	mach_write_to_8(buf + FIL_PAGE_FILE_FLUSH_LSN, lsn);
	mach_write_to_4(buf + FIL_PAGE_ARCH_LOG_NO, arch_log_no);
	fil_write(TRUE, space_id, sum_of_sizes, 0, UNIV_PAGE_SIZE, buf, NULL);

	return DB_SUCCESS;
}

ulint fil_write_flushed_lsn_to_data_files(dulint lsn, ulint arch_log_no)
{
	fil_space_t*	space;
	fil_node_t*	node;
	ulint		sum_of_sizes;
	ulint		err;

	mutex_enter(&(fil_system->mutex));

	space = UT_LIST_GET_FIRST(fil_system->space_list);
	while(space != NULL){
		if(space->purpose == FIL_TABLESPACE){ /*��ռ��ļ�*/
			node = UT_LIST_GET_FIRST(space->chain);

			while(node != NULL){
				mutex_exit(&(fil_system->mutex));

				/*д��lsn��arch_log_no��page��*/
				err = fil_write_lsn_and_arch_no_to_file(space->id, sum_of_sizes, lsn, arch_log_no);
				if(err != DB_SUCCESS)
					return err;

				mutex_enter(&(fil_system->mutex));

				sum_of_sizes += node->size;
				node = UT_LIST_GET_NEXT(chain, node);
			}
		}

		space = UT_LIST_GET_NEXT(space_list, space);
	}
}

void fil_read_flushed_lsn_and_arch_log_no(os_file_t data_file, ibool one_read_already, 
	dulint* min_flushed_lsn, ulint* min_arch_log_no, dulint* max_flushed_lsn, ulint* max_arch_log_no)
{
	byte*	buf;
	byte*	buf2;
	dulint	flushed_lsn;
	ulint	arch_log_no;

	buf2 = ut_malloc(2 * UNIV_PAGE_SIZE);
	buf = ut_align(buf2, UNIV_PAGE_SIZE);

	/*���ļ��ж�ȡһ��page������*/
	os_file_read(data_file, buf, 0, 0, UNIV_PAGE_SIZE);
	/*��page��Ϣ�л��flush lsn ��arch log no*/
	flushed_lsn = mach_read_from_8(buf + FIL_PAGE_FILE_FLUSH_LSN);
	arch_log_no = mach_read_from_4(buf + FIL_PAGE_ARCH_LOG_NO);

	ut_free(buf2);

	if (!one_read_already){
		*min_flushed_lsn = flushed_lsn;
		*max_flushed_lsn = flushed_lsn;
		*min_arch_log_no = arch_log_no;
		*max_arch_log_no = arch_log_no;

		return;
	}

	/*��ֵ���룬��Ҫ���жԱ�*/
	if (ut_dulint_cmp(*min_flushed_lsn, flushed_lsn) > 0)
		*min_flushed_lsn = flushed_lsn;

	if (ut_dulint_cmp(*max_flushed_lsn, flushed_lsn) < 0)
		*max_flushed_lsn = flushed_lsn;

	if (*min_arch_log_no > arch_log_no)
		*min_arch_log_no = arch_log_no;

	if (*max_arch_log_no < arch_log_no)
		*max_arch_log_no = arch_log_no;
}

void fil_space_create(char* name, ulint id, ulint purpose)
{
	fil_space_t* space;
	char* name2;
	fil_system_t* system = fil_system;

	ut_a(system);
	ut_a(name);

#ifndef UNIV_BASIC_LOG_DEBUG
	ut_a((purpose == FIL_LOG) || (id % 2 == 0));
#endif

	mutex_enter(&(system->mutex));
	space = mem_alloc(sizeof(fil_space_t));
	name2 = mem_alloc(ut_strlen(name) + 1);

	ut_strcpy(name2, name);

	space->name = name2;
	space->id = id;
	space->purpose = purpose;
	space->size = 0;
	space->n_reserved_extents = 0;

	UT_LIST_INIT(space->chain);
	space->magic_n = FIL_SPACE_MAGIC_N;
	space->ibuf_data = NULL;
	/*����latch*/
	rw_lock_create(&(space->latch));
	rw_lock_set_level(&(space->latch), SYNC_FSP);
	/*����fil system����*/
	HASH_INSERT(fil_space_t, hash, system->spaces, id, space);
	UT_LIST_ADD_LAST(space_list, system->space_list, space);

	mutex_exit(&(system->mutex));
}

void fil_space_free(ulint id)
{
	fil_space_t*	space;
	fil_node_t*	fil_node;
	fil_system_t*	system 	= fil_system;

	/*��fil_system��hash table���ҵ���Ӧ��space����fil_system����ɾ��*/
	HASH_SEARCH(hash, system->spaces, id, space, space->id == id);
	HASH_DELETE(fil_space_t, hash, system->spaces, id, space);
	UT_LIST_REMOVE(space_list, system->space_list, space);

	/*ħ����У��*/
	ut_ad(space->magic_n == FIL_SPACE_MAGIC_N);

	/*�ͷ�space�е�fil_node*/
	fil_node = UT_LIST_GET_FIRST(space->chain);
	ut_d(UT_LIST_VALIDATE(chain, fil_node_t, space->chain));
	while(fil_node != NULL){
		/*����ͷ�fil_node*/
		fil_node_free(fil_node, system, space);
		fil_node = UT_LIST_GET_FIRST(space->chain);
	}

	ut_d(UT_LIST_VALIDATE(chain, fil_node_t, space->chain));
	ut_ad(0 == UT_LIST_GET_LEN(space->chain));

	mutex_exit(&(system->mutex));
	/*�ͷ�space���ڴ�ռ�*/
	mem_free(space->name);
	mem_free(space);
}

ulint fil_space_get_size(ulint id)
{
	fil_space_t*	space;
	fil_system_t*	system = fil_system;
	ulint		    size = 0;
	
	ut_ad(system);
	mutex_enter(&(system->mutex));
	HASH_SEARCH(hash, system->spaces, id, space, space->id == id);
	size = space->size;
	mutex_exit(&(system->mutex));

	return size;
}

ibool fil_check_adress_in_tablespace(ulint id, ulint page_no)
{
	fil_space_t*	space;
	fil_system_t*	system = fil_system;
	ulint		    size = 0;
	ibool			ret;

	ut_ad(system);

	mutex_enter(&(system->mutex));
	HASH_SEARCH(hash, system->spaces, id, space, space->id == id);
	if(sapce == NULL)
		ret = FALSE;
	else{
		size = space->size;
		if(page_no > size) /*page no���������space*/
			return FALSE;
		else if(space->purpose != FIL_TABLESPACE) /*���space���Ǳ�ռ�����*/
			return FALSE;
		else
			ret = TRUE;
	}

	mutex_exit(&(system->mutex));

	return ret;
}

/*Ԥ��ָ���Ŀ��пռ�*/
ibool fil_space_reserve_free_extents(ulint id, ulint n_free_now, ulint n_to_reserve)
{
	fil_space_t* space;
	fil_system_t* system = fil_system;
	ibool success;

	ut_ad(system);
	/*���Ҷ�Ӧ��space*/
	mutex_enter(&(system->mutex));
	HASH_SEARCH(hash, system->spaces, id, space, space->id == id);

	/*��Ԥ���ĺϷ����жϣ����space���Ѿ����е����� + ָ����ҪԤ����n_to_reserve֮�� ��n_free_now��ƥ��,��ʾԤ��ʧ��*/
	if(space->n_reserved_extents + n_to_reserve > n_free_now)
		success = FALSE;
	else{
		space->n_reserved_extents += n_to_reserve;
		success = TRUE;
	}

	mutex_exit(&(system->mutex));

	return success;
}

/*��Сռ�÷�Χ*/
void fil_space_release_free_extents(ulint id, ulint n_reserved)
{
	fil_space_t* space;
	fil_system_t* system = fil_system;

	mutex_enter(&(system->mutex));

	HASH_SEARCH(hash, system->spaces, id, space, space->id == id);
	ut_a(space->n_reserved_extents >= n_reserved);
	space->n_reserved_extents -= n_reserved;

	mutex_exit(&(system->mutex));
}

/*Ϊfil_node��IO����׼����У�飬ͬʱ��fil_node�е��ļ�*/
static void fil_node_prepare_for_io(fil_node_t* node, fil_system_t* system, fil_space_t* space)
{
	ibool ret; 
	fil_node_t* last_node;
	
	/*fil_node��Ӧ���ļ��ǹرյ�*/
	if(!node->open){
		ut_a(node->n_pending == 0);

		/*�ж��Ƿ���Դ��µ��ļ�,����򿪵��ļ����Ѿ��ﵽϵͳ�����ޣ��ر�����һ��*/
		if(system->n_open_pending + UT_LIST_GET_LEN(system->LRU) == system->max_n_open){
			ut_a(UT_LIST_GET_LEN(system->LRU) > 0);
			/*���û�������ȡ�����һ��fil_node*/
			last_node = UT_LIST_GET_LAST(system->LRU);
			if (last_node == NULL) {
				fprintf(stderr, "InnoDB: Error: cannot close any file to open another for i/o\n"
					"InnoDB: Pending i/o's on %lu files exist\n", system->n_open_pending);

				ut_a(0);
			}
			/*n_open_pending��״̬�����������������ͳһ����*/
			fil_node_close(last_node, system);
		}

		if(space->purpose == FIL_LOG)
			node->handle = os_file_create(node->name, OS_FILE_OPEN, OS_FILE_AIO, OS_LOG_FILE, &ret);
		else
			node->handle = os_file_create(node->name, OS_FILE_OPEN, OS_DATA_FILE, OS_LOG_FILE, &ret);

		ut_a(ret);
		/*����״̬*/
		node->open = TRUE;
		system->n_open_pending ++;
		node->n_pending = 1;

		return;
	}

	/*�����ļ��Ǵ򿪵�,������LRU����*/
	if(node->n_pending == 0){
		/*��LRU������ɾ��*/
		UT_LIST_REMOVE(LRU, system->LRU, node);
		system->n_open_pending ++;
		node->n_pending = 1;
	}
	else /*ֻ����io�����ļ�����*/
		node->n_pending ++;
}

/*io������ɺ󣬸��¶�Ӧ��״̬,����node�ŵ�LRU���е���*/
static void fil_node_complete_io(fil_node_t* node, fil_system_t* system, ulint type)
{
	ut_ad(node);
	ut_ad(system);
	ut_ad(mutex_own(&(system->mutex)));
	ut_a(node->n_pending > 0);

	node->n_pending --;
	/*���Ƕ���������˵��cache���Ķ���,���̺�cache��һ��*/
	if(type != OS_FILE_READ)
		node->is_modified = TRUE;

	/*û��������io�������ڽ���*/
	if(node->n_pending == 0){
		UT_LIST_ADD_FIRST(LRU, system->LRU, node);

		ut_a(system->n_open_pending > 0);
		system->n_open_pending --;
		/*���Դ򿪸�����ļ������Ͷ�Ӧ�ź�*/
		if(system->n_open_pending == system->max_n_open - 1)
			os_event_set(system->can_open);
	}
}

ibool fil_extend_last_data_file(ulint* actual_increase, ulint size_increase)
{
	fil_node_t*		node;
	fil_space_t*	space;
	fil_system_t*	system	= fil_system;

	byte*		buf;
	ibool		success;
	ulint		i;

	mutex_enter(&(system->mutex));
	HASH_SEARCH(hash, system->spaces, 0, space, space->id == 0);
	node = UT_LIST_GET_LAST(space->chain);

	/*����io�����жϲ����ļ�*/
	fil_node_prepare_for_io(node, system, space);
	buf = mem_alloc(1024 * 1024);
	memset(buf, 0, 1024 * 1024);
	
	/*��1MΪ��λд����*/
	for(i = 0; i < size_increase / ((1024 * 1024) / UNIV_PAGE_SIZE); i ++){
		success = os_file_write(node->name, node->handle, buf, (node->size << UNIV_PAGE_SIZE_SHIFT) & 0xFFFFFFFF,
			node->size >> (32 - UNIV_PAGE_SIZE_SHIFT),1024 * 1024);
		if(!success)
			break;

		node->size += (1024 * 1024) / UNIV_PAGE_SIZE;
		space->size += (1024 * 1024) / UNIV_PAGE_SIZE;
		/*�����п��пռ�*/
		os_has_said_disk_full = FALSE;
	}

	mem_free(buf);
	/*��IO��ɣ����Ķ�Ӧnode��״̬��Ϣ*/
	fil_node_complete_io(node, system, OS_FILE_WRITE);
	mutex_exit(&(system->mutex));

	/*ʵ��д���page��*/
	*actual_increase = i * ((1024 * 1024) / UNIV_PAGE_SIZE);
	/*node����ˢ��*/
	fil_flush(0);

	srv_data_file_sizes[srv_n_data_files - 1] += *actual_increase;

	return TRUE;
}

void fil_io(ulint type, ibool sync, ulint space_id, ulint block_offset, ulint byte_offset, ulint len, void* buf, void* message)
{
	ulint			mode;
	fil_space_t*	space;
	fil_node_t*		node;
	ulint			offset_high;
	ulint			offset_low;
	fil_system_t*	system;
	os_event_t		event;
	ibool			ret;
	ulint			is_log;
	ulint			wake_later;
	ulint			count;

	is_log = type & OS_FILE_LOG;
	type = type & ~OS_FILE_LOG;

	wake_later = type & OS_AIO_SIMULATED_WAKE_LATER;
	type = type & ~OS_AIO_SIMULATED_WAKE_LATER;

	ut_ad(byte_offset < UNIV_PAGE_SIZE);
	ut_ad(buf);
	ut_ad(len > 0);
	ut_ad((1 << UNIV_PAGE_SIZE_SHIFT) == UNIV_PAGE_SIZE);
	ut_ad(fil_validate());

#ifndef UNIV_LOG_DEBUG
	/* ibuf bitmap pages must be read in the sync aio mode: ibuf bitmap��page������ͬ����ȡ��*/
	ut_ad(recv_no_ibuf_operations || (type == OS_FILE_WRITE) || !ibuf_bitmap_page(block_offset) || sync || is_log);
#ifdef UNIV_SYNC_DEBUG
	ut_ad(!ibuf_inside() || is_log || (type == OS_FILE_WRITE)
		|| ibuf_page(space_id, block_offset));
#endif
#endif

	/*�ļ���ģʽ���ж�*/
	if(sync)
		mode = OS_AIO_SYNC;
	else if(type == OS_FILE_READ && !is_log && ibuf_page(space_id, block_offset))
		mode = OS_AIO_IBUF;
	else if(is_log)
		mode = OS_AIO_LOG;
	else 
		mode = OS_AIO_NORMAL;

	system = fil_system;
	count = 0;

loop:
	count ++;
	mutex_enter(&(system->mutex));
	/*��ǰ�Ķ�дio��������������޶ȵ�3/4,��ֹ����*/
	if(count < 500 && !is_log && ibuf_inside() && system->n_open_pending >= (3 * system->max_n_open) / 4){
		mutex_exit(&(system->mutex));
		/*����aio�Ĳ����߳̽��в���*/
		os_aio_simulated_wake_handler_threads();
		os_thread_sleep(100000);

		if(count > 50)
			fprintf(stderr, "InnoDB: Warning: waiting for file closes to proceed\n" 
				"InnoDB: round %lu\n", count);
		/*�����ж�*/
		goto loop;
	}

	/*��ǰ�Ķ�дIO�������Ѿ���������*/
	if(system->n_open_pending == system->max_n_open){
		event = system->can_open;
		os_event_reset(event);
		mutex_exit(&(system->mutex));

		/*����aio�Ĳ����߳̽��в���*/
		os_aio_simulated_wake_handler_threads();
		/*�ȴ�������ļ����ź�*/
		os_event_wait(event);

		goto loop;
	}
	/*������Ҫio������space*/
	HASH_SEARCH(hash, system->spaces, space_id, space, space->id == space_id);
	ut_a(space);
	ut_ad((mode != OS_AIO_IBUF) || (space->purpose == FIL_TABLESPACE));

	node = UT_LIST_GET_FIRST(space->chain);
	for(;;){
		if (node == NULL) {
			fprintf(stderr,
				"InnoDB: Error: trying to access page number %lu in space %lu\n"
				"InnoDB: which is outside the tablespace bounds.\n"
				"InnoDB: Byte offset %lu, len %lu, i/o type %lu\n", block_offset, space_id, byte_offset, len, type);
			ut_a(0);
		}
		/*��λ����Ҫ������nodeλ��,����page��Ӧ��ϵ�ҵ���Ӧ��λ��*/
		if(node->size > block_offset)
			break;
		else{
			block_offset -= node->size;
			node = UT_LIST_GET_NEXT(chain, node);
		}
	}
	/*����io�����жϲ����ļ�*/
	fil_node_prepare_for_io(node, system, space);
	mutex_enter(&(system->mutex));

	/*�����λƫ�ƺ͵�λƫ��*/
	offset_high = (block_offset >> (32 - UNIV_PAGE_SIZE_SHIFT));
	offset_low  = ((block_offset << UNIV_PAGE_SIZE_SHIFT) & 0xFFFFFFFF) + byte_offset;

	ut_a(node->size - block_offset >= (byte_offset + len + (UNIV_PAGE_SIZE - 1)) / UNIV_PAGE_SIZE);
	ut_a(byte_offset % OS_FILE_LOG_BLOCK_SIZE == 0);
	ut_a((len % OS_FILE_LOG_BLOCK_SIZE) == 0);
	/*����aio����*/
	ret = os_aio(type, mode | wake_later, node->name, node->handle, buf,
		offset_low, offset_high, len, node, message);
	ut_a(ret);

	if(mode == OS_AIO_SYNC){ /*ͬ�����ã�����os_aio��ˢ��*/
		mutex_enter(&(system->mutex));
		/*io��ɣ����¶�Ӧ��node״̬*/
		fil_node_complete_io(node, system, type);
		mutex_exit(&(system->mutex));

		ut_ad(fil_validate());
	}
}

void fil_read(ibool sync, ulint space_id, ulint block_offset, ulint byte_offset, ulint len, void* buf, void* message)
{
	fil_io(OS_FILE_READ, sync, space_id, block_offset, byte_offset, len, buf, message);
}

void fil_write(ibool sync, ulint space_id, ulint block_offset, ulint byte_offset, ulint len, void* buf, void* message)
{
	fil_io(OS_FILE_WRITE, sync, space_id, block_offset, byte_offset, len, buf, message);
}

void fil_aio_wait(ulint segment)
{		
	fil_node_t*		fil_node;
	fil_system_t*	system	= fil_system;
	void*			message;
	ulint			type;
	ibool			ret;

	ut_ad(fil_validate());

	if(os_aio_use_native_aio){ /*��ϵͳ��aio*/
		srv_io_thread_op_info[segment] = "native aio handle";
#ifdef POSIX_ASYNC_IO
		ret = os_aio_posix_handle(segment, &fil_node, &message);
#else
		ret = 0; /* Eliminate compiler warning */
		ut_a(0);
#endif
	}
	else{ /*ģ���aio*/
		srv_io_thread_op_info[segment] = "simulated aio handle";
		ret = os_aio_simulated_handle(segment, (void**)&fil_node, &message, &type);
	}

	ut_a(ret);
	srv_io_thread_op_info[segment] = "complete io for fil node";
	
	mutex_enter(&(system->mutex));
	/*�첽�����IO,���Ķ�Ӧ��fil_node״̬*/
	fil_node_complete_io(fil_node, fil_system, type);
	mutex_exit(&(system->mutex));

	if(buf_pool_is_block(message)){ /*pageˢ�����*/
		srv_io_thread_op_info[segment] = "complete io for buf page";
		buf_page_io_complete(message);
	}
	else{ /*��־ˢ�����*/
		srv_io_thread_op_info[segment] = "complete io for log";
		log_io_complete(message);
	}
}

/*��spaceˢ��*/
void fil_flush(ulint space_id)
{
	fil_system_t*	system	= fil_system;
	fil_space_t*	space;
	fil_node_t*		node;
	os_file_t		file;

	mutex_enter(&(system->mutex));
	HASH_SEARCH(hash, system->spaces, space_id, space, space->id == space_id);
	ut_a(space);

	node = UT_LIST_GET_FIRST(space->chain);
	while(node){
		if(node->open && node->is_modified){ /*������ҳ����Ҫ����flush*/
			node->is_modified = FALSE;
			file = node->handle;
			
			if(space->purpose == FIL_TABLESPACE)
				fil_n_pending_tablespace_flushes ++;
			else
				fil_n_pending_log_flushes ++;

			mutex_exit(&(system->mutex));

			/*����flush*/
			os_file_flush(file);

			mutex_enter(&(system->mutex));
			if(space->purpose == FIL_TABLESPACE)
				fil_n_pending_tablespace_flushes --;
			else
				fil_n_pending_log_flushes --;
		}

		node = UT_LIST_GET_NEXT(chain, node);
	}

	mutex_exit(&(system->mutex));
}

/*��purpose��space����ˢ��*/
void fil_flush_file_spaces(ulint purpose)
{
	fil_system_t*	system	= fil_system;
	fil_space_t*	space;

	mutex_enter(&(system->mutex));

	space = UT_LIST_GET_FIRST(system->space_list);
	while(space){
		if (space->purpose == purpose) {
			mutex_exit(&(system->mutex));
			fil_flush(space->id);

			mutex_enter(&(system->mutex));
		}

		space = UT_LIST_GET_NEXT(space_list, space);
	}
	mutex_exit(&(system->mutex));
}

ibool fil_validate(void)

{	
	fil_space_t*	space;
	fil_node_t*	fil_node;
	ulint		pending_count	= 0;
	fil_system_t*	system;
	ulint		i;

	system = fil_system;

	mutex_enter(&(system->mutex));

	for (i = 0; i < hash_get_n_cells(system->spaces); i++) {
		space = HASH_GET_FIRST(system->spaces, i);
		while (space != NULL) {
			UT_LIST_VALIDATE(chain, fil_node_t, space->chain); 
			
			fil_node = UT_LIST_GET_FIRST(space->chain);
			while (fil_node != NULL) {
				if (fil_node->n_pending > 0) {
					pending_count++;
					ut_a(fil_node->open);
				}

				fil_node = UT_LIST_GET_NEXT(chain, fil_node);
			}

			space = HASH_GET_NEXT(hash, space);
		}
	}

	ut_a(pending_count == system->n_open_pending);
	UT_LIST_VALIDATE(LRU, fil_node_t, system->LRU);
	fil_node = UT_LIST_GET_FIRST(system->LRU);

	while (fil_node != NULL) {
		ut_a(fil_node->n_pending == 0);
		ut_a(fil_node->open);

		fil_node = UT_LIST_GET_NEXT(LRU, fil_node);
	}
	mutex_exit(&(system->mutex));

	return(TRUE);
}

ibool fil_addr_is_null(fil_addr_t addr)
{
	if(addr.page == FIL_NULL)
		return TRUE;
	else
		return FALSE;
}

ulint fil_page_get_prev(byte* page)
{
	return(mach_read_from_4(page + FIL_PAGE_PREV));
}

ulint fil_page_get_next(byte* page)
{
	return(mach_read_from_4(page + FIL_PAGE_NEXT));
}

void fil_page_set_type(byte* page, ulint type)
{
	ut_ad(page);
	ut_ad((type == FIL_PAGE_INDEX) || (type == FIL_PAGE_UNDO_LOG));

	mach_write_to_2(page + FIL_PAGE_TYPE, type);
}

ulint fil_page_get_type(byte* page)
{
	ut_ad(page);
	return mach_read_from_2(page + FIL_PAGE_TYPE);
}

