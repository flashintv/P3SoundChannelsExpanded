//========= Copyright © 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $NoKeywords: $
//=============================================================================//
#if !defined( MOD_LOADER_H )
#define MOD_LOADER_H
#ifdef _WIN32
#pragma once
#endif

struct model_t;
class IMaterial;
class IFileList;

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
class IModelLoader
{
public:
	enum REFERENCETYPE
	{
		// The name is allocated, but nothing else is in memory or being referenced
		FMODELLOADER_NOTLOADEDORREFERENCED = 0,
		// The model has been loaded into memory
		FMODELLOADER_LOADED	= (1<<0),

		// The model is being referenced by the server code
		FMODELLOADER_SERVER = (1<<1),
		// The model is being referenced by the client code
		FMODELLOADER_CLIENT = (1<<2),
		// The model is being referenced in the client .dll
		FMODELLOADER_CLIENTDLL = (1<<3),
		// The model is being referenced by static props
		FMODELLOADER_STATICPROP	= (1<<4),
		// The model is a detail prop
		FMODELLOADER_DETAILPROP = (1<<5),
		FMODELLOADER_REFERENCEMASK = (FMODELLOADER_SERVER | FMODELLOADER_CLIENT | FMODELLOADER_CLIENTDLL | FMODELLOADER_STATICPROP | FMODELLOADER_DETAILPROP ),

		// The model was touched by the preload method
		FMODELLOADER_TOUCHED_BY_PRELOAD = (1<<15),
		// The model was loaded by the preload method, a postload fixup is required
		FMODELLOADER_LOADED_BY_PRELOAD = (1<<16),
		// The model touched its materials as part of its load
		FMODELLOADER_TOUCHED_MATERIALS = (1<<17),
	};

	enum ReloadType_t
	{
		RELOAD_LOD_CHANGED = 0,
		RELOAD_EVERYTHING,
		RELOAD_REFRESH_MODELS,
	};

	// Start up modelloader subsystem
	virtual void		Init( void ) = 0;
	virtual void		Shutdown( void ) = 0;

	virtual int			GetCount( void ) = 0;
	virtual model_t		*GetModelForIndex( int i ) = 0;

	// Look up name for model
	virtual const char *GetName( const model_t *model ) = 0;

	// Check for extra data, reload studio model if needed
	virtual void		*GetExtraData( model_t *model ) = 0;

	// Get disk size for model
	virtual int			GetModelFileSize( const char *name ) = 0;

	// Finds the model, and loads it if it isn't already present.  Updates reference flags
	virtual model_t		*GetModelForName( const char *name, REFERENCETYPE referencetype ) = 0;
	virtual model_t		*ReferenceModel( const char *name, REFERENCETYPE referencetype ) = 0;
	// Unmasks the referencetype field for the model
	virtual void		UnreferenceModel( model_t *model, REFERENCETYPE referencetype ) = 0;
	// Unmasks the specified reference type across all models
	virtual void		UnreferenceAllModels( REFERENCETYPE referencetype ) = 0;

	// For any models with referencetype blank, frees all memory associated with the model
	//  and frees up the models slot
	virtual void		UnloadUnreferencedModels( void ) = 0;
	virtual void		PurgeUnusedModels( void ) = 0;

	// On the client only, there is some information that is computed at the time we are just
	//  about to render the map the first time.  If we don't change/unload the map, then we
	//  shouldn't have to recompute it each time we reconnect to the same map
	virtual bool		Map_GetRenderInfoAllocated( void ) = 0;
	virtual void		Map_SetRenderInfoAllocated( bool allocated ) = 0;

	// Load all the displacements for rendering. Set bRestoring to true if we're recovering from an alt+tab.
	virtual void		Map_LoadDisplacements( model_t *model, bool bRestoring ) = 0;

	// Print which models are in the cache/known
	virtual void		Print( void ) = 0;

	// Validate version/header of a .bsp file
	virtual bool		Map_IsValid( char const *mapname ) = 0;

	// Recomputes surface flags
	virtual void		RecomputeSurfaceFlags( model_t *mod ) = 0;

	// Reloads all models
	virtual void		Studio_ReloadModels( ReloadType_t reloadType ) = 0;

	// Is a model loaded?
	virtual bool		IsLoaded( const model_t *mod ) = 0;

	virtual bool		LastLoadedMapHasHDRLighting( void ) = 0;

	// See CL_HandlePureServerWhitelist for what this is for.
	virtual void ReloadFilesInList( IFileList *pFilesToReload ) = 0;

	virtual const char *GetActiveMapName( void ) = 0;
};
#endif // MOD_LOADER_H
