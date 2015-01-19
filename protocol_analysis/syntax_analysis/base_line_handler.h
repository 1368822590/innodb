#ifndef BASE_LINE_HANDLER_H_
#define BASE_LINE_HANDLER_H_

#include "basedefine.h"
#include <string>
#include "global.h"

using namespace std;

class CBaseLineHandler
{
public:
	CBaseLineHandler();
	virtual ~CBaseLineHandler();

public:
	virtual void	AnalysisLine(string& str_line);

	//�����Կո�Ϊ�ַ��ָȡ���������ʡ�','�����š�ð�š���������Ԫ��
	//�����в���ע�ͻ���У�v_elem���ָܷ���Ԫ��
	static void		SplitLineWithSpace(string& str_line, StringArray& v_elem);

	//����ֻ������ĸ�����»��ߣ���������ĸ��ͷ
	static bool		IsVarNameLegal(string& str_varname);

	//���str_type�Ǹ������ͣ���array<>��ʽ��Ϊ��������,��ָ������ͳ�2����������ͨ��v_type����
	//�������vector
	static void		SplitComplexType(const string& str_type, StringArray& v_type);

	//��2���ַ�������ĸת���ɴ�дƴ������һ���µ��ַ�������,��Array_Int8
	static string	GenerateNewAlias(string str1, string str2);

	//ȥ���ַ������˵Ŀհ�
	static string	StripSpaceAndTab(string str);

	//���ַ������е���ĸת��ΪСд
	static void		StringToLower(string& str);

	//���ַ������е���ĸת��Ϊ��д
	static void		StringToUpper(string& str);

	//�ж������ֻ�������
	static BaseType WhichType(const string& str);

	static BaseType AnalysisValueType(const string& type);
	
	static bool		IsCanConvertNum(const string& str);
};
#endif //BASE_LINE_HANDLER_H_