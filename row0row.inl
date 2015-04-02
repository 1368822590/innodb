#include "dict0dict.h"
#include "rem0rec.h"
#include "trx0undo.h"

dulint row_get_rec_sys_field(ulint type, rec_t* rec, dict_index_t* index);

void row_set_rec_sys_field(ulint type, rec_t* rec, dict_index_t* index, dulint val);

/*�Ӽ�¼�ж�ȡ�ۼ��������һ�β���������ID*/
UNIV_INLINE dulint row_get_rec_trx_id(rec_t* rec, dict_index_t* index)
{
	ulint	offset;

	ut_ad(index->type & DICT_CLUSTERED);

	/*�������еõ�trx id�е�ƫ��*/
	offset = index->trx_id_offset;
	if(offset)
		return trx_read_trx_id(rec + offset);
	else
		return row_get_rec_sys_field(DATA_TRX_ID, rec, index);
}

/*��ü�¼�оۼ�������¼��roll ptr*/
UNIV_INLINE dulint row_get_rec_roll_ptr(rec_t* rec, dict_index_t* index)
{
	ulint	offset;

	ut_ad(index->type & DICT_CLUSTERED);

	offset = index->trx_id_offset;
	if(offset)
		return trx_read_roll_ptr(rec + offset + DATA_TRX_ID_LEN); /*ROLL PTR��trx id�еĺ���*/
	else
		return row_get_rec_sys_field(DATA_ROLL_PTR, rec, index);
}

/*���þۼ�������¼��trx id*/
UNIV_INLINE void row_set_rec_trx_id(rec_t* rec, dict_index_t* index, dulint trx_id)
{
	ulint	offset;

	ut_ad(index->type & DICT_CLUSTERED);

	offset = index->trx_id_offset;
	if(offset)
		trx_write_trx_id(rec + offset, trx_id);

}

/*�ο��ۼ������ļ�¼(tuple)����һ������������¼(rec),map����Ҫ����������ŵ������б�
tuple -> rec_t*/
UNIV_INLINE void row_build_row_ref_fast(dtuple_t* ref, ulint* map, rec_t* rec)
{
	dfield_t*	dfield;
	byte*		field;
	ulint		len;
	ulint		ref_len;
	ulint		field_no;
	ulint		i;

	ref_len = dtuple_get_n_fields(ref);

	for(i = 0; i < ref_len; i ++){
		dfield = dtuple_get_nth_field(ref, i);
		field_no = *(map + i); /*�����Ҫ�������ж���*/

		if(field_no != ULINT_UNDEFINED){
			field = rec_get_nth_field(rec, field_no, &len);
			dfield_set_data(dfield, field, len);
		}
	}
}


