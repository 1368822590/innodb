#include "ut0bh.h"
#include "ut0mem.h"

#include <string.h>


UNIV_INLINE ulint ib_bh_size(const ib_bh_t* ib_bh)
{
	return ib_bh->n_elems;
}

UNIV_INLINE ibool ib_bh_is_empty(const ib_bh_t* ib_bh)
{
	return ib_bh_size(ib_bh) == 0;
}


UNIV_INLINE ibool ib_bh_is_full(const ib_bh_t* ib_bh)
{
	return (ib_bh->n_elems >= ib_bh->max_elems);
}

UNIV_INLINE void* ib_bh_get(ib_bh_t* ib_bh, ulint i)
{
	byte* ptr = (byte*)(ib_bh + 1);

	ut_a(i < ib_bh_size(ib_bh));

	return (ptr + (ib_bh->sizeof_elem * i));
}

UNIV_INLINE void* ib_bh_set(ib_bh_t* ib_bh, ulint i, const void* elem)
{
	void* ptr = ib_bh_get(ib_bh, i);
	ut_memcpy(ptr, elem, ib_bh->sizeof_elem);

	return ptr;
}

UNIV_INLINE void* ib_bh_first(ib_bh_t* ib_bh)
{
	return (ib_bh_is_empty(ib_bh) ? NULL : ib_bh_get(ib_bh, 0));
}

UNIV_INLINE void* ib_bh_last(ib_bh_t* ib_bh)
{
	return (ib_bh_is_empty(ib_bh) ? NULL : ib_bh_get(ib_bh, ib_bh->n_elems - 1));
}

UNIV_INTERN ib_bh_t* ib_bh_create(ib_bh_cmp_t compare, ulint sizeof_elem, ulint max_elems)
{
	ulint sz;
	ib_bh_t* ib_bh;

	sz = sizeof(ib_bh_t) + (sizeof_elem * max_elems);
	/*����һ�����ڴ棬������ڴ������洢���������*/
	ib_bh = ut_malloc(sz);
	/*Ϊʲô���ڴ�ֱ�Ӵ������ڴ���������ڴ棿*/
	memset(ib_bh, 0x0, sz);

	ib_bh->compare = compare;
	ib_bh->max_elems = max_elems;
	ib_bh->n_elems = 0;
	ib_bh->sizeof_elem = sizeof_elem;

	return ib_bh;
}

UNIV_INTERN void ib_bh_free(ib_bh_t* ib_bh)
{
	ut_free(ib_bh);
}
/*���õ��Ƕ��ֲ��ҷ������Ӷ���logN,���ֻ��ѭ���ĸ��Ӷȡ������ĸ��Ӷ�ȡ��compare������ib_bh_set��ֵ��ֵ���Ӷ�*/
UNIV_INTERN void* ib_bh_push(ib_bh_t* ib_bh, const void* elem)
{
	void* ptr;

	if(ib_bh_is_full(ib_bh))
		return NULL;
	else if(ib_bh_is_empty(ib_bh)){
		ib_bh->n_elems ++;
		return ib_bh_set(ib_bh, 0, ptr);
	}
	else{
		ulint i;
		i = ib_bh->n_elems;
		++ib_bh->n_elems;

		for(ptr = ib_bh_get(ib_bh, i >> 1); i > 0 && ib_bh->compare(ptr, elem) > 0; i >>= 1, ptr = ib_bh_get(ib_bh, i >> 1)){
			ib_bh_set(ib_bh, i, ptr);
		}
		/*�����ѭ��Ӧ�õ�ͬ�����´���:
		ptr = ib_bh_get(ib_bh, i >> 1);
		while(i > 0 && ib_bh->compare(ptr, elem) > 0){
			ib_bh_set(ib_bh, i, ptr);
			i >>= 1;
			ptr = ib_bh_get(ib_bh, i >> 1);
		}
		*/
		ptr = ib_bh_set(ib_bh, i, elem);
	}
}



