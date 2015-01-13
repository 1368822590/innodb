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
	ulint		n_bits;
}lock_rec_t;

struct lock_struct
{
	trx_t*			trx;
	ulint			type_mode;
	hash_node_t		hash;
	dict_index_t*	index;
	UT_LIST_NODE_T(lock_t) locks;
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

/*��ȡҲ�õ��м�¼����һ����*/
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

/*����м�¼����һ����ʽ��*/
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


/************************************************************************/


