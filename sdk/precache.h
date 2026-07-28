//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//

#ifndef PRECACHE_H
#define PRECACHE_H
#ifdef _WIN32
#pragma once
#endif

#include "networkstringtabledefs.h"

#define PRECACHE_USER_DATA_NUMBITS		2			// Only network two bits from flags field...

#pragma pack(1)
//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
struct CPrecacheUserData
{
	unsigned char flags : PRECACHE_USER_DATA_NUMBITS;
};

#pragma pack()



class CSfxTable;
struct model_t;

//#define DEBUG_PRECACHE 1

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
class CPrecacheItem
{
public:
					CPrecacheItem( void );

	// accessorts
	model_t			*GetModel( void );
	char const		*GetGeneric( void );
	CSfxTable		*GetSound( void );
	char const		*GetName( void );
	char const		*GetDecal( void );

	void			SetModel( model_t const *pmodel );
	void			SetGeneric( char const *pname );
	void			SetSound( CSfxTable const *psound );
	void			SetName( char const *pname );
	void			SetDecal( char const *decalname );

	float			GetFirstReference( void );
	float			GetMostRecentReference( void );
	unsigned int	GetReferenceCount( void );

private:
	enum
	{
		TYPE_UNK = 0,
		TYPE_MODEL,
		TYPE_SOUND,
		TYPE_GENERIC,
		TYPE_DECAL
	};

	void			Init( int type, void const *ptr );
	void			ResetStats( void );
	void			Reference( void );

	unsigned int	m_nType : 3;
	unsigned int	m_uiRefcount : 29;
	union precacheptr
	{
		model_t		*model;
		char const	*generic;
		CSfxTable	*sound;
		char const	*name;
	} u;
#if DEBUG_PRECACHE
	float			m_flFirst;
	float			m_flMostRecent;
#endif
};

#define MODEL_PRECACHE_TABLENAME	"modelprecache"
#define GENERIC_PRECACHE_TABLENAME	"genericprecache"
#define SOUND_PRECACHE_TABLENAME	"soundprecache"
#define DECAL_PRECACHE_TABLENAME	"decalprecache"

char const *GetFlagString( int flags );

#define RES_FATALIFMISSING (1<<0)
#define RES_PRELOAD (1<<1)
#define COPY_ALL_CHARACTERS -1
#define ANALYZE_SUPPRESS(wnum) __pragma(warning(suppress: wnum))
char* Q_strncat(char* pDest, const char* pSrc, size_t destBufferSize, int max_chars_to_copy)
{
	size_t charstocopy = (size_t)0;

	Assert((ptrdiff_t)destBufferSize >= 0);
	AssertValidStringPtr(pDest);
	AssertValidStringPtr(pSrc);

	size_t len = strlen(pDest);
	size_t srclen = strlen(pSrc);
	if (max_chars_to_copy <= COPY_ALL_CHARACTERS)
	{
		charstocopy = srclen;
	}
	else
	{
		charstocopy = (size_t)min(max_chars_to_copy, (int)srclen);
	}

	if (len + charstocopy >= destBufferSize)
	{
		charstocopy = destBufferSize - len - 1;
	}

	if ((int)charstocopy <= 0)
	{
		return pDest;
	}

	ANALYZE_SUPPRESS(6059); // warning C6059: : Incorrect length parameter in call to 'strncat'. Pass the number of remaining characters, not the buffer size of 'argument 1'
	char* pOut = strncat(pDest, pSrc, charstocopy);
	return pOut;
}

//-----------------------------------------------------------------------------
// Purpose: Print out flag names
// Input  : flags - 
// Output : char const
//-----------------------------------------------------------------------------
char const* GetFlagString(int flags)
{
	static char ret[512];
	ret[0] = 0;

	bool first = true;

	if (!flags)
		return "None";

	if (flags & RES_FATALIFMISSING)
	{
		if (!first)
		{
			Q_strncat(ret, " | ", sizeof(ret), COPY_ALL_CHARACTERS);
		}
		Q_strncat(ret, "RES_FATALIFMISSING", sizeof(ret), COPY_ALL_CHARACTERS);
		first = false;
	}

	if (flags & RES_PRELOAD)
	{
		if (!first)
		{
			Q_strncat(ret, " | ", sizeof(ret), COPY_ALL_CHARACTERS);
		}
		Q_strncat(ret, "RES_PRELOAD", sizeof(ret), COPY_ALL_CHARACTERS);
		first = false;
	}

	return ret;
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CPrecacheItem::CPrecacheItem(void)
{
	Init(TYPE_UNK, NULL);
	ResetStats();
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CPrecacheItem::ResetStats(void)
{
	m_uiRefcount = 0;
#if DEBUG_PRECACHE
	m_flFirst = 0.0f;
	m_flMostRecent = 0.0f;
#endif
}

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
void CPrecacheItem::Reference(void)
{
	m_uiRefcount++;
#if DEBUG_PRECACHE
	m_flMostRecent = realtime;
	if (!m_flFirst)
	{
		m_flFirst = realtime;
	}
#endif
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : type - 
//			*ptr - 
//-----------------------------------------------------------------------------
void CPrecacheItem::Init(int type, void const* ptr)
{
	m_nType = type;
	u.model = (model_t*)ptr;
	if (ptr)
	{
		ResetStats();
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : model_t
//-----------------------------------------------------------------------------
model_t* CPrecacheItem::GetModel(void)
{
	if (!u.model)
		return NULL;

	Assert(m_nType == TYPE_MODEL);

	Reference();

	return u.model;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : char const
//-----------------------------------------------------------------------------
char const* CPrecacheItem::GetGeneric(void)
{
	if (!u.generic)
		return NULL;

	Assert(m_nType == TYPE_GENERIC);

	Reference();

	return u.generic;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : CSfxTable
//-----------------------------------------------------------------------------
CSfxTable* CPrecacheItem::GetSound(void)
{
	if (!u.sound)
		return NULL;

	Assert(m_nType == TYPE_SOUND);

	Reference();

	return u.sound;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : char const
//-----------------------------------------------------------------------------
char const* CPrecacheItem::GetName(void)
{
	if (!u.name)
		return NULL;

	Assert(m_nType == TYPE_SOUND);

	Reference();

	return u.name;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : char const
//-----------------------------------------------------------------------------
char const* CPrecacheItem::GetDecal(void)
{
	if (!u.name)
		return NULL;

	Assert(m_nType == TYPE_DECAL);

	Reference();

	return u.name;
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *pmodel - 
//-----------------------------------------------------------------------------
void CPrecacheItem::SetModel(model_t const* pmodel)
{
	Init(TYPE_MODEL, pmodel);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *pname - 
//-----------------------------------------------------------------------------
void CPrecacheItem::SetGeneric(char const* pname)
{
	Init(TYPE_GENERIC, pname);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *psound - 
//-----------------------------------------------------------------------------
void CPrecacheItem::SetSound(CSfxTable const* psound)
{
	Init(TYPE_SOUND, psound);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *name - 
//-----------------------------------------------------------------------------
void CPrecacheItem::SetName(char const* name)
{
	Init(TYPE_SOUND, name);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *decalname - 
//-----------------------------------------------------------------------------
void CPrecacheItem::SetDecal(char const* decalname)
{
	Init(TYPE_DECAL, decalname);
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : float
//-----------------------------------------------------------------------------
float CPrecacheItem::GetFirstReference(void)
{
#if DEBUG_PRECACHE
	return m_flFirst;
#else
	return 0;
#endif
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : float
//-----------------------------------------------------------------------------
float CPrecacheItem::GetMostRecentReference(void)
{
#if DEBUG_PRECACHE
	return m_flMostRecent;
#else
	return 0;
#endif
}

//-----------------------------------------------------------------------------
// Purpose: 
// Output : unsigned int
//-----------------------------------------------------------------------------
unsigned int CPrecacheItem::GetReferenceCount(void)
{
	return m_uiRefcount;
}

#endif // PRECACHE_H
