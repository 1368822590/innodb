#ifndef FILE_HANDLER_H_
#define FILE_HANDLER_H_

#include <string>
#include <fstream>

#include "base_block_handler.h"

using namespace std;

class CFileHandler
{
public:
	CFileHandler(string& str_fpath);
	virtual ~CFileHandler();

public:
	void	AnalysisFile();

	bool	IsOccurError(){return is_occur_error_;}

private:
	void	OpenFile(ifstream& infile);

	//����һ���е�һ������
	string AnalysisFirstword(string& str_line);

	void	SetBlockHandler(string& str_keyword);

	//������к�#ע����
	bool	IsSpaceorConmentLine(string& str_line);

	bool	IsBlockBeginSign(string& str_line);

	//ȥ���п�ʼ�Ŀո��tab
	void	StripSpaceAndTab(string& str_line);

private:
	string	fname_path_;//defԴ�ļ���·�������·�������·��
	int		fline_num_;

	bool	is_begin_block_;
	bool	is_occur_error_;//�Ƿ񲶻��˴���

	CBaseBlockHandler* handle_block_;
};
#endif //FILE_HANDLER_H_