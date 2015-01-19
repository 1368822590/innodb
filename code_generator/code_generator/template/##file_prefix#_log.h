/*************************************************************************************
*filename:	sample_log_macro.h
*
*to do:		����һ����־,ʵ�ָ����������־��¼
*Create on: 2012-05
*Author:	zerok
*check list:
*************************************************************************************/
#ifndef __##marco_prefix#_LOG_MACRO_H
#define __##marco_prefix#_LOG_MACRO_H

#include "revolver/base_log.h"
#include "revolver/base_hex_string.h"

extern BaseLogStream ##var_prefix#_log;

#define SRV_DEBUG(arg)\
	DEBUG_TRACE(##var_prefix#_log, arg)

#define SRV_INFO(arg)\
	INFO_TRACE(##var_prefix#_log, arg)

#define SRV_WARNING(arg)\
	WARNING_TRACE(##var_prefix#_log, arg)

#define SRV_ERROR(arg)\
	ERROR_TRACE(##var_prefix#_log, arg)

#define SRV_FATAL(arg)\
	FATAL_TRACE(##var_prefix#_log, arg)

#endif

/************************************************************************************/
