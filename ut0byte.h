#ifndef ut0byte_h
#define ut0byte_h

/*�����ĳ�����mySQL��صĳ���*/
#include "univ.h"

/*����һ��64λ��������*/
UNIV_INLINE ib_uint64_t ut_ull_create(ulint high, ulint low) __attribute__((const));

/*С��n�����algin_no�ı�����algin_no������2�Ĵη���������ut_uint64_algin_down��11��2�� = 10*/
UNIV_INLINE ib_uint64_t ut_uint64_algin_down(ib_uint64_t n, ulint algin_no);

/*����n����Сalgin_no�ı�����algin_no������2�Ĵη���������ut_uint64_algin_down��11��2�� = 12*/
UNIV_INLINE ib_uint64_t ut_uint64_algin_up(ib_uint64_t n, ulint algin_no);

/*��ַ���룬��ut_uint64_algin_up����*/
UNIV_INLINE void* ut_align(const void* ptr, ulint align_no);

/*��ַ���룬��ut_uint64_algin_down����*/
UNIV_INLINE void* ut_align_down(const void* ptr, ulint align_no) __attribute__(const);

/*���align_noΪ��λ��ַƫ����*/
UNIV_INLINE ulint ut_align_offset(const void* ptr, ulint align_no) __attribute__(const);

/*�ж�a�ĵ�nλ���Ƿ�Ϊ1*/
UNIV_INLINE ibool ut_bit_get_nth(ulint a, ulint n);

/*��a�ĵ�nλ����Ϊ0 ����1*/
UNIV_INLINE ulint ut_bit_set_nth(ulint a, ulint b, ibool val);
#endif





