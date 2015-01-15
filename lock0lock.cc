#include "lock0lock.h"
#include "usr0sess.h"
#include "trx0purge.h"

#define LOCK_MAX_N_STEPS_IN_DEADLOCK_CHECK	1000000

#define LOCK_RELEASE_KERNEL_INTERVAL		1000

#define LOCK_PAGE_BITMAP_MARGIN				64


ibool lock_print_waits = FALSE;
/*������ϵͳ���*/
lock_sys_t* lock_sys = NULL;

/*��������������*/
typedef struct lock_table_struct
{
	dict_table_t* table;			/*�ֵ�Ԫ�����еı������*/
	UT_LIST_NODE_T(lock_t) locks;	/*�ڱ��ϵ��������б�*/
}lock_table_t;

typedef struct lock_rec_struct
{
	ulint		space;		/*��¼������space��ID*/
	ulint		page_no;	/*��¼������pageҳ��*/
	ulint		n_bits;		/*������bitmapλ����lock_t�ṹ������һ��BUF������Ϊn_bits / 8*/
}lock_rec_t;

/*������*/
struct lock_struct
{
	trx_t*			trx;
	ulint			type_mode;
	hash_node_t		hash;
	dict_index_t*	index;
	UT_LIST_NODE_T(lock_t) trx_locks;
	union{
		lock_table_t	tab_lock;/*����*/
		lock_rec_t		rec_lock;/*����*/
	}un_member;
};

/*������ʶ*/
ibool lock_deadlock_found = FALSE;
/*������Ϣ��������5000�ֽ�*/
char* lock_latest_err_buf;


static ibool		lock_deadlock_occurs(lock_t* lock, trx_t* trx);
static ibool		lock_deadlock_recursive(trx_t* start, trx_t* trx, lock_t* wait_lock, ulint cost);

/************************************************************************/
/*kernel_mutex����srv0srv.h�����ȫ���ں���*/
UNIV_INLINE void lock_mutex_enter_kernel()
{
	mutex_enter(&kernel_mutex);
}

UNIV_INLINE void lock_mutex_exit_kernel()
{
	mutex_exit(&kernel_mutex);
}

/*����¼�Ƿ���Խ���һ���Զ�*/
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

/*���Ǿۺ�������¼�Ƿ���Խ���һ���Զ�*/
ulint  lock_sec_rec_cons_read_sees(rec_t* rec, dict_index_t* index, read_view_t* view)
{
	dulint	max_trx_id;

	ut_ad(index->type & DICT_CLUSTERED);
	ut_ad(page_rec_is_user_rec(rec));

	if(recv_recovery_is_on()) /*���redo log�Ƿ��ڽ�����־�ָ�*/
		return FALSE;

	/*��ö�Ӧ��¼��max trx id*/
	max_trx_id = page_get_max_trx_id(buf_frame_align(rec));
	if(ut_dulint_cmp(max_trx_id, view->up_limit_id) >= 0)
		return FALSE;

	return TRUE;
}

/*����һ��ϵͳ������*/
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

/*�����������ģʽ(IS, IX, S, X, NONE)*/
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

/*������Ƿ��ڵȴ�״̬*/
UNIV_INLINE ibool lock_get_wait(lock_t* lock)
{
	ut_ad(lock);
	if(lock->type_mode & LOCK_WAIT) /*�����ڵȴ�״̬*/
		return TRUE;

	return FALSE;
}

/*�����������ĵȴ�*/
UNIV_INLINE void lock_set_lock_and_trx_wait(lock_t* lock, trx_t* trx)
{
	ut_ad(lock);
	ut_ad(trx->wait_lock == NULL);

	trx->wait_lock = lock;
	lock->type_mode = lock->type_mode | LOCK_WAIT;
}

/*��λ�������ĵȴ�״̬*/
UNIV_INLINE void lock_reset_lock_and_trx_wait(lock_t* lock)
{
	ut_ad((lock->trx)->wait_lock == lock);
	ut_ad(lock_get_wait(lock));

	(lock->trx)->wait_lock = NULL;
	lock->type_mode = lock->type_mode & ~LOCK_WAIT;
}

/*����������ļ�¼��Χ����״̬*/
UNIV_INLINE ibool lock_rec_get_gap(lock_t* lock)
{
	ut_ad(lock);
	ut_ad(lock_get_type(lock) == LOCK_REC);

	if(lock->type_mode & LOCK_GAP)
		return TRUE;

	return FALSE;
}

/*�����������ļ�¼��Χ����״̬*/
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

/*�ж���������mode1�Ƿ��mode2�����Ƶ��ϣ�һ��LOCK_X > LOCK_S > LOCK_IX > LOCK_IS*/
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

/*����mode == LOCK_S,����LOCK_X*/
UNIV_INLINE ulint lock_get_confl_mode(ulint mode)
{
	ut_ad(mode == LOCK_X || mode == LOCK_S);
	if(mode == LOCK_S)
		return LOCK_X;
	
	return LOCK_S;
}

/*�ж�lock1�Ƿ��lock2����*/
UNIV_INLINE ibool lock_has_to_wait(lock_t* lock1, lock_t* lock2)
{
	if(lock1->trx != lock2->trx && !lock_mode_compatible(lock_get_mode(lock1), lock_get_mode(lock2)))
		return TRUE;
	return FALSE;
}

/*��ü�¼������bits*/
UNIV_INLINE ulint lock_rec_get_n_bits(lock_t* lock)
{
	return lock->un_member.rec_lock.n_bits;
}

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

/*���rec lock bitmap��һ����Чλ�����*/
static ulint lock_rec_find_set_bit(lock_t* lock)
{
	ulint i;
	for(i = 0; i < lock_rec_get_n_bits(lock); i ++){
		if(lock_rec_get_nth_bit(lock, i))
			return i;
	}

	return ULINT_UNDEFINED;
}

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

/*��ȡҲ���м�¼����һ����,��ͬһ��page��*/
UNIV_INLINE lock_t* lock_rec_get_next_on_page(lock_t* lock)
{
	ulint	space;
	ulint	page_no;

	ut_ad(mutex_own(&kernel_mutex));

	space = lock->un_member.rec_lock.space;
	page_no = lock->un_member.rec_lock.page_no;

	for(;;){
		lock = HASH_GET_NEXT(hash, lock);
		if(lock == NULL)
			break;

		/*���ҵ��˶�Ӧ��*/
		if(lock->un_member.rec_lock.space == space && lock->un_member.rec_lock.page_no = page_no)
			break;
	}

	return lock;
}

/*���page�ĵ�һ����*/
UNIV_INLINE lock_t* lock_rec_get_first_on_page_addr(ulint space, ulint page_no)
{
	lock_t* lock;

	ut_ad(mutex_own(&kernel_mutex));

	lock = HASH_GET_FIRST(lock_sys->rec_hash, lock_rec_hash(space, page_no));
	while(lock){
		if ((lock->un_member.rec_lock.space == space) 
			&& (lock->un_member.rec_lock.page_no == page_no))
			break;

		lock = HASH_GET_NEXT(hash, lock);
	}

	return lock;
}

/*�ж�space page_no��Ӧ��ҳ�Ƿ�����*/
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
		space = buf_frame_get_space_id(ptr);
		page_no = buf_frame_get_page_no(ptr);

		if(space == lock->un_member.rec_lock.space && page_no == lock->un_member.rec_lock.page_no)
			break;

		lock = HASH_GET_NEXT(hash, lock);
	}

	return lock;
}

/*����м�¼����һ����*/
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

/*���in_lock��ǰһ�����м�¼��*/
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

/*�ж�����trx�Ƿ����table������mode���ϸ�*/
UNIV_INLINE lock_t* lock_table_has(trx_t* trx, dict_table_t* table, ulint mode)
{
	lock_t* lock;

	ut_ad(mutex_own(&kernel_mutex));

	/*�Ӻ���ɨ�赽ǰ��, �п��������Ѿ���������*/
	lock = UT_LIST_GET_LAST(table->locks);
	while(lock != NULL){
		if(lock->trx == trx && lock_mode_stronger_or_eq(lock_get_mode(lock), mode)){
			ut_ad(!lock_get_wait(lock));
			return lock;
		}

		lock = UT_LIST_GET_PREV(un_member.tab_lock.locks, lock);
	}
}

/*���һ����mode���ϸ��rec�м�¼���������������trx����ģ����Ҵ���non_gap״̬*/
UNIV_INLINE lock_t* lock_rec_has_expl(ulint mode, rec_t* rec, trx_t* trx)
{
	lock_t* lock;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad((mode == LOCK_X) || (mode == LOCK_S));

	lock = lock_rec_get_first(rec);
	while(lock != NULL){
		if(lock->trx == trx && lock_mode_stronger_or_eq(lock_get_mode(lock), mode)
			&& !lock_get_wait(lock) && !lock_rec_get_gap(lock) || page_rec_is_supremum(rec))
			return lock;

		lock = lock_rec_get_next(rec, lock);
	}
}

/*����Ƿ��trx����������г��б�mode���ϸ������*/
UNIV_INLINE lock_t* lock_rec_other_has_expl_req(ulint mode, ulint gap, ulint wait, rec_t* rec, trx_t* trx)
{
	lock_t* lock;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad((mode == LOCK_X) || (mode == LOCK_S));

	lock = lock_rec_get_first(rec);
	while(lock != NULL){
		if(lock->trx != trx && (gap || !(lock_rec_get_gap(lock) || page_rec_is_supremum(rec))
			&& (wait || !lock_get_wait(lock)) && lock_mode_stronger_or_eq(lock_get_mode(lock), mode)))
			return lock;

		lock = lock_rec_get_next(rec, lock);
	}

	return NULL;
}

/*�ڼ�¼���ڵ�page�У�����trx�������type_modeģʽ�ĵ���*/
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

/*�����ڼ�¼rec�Ķ��������ϴ洢LOCK_IX,���ط��������������trx_t*/
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

	/*����trx������ģʽΪtype_mode�ļ�¼��������������rec������page�е���*/
	similar_lock = lock_rec_find_similar_on_page(type_mode, rec, trx);

	/*��������similar_lock*/
	if(similar_lock != NULL && !somebody_waits && !(type_mode & LOCK_WAIT)){
		lock_rec_set_nth_bit(similar_lock, heap_no);
		return similar_lock;
	}

	/*����һ���µ��������������Ŷ�*/
	return lock_rec_create(type_mode, rec, index, trx);
}

/*���ٻ�������Ŀ���Ȩ���󲿷����̶���������*/
UNIV_INLINE ibool lock_rec_lock_fast(ibool impl, ulint mode, rec_t* rec, dict_index_t* index, que_thr_t* thr)
{
	lock_t*	lock;
	ulint	heap_no;

	ut_ad(mutex_own(&kernel_mutex));
	ut_ad(mode == LOCK_X || mode == LOCK_S);

	heap_no = rec_get_heap_no(rec);
	lock = lock_rec_get_first_on_page(rec);
	if(lock == NULL){ /*page��û����������, ����һ��mode���͵���*/
		if(!impl)
			lock_rec_create(mode, rec, index, thr_get_trx(thr));
		return TRUE;
	}

	/*page���ж��LOCK*/
	if(lock_rec_get_next_on_page(lock))
		return FALSE;

	/*lock��������thr�е�trx����ͬ�����߲�������������lock�ļ�¼��rec����ͬ��ֱ�ӷ���*/
	if(lock->trx != thr_get_trx(thr) || lock->type_mode != (mode | LOCK_REC) 
		|| lock_rec_get_n_bits(lock) <= heap_no)
			return FALSE;

	/*����ֻ�и������������������ָ��ļ�¼rec,ֱ����Ϊ���Ի����Ȩ*/
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

	/*trx�б�mode�����ϸ����ģʽ����rec����*/
	if(lock_rec_has_expl(mode, rec, trx))
		err = DB_SUCCESS;
	else if(lock_rec_other_has_expl_req(confl_mode, 0, LOCK_WAIT, rec, trx)) /*���������и��ϸ������rec����*/
		err = lock_rec_enqueue_waiting(mode, rec, index, thr); /*����һ�����������еȴ�*/
	else{
		if(!impl) /*����һ�������������뵽��������*/
			lock_rec_add_to_queue(LOCK_REC | mode, rec, index, trx);
		err = DB_SUCCESS;
	}

	return err;
}

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

/*���wait_lock�Ƿ���ָ��ͬһ�м�¼���Ҳ����ݵ�����Ҳ�����ж�wait lock�Ƿ�Ҫ���еȴ�*/
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

/*������Ȩ*/
void lock_grant(lock_t* lock)
{
	ut_ad(mutex_own(&kernel_mutex));

	/*����lock wait��ʶ��λ*/
	lock_reset_lock_and_trx_wait(lock);

	/*������������ģʽ*/
	if(lock_get_mode(lock) == LOCK_AUTO_INC){
		if(lock->trx->auto_inc_lock != NULL)
			fprintf(stderr, "InnoDB: Error: trx already had an AUTO-INC lock!\n");
		
		lock->trx->auto_inc_lock = lock;
	}

	if(lock_print_waits)
		printf("Lock wait for trx %lu ends\n", ut_dulint_get_low(lock->trx->id));

	/*��������ȴ�*/
	trx_end_lock_wait(lock->trx);
}

/*ȡ�����ڵȴ�����*/
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

/*��in_lock��lock_sys��ɾ���� ���������Ӧҳ��һ���ȴ�����, in_lock������һ��waiting����granted״̬����*/
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

/*����page�������м�¼��*/
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

/*����ͬʱ����old page��page�е���������������Щ������page��Ӧ���м�¼��,���Ӷ�O(m * n)*/
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

/*��������root page��*/
void lock_update_root_raise(page_t* new_page, page_t* root)
{
	lock_mutex_enter_kernel();

	lock_rec_move(page_get_supremum_rec(new_page), page_get_supremum_rec(root));

	lock_mutex_exit_kernel();
}

void lock_update_copy_and_discard(page_t* new_page, page_t* page)
{
	lock_mutex_enter_kernel();

	/*��page����ȫ������new_page��supremum�ϣ��൱��GAP��Χ����*/
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

/*���heir�ļ�¼����������̳�rec�������heir��GAP��Χ�����൱��������*/
void lock_rec_reset_and_inherit_gap_locks(rec_t* heir, rec_t* rec)
{
	mutex_enter(&kernel_mutex);	      		

	lock_rec_reset_and_release_wait(heir);
	lock_rec_inherit_to_gap(heir, rec);

	mutex_exit(&kernel_mutex);	 
}

/*��page����������ת�Ƶ�heir��*/
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

	/*�ͷŵ�����page�ϵ����ȴ�*/
	lock_rec_free_all_from_discard_page(page);
}

/*��¼���ʱ�������̳�*/
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

/*��page��infimum��¼����ת�Ƶ�rec��¼��*/
void lock_rec_restore_from_page_infimum(rec_t* rec, page_t* page)
{
	lock_mutex_enter_kernel();

	lock_rec_move(rec, page_get_infimum_rec(page));

	lock_mutex_exit_kernel();
}

/************************************************************************/


