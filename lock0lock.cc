#include "lock0lock.h"
#include "usr0sess.h"
#include "trx0purge.h"

#define LOCK_MAX_N_STEPS_IN_DEADLOCK_CHECK	1000000

#define LOCK_RELEASE_KERNEL_INTERVAL		1000

#define LOCK_PAGE_BITMAP_MARGIN				64


ibool lock_print_waits = FALSE;

/*��������ȫ��HASH��,��������������У�����ֻͨ��dict table������*/
lock_sys_t* lock_sys = NULL;

/*��������������*/
typedef struct lock_table_struct
{
	dict_table_t* table;			/*�ֵ�Ԫ�����еı������*/
	UT_LIST_NODE_T(lock_t) locks;	/*�ڱ��ϵ��������б�*/
}lock_table_t;

typedef struct lock_rec_struct
{
	ulint		space;				/*��¼������space��ID*/
	ulint		page_no;			/*��¼������pageҳ��*/
	ulint		n_bits;				/*������bitmapλ����lock_t�ṹ������һ��BUF������Ϊn_bits / 8*/
}lock_rec_t;

/*������*/
struct lock_struct
{
	trx_t*			trx;			/*ִ������ָ��*/
	ulint			type_mode;		/*�����ͺ�״̬��������LOCK_ERC��LOCK_TABLE,״̬��LOCK_WAIT, LOCK_GAP,ǿ����LOCK_X,LOCK_S��*/
	hash_node_t		hash;			/*hash��Ķ�Ӧ�ڵ㣬table lock����Ч��*/
	dict_index_t*	index;			/*�������м�¼����*/
	UT_LIST_NODE_T(lock_t) trx_locks; /*һ��trx_locks���б�ǰ���ϵ*/
	union{
		lock_table_t	tab_lock;	/*����*/
		lock_rec_t		rec_lock;	/*����*/
	}un_member;
};

/*������ʶ*/
ibool lock_deadlock_found = FALSE;
/*������Ϣ��������5000�ֽ�*/
char* lock_latest_err_buf;

/*������⺯��*/
static ibool		lock_deadlock_occurs(lock_t* lock, trx_t* trx);
static ibool		lock_deadlock_recursive(trx_t* start, trx_t* trx, lock_t* wait_lock, ulint* cost);

/************************************************************************/
/*kernel_mutex����srv0srv.h�����ȫ���ں�mutex latch*/
UNIV_INLINE void lock_mutex_enter_kernel()
{
	mutex_enter(&kernel_mutex);
}

UNIV_INLINE void lock_mutex_exit_kernel()
{
	mutex_exit(&kernel_mutex);
}

/*ͨ���ۺ���������¼�Ƿ���Խ���һ����������*/
ibool lock_clust_rec_cons_read_sees(rec_t* rec, dict_index_t* index, read_view_t* view)
{
	dulint	trx_id;

	ut_ad(index->type & DICT_CLUSTERED);
	ut_ad(page_rec_is_user_rec(rec));

	trx_id = row_get_rec_trx_id(rec, index);
	if(read_view_sees_trx_id(view, trx_id))
		return TRUE;

	return FALSE;
}

/*���Ǿۼ�������¼�Ƿ���Խ���һ���Զ�*/
ulint  lock_sec_rec_cons_read_sees(rec_t* rec, dict_index_t* index, read_view_t* view)
{
	dulint	max_trx_id;

	ut_ad(index->type & DICT_CLUSTERED);
	ut_ad(page_rec_is_user_rec(rec));

	if(recv_recovery_is_on()) /*���redo log�Ƿ��ڽ�����־�ָ�*/
		return FALSE;

	/*��ö�Ӧpage�Ķ�������������������ID*/
	max_trx_id = page_get_max_trx_id(buf_frame_align(rec));
	if(ut_dulint_cmp(max_trx_id, view->up_limit_id) >= 0) /*view�е�����ID����ҳ�е�max trx id,���ܽ���һ����������*/
		return FALSE;

	return TRUE;
}

/*����һ��ϵͳ������ϣ�����*/
void lock_sys_create(ulint n_cells)
{
	/*����lock sys����*/
	lock_sys = mem_alloc(sizeof(lock_sys_t));
	/*����һ��lock_sys�еĹ�ϣ��*/
	lock_sys->rec_hash = hash_create(n_cells);
	/*������������Ϣ�Ļ�����*/
	lock_latest_err_buf = mem_alloc(5000);
}

ulint lock_get_size()
{
	return (ulint)(sizeof(lock_t));
}

/*�����������ģʽ(IS, IX, S, X, AINC,NONE)*/
UNIV_INLINE ulint lock_get_mode(lock_t* lock)
{
	ut_ad(lock);
	return lock->type_mode & LOCK_MODE_MASK;
}

/*�����������(table lock, rec lock)*/
UNIV_INLINE ulint lock_get_type(lock_t* lock)
{
	ut_ad(lock);
	return lock->type_mode & LOCK_TYPE_MASK;
}

/*������Ƿ���LOCK_WAIT״̬*/
UNIV_INLINE ibool lock_get_wait(lock_t* lock)
{
	ut_ad(lock);
	if(lock->type_mode & LOCK_WAIT) /*�����ڵȴ�״̬*/
		return TRUE;

	return FALSE;
}

/*�����������ĵȴ�״̬LOCK_WAIT*/
UNIV_INLINE void lock_set_lock_and_trx_wait(lock_t* lock, trx_t* trx)
{
	ut_ad(lock);
	ut_ad(trx->wait_lock == NULL);

	trx->wait_lock = lock;
	lock->type_mode = lock->type_mode | LOCK_WAIT;
}

/*������ĵȴ�LOCK_WAIT״̬*/
UNIV_INLINE void lock_reset_lock_and_trx_wait(lock_t* lock)
{
	ut_ad((lock->trx)->wait_lock == lock);
	ut_ad(lock_get_wait(lock));

	(lock->trx)->wait_lock = NULL;
	lock->type_mode = lock->type_mode & ~LOCK_WAIT;
}

/*�ж����Ƿ���LOCK_GAP��Χ��״̬*/
UNIV_INLINE ibool lock_rec_get_gap(lock_t* lock)
{
	ut_ad(lock);
	ut_ad(lock_get_type(lock) == LOCK_REC);

	if(lock->type_mode & LOCK_GAP)
		return TRUE;

	return FALSE;
}

/*�����������ļ�¼LOCK_GAP��Χ��״̬*/
UNIV_INLINE void lock_rec_set_gap(lock_t* lock, ibool val)
{
	ut_ad(lock);
	ut_ad((val == TRUE) || (val == FALSE));
	ut_ad(lock_get_type(lock) == LOCK_REC);

	if(val)
		lock->type_mode = lock->type_mode | LOCK_GAP;
	else
		lock->type_mode = lock->type_mode & ~LOCK_GAP;
}

/*�ж�����mode1�Ƿ��mode2����ǿ�ȣ�һ��LOCK_X > LOCK_S > LOCK_IX >= LOCK_IS*/
UNIV_INLINE ibool lock_mode_stronger_or_eq(ulint mode1, ulint mode2)
{
	ut_ad(mode1 == LOCK_X || mode1 == LOCK_S || mode1 == LOCK_IX
		|| mode1 == LOCK_IS || mode1 == LOCK_AUTO_INC);

	ut_ad(mode2 == LOCK_X || mode2 == LOCK_S || mode2 == LOCK_IX
		|| mode2 == LOCK_IS || mode2 == LOCK_AUTO_INC);
	
	if(mode1 == LOCK_X)
		return TRUE;
	else if(mode1 == LOCK_AUTO_INC && mode2 == LOCK_AUTO_INC)
		return TRUE;
	else if(mode1 == LOCK_S && (mode2 == LOCK_S || mode2 == LOCK_IS))
		return TRUE;
	else if(mode1 == LOCK_IX && (mode2 == LOCK_IX || mode2 == LOCK_IS))
		return TRUE;

	return FALSE;
}

/*�ж���������mode1�Ƿ����mode2ģʽ
		AINC	IS		IX		S		X
AINC	n		y		y		n		n
IS		y		y		y		y		n
IX		y		y		y		n		n
S		n		y		n		y		n
X		n		n		n		n		n
*****************************************/
UNIV_INLINE ibool lock_mode_compatible(ulint mode1, ulint mode2)
{
	ut_ad(mode1 == LOCK_X || mode1 == LOCK_S || mode1 == LOCK_IX
		|| mode1 == LOCK_IS || mode1 == LOCK_AUTO_INC);
	ut_ad(mode2 == LOCK_X || mode2 == LOCK_S || mode2 == LOCK_IX
		|| mode2 == LOCK_IS || mode2 == LOCK_AUTO_INC);

	/*�������Ǽ��ݹ�����������������*/
	if(mode1 == LOCK_S && (mode2 == LOCK_IS || mode2 == LOCK_S))
		return TRUE;
	/*�������ǲ������κ�������ʽ����*/
	else if(mode1 == LOCK_X) 
		return FALSE;
	/*���������Ǽ�����������*/
	else if(mode1 == LOCK_AUTO_INC && (mode2 == LOCK_IS || mode2 == LOCK_IX))
		return TRUE;
	/*��������ģʽ���ݹ���������������������������͹���������*/
	else if(mode1 == LOCK_IS && (mode2 == LOCK_IS || mode2 == LOCK_IX
								 || mode2 == LOCK_AUTO_INC || mode2 == LOCK_S))
		return TRUE;
	/*�����ռ��ģʽ�����������������������������ռ��*/
	else if(mode1 == LOCK_IX &&(mode2 == LOCK_IS || mode2 == LOCK_AUTO_INC || mode2 == LOCK_IX))
		return TRUE;

	return FALSE;							 
}

/*����mode == LOCK_S,����LOCK_X��mode = LOCK_X����LOCK_S*/
UNIV_INLINE ulint lock_get_confl_mode(ulint mode)
{
	ut_ad(mode == LOCK_X || mode == LOCK_S);
	if(mode == LOCK_S)
		return LOCK_X;
	
	return LOCK_S;
}

/*�ж�lock1�Ƿ����Ϊlock2�Ĵ��ڶ���������*/
UNIV_INLINE ibool lock_has_to_wait(lock_t* lock1, lock_t* lock2)
{
	if(lock1->trx != lock2->trx && !lock_mode_compatible(lock_get_mode(lock1), lock_get_mode(lock2)))
		return TRUE;
	return FALSE;
}

/*��ü�¼��������bitmap����*/
UNIV_INLINE ulint lock_rec_get_n_bits(lock_t* lock)
{
	return lock->un_member.rec_lock.n_bits;
}

/*���ҳ��i�еļ�¼����*/
UNIV_INLINE ibool lock_rec_get_nth_bit(lock_t* lock, ulint i)
{
	ulint	byte_index;
	ulint	bit_index;
	ulint	b;

	ut_ad(lock);
	ut_ad(lock_get_type(lock) == LOCK_REC);

	if(i >= lock->un_member.rec_lock.n_bits)
		return FALSE;

	byte_index = i / 8;
	bit_index = i % 8;

	/*����bitmap��λ*/
	b = (ulint)*((byte*)lock + sizeof(lock_t) + byte_index);
	return ut_bit_get_nth(b, bit_index);
}

/*ҳ��i�����һ��lock����*/
UNIV_INLINE void lock_rec_set_nth_bit(lock_t* lock, ulint i)
{
	ulint	byte_index;
	ulint	bit_index;
	byte*	ptr;
	ulint	b;

	ut_ad(lock);
	ut_ad(lock_get_type(lock) == LOCK_REC);
	ut_ad(i < lock->un_member.rec_lock.n_bits);

	byte_index = i / 8;
	bit_index = i % 8;

	ptr = (byte*)lock + sizeof(lock_t) + byte_index;
	b = (ulint)*ptr;
	b = ut_bit_set_nth(b, bit_index, TRUE);
	*ptr = (byte)b;
}

/*���rec lock bitmap��һ����lock�����������*/
static ulint lock_rec_find_set_bit(lock_t* lock)
{
	ulint i;
	for(i = 0; i < lock_rec_get_n_bits(lock); i ++){
		if(lock_rec_get_nth_bit(lock, i))
			return i;
	}

	return ULINT_UNDEFINED;
}

/*���ҳ�ĵ�i�е�lock����״̬*/
UNIV_INLINE void lock_rec_reset_nth_bit(lock_t* lock, ulint i)
{
	ulint	byte_index;
	ulint	bit_index;
	byte*	ptr;
	ulint	b;

	ut_ad(lock);
	ut_ad(lock_get_type(lock) == LOCK_REC);
	ut_ad(i < lock->un_member.rec_lock.n_bits);

	byte_index = i / 8;
	bit_index = i % 8;

	ptr = (byte*)lock + sizeof(lock_t) + byte_index;
	b = (ulint)*ptr;
	b = ut_bit_set_nth(b, bit_index, FALSE);
	*ptr = (byte)b;
}

/*��ȡҲ�ü�¼ͬһ��ҳ����һ����*/
UNIV_INLINE lock_t* lock_rec_get_next_on_page(lock_t* lock)
{
	ulint	space;
	ulint	page_no;

	ut_ad(mutex_own(&kernel_mutex));

	space = lock->un_member.rec_lock.space;
	page_no = lock->un_member.rec_lock.page_no;

	/*��lock_sys�Ĺ�ϣ���в���*/
	for(;;){
		lock = HASH_GET_NEXT(hash, lock);
		if(lock == NULL)
			break;

		/*LOCK������ͬһҳ��*/
		if(lock->un_member.rec_lock.space == space && lock->un_member.rec_lock.page_no = page_no)
			break;
	}

	return lock;
}

/*��ã�space, page_no��ָ���page�ĵ�һ������*/
UNIV_INLINE lock_t* lock_rec_get_first_on_page_addr(ulint space, ulint page_no)
{
	lock_t* lock;

	ut_ad(mutex_own(&kernel_mutex));

	/*lock_sys��ϣ������*/
	lock = HASH_GET_FIRST(lock_sys->rec_hash, lock_rec_hash(space, page_no));
	while(lock){
		if ((lock->un_member.rec_lock.space == space) 
			&& (lock->un_member.rec_lock.page_no == page_no))
			break;

		lock = HASH_GET_NEXT(hash, lock);
	}

	return lock;
}

/*�жϣ�space page_no��ָ���ҳ�Ƿ�����ʽ����*/
ibool lock_rec_expl_exist_on_page(ulint space, ulint page_no)
{
	ibool ret;

	mutex_enter(&kernel_mutex);
	if(lock_rec_get_first_on_page_addr(space, page_no))
		ret = TRUE;
	else
		ret = FALSE;

	mutex_exit(&kernel_mutex);

	return ret;
}

/*���ptr����ҳ�ĵ�һ����*/
UNIV_INLINE lock_t* lock_rec_get_first_on_page(byte* ptr)
{
	ulint	hash;
	lock_t*	lock;
	ulint	space;
	ulint	page_no;

	ut_ad(mutex_own(&kernel_mutex));

	hash = buf_frame_get_lock_hash_val(ptr);
	lock = HASH_GET_FIRST(lock_sys->rec_hash, hash);
	while(lock){
		/*Ϊʲô���Ƿ��������أ����˾���Ӧ�÷�������ȽϺ�*/
		space = buf_frame_get_space_id(ptr);
		page_no = buf_frame_get_page_no(ptr);

		if(space == lock->un_member.rec_lock.space && page_no == lock->un_member.rec_lock.page_no)
			break;

		lock = HASH_GET_NEXT(hash, lock);
	}

	return lock;
}

/*����м�¼��lock��һ����ʽ����*/
UNIV_INLINE lock_t* lock_rec_get_next(rec_t* rec, lock_t* lock)
{
	ut_ad(mutex_own(&kernel_mutex));

	for(;;){
		lock = lock_rec_get_next_on_page(lock);
		if(lock == NULL)
			return NULL;

		if(lock_rec_get_nth_bit(lock, rec_get_heap_no(rec)))
			return lock;
	}
}

/*���rec��¼��һ����ʽ����*/
UNIV_INLINE lock_t* lock_rec_get_first(rec_t* rec)
{
	lock_t* lock;

	ut_ad(mutex_own(&kernel_mutex));
	lock = lock_rec_get_first_on_page(rec);
	while(lock){
		if(lock_rec_get_nth_bit(lock, rec_get_heap_no(rec))) /*�ж�lock��bitmap�Ƿ��ж�Ӧ��λ״̬*/
			break;

		lock = lock_rec_get_next_on_page(lock);
	}

	return lock;
}

/*���lock��bitmap,�������������������Ϊ�������������ʱ����ã�ֻ����������ʱ��ʼ����*/
static void lock_rec_bitmap_reset(lock_t* lock)
{
	byte*	ptr;
	ulint	n_bytes;
	ulint	i;

	ptr = (byte*)lock + sizeof(lock_t);
	n_bytes = lock_rec_get_n_bits(lock) / 8;
	ut_ad(lock_rec_get_n_bits(lock) % 8 == 0);

	/*��lock��bitmap��Ϊ0*/
	for(i = 0; i < n_bytes; i ++){
		*ptr = 0;
		ptr ++;
	}
}

/*����һ���м�¼��������lock�е����ݿ������·����������*/
static lock_t* lock_rec_copy(lock_t* lock, mem_heap_t* heap)
{
	lock_t*	dupl_lock;
	ulint	size;

	/*���lock����ռ�õĿռ��С*/
	size = sizeof(lock_t) + lock_rec_get_n_bits(lock) / 8;
	dupl_lock = mem_heap_alloc(heap, size);

	ut_memcpy(dupl_lock, lock, size);

	return dupl_lock;
}

/*���in_lock��ǰһ�����м�¼��,�����¼�е��������heap_no*/
static lock_t* lock_rec_get_prev(lock_t* in_lock, ulint heap_no)
{
	lock_t*	lock;
	ulint	space;
	ulint	page_no;
	lock_t*	found_lock 	= NULL;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad(lock_get_type(in_lock) == LOCK_REC);

	space = in_lock->un_member.rec_lock.space;
	page_no = in_lock->un_member.rec_lock.page_no;

	lock = lock_rec_get_first_on_page_addr(space, page_no);
	for(;;){
		ut_ad(lock);

		if(lock == in_lock)
			return found_lock;

		if(lock_rec_get_nth_bit(lock, heap_no)) /*�ж��Ƿ��Ǳ��м�¼��lock*/
			found_lock = lock;

		lock = lock_rec_get_next_on_page(lock);
	}
}

/*�ж�����trx�Ƿ����table������mode����ǿ�ȵ���,����У�����lockָ��*/
UNIV_INLINE lock_t* lock_table_has(trx_t* trx, dict_table_t* table, ulint mode)
{
	lock_t* lock;

	ut_ad(mutex_own(&kernel_mutex));

	/*�Ӻ���ɨ�赽ǰ��, ����trx�����Ѿ��и���ǿ�ȵ��������table��*/
	lock = UT_LIST_GET_LAST(table->locks);
	while(lock != NULL){
		if(lock->trx == trx && lock_mode_stronger_or_eq(lock_get_mode(lock), mode)){
			ut_ad(!lock_get_wait(lock));
			return lock;
		}

		lock = UT_LIST_GET_PREV(un_member.tab_lock.locks, lock);
	}

	return NULL;
}

/*���һ����mode����ǿ�ȵ�rec�м�¼������ʽ�����������������trx����ģ����Ҵ���non_gap״̬*/
UNIV_INLINE lock_t* lock_rec_has_expl(ulint mode, rec_t* rec, trx_t* trx)
{
	lock_t* lock;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad((mode == LOCK_X) || (mode == LOCK_S));

	lock = lock_rec_get_first(rec);
	while(lock != NULL){
		if(lock->trx == trx && lock_mode_stronger_or_eq(lock_get_mode(lock), mode)
			&& !lock_get_wait(lock) && !(lock_rec_get_gap(lock) || page_rec_is_supremum(rec)))
			return lock;

		lock = lock_rec_get_next(rec, lock);
	}

	return NULL;
}

/*����Ƿ��trx�����������rec��¼�ϳ��б�mode����ǿ�ȵ�������ʽ����*/
UNIV_INLINE lock_t* lock_rec_other_has_expl_req(ulint mode, ulint gap, ulint wait, rec_t* rec, trx_t* trx)
{
	lock_t* lock;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad((mode == LOCK_X) || (mode == LOCK_S));

	lock = lock_rec_get_first(rec);
	while(lock != NULL){
		if(lock->trx != trx && (gap || !(lock_rec_get_gap(lock) || page_rec_is_supremum(rec))) /*gap����supremum������+���Χ*/
			&& (wait || !lock_get_wait(lock)) 
			&& lock_mode_stronger_or_eq(lock_get_mode(lock), mode))
			return lock;

		lock = lock_rec_get_next(rec, lock);
	}

	return NULL;
}

/*�ڼ�¼���ڵ�page�У�����trx�������type_modeģʽ�ĵ���,�����������е���ű������rec�����*/
UNIV_INLINE lock_t* lock_rec_find_similar_on_page(ulint type_mode, rec_t* rec, trx_t* trx)
{
	lock_t*	lock;
	ulint	heap_no;

	ut_ad(mutex_own(&kernel_mutex));

	heap_no = rec_get_heap_no(rec);
	lock = lock_rec_get_first_on_page(rec);
	while(lock != NULL){
		if(lock->trx == trx && lock->type_mode == type_mode
			&& lock_rec_get_n_bits(lock) > heap_no)
			return lock;

		lock = lock_rec_get_next_on_page(lock);
	}

	return NULL;
}

/*����rec��¼�Ķ��������Ƿ���ʽ��*/
trx_t* lock_sec_rec_some_has_impl_off_kernel(rec_t* rec, dict_index_t* index)
{
	page_t*	page;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad(!(index->type & DICT_CLUSTERED));
	ut_ad(page_rec_is_user_rec(rec));

	/*���rec��Ӧ��page*/
	page = buf_frame_align(rec);

	if(!(ut_dulint_cmp(page_get_max_trx_id(page), trx_list_get_min_trx_id()) >= 0) 
		&& !recv_recovery_is_on())
		return NULL;

	return row_vers_impl_x_locked_off_kernel(rec, index);
}

/*����һ����¼����*/
static lock_t* lock_rec_create(ulint type_mode, rec_t* rec, dict_index* index, trx_t* trx)
{
	page_t*	page;
	lock_t*	lock;
	ulint	page_no;
	ulint	heap_no;
	ulint	space;
	ulint	n_bits;
	ulint	n_bytes;

	ut_ad(mutex_own(&kernel_mutex));

	page = buf_frame_align(rec);
	space = buf_frame_get_space_id(page);
	page_no	= buf_frame_get_page_no(page);
	heap_no = rec_get_heap_no(rec);

	/*supremum��¼�ǲ���ʹ��LOCK_GAP��Χ����*/
	if(rec == page_get_supremum_rec(page))
		type_mode = type_mode & ~LOCK_GAP;

	/*����rec_lock bitmap,��page�Ѿ������ȥ�ļ�¼�� + ��ֵ����ֵ64��Ϊ�˻�������ȫ��*/
	n_bits = page_header_get_field(page, PAGE_N_HEAP) + LOCK_PAGE_BITMAP_MARGIN;
	n_bytes = n_bits / 8 + 1;

	/*����lock_t*/
	lock = mem_heap_alloc(trx->lock_heap, sizeof(lock_t) + n_bytes);
	if(lock == NULL)
		return NULL;

	/*��lock���뵽trx���������*/
	UT_LIST_ADD_LAST(trx_locks, trx->trx_locks, lock);
	lock->trx = trx;

	lock->type_mode = (type_mode & ~LOCK_TYPE_MASK) | LOCK_REC;
	lock->index = index;

	lock->un_member.rec_lock.space = space;
	lock->un_member.rec_lock.page_no = page_no;
	lock->un_member.rec_lock.n_bits = n_bytes * 8;

	/*��ʼ��lock bitmap*/
	lock_rec_bitmap_reset(lock);
	lock_rec_set_nth_bit(lock, heap_no);

	HASH_INSERT(lock_t, hash, lock_sys->rec_hash, lock_rec_fold(space, page_no), lock);
	if(type_mode & LOCK_WAIT) /*����wait flag*/
		lock_set_lock_and_trx_wait(lock, trx);

	return lock;
}

/*����һ����¼������������lock wait״̬�Ŷӣ�*/
static ulint lock_rec_enqueue_waiting(ulint type_mode, rec_t* rec, dict_index_t* index, que_thr_t* thr)
{
	lock_t* lock;
	trx_t* trx;

	ut_ad(mutex_own(&kernel_mutex));

	/*��ѯ�߳���ͣ*/
	if(que_thr_stop(thr)){
		ut_a(0);
		return DB_QUE_THR_SUSPENDED;
	}

	trx = thr_get_trx(thr);
	if(trx->dict_operation){
		ut_print_timestamp(stderr);
		fprintf(stderr,"  InnoDB: Error: a record lock wait happens in a dictionary operation!\n"
			"InnoDB: Table name %s. Send a bug report to mysql@lists.mysql.com\n", index->table_name);
	}

	/*����һ������������lock wait״̬*/
	lock = lock_rec_create(type_mode | LOCK_WAIT, rec, index, trx);
	/*�������*/
	if(lock_deadlock_occurs(lock, trx)){
		/*����lock wait��λ*/
		lock_reset_lock_and_trx_wait(lock);
		lock_rec_reset_nth_bit(lock, rec_get_heap_no(rec));

		return DB_DEADLOCK;
	}

	trx->que_state = TRX_QUE_LOCK_WAIT;
	trx->wait_started = time(NULL);

	ut_a(que_thr_stop(thr));
	if(lock_print_waits)
		printf("Lock wait for trx %lu in index %s\n", ut_dulint_get_low(trx->id), index->name);

	return DB_LOCK_WAIT;
}

/*����һ����¼�������������������Ķ�����*/
static lock_t* lock_rec_add_to_queue(ulint type_mode, rec_t* rec, dict_index_t* index, trx_t* trx)
{
	lock_t*	lock;
	lock_t*	similar_lock	= NULL;
	ulint	heap_no;
	page_t*	page;
	ibool	somebody_waits	= FALSE;

	ut_ad(mutex_own(&kernel_mutex));

	/*�������ϸ��������*/
	ut_ad((type_mode & (LOCK_WAIT | LOCK_GAP))
		|| ((type_mode & LOCK_MODE_MASK) != LOCK_S)
		|| !lock_rec_other_has_expl_req(LOCK_X, 0, LOCK_WAIT, rec, trx));

	ut_ad((type_mode & (LOCK_WAIT | LOCK_GAP))
		|| ((type_mode & LOCK_MODE_MASK) != LOCK_X)
		|| !lock_rec_other_has_expl_req(LOCK_S, 0, LOCK_WAIT, rec, trx));

	type_mode = type_mode | LOCK_REC;
	page = buf_frame_align(rec);

	/*��¼��supremum*/
	if(rec == page_get_supremum_rec(page)){
		type_mode = type_mode & ~LOCK_GAP;
	}

	heap_no = rec_get_heap_no(rec);
	lock = lock_rec_get_first_on_page(rec);

	/*�����м�¼rec�Ƿ��ô���lock wait״̬������*/
	while(lock != NULL){
		if(lock_get_wait(lock) && lock_rec_get_nth_bit(lock, heap_no))
			somebody_waits = TRUE;

		lock = lock_rec_get_next_on_page(lock);
	}

	/*����trx������ģʽΪtype_mode�ļ�¼��������������rec������page�е��м�¼*/
	similar_lock = lock_rec_find_similar_on_page(type_mode, rec, trx);

	/*��������similar_lock,һ��ִ��������һ������ֻ����һ������*/
	if(similar_lock != NULL && !somebody_waits && !(type_mode & LOCK_WAIT)){
		lock_rec_set_nth_bit(similar_lock, heap_no); /*�ڶ�Ӧ��λ�ϼ�������ʶ��ֻ��Ŀ�������������ڵȴ��Ż���ӱ�ʶ�������п��ܿ���ֱ��ִ��*/
		return similar_lock;
	}

	/*û�ж�Ӧ��������ֱ�Ӵ���һ�����������Ŷ�*/
	return lock_rec_create(type_mode, rec, index, trx);
}

/*���ٻ���������󲿷����̶���������,û���κ���������м�¼��*/
UNIV_INLINE ibool lock_rec_lock_fast(ibool impl, ulint mode, rec_t* rec, dict_index_t* index, que_thr_t* thr)
{
	lock_t*	lock;
	ulint	heap_no;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad(mode == LOCK_X || mode == LOCK_S);

	heap_no = rec_get_heap_no(rec);
	lock = lock_rec_get_first_on_page(rec);
	if(lock == NULL){ /*page��û����������, ����һ��mode���͵���*/
		if(!impl) /*û����ʽ��������һ����ʽ��������м�¼��*/
			lock_rec_create(mode, rec, index, thr_get_trx(thr));
		return TRUE;
	}

	/*page���ж��LOCK,���ܿ��ٻ����������SLOWģʽ*/
	if(lock_rec_get_next_on_page(lock))
		return FALSE;

	/*lock��������thr�е�trx����ͬ�����߲�������������lock�ļ�¼��rec����ͬ��ֱ�ӷ��ؽ���SLOWģʽ*/
	if(lock->trx != thr_get_trx(thr) || lock->type_mode != (mode | LOCK_REC) 
		|| lock_rec_get_n_bits(lock) <= heap_no)
			return FALSE;

	/*����ֻ�и�1����(������������)��������ϣ������������ָ��ļ�¼rec,ֱ����Ϊ���Ի����Ȩ*/
	if(!impl) 
		lock_rec_set_nth_bit(lock, heap_no);

	return TRUE;
}

/*���ŶӶ�����ѡ��lock,�������Ȩ,�������ļ�����*/
static ulint lock_rec_lock_slow(ibool impl, ulint mode, rec_t* rec, dict_index_t* index, que_thr_t* thr)
{
	ulint	confl_mode;
	trx_t*	trx;
	ulint	err;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad((mode == LOCK_X) || (mode == LOCK_S));

	trx = thr_get_trx(thr);
	confl_mode = lock_get_confl_mode(mode);

	ut_ad((mode != LOCK_S) || lock_table_has(trx, index->table, LOCK_IS));
	ut_ad((mode != LOCK_X) || lock_table_has(trx, index->table, LOCK_IX));

	/*trx�б�mode�����ϸ����ģʽ����rec����(��ʽ��)��û��Ҫ��������*/
	if(lock_rec_has_expl(mode, rec, trx))
		err = DB_SUCCESS;
	else if(lock_rec_other_has_expl_req(confl_mode, 0, LOCK_WAIT, rec, trx)) /*���������и��ϸ����(��)��rec����*/
		err = lock_rec_enqueue_waiting(mode, rec, index, thr); /*����һ������ʽ�������еȴ�*/
	else{
		if(!impl) /*����һ�������������뵽��������*/
			lock_rec_add_to_queue(LOCK_REC | mode, rec, index, trx);
		err = DB_SUCCESS;
	}

	return err;
}

/*�Լ�¼����������*/
ulint lock_rec_lock(ibool impl, ulint mode, rec_t* rec, dict_index_t* index, que_thr_t* thr)
{
	ulint	err;

	ut_ad(mutex_own(&kernel_mutex));

	ut_ad((mode != LOCK_S) || lock_table_has(thr_get_trx(thr), index->table, LOCK_IS));
	ut_ad((mode != LOCK_X) || lock_table_has(thr_get_trx(thr), index->table, LOCK_IX));

	/*�ȳ��Կ��ټ���*/
	if(lock_rec_lock_fast(impl, mode, rec, index, thr))
		err = DB_SUCCESS;
	else
		err = lock_rec_lock_slow(impl, mode, rec, index, thr);

	return err;
}

/*���wait_lock�Ƿ���ָ��ͬһ�м�¼���Ҳ����ݵ���,O(n)*/
static ibool lock_rec_has_to_wait_in_queue(lock_t* wait_lock)
{
	lock_t*	lock;
	ulint	space;
	ulint	page_no;
	ulint	heap_no;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad(lock_get_wait(wait_lock));

	/*��õĶ�λ��Ϣspace page rec*/
	space = wait_lock->un_member.rec_lock.space;
	page_no = wait_lock->un_member.rec_lock.page_no;
	heap_no = lock_rec_find_set_bit(wait_lock);

	lock = lock_rec_get_first_on_page_addr(space, page_no);
	while(lock != wait_lock){
		/*wait_lock��lock�����ݣ����Ҵ���ͬһrec��¼��*/
		if (lock_has_to_wait(wait_lock, lock) && lock_rec_get_nth_bit(lock, heap_no))
			return TRUE;

		lock = lock_rec_get_next_on_page(lock);
	}

	return FALSE;
}

/*�����Ȩ*/
void lock_grant(lock_t* lock)
{
	ut_ad(mutex_own(&kernel_mutex));

	/*����lock wait��ʶ��λ*/
	lock_reset_lock_and_trx_wait(lock);

	/*������������ģʽ,��������*/
	if(lock_get_mode(lock) == LOCK_AUTO_INC){
		if(lock->trx->auto_inc_lock != NULL)
			fprintf(stderr, "InnoDB: Error: trx already had an AUTO-INC lock!\n");
		
		lock->trx->auto_inc_lock = lock;
	}

	if(lock_print_waits)
		printf("Lock wait for trx %lu ends\n", ut_dulint_get_low(lock->trx->id));

	/*��������ȴ�,��������ִ��*/
	trx_end_lock_wait(lock->trx);
}

/*ȡ�����ڵȴ�����,�����������Ӧ������*/
static void lock_rec_cancel(lock_t* lock)
{
	ut_ad(mutex_own(&kernel_mutex));

	/*��λ����bitmapλ*/
	lock_rec_reset_nth_bit(lock, lock_rec_find_set_bit(lock));
	/*��λ����lock wait״̬*/
	lock_reset_lock_and_trx_wait(lock);

	/*������Ӧ����ĵȴ�*/
	trx_end_lock_wait(lock->trx);
}

/*��in_lock��lock_sys��ɾ���� ���������Ӧҳ��һ���ȴ�����������, in_lock������һ��waiting����granted״̬����*/
void lock_rec_dequeue_from_page(lock_t* in_lock)
{
	ulint	space;
	ulint	page_no;
	lock_t*	lock;
	trx_t*	trx;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad(lock_get_type(in_lock) == LOCK_REC);

	trx = in_lock->trx;

	space = in_lock->un_member.rec_lock.space;
	page_no = in_lock->un_member.rec_lock.page_no;

	/*��in_lock��lock_sys��trx��ɾ��*/
	HASH_DELETE(lock_t, hash, lock_sys->rec_hash, lock_rec_fold(space, page_no), in_lock);
	UT_LIST_REMOVE(trx_locks, trx->trx_locks, in_lock);

	/*����ͬһ��ҳ�п��Լ���(grant)������*/
	lock = lock_rec_get_first_on_page_addr(space, page_no);
	while(lock != NULL){
		if(lock_get_wait(lock) && !lock_rec_has_to_wait_in_queue(lock)) /*lock���ڵȴ�״̬������ָ����м�¼û���������ų���*/
			lock_grant(lock);

		lock = lock_rec_get_next_on_page(lock);
	}
}
/*��in_lock��lock_sys��ɾ��, in_lock������һ��waiting����granted״̬����*/
static void lock_rec_discard(lock_t* in_lock)
{
	ulint	space;
	ulint	page_no;
	trx_t*	trx;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad(lock_get_type(in_lock) == LOCK_REC);

	trx = in_lock->trx;

	space = in_lock->un_member.rec_lock.space;
	page_no = in_lock->un_member.rec_lock.page_no;

	HASH_DELETE(lock_t, hash, lock_sys->rec_hash, lock_rec_fold(space, page_no), in_lock);
	UT_LIST_REMOVE(trx_locks, trx->trx_locks, in_lock);
}

/*����page�������м�¼������*/
static void lock_rec_free_all_from_discard_page(page_t* page)
{
	ulint	space;
	ulint	page_no;
	lock_t*	lock;
	lock_t*	next_lock;

	ut_ad(mutex_own(&kernel_mutex));

	space = buf_frame_get_space_id(page);
	page_no = buf_frame_get_page_no(page);

	lock = lock_rec_get_first_on_page_addr(space, page_no);
	while(lock != NULL){
		ut_ad(lock_rec_find_set_bit(lock) == ULINT_UNDEFINED);
		ut_ad(!lock_get_wait(lock));

		next_lock = lock_rec_get_next_on_page(lock);

		lock_rec_discard(lock);
		lock = next_lock;
	}
}

/*��λrec����bitmap,��ȡ����Ӧ������ĵȴ�*/
void lock_rec_reset_and_release_wait(rec_t* rec)
{
	lock_t* lock;
	ulint heap_no;

	ut_ad(mutex_own(&kernel_mutex));
	
	/*��ü�¼���*/
	heap_no = rec_get_heap_no(rec);
	lock = lock_rec_get_first(rec);
	while(lock != NULL){
		if(lock_get_wait(lock))
			lock_rec_cancel(lock);
		else
			lock_rec_reset_nth_bit(lock, heap_no);

		lock = lock_rec_get_next(rec, lock);
	}
}

/*heir��¼�����̳�rec���еļ�¼��������lock wait���̳�ΪGAP��Χ��*/
void lock_rec_inherit_to_gap(rec_t* heir, rec_t* rec)
{
	lock_t* lock;

	ut_ad(mutex_own(&kernel_mutex));

	lock = lock_rec_get_first(rec);
	while(lock != NULL){
		lock_rec_add_to_queue((lock->type_mode | LOCK_GAP) & ~LOCK_WAIT, heir, lock->index, lock->trx);
		lock = lock_rec_get_next(rec, lock);
	}
}

/*��donator��¼������ת�Ƶ�receiver��¼�ϣ����Ҹ�λdonator����bitmap*/
static void lock_rec_move(rec_t* receiver, rec_t* donator)
{
	lock_t*	lock;
	ulint	heap_no;
	ulint	type_mode;

	ut_ad(mutex_own(&kernel_mutex));

	heap_no = rec_get_heap_no(donator);
	lock = lock_rec_get_first(donator);
	/*reciver������û���κ�������*/
	ut_ad(lock_rec_get_first(receiver) == NULL);

	while(lock != NULL){
		type_mode = lock->type_mode;
		lock_rec_reset_nth_bit(lock, heap_no);

		if(lock_get_wait(lock))
			lock_reset_lock_and_trx_wait(lock);

		/*��receiver��¼�����Ӷ�Ӧģʽ������*/
		lock_rec_add_to_queue(type_mode, receiver, lock->index, lock->trx);
		lock = lock_rec_get_next(donator, lock);
	}

	/*donator������Ӧ�ñ���ȫ���*/
	ut_ad(lock_rec_get_first(donator) == NULL);
}

/*����ͬʱ����old page����������������Щ������page��Ӧ���м�¼��,���Ӷ�O(m * n)*/
void lock_move_reorganize_page(page_t* page, page_t* old_page)
{
	lock_t*		lock;
	lock_t*		old_lock;
	page_cur_t	cur1;
	page_cur_t	cur2;
	ulint		old_heap_no;
	
	mem_heap_t*	heap = NULL;
	rec_t*		sup;

	UT_LIST_BASE_NODE_T(lock_t)	old_locks;

	lock_mutex_enter_kernel();

	lock = lock_rec_get_first_on_page(page);
	if(lock == NULL){
		lock_mutex_enter_kernel();
		return;
	}

	heap = mem_heap_create(256);

	UT_LIST_INIT(old_locks);

	/*��page���е��������Ƶ�old_locks�У��������*/
	while(lock != NULL){
		old_lock = lock_rec_copy(lock, heap);
		UT_LIST_ADD_LAST(trx_locks, old_locks, old_lock);

		lock_rec_bitmap_reset(lock);
		if(lock_get_wait(lock))
			lock_reset_lock_and_trx_wait(lock);

		lock = lock_rec_get_next_on_page(lock);
	}

	sup = page_get_supremum_rec(page);
	
	lock = UT_LIST_GET_FIRST(old_locks);
	while(lock){
		/*��ҳ�α궨λ��ҳ����ʼ��¼*/
		page_cur_set_before_first(page, &cur1);
		page_cur_set_before_first(old_page, &cur2);

		/*ȫ��ɨ��old page�е����м�¼��������Ӧ����ת�Ʋ����鵽page�ļ�¼��,�м�¼������ȴ�����old page�еļ�¼*/
		for(;;){
			ut_ad(0 == ut_memcmp(page_cur_get_rec(&cur1), page_cur_get_rec(&cur2), rec_get_data_size(page_cur_get_rec(&cur2))));

			old_heap_no = rec_get_heap_no(page_cur_get_rec(&cur2));
			if(lock_rec_get_nth_bit(lock, old_heap_no))
				lock_rec_add_to_queue(lock->type_mode, page_cur_get_rec(&cur1), lock->index, lock->trx);

			if(page_cur_get_rec(&cur1) == sup)
				break;

			page_cur_move_to_next(&cur1);
			page_cur_move_to_next(&cur2);
		}

		lock = UT_LIST_GET_NEXT(trx_locks, lock);
	}

	lock_mutex_exit_kernel();

	mem_heap_free(heap);
}

/*������page��rec֮��(����rec)������ת�Ƶ�new page�ϣ���ȡ����page�϶�Ӧ������,new page����ʼ�п�ʼ,���Ӷ�O(m * n)*/
void lock_move_rec_list_end(page_t* new_page, page_t* page, rec_t* rec)
{
	lock_t*		lock;
	page_cur_t	cur1;
	page_cur_t	cur2;
	ulint		heap_no;
	rec_t*		sup;
	ulint		type_mode;

	lock_mutex_enter_kernel();

	sup = page_get_supremum_rec(page);

	lock = lock_rec_get_first_on_page(page);
	while(lock != NULL){
		/*��λrec����page���α���ʼλ��*/
		page_cur_position(rec, &cur1);
		if(page_cur_is_before_first(&cur1))
			page_cur_move_to_next(&cur1);

		page_cur_set_before_first(new_page, &cur2);
		page_cur_move_to_next(&cur2);

		while(page_cur_get_rec(&cur1) != sup){
			ut_ad(0 == ut_memcmp(page_cur_get_rec(&cur1), page_cur_get_rec(&cur2), rec_get_data_size(page_cur_get_rec(&cur2))));

			heap_no = rec_get_heap_no(page_cur_get_rec(&cur1));
			if(lock_rec_get_nth_bit(lock, heap_no)){
				type_mode = lock->type_mode;

				/*�����lock��״̬*/
				lock_rec_reset_nth_bit(lock, heap_no);
				if(lock_get_wait(lock))
					lock_reset_lock_and_trx_wait(lock);

				/*��cur2��Ӧ���м�¼�ϼ���һ����Ӧ����*/
				lock_rec_add_to_queue(type_mode, page_cur_get_rec(&cur2), lock->index, lock->trx);
			}

			page_cur_move_to_next(&cur1);
			page_cur_move_to_next(&cur2);
		}

		lock = lock_rec_get_next_on_page(lock);
	}

	lock_mutex_exit_kernel();
}
/*������page��rec֮ǰ(������rec)������ת�Ƶ�new page�ϣ���ȡ����page�϶�Ӧ������, �¼�¼�д�old_end��ʼ,���Ӷ�O(m * n)*/
void lock_move_rec_list_start(page_t* new_page, page_t* page, rec_t* rec, rec_t* old_end)
{
	lock_t*		lock;
	page_cur_t	cur1;
	page_cur_t	cur2;
	ulint		heap_no;
	ulint		type_mode;

	ut_ad(new_page);

	lock_mutex_enter_kernel();

	lock = lock_rec_get_first_on_page(page);
	while(lock != NULL){
		page_cur_set_before_first(page, &cur1);
		page_cur_move_to_next(&cur1);

		page_cur_position(old_end, &cur2);
		page_cur_move_to_next(&cur2);

		while(page_cur_get_rec(&cur1) != rec){
			ut_ad(0 == ut_memcmp(page_cur_get_rec(&cur1), page_cur_get_rec(&cur2), rec_get_data_size(page_cur_get_rec(&cur2))));

			heap_no = rec_get_heap_no(page_cur_get_rec(&cur1));
			if(lock_rec_get_nth_bit(lock, heap_no)){
				type_mode = lock->type_mode;

				lock_rec_reset_nth_bit(lock, heap_no);
				if(lock_get_wait(lock))
					lock_reset_lock_and_trx_wait(lock);

				lock_rec_add_to_queue(type_mode, page_cur_get_rec(&cur2), lock->index, lock->trx);
			}

			page_cur_move_to_next(&cur1);
			page_cur_move_to_next(&cur2);
		}

		lock = lock_rec_get_next_on_page(lock);
	}

	lock_mutex_exit_kernel();
}

/*page�������ұ߷���,�����¼����������*/
void lock_update_split_right(page_t* right_page, page_t* left_page)
{
	lock_mutex_enter_kernel();
	/*��left page��supremum������ת�Ƶ�right page��*/
	lock_rec_move(page_get_supremum_rec(right_page), page_get_supremum_rec(left_page));
	/*left page��supremum�̳�right page�ĵ�һ����¼����*/
	lock_rec_inherit_to_gap(page_get_supremum_rec(left_page), page_rec_get_next(page_get_infimum_rec(right_page)));

	lock_mutex_exit_kernel();
}

void lock_update_merge_right(rec_t* orig_succ, page_t* left_page)
{
	lock_mutex_enter_kernel();

	/*��left page���е�������ת����һ��gap ��Χ������ͨ��right page�ĵ�һ����¼�����̳У�
	�����Ϳ����ͷ�����֮ǰleft page�ϵ��������൱��������*/
	lock_rec_inherit_to_gap(orig_succ, page_get_supremum_rec(left_page));

	/*��Ϊ�Ƕ�left page�е�������������ֱ�ӿ����ͷŵ�ԭ��������*/
	lock_rec_reset_and_release_wait(page_get_supremum_rec(left_page));
	lock_rec_free_all_from_discard_page(left_page);

	lock_mutex_exit_kernel();
}

/*��root page�ϵ�����ת�Ƶ�new_page*/
void lock_update_root_raise(page_t* new_page, page_t* root)
{
	lock_mutex_enter_kernel();

	lock_rec_move(page_get_supremum_rec(new_page), page_get_supremum_rec(root));

	lock_mutex_exit_kernel();
}

void lock_update_copy_and_discard(page_t* new_page, page_t* page)
{
	lock_mutex_enter_kernel();

	/*��page����ȫ��ת�Ƶ�new_page��supremum�ϣ��൱��GAP��Χ����*/
	lock_rec_move(page_get_supremum_rec(new_page), page_get_supremum_rec(page));
	lock_rec_free_all_from_discard_page(page);

	lock_mutex_exit_kernel();
}

void lock_update_split_left(page_t* right_page, rec_t* left_page)
{
	lock_mutex_enter_kernel();

	/*��left page��supremum�̳�right page�ĵ�һ�м�¼������*/
	lock_rec_inherit_to_gap(page_get_supremum_rec(left_page), page_rec_get_next(page_get_infimum_rec(right_page)));

	lock_mutex_exit_kernel();
}

void lock_update_merge_left(page_t* left_page, rec_t* orig_pred, page_t* right_page)
{
	lock_mutex_enter_kernel();

	if(page_rec_get_next(orig_pred) != page_get_supremum_rec(left_page)){
		/*��orig_pred�̳�left page��supremum��������orig_pred��suprmum��ǰһ����¼*/
		lock_rec_inherit_to_gap(page_rec_get_next(orig_pred), page_get_supremum_rec(left_page));
		/*�ͷŵ�supremum������*/
		lock_rec_reset_and_release_wait(page_get_supremum_rec(left_page));
	}

	/*��right_page����������ת�Ƶ�left page��supremum�ϣ�������ΪGAP��Χ��(�൱��������)*/
	lock_rec_move(page_get_supremum_rec(left_page), page_get_supremum_rec(right_page));
	lock_rec_free_all_from_discard_page(right_page);

	lock_mutex_exit_kernel();
}

/*���heir�ļ�¼����������̳�rec�������heir��GAP��Χ��*/
void lock_rec_reset_and_inherit_gap_locks(rec_t* heir, rec_t* rec)
{
	mutex_enter(&kernel_mutex);	      		

	lock_rec_reset_and_release_wait(heir);
	lock_rec_inherit_to_gap(heir, rec);

	mutex_exit(&kernel_mutex);	 
}

/*��page����������ת�Ƶ�heir����*/
void lock_update_discard(rec_t* heir, page_t* page)
{
	rec_t* rec;

	lock_mutex_enter_kernel();
	/*page��û������*/
	if(lock_rec_get_first_on_page(page) == NULL){
		lock_mutex_exit_kernel();
		return ;
	}

	rec = page_get_infimum_rec(page);
	for(;;){
		lock_rec_inherit_to_gap(heir, rec);
		lock_rec_reset_and_release_wait(rec);

		if(rec == page_get_supremum_rec(page))
			break;

		rec = page_rec_get_next(rec);
	}

	/*����������page�ϵ����ȴ�*/
	lock_rec_free_all_from_discard_page(page);
}

/*��¼��ӻ��޸�ʱ���������Ǹ�GAP��Χ����*/
void lock_update_insert(rec_t* rec)
{
	lock_mutex_enter_kernel();

	lock_rec_inherit_to_gap(rec, page_rec_get_next(rec));

	lock_mutex_exit_kernel();
}

/*��¼ɾ��ʱ������ת��*/
void lock_update_delete(rec_t* rec)
{
	lock_mutex_enter_kernel();

	lock_rec_inherit_to_gap(page_rec_get_next(rec), rec);
	lock_rec_reset_and_release_wait(rec);

	lock_mutex_exit_kernel();
}

/*��recȫ���Ƶ�infimum��*/
void lock_rec_store_on_page_infimum(rec_t* rec)
{
	page_t* page;
	page = buf_frame_align(rec);

	lock_mutex_enter_kernel();
	lock_rec_move(page_get_infimum_rec(page), rec);
	lock_mutex_exit_kernel();
}

/*��page��infimum��¼����ת�Ƶ�rec��¼��*/
void lock_rec_restore_from_page_infimum(rec_t* rec, page_t* page)
{
	lock_mutex_enter_kernel();

	lock_rec_move(rec, page_get_infimum_rec(page));

	lock_mutex_exit_kernel();
}

/*���һ���������Ƿ�������������,��һ���ݹ������*/
static ibool lock_deadlock_occurs(lock_t* lock, trx_t* trx)
{
	dict_table_t*	table;
	dict_index_t*	index;
	trx_t*		mark_trx;
	ibool		ret;
	ulint		cost	= 0;
	char*		err_buf;

	ut_ad(trx && lock);
	ut_ad(mutex_own(&kernel_mutex));

	/*��ʼ�����������deadlock_mark*/
	mark_trx = UT_LIST_GET_FIRST(trx_list, mark_trx);
	while(mark_trx){
		mark_trx->deadlock_mark = 0;
		mark_trx = UT_LIST_GET_NEXT(trx_list, mark_trx);
	}
	/*���������м��*/
	ret = lock_deadlock_recursive(trx, trx, lock, &cost);
	if(ret){ /*����������Ϣ*/
		if(lock_get_type(lock) == LOCK_TABLE){
			table = lock->un_member.tab_lock.table;
			index = NULL;
		}
		else{
			index = lock->index;
			table = index->table;
		}

		lock_deadlock_found = TRUE;

		err_buf = lock_latest_err_buf + sizeof(lock_latest_err_buf);
		err_buf += sprintf(err_buf, "*** (2) WAITING FOR THIS LOCK TO BE GRANTED:\n");
		ut_a(err_buf <= lock_latest_err_buf + 4000);

		if(lock_get_type(lock) == LOCK_REC){
			lock_rec_print(err_buf, lock);
			err_buf += strlen(err_buf);
		}
		else{
			lock_table_print(err_buf, lock);
			err_buf += strlen(err_buf);
		}

		ut_a(err_buf <= lock_latest_err_buf + 4000);
		err_buf += sprintf(err_buf, "*** WE ROLL BACK TRANSACTION (2)\n");
		ut_a(strlen(lock_latest_err_buf) < 4100);
	}

	return ret;
}

static ibool lock_deadlock_recursive(trx_t* start, trx_t* trx, lock_t* wait_lock, ulint* cost)
{
	lock_t*	lock;
	ulint	bit_no;
	trx_t*	lock_trx;
	char*	err_buf;

	ut_a(trx && start && wait_lock);
	ut_ad(mutex_own(&kernel_mutex));

	if(trx->deadlock_mark == 1)
		return TRUE;

	*cost = *cost + 1;
	if(*cost > LOCK_MAX_N_STEPS_IN_DEADLOCK_CHECK)
		return TRUE;

	lock = wait_lock;
	if(lock_get_type(wait_lock) == LOCK_REC){
		bit_no = lock_rec_find_set_bit(wait_lock);
		ut_a(bit_no != ULINT_UNDEFINED);
	}

	for(;;){
		if(lock_get_type(lock) == LOCK_TABLE){ /*������һ������*/
			lock = UT_LIST_GET_PREV(un_member.tab_lock.locks, lock);
		}
		else{ /*���Ҽ�¼�е���һ������*/
			ut_ad(lock_get_type(lock) == LOCK_REC);
			lock = lock_rec_get_prev(lock, bit_no);
		}

		if(lock == NULL){
			trx->deadlock_mark = 1;
			return FALSE;
		}

		if(lock_has_to_wait(wait_lock, lock)){
			lock_trx = lock->trx;

			if(lock_trx == start){ /*�Ѿ�����������*/
				err_buf = lock_latest_err_buf;

				ut_sprintf_timestamp(err_buf);
				err_buf += strlen(err_buf);

				err_buf += sprintf(err_buf,
					"  LATEST DETECTED DEADLOCK:\n"
					"*** (1) TRANSACTION:\n");

				trx_print(err_buf, wait_lock->trx);
				err_buf += strlen(err_buf);

				err_buf += sprintf(err_buf,
					"*** (1) WAITING FOR THIS LOCK TO BE GRANTED:\n");

				ut_a(err_buf <= lock_latest_err_buf + 4000);

				if (lock_get_type(wait_lock) == LOCK_REC) {
					lock_rec_print(err_buf, wait_lock);
					err_buf += strlen(err_buf);
				} else {
					lock_table_print(err_buf, wait_lock);
					err_buf += strlen(err_buf);
				}

				ut_a(err_buf <= lock_latest_err_buf + 4000);
				err_buf += sprintf(err_buf,
					"*** (2) TRANSACTION:\n");

				trx_print(err_buf, lock->trx);
				err_buf += strlen(err_buf);

				err_buf += sprintf(err_buf,
					"*** (2) HOLDS THE LOCK(S):\n");

				ut_a(err_buf <= lock_latest_err_buf + 4000);

				if (lock_get_type(lock) == LOCK_REC) {
					lock_rec_print(err_buf, lock);
					err_buf += strlen(err_buf);
				} else {
					lock_table_print(err_buf, lock);
					err_buf += strlen(err_buf);
				}

				ut_a(err_buf <= lock_latest_err_buf + 4000);

				if (lock_print_waits) {
					printf("Deadlock detected\n");
				}

				return(TRUE);
			}

			/*�����lock_trx�ȴ�״̬�����еݹ��ж�*/
			if(lock_trx->que_state == TRX_QUE_LOCK_WAIT){
				if(lock_deadlock_recursive(start, lock_trx, lock_trx->wait_lock, cost))
					return TRUE;
			}
		}
	}
}

UNIV_INLINE lock_t* lock_table_create(dict_table_t* table, ulint type_mode, trx_t* trx)
{
	lock_t*	lock;

	ut_ad(table && trx);
	ut_ad(mutex_own(&kernel_mutex));

	if(type_mode == LOCK_AUTO_INC){ /*ֱ�ӽ��������������*/
		lock = table->auto_inc_lock;
		ut_a(trx->auto_inc_lock);
		trx->auto_inc_lock = lock;
	}
	else /*��trx����ķ���heap����һ��lock*/
		lock = mem_heap_alloc(trx->lock_heap, sizeof(lock_t));

	if(lock == NULL)
		return NULL;

	/*���뵽��������б���*/
	UT_LIST_ADD_LAST(trx_locks, trx->trx_locks, lock);
	
	lock->type_mode = type_mode | LOCK_TABLE;
	lock->trx = trx;

	lock->un_member.tab_lock.table = table;
	UT_LIST_ADD_LAST(un_member.tab_lock.locks, table->locks, lock);

	if(type_mode & LOCK_WAIT)
		lock_set_lock_and_trx_wait(lock, trx);

	/*todo:�����ǲ������ȫ�ֵ�lock_sys����*/
	return lock;
}

/*�Ƴ�table_lock*/
UNIV_INLINE void lock_table_remove_low(lock_t* lock)
{
	dict_table_t*	table;
	trx_t*			trx;

	ut_ad(mutex_own(&kernel_mutex));

	table = lock->un_member.table_lock.table;
	trx = lock->trx;

	/*��������*/
	if(lock == trx->auto_inc_lock)
		trx->auto_inc_lock = NULL;

	/*��trx�����Ƴ�lock*/
	UT_LIST_REMOVE(trx_locks, trx->trx_locks, lock);
	/*��table���Ƴ�locks*/
	UT_LIST_REMOVE(un_member.tab_lock.locks, table->locks, lock);
}

ulint lock_table_enqueue_waiting(ulint mode, dict_table_t* table, que_thr_t* thr)
{
	lock_t*	lock;
	trx_t*	trx;

	ut_ad(mutex_own(&kernel_mutex));

	if(que_thr_stop(thr)){
		ut_a(0);
		return DB_QUE_THR_SUSPENDED;
	}

	/*���thr����ִ�е�����*/
	trx = thr_get_trx(thr);
	if(trx->dict_operation){
		ut_print_timestamp(stderr);
		fprintf(stderr, "  InnoDB: Error: a table lock wait happens in a dictionary operation!\n"
			"InnoDB: Table name %s. Send a bug report to mysql@lists.mysql.com\n", table->name);
	}

	/*����һ������*/
	lock = lock_table_create(table, mode | LOCK_WAIT, trx);
	if(lock_deadlock_occurs(lock, trx)){ /*�����ˣ���*/
		lock_reset_lock_and_trx_wait(lock);
		lock_table_remove_low(lock);

		return(DB_DEADLOCK);
	}

	trx->que_state = TRX_QUE_LOCK_WAIT;
	trx->wait_started = time(NULL);

	ut_a(que_thr_stop(thr));

	return DB_LOCK_WAIT;
}

/*������еı����Ƿ��modeģʽ�ų⣿*/
UNIV_INLINE ibool lock_table_other_has_incompatible(trx_t* trx, ulint wait, dict_table_t* table, ulint mode)
{
	lock_t* lock;

	ut_ad(mutex_own(&kernel_mutex));

	lock = UT_LIST_GET_LAST(table->locks);
	while(lock != NULL){
		if(lock->trx == trx && (!lock_mode_compatible(lock_get_mode(lock), mode))
			&& (wait || !lock_get_wait(lock)))
			return TRUE;

		lock = UT_LIST_GET_PREV(un_member.tab_lock.locks, lock);
	}

	return FALSE; 
}

ulint lock_table(ulint flags, dict_table_t* table, ulint mode, que_thr_t* thr)
{
	trx_t*	trx;
	ulint	err;

	ut_ad(table && thr);

	if(flags & BTR_NO_LOCKING_FLAG)
		return DB_SUCCESS;

	trx = thr_get_trx(thr);

	lock_mutex_enter_kernel();

	/*�ж��Ƿ��и��ϵ���*/
	if(lock_table_has(trx, table, mode)){
		lock_mutex_exit_kernel();
		return DB_SUCCESS;
	}

	/*������Ƿ��ų�*/
	if(lock_table_other_has_incompatible(trx, LOCK_WAIT, table, mode)){ /*���ų⣬��������Ŷ�*/
		err = lock_table_enqueue_waiting(mode, table, thr);
		lock_mutex_exit_kernel();

		return err;
	}
	/*������������LOCK_WAIT*/
	lock_table_create(table, mode, trx);

	lock_mutex_exit_kernel();

	return DB_SUCCESS;
}
/*�ж�table���б�������*/
ibool lock_is_on_table(dict_table_t* table)
{
	ibool	ret;

	ut_ad(table);

	lock_mutex_enter_kernel();
	
	if(UT_LIST_GET_LAST(table->locks) != NULL)
		ret = TRUE;
	else
		ret = FALSE;

	lock_mutex_exit_kernel();

	return ret;
}

/*�ж�wait_lock�Ƿ���Ҫ�ڶ����еȴ�������*/
static ibool lock_table_has_to_wait_in_queue(lock_t* wait_lock)
{
	dict_table_t*	table;
	lock_t*			lock;

	ut_ad(lock_get_wait(wait_lock));

	table = wait_lock->un_member.tab_lock.table;

	lock = UT_LIST_GET_FIRST(table->locks);
	while(lock != wait_lock){
		if(lock_has_to_wait(wait_lock, lock))
			return TRUE;

		lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock);
	}

	return TRUE;
}

/*��in_lock�����Ƴ�����������Լ�������еı�������*/
void lock_table_dequeue(lock_t* in_lock)
{
	lock_t* lock;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad(lock_get_type(in_lock) == LOCK_TABLE);

	/*���in_lock��һ���������*/
	lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, in_lock);
	/*�Ƴ�in_lock*/
	lock_table_remove_low(in_lock);
	/*�������п��Լ������������*/
	while(lock != NULL){
		if(lock_get_wait(lock) && !lock_table_has_to_wait_in_queue(lock))
			lock_grant(lock);

		lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock);
	}
}

/*�ͷ�һ����������*/
void lock_table_unlock_auto_inc(trx_t* trx)
{
	if(trx->auto_inc_lock){
		mutex_enter(&kernel_mutex);

		lock_table_dequeue(trx->auto_inc_lock);

		mutex_exit(&kernel_mutex);
	}
}

/*�ͷ�һ�����������������*/
void lock_release_off_kernel(trx_id* trx)
{
	ulint	count;
	lock_t*	lock;

	ut_ad(mutex_own(&kernel_mutex));

	lock = UT_LIST_GET_LAST(trx->trx_locks);
	count = 0;
	while(lock != NULL){
		count ++;

		/*������һ����¼�л��߱���*/
		if(lock_get_type(lock) == LOCK_REC)
			lock_rec_dequeue_from_page(lock);
		else{
			ut_ad(lock_get_type(lock) == LOCK_TABLE);
			lock_table_dequeue(lock);
		}

		/*�ͷ�һ��kernel latch,�Ա������߳̽��в�������ֹlock_release_off_kernel������ʱ�����*/
		if(count == LOCK_RELEASE_KERNEL_INTERVAL){
			lock_mutex_exit_kernel();
			lock_mutex_enter_kernel();
			count = 0;
		}

		lock = UT_LIST_GET_LAST(trx->trx_locks);
	}

	/*�ͷ������Ӧ��lock�����*/
	mem_heap_empty(trx->lock_heap);

	ut_a(trx->auto_inc_lock == NULL);
}

/*ȡ��lock����������һ����Ӧ��lock*/
void lock_cancel_waiting_and_release(lock_t* lock)
{
	ut_ad(mutex_own(&kernel_mutex));

	/*����ȴ�����������*/
	if(lock_get_type(lock) == LOCK_REC)
		lock_rec_dequeue_from_page(lock);
	else{
		ut_ad(lock_get_type(lock) == LOCK_TABLE);
		lock_table_dequeue(lock);
	}

	/*ȡ��lock�ĵȴ�λ*/
	lock_reset_lock_and_trx_wait(lock);
	/*lock���������ִ��*/
	trx_end_lock_wait(lock->trx);
}

/*��λ����trx���е�������Ҫ�Ǵӵȴ�������ɾ��*/
static void lock_reset_all_on_table_for_trx(dict_table_t* table, trx_t* trx)
{
	lock_t*	lock;
	lock_t*	prev_lock;

	ut_ad(mutex_own(&kernel_mutex));

	lock = UT_LIST_GET_LAST(trx->trx_locks);
	while(lock != NULL){
		prev_lock = UT_LIST_GET_PREV(trx_locks, lock);

		if(lock_get_type(lock) == LOCK_REC && lock->index->table == table){
			ut_a(!lock_get_wait(lock));
			lock_rec_discard(lock);
		}
		else if(lock_get_type(lock) == LOCK_TABLE && lock->un_member.tab_lock.table == table){
			ut_a(!lock_get_wait(lock));
			lock_table_remove_low(lock);
		}

		lock = prev_lock;
	}
}

/*��λtable�ı�������*/
void lock_reset_all_on_table(dict_table_t* table)
{
	lock_t* lock;

	mutex_enter(&kernel_mutex);

	lock = UT_LIST_GET_FIRST(table->locks);

	while(lock){
		ut_a(!lock_get_wait(lock));

		/*��λlock->trx�ı�������*/
		lock_reset_all_on_table_for_trx(table, lock->trx);

		lock = UT_LIST_GET_FIRST(table->locks);
	}

	mutex_exit(&kernel_mutex);
}

/**********************VALIDATION AND DEBUGGING *************************/
void
lock_table_print(char* buf, lock_t* lock)
{
	ut_ad(mutex_own(&kernel_mutex));
	ut_a(lock_get_type(lock) == LOCK_TABLE);

	buf += sprintf(buf, "TABLE LOCK table %s trx id %lu %lu",
		lock->un_member.tab_lock.table->name, (lock->trx)->id.high, (lock->trx)->id.low);

	if (lock_get_mode(lock) == LOCK_S)
		buf += sprintf(buf, " lock mode S");
	else if (lock_get_mode(lock) == LOCK_X)
		buf += sprintf(buf, " lock_mode X");
	else if (lock_get_mode(lock) == LOCK_IS)
		buf += sprintf(buf, " lock_mode IS");
	else if (lock_get_mode(lock) == LOCK_IX)
		buf += sprintf(buf, " lock_mode IX");
	else if (lock_get_mode(lock) == LOCK_AUTO_INC)
		buf += sprintf(buf, " lock_mode AUTO-INC");
	else
		buf += sprintf(buf," unknown lock_mode %lu", lock_get_mode(lock));

	if (lock_get_wait(lock))
		buf += sprintf(buf, " waiting");

	buf += sprintf(buf, "\n");
}

void
lock_rec_print(char* buf,lock_t* lock)
{
	page_t*	page;
	ulint	space;
	ulint	page_no;
	ulint	i;
	ulint	count	= 0;
	char*	buf_start	= buf;
	mtr_t	mtr;

	ut_ad(mutex_own(&kernel_mutex));
	ut_a(lock_get_type(lock) == LOCK_REC);

	space = lock->un_member.rec_lock.space;
 	page_no = lock->un_member.rec_lock.page_no;

	buf += sprintf(buf, "RECORD LOCKS space id %lu page no %lu n bits %lu",
		    space, page_no, lock_rec_get_n_bits(lock));

	buf += sprintf(buf, " table %s index %s trx id %lu %lu",
		lock->index->table->name, lock->index->name,
		(lock->trx)->id.high, (lock->trx)->id.low);

	if (lock_get_mode(lock) == LOCK_S) {
		buf += sprintf(buf, " lock mode S");
	} else if (lock_get_mode(lock) == LOCK_X) {
		buf += sprintf(buf, " lock_mode X");
	} else {
		ut_error;
	}

	if (lock_rec_get_gap(lock)) {
		buf += sprintf(buf, " gap type lock");
	}

	if (lock_get_wait(lock)) {
		buf += sprintf(buf, " waiting");
	}

	mtr_start(&mtr);

	buf += sprintf(buf, "\n");

	/* If the page is not in the buffer pool, we cannot load it
	because we have the kernel mutex and ibuf operations would
	break the latching order */
	
	page = buf_page_get_gen(space, page_no, RW_NO_LATCH, NULL, BUF_GET_IF_IN_POOL, IB__FILE__, __LINE__, &mtr);
	if (page) {
		page = buf_page_get_nowait(space, page_no, RW_S_LATCH, &mtr);
	}
				
	if (page) {
		buf_page_dbg_add_level(page, SYNC_NO_ORDER_CHECK);
	}

	for (i = 0; i < lock_rec_get_n_bits(lock); i++) {
		if (buf - buf_start > 300) {
			buf += sprintf(buf,"Suppressing further record lock prints for this page\n");
			mtr_commit(&mtr);

			return;
		}
	
		if (lock_rec_get_nth_bit(lock, i)) {
			buf += sprintf(buf, "Record lock, heap no %lu ", i);

			if (page) {
				buf += rec_sprintf(buf, 120, page_find_rec_with_heap_no(page, i));
				*buf = '\0';
			}

			buf += sprintf(buf, "\n");
			count++;
		}
	}

	mtr_commit(&mtr);
}

/*ͳ�Ƽ�¼�����ĸ���*/
static ulint lock_get_n_rec_locks()
{
	lock_t*	lock;
	ulint n_locks = 0;
	ulint i;

	ut_ad(mutex_own(&kernel_mutex));

	for(i = 0; i < hash_get_n_cells(lock_sys->rec_hash); i ++){
		lock = HASH_GET_FIRST(lock_sys->rec_hash, i);
		while(lock){
			n_locks ++;
			lock = HASH_GET_NEXT(hash, lock);
		}
	}

	return n_locks;
}

void lock_print_info(char*	buf, char*	buf_end)
{
	lock_t*	lock;
	trx_t*	trx;
	ulint	space;
	ulint	page_no;
	page_t*	page;
	ibool	load_page_first = TRUE;
	ulint	nth_trx		= 0;
	ulint	nth_lock	= 0;
	ulint	i;
	mtr_t	mtr;

	if (buf_end - buf < 600) {
		sprintf(buf, "... output truncated!\n");
		return;
	}

	buf += sprintf(buf, "Trx id counter %lu %lu\n", 
		ut_dulint_get_high(trx_sys->max_trx_id),
		ut_dulint_get_low(trx_sys->max_trx_id));

	buf += sprintf(buf,
	"Purge done for trx's n:o < %lu %lu undo n:o < %lu %lu\n",
		ut_dulint_get_high(purge_sys->purge_trx_no),
		ut_dulint_get_low(purge_sys->purge_trx_no),
		ut_dulint_get_high(purge_sys->purge_undo_no),
		ut_dulint_get_low(purge_sys->purge_undo_no));
	
	lock_mutex_enter_kernel();

	buf += sprintf(buf,"Total number of lock structs in row lock hash table %lu\n", lock_get_n_rec_locks());
	if (lock_deadlock_found) {

		if ((ulint)(buf_end - buf)
			< 100 + strlen(lock_latest_err_buf)) {

			lock_mutex_exit_kernel();
			sprintf(buf, "... output truncated!\n");

			return;
		}

		buf += sprintf(buf, "%s", lock_latest_err_buf);
	}

	if (buf_end - buf < 600) {
		lock_mutex_exit_kernel();
		sprintf(buf, "... output truncated!\n");

		return;
	}

	buf += sprintf(buf, "LIST OF TRANSACTIONS FOR EACH SESSION:\n");

	/* First print info on non-active transactions */

	trx = UT_LIST_GET_FIRST(trx_sys->mysql_trx_list);

	while (trx) {
		if (buf_end - buf < 900) {
			lock_mutex_exit_kernel();
			sprintf(buf, "... output truncated!\n");

			return;
		}

		if (trx->conc_state == TRX_NOT_STARTED) {
		    buf += sprintf(buf, "---");
			trx_print(buf, trx);

			buf += strlen(buf);
		}
			
		trx = UT_LIST_GET_NEXT(mysql_trx_list, trx);
	}

loop:
	trx = UT_LIST_GET_FIRST(trx_sys->trx_list);

	i = 0;

	/* Since we temporarily release the kernel mutex when
	reading a database page in below, variable trx may be
	obsolete now and we must loop through the trx list to
	get probably the same trx, or some other trx. */
	
	while (trx && (i < nth_trx)) {
		trx = UT_LIST_GET_NEXT(trx_list, trx);
		i++;
	}

	if (trx == NULL) {
		lock_mutex_exit_kernel();
		return;
	}

	if (buf_end - buf < 900) {
		lock_mutex_exit_kernel();
		sprintf(buf, "... output truncated!\n");

		return;
	}

	if (nth_lock == 0) {
	        buf += sprintf(buf, "---");
		trx_print(buf, trx);

		buf += strlen(buf);
		
		if (buf_end - buf < 500) {
			lock_mutex_exit_kernel();
			sprintf(buf, "... output truncated!\n");

			return;
		}
		
	        if (trx->read_view) {
	  	        buf += sprintf(buf,
       "Trx read view will not see trx with id >= %lu %lu, sees < %lu %lu\n",
		       	ut_dulint_get_high(trx->read_view->low_limit_id),
       			ut_dulint_get_low(trx->read_view->low_limit_id),
       			ut_dulint_get_high(trx->read_view->up_limit_id),
       			ut_dulint_get_low(trx->read_view->up_limit_id));
	        }

		if (trx->que_state == TRX_QUE_LOCK_WAIT) {
			buf += sprintf(buf,
 "------- TRX HAS BEEN WAITING %lu SEC FOR THIS LOCK TO BE GRANTED:\n",
		   (ulint)difftime(time(NULL), trx->wait_started));

			if (lock_get_type(trx->wait_lock) == LOCK_REC) {
				lock_rec_print(buf, trx->wait_lock);
			} else {
				lock_table_print(buf, trx->wait_lock);
			}

			buf += strlen(buf);
			buf += sprintf(buf,
			"------------------\n");
		}
	}

	if (!srv_print_innodb_lock_monitor) {
	  	nth_trx++;
	  	goto loop;
	}

	i = 0;

	/* Look at the note about the trx loop above why we loop here:
	lock may be an obsolete pointer now. */
	
	lock = UT_LIST_GET_FIRST(trx->trx_locks);
		
	while (lock && (i < nth_lock)) {
		lock = UT_LIST_GET_NEXT(trx_locks, lock);
		i++;
	}

	if (lock == NULL) {
		nth_trx++;
		nth_lock = 0;

		goto loop;
	}

	if (buf_end - buf < 500) {
		lock_mutex_exit_kernel();
		sprintf(buf, "... output truncated!\n");

		return;
	}

	if (lock_get_type(lock) == LOCK_REC) {
		space = lock->un_member.rec_lock.space;
 		page_no = lock->un_member.rec_lock.page_no;

 		if (load_page_first) {
			lock_mutex_exit_kernel();

			mtr_start(&mtr);
			
			page = buf_page_get_with_no_latch(space, page_no, &mtr);

			mtr_commit(&mtr);

			load_page_first = FALSE;

			lock_mutex_enter_kernel();

			goto loop;
		}
		
		lock_rec_print(buf, lock);
	} else {
		ut_ad(lock_get_type(lock) == LOCK_TABLE);
		lock_table_print(buf, lock);
	}

	buf += strlen(buf);
	
	load_page_first = TRUE;

	nth_lock++;

	if (nth_lock >= 10) {
		buf += sprintf(buf, "10 LOCKS PRINTED FOR THIS TRX: SUPPRESSING FURTHER PRINTS\n");
	
		nth_trx++;
		nth_lock = 0;

		goto loop;
	}

	goto loop;
}

/*�ж�һ�������ĺϷ���*/
ibool lock_table_queue_validate(dict_table_t* table)
{
	lock_t*	lock;
	ibool	is_waiting;

	ut_ad(mutex_own(&kernel_mutex));

	is_waiting = FALSE;

	lock = UT_LIST_GET_FIRST(table->locks);

	while (lock) {
		ut_a(((lock->trx)->conc_state == TRX_ACTIVE) || ((lock->trx)->conc_state == TRX_COMMITTED_IN_MEMORY));

		if (!lock_get_wait(lock)) { /*��������ִ��Ȩ*/
			ut_a(!is_waiting);
			ut_a(!lock_table_other_has_incompatible(lock->trx, 0, table, lock_get_mode(lock)));
		} else {
			is_waiting = TRUE;
			ut_a(lock_table_has_to_wait_in_queue(lock));
		}

		lock = UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock);
	}

	return(TRUE);
}

ibool lock_rec_queue_validate(rec_t* rec, dict_index_t* index)	
{
	trx_t*	impl_trx;	
	lock_t*	lock;
	ibool	is_waiting;
	
	ut_a(rec);

	lock_mutex_enter_kernel();

	/*supremum/infimum�������ж�*/
	if (page_rec_is_supremum(rec) || page_rec_is_infimum(rec)) {
		lock = lock_rec_get_first(rec);

		while (lock) {
			ut_a(lock->trx->conc_state == TRX_ACTIVE || lock->trx->conc_state == TRX_COMMITTED_IN_MEMORY);
			ut_a(trx_in_trx_list(lock->trx));
			
			/*������LOCK_WAIT״̬�£������ڵȴ�������*/
			if (lock_get_wait(lock))
				ut_a(lock_rec_has_to_wait_in_queue(lock));

			/*lock��index������index*/
			if (index)
				ut_a(lock->index == index);

			lock = lock_rec_get_next(rec, lock);
		}

		lock_mutex_exit_kernel();

	    return(TRUE);
	}

	if (index && (index->type & DICT_CLUSTERED)) {
		impl_trx = lock_clust_rec_some_has_impl(rec, index);

		if (impl_trx && lock_rec_other_has_expl_req(LOCK_S, 0, LOCK_WAIT, rec, impl_trx))
			ut_a(lock_rec_has_expl(LOCK_X, rec, impl_trx));
	}

	if (index && !(index->type & DICT_CLUSTERED)) {
		
		/* The kernel mutex may get released temporarily in the
		next function call: we have to release lock table mutex
		to obey the latching order */
		
		impl_trx = lock_sec_rec_some_has_impl_off_kernel(rec, index);

		if (impl_trx && lock_rec_other_has_expl_req(LOCK_S, 0,
						LOCK_WAIT, rec, impl_trx)) {

			ut_a(lock_rec_has_expl(LOCK_X, rec, impl_trx));
		}
	}

	is_waiting = FALSE;

	lock = lock_rec_get_first(rec);

	while (lock) {
		ut_a(lock->trx->conc_state == TRX_ACTIVE
		     || lock->trx->conc_state == TRX_COMMITTED_IN_MEMORY);
		ut_a(trx_in_trx_list(lock->trx));
	
		if (index) {
			ut_a(lock->index == index);
		}

		if (!lock_rec_get_gap(lock) && !lock_get_wait(lock)) {

			ut_a(!is_waiting);
		
			if (lock_get_mode(lock) == LOCK_S) {
				ut_a(!lock_rec_other_has_expl_req(LOCK_X,0, 0, rec, lock->trx));
			} else {
				ut_a(!lock_rec_other_has_expl_req(LOCK_S,0, 0, rec, lock->trx));
			}

		} else if (lock_get_wait(lock) && !lock_rec_get_gap(lock)) {
			is_waiting = TRUE;
			ut_a(lock_rec_has_to_wait_in_queue(lock));
		}

		lock = lock_rec_get_next(rec, lock);
	}

	lock_mutex_exit_kernel();

	return(TRUE);
}

ibool lock_rec_validate_page(ulint space, ulint	page_no)
{
	dict_index_t*	index;
	page_t*	page;
	lock_t*	lock;
	rec_t*	rec;
	ulint	nth_lock	= 0;
	ulint	nth_bit		= 0;
	ulint	i;
	mtr_t	mtr;

	ut_ad(!mutex_own(&kernel_mutex));

	mtr_start(&mtr);

	page = buf_page_get(space, page_no, RW_X_LATCH, &mtr);
	buf_page_dbg_add_level(page, SYNC_NO_ORDER_CHECK);

	lock_mutex_enter_kernel();

loop:	
	lock = lock_rec_get_first_on_page_addr(space, page_no);
	if (lock == NULL)
		goto function_exit;

	for (i = 0; i < nth_lock; i++) {
		lock = lock_rec_get_next_on_page(lock);

		if (!lock) 
			goto function_exit;
	}

	ut_a(trx_in_trx_list(lock->trx));
	ut_a(lock->trx->conc_state == TRX_ACTIVE
		|| lock->trx->conc_state == TRX_COMMITTED_IN_MEMORY);

	for (i = nth_bit; i < lock_rec_get_n_bits(lock); i++) {
		if (i == 1 || lock_rec_get_nth_bit(lock, i)) {

			index = lock->index;
			rec = page_find_rec_with_heap_no(page, i);

			printf("Validating %lu %lu\n", space, page_no);

			lock_mutex_exit_kernel();

			lock_rec_queue_validate(rec, index);

			lock_mutex_enter_kernel();

			nth_bit = i + 1;

			goto loop;
		}
	}

	nth_bit = 0;
	nth_lock++;

	goto loop;

function_exit:
	lock_mutex_exit_kernel();
	mtr_commit(&mtr);

	return(TRUE);
}

/*��������������У��*/
ibool lock_validate(void)
{
	lock_t*	lock;
	trx_t*	trx;
	dulint	limit;
	ulint	space;
	ulint	page_no;
	ulint	i;

	lock_mutex_enter_kernel();

	trx = UT_LIST_GET_FIRST(trx_sys->trx_list);

	while (trx) {
		lock = UT_LIST_GET_FIRST(trx->trx_locks);

		while (lock) {
			if (lock_get_type(lock) == LOCK_TABLE)
				lock_table_queue_validate(lock->un_member.tab_lock.table);

			lock = UT_LIST_GET_NEXT(trx_locks, lock);
		}

		trx = UT_LIST_GET_NEXT(trx_list, trx);
	}

	for (i = 0; i < hash_get_n_cells(lock_sys->rec_hash); i++) {

		limit = ut_dulint_zero;

		for (;;) {
			lock = HASH_GET_FIRST(lock_sys->rec_hash, i);

			while (lock) {
				ut_a(trx_in_trx_list(lock->trx));

				space = lock->un_member.rec_lock.space;
				page_no = lock->un_member.rec_lock.page_no;

				if (ut_dulint_cmp(ut_dulint_create(space, page_no),limit) >= 0) {
					break;
				}

				lock = HASH_GET_NEXT(hash, lock);
			}

			if (!lock) {
				break;
			}

			lock_mutex_exit_kernel();

			lock_rec_validate_page(space, page_no);

			lock_mutex_enter_kernel();

			limit = ut_dulint_create(space, page_no + 1);
		}
	}

	lock_mutex_exit_kernel();

	return(TRUE);
}

ulint lock_rec_insert_check_and_lock(ulint flags, rec_t* rec, dict_index_t* index, que_thr_t* thr, ibool inherit)
{
	rec_t*	next_rec;
	trx_t*	trx;
	lock_t*	lock;
	ulint	err;

	if(flags && BTR_NO_LOCKING_FLAG)
		return DB_SUCCESS;

	ut_ad(rec);

	trx = thr_get_trx(thr);
	next_rec = page_rec_get_next(rec);

	*thr_get_trx = FALSE;

	lock_mutex_enter_kernel();

	ut_ad(lock_table_has(thr_get_trx(thr), index->table, LOCK_IX));

	lock = lock_rec_get_first(next_rec);
	if(lock == NULL){ /*�м�¼���Բ���*/
		lock_mutex_exit_kernel();
		
		/*����page���ִ�е�����ID*/
		if(!(index->type) & DICT_CLUSTERED)
			page_update_max_trx_id(buf_frame_align(rec), thr_get_trx(thr)->id);

		return DB_SUCCESS;
	}

	*inherit = TRUE;

	/*next_rec���б�LOCK_S���ϸ�����������Ҳ���trx�����,insert�����������һ��LOCK_X��֧�ֲ������*/
	if(lock_rec_other_has_expl_req(LOCK_S, LOCK_GAP, LOCK_WAIT, next_rec, trx))
		err = lock_rec_enqueue_waiting(LOCK_X | LOCK_GAP, next_rec, index, thr); /*����һ��LOCK_X��ռ�������в���*/
	else
		err = DB_SUCCESS;

	lock_mutex_exit_kernel();

	/*�����������page������ID*/
	if(!(index->type & DICT_CLUSTERED) && (err == DB_SUCCESS)){
		page_update_max_trx_id(buf_frame_align(rec), thr_get_trx(thr)->id);
	}

	ut_ad(lock_rec_queue_validate(next_rec, index));

	return err;
}

/*����ʽ��ת����һ����ʾ��LOCK_X*/
static void lock_rec_convert_impl_to_expl(rec_t* rec, dict_index_t* index)
{
	trx_t*	impl_trx;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad(page_rec_is_user_rec(rec));

	if(index->type & DICT_CLUSTERED) /*�ۼ������ϵ���ʽ���������ȡ*/
		impl_trx = lock_clust_rec_some_has_impl(rec, index);
	else /*���������ϵ���ʽ�����������ȡ��������̷ǳ����ӣ�����*/
		impl_trx = lock_sec_rec_some_has_impl_off_kernel(rec, index);

	if(impl_trx){
		if(lock_rec_has_expl(LOCK_X, rec, impl_trx) == NULL) /*impl_trxû��һ��rec��¼������LOCK_X���ϸ�*/
			lock_rec_add_to_queue(LOCK_REC | LOCK_X, rec, index, impl_trx); /*����һ��LOCK_X���������*/
	}
}

/*����һ��������Ҫ�Լ�¼REC�����޸ģ�����������Ƿ�����ʽ���������ת������ʾ�������ҳ��Ի����Ȩִ������*/
ulint lock_clust_rec_modify_check_and_lock(ulint flags, rec_t* rec, dict_index_t* index, que_thr_t* thr)
{
	trx_t*	trx;
	ulint	err;

	if(flags & BTR_NO_LOCKING_FLAG)
		return DB_SUCCESS;

	ut_ad(index->type & DICT_CLUSTERED);

	trx = thr_get_trx(thr);

	lock_mutex_enter_kernel();
	ut_ad(lock_table_has(thr_get_trx(thr), index->table, LOCK_IX));
	/*����ۼ������ϴ�����ʽ����ת������ʾ��*/
	lock_rec_convert_impl_to_expl(rec, index);

	/*���Ի��thr��Ӧִ�������rec������LOCK_Xִ��Ȩ*/
	err = lock_rec_lock(TRUE, LOCK_X, rec, index, thr);

	lock_mutex_exit_kernel();

	ut_ad(lock_rec_queue_validate(rec, index));

	return err;
}

/*ͨ�����������޸ļ�¼�У�����һ���ȴ���LOCK_X��������ɹ�������PAGE�Ĳ���trx_id*/
ulint lock_sec_rec_modify_check_and_lock(ulint flags, rec_t* rec, dict_index_t* index, que_thr_t* thr)
{
	if(flags & BTR_NO_LOCKING_FLAG)
		return DB_SUCCESS;

	ut_ad(!(index->type & DICT_CLUSTERED));

	lock_mutex_enter_kernel();
	
	ut_ad(lock_table_has(thr_get_trx(thr), index->table, LOCK_IX));
	/*��rec���ϼ���һ��lock_x����ִ��Ȩ*/
	err = lock_rec_lock(TRUE, LOCK_X, rec, index, thr);

	lock_mutex_exit_kernel();

	ut_ad(lock_rec_queue_validate(rec, index));

	if(err == DB_SUCCESS)
		page_update_max_trx_id(buf_frame_algin(rec), thr_get_trx(thr)->id)
}

/*ͨ������������ȡ��¼�У������¼�Ķ�����������һ����ʽ����ת������ʾ����LOCK_X��,�����Ի���������ִ��Ȩ*/
ulint lock_sec_rec_read_check_and_lock(ulint flags, rec_t* rec, dict_index_t* index, ulint mode, que_thr_t* thr)
{
	ulint err;

	ut_ad(!(index->type & DICT_CLUSTERED));
	ut_ad(page_rec_is_user_rec(rec) || page_rec_is_supremum(rec));

	if(flags & BTR_NO_LOCKING_FLAG)
		return DB_SUCCESS;

	lock_mutex_enter_kernel();

	ut_ad(mode != LOCK_X || lock_table_has(thr_get_trx(thr), index->table, LOCK_IX));
	ut_ad(mode != LOCK_S || lock_table_has(thr_get_trx(thr), index->table, LOCK_IS));

	if((ut_dulint_cmp(page_get_max_trx_id(buf_frame_align(rec)), trx_list_get_min_trx_id()) >= 0|| recv_recovery_is_on()) 
		&& !page_rec_is_supremum(rec))
		lock_rec_convert_impl_to_expl(rec, index); /*��rec��¼�ϼ���һ��LOCK_X����*/

	err = lock_rec_lock(FALSE, mode, rec, index, thr);

	lock_mutex_exit_kernel();

	ut_ad(lock_rec_queue_validate(rec, index));

	return err;
}

ulint lock_clust_rec_read_check_and_lock(ulint flags, rec_t* rec, dict_index_t* index, ulint mode, que_thr_t* thr)
{
	ulint	err;

	ut_ad(index->type & DICT_CLUSTERED);
	ut_ad(page_rec_is_user_rec(rec) || page_rec_is_supremum(rec));

	if(flags & BTR_NO_LOCKING_FLAG)
		return DB_SUCCESS;

	lock_mutex_enter_kernel();

	ut_ad(mode != LOCK_X || lock_table_has(thr_get_trx(thr), index->table, LOCK_IX));
	ut_ad(mode != LOCK_S || lock_table_has(thr_get_trx(thr), index->table, LOCK_IS));

	if(!page_rec_is_supremum(rec))
		lock_rec_convert_impl_to_expl(rec, index); /*�ڼ�¼���ϼ���һ��LOCK_X��*/

	err = lock_rec_lock(FALSE, mode, rec, index, thr);

	lock_mutex_exit_kernel();

	ut_ad(lock_rec_queue_validate(rec, index));

	return err;
}

/************************************************************************/


