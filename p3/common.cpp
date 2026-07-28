#include "common.h"
#include <stdio.h>

bool IsSoundChar(char c)
{
	bool b;

	b = (c == CHAR_STREAM || c == CHAR_USERVOX || c == CHAR_SENTENCE || c == CHAR_DRYMIX || c == CHAR_OMNI);
	b = b || (c == CHAR_DOPPLER || c == CHAR_DIRECTIONAL || c == CHAR_DISTVARIANT || c == CHAR_SPATIALSTEREO || c == CHAR_FAST_PITCH);

	return b;
}

char* PSkipSoundChars(const char* pch)
{
	char* pcht = (char*)pch;

	while (1)
	{
		if (!IsSoundChar(*pcht))
			break;
		pcht++;
	}

	return pcht;
}

bool TestSoundChar( const char* pch, char c )
{
	char* pcht = (char*)pch;

	while ( 1 )
	{
		if ( !IsSoundChar( *pcht ) )
			break;
		if ( *pcht == c )
			return true;
		pcht++;
	}

	return false;
}

char* va(const char* format, ...)
{
	va_list		argptr;
	static char	string[8][512];
	static int	curstring = 0;

	curstring = (curstring + 1) % 8;

	va_start(argptr, format);
	vsnprintf(string[curstring], sizeof(string[curstring]), format, argptr);
	va_end(argptr);

	return string[curstring];
}