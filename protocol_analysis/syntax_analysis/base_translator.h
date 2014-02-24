/*
 * ��CFileDescMap��CVarDefMap��CTypedefSet��
 * CUserClassMap��CMsgBodyDefMap�е���Ϣ����
 * �ɾ�������
 */
#ifndef BASE_TRANSLATOR_H_
#define BASE_TRANSLATOR_H_

class CBaseTranslator
{
public:
	CBaseTranslator();
	virtual ~CBaseTranslator();

public:
	virtual void TranslateProtocol() = 0;

protected:
	virtual void AddFileDescToFile();
	virtual void AddVarDefToFile();
	virtual void AddTypeDefToFile();
	virtual void AddUserClassDef();
	virtual void AddMsgBodyDef();
};

#endif //BASE_TRANSLATOR_H_