//===== Copyright � 1996-2005, Valve Corporation, All rights reserved. ======//
//
// Purpose: 
//
// $NoKeywords: $
//
//===========================================================================//

#include <stdio.h>
#include "safetyhook.hpp"
#include "sourcemod/sigscan.h"

#include "sdk/interface.h"
#include "sdk/precache.h"
#include "sdk/modelloader.h"
#include "sdk/IServerPlugin.h"
#include "sdk/ienginereplay.h"
#include "sdk/icommandline.h"
#include "sdk/vgui_baseui_interface.h"
#include "sdk/icvar.h"
#include "sdk/soundservice.h"
#include "sdk/snd_device.h"
#include "sdk/icliententitylist.h"
#include "sdk/cdll_int.h"
#include "sdk/soundinfo.h"
#include "sdk/threadtools.h"
#include "sdk/snd_mix_buf.h"

#include "sdk/utlvector.h"
#include "sdk/utllinkedlist.h"
#include "sdk/utlmap.h"

#include "p3/snd_channels.h"

//---------------------------------------------------------------------------------
// Purpose: a sample 3rd party plugin class
//---------------------------------------------------------------------------------
class CEmptyServerPlugin: public IServerPluginCallbacks
{
public:
	CEmptyServerPlugin() {}
	~CEmptyServerPlugin() {}

	// IServerPluginCallbacks methods
	virtual bool			Load(CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory);
	virtual void			Unload(void);
	virtual void			Pause(void) {}
	virtual void			UnPause(void) {}
	virtual const char*		GetPluginDescription(void) { return "P3SoundChannelsExpanded - Increases the amount of dynamic sound channels. (By Grizzle)"; }
	virtual void			LevelInit(char const* pMapName) {}
	virtual void			ServerActivate(void* pEdictList, int edictCount, int clientMax) {}
	virtual void			GameFrame(bool simulating) {}
	virtual void			LevelShutdown(void) {}
	virtual void			ClientActive(void* pEntity) {}
	virtual void			ClientDisconnect(void* pEntity) {}
	virtual void			ClientPutInServer(void* pEntity, char const* playername) {}
	virtual void			SetCommandClient(int index) {}
	virtual void			ClientSettingsChanged(void* pEdict) {}
	virtual PLUGIN_RESULT	ClientConnect(bool* bAllowConnect, void* pEntity, const char* pszName, const char* pszAddress, char* reject, int maxrejectlen) { return PLUGIN_CONTINUE; }
	virtual PLUGIN_RESULT	ClientCommand(void* pEntity, const CCommand& args) { return PLUGIN_CONTINUE; }
	virtual PLUGIN_RESULT	NetworkIDValidated(const char* pszUserName, const char* pszNetworkID) { return PLUGIN_CONTINUE; }
	virtual void			OnQueryCvarValueFinished(QueryCvarCookie_t iCookie, void* pPlayerEntity, EQueryCvarValueStatus eStatus, const char* pCvarName, const char* pCvarValue) {}
};
CEmptyServerPlugin g_EmptyServerPlugin;
EXPOSE_SINGLE_INTERFACE_GLOBALVAR(CEmptyServerPlugin, IServerPluginCallbacks, INTERFACEVERSION_ISERVERPLUGINCALLBACKS, g_EmptyServerPlugin );

// -----------------------------------------------------------------------------

template< typename T >
void WriteBytes( uintptr_t addr, T thing )
{
	DWORD oldProtect;
	VirtualProtect( (void*)addr, sizeof( T ), PAGE_EXECUTE_READWRITE, &oldProtect );
	memcpy( (void*)addr, &thing, sizeof( T ) );
	VirtualProtect( (void*)addr, sizeof( T ), oldProtect, &oldProtect );
}

void WriteInt( uintptr_t addr, int thing )
{
	WriteBytes( addr, thing );
}

// Helper to replace all pointers in .text with another pointer
int FindPointerAndReplaceWith( 
	const char* pszmodule, uintptr_t findPointer, 
	uintptr_t desiredPointer, std::vector<uintptr_t>* foundPointers = NULL )
{
	HMODULE module = GetModuleHandle( pszmodule );
	if ( module == NULL )
		return 0;

	IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)module;
	IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)((uintptr_t)dos + (uintptr_t)dos->e_lfanew);

	uintptr_t start = NULL;
	uintptr_t end = NULL;

	auto section = IMAGE_FIRST_SECTION( nt );
	for ( WORD i = 0; i < nt->FileHeader.NumberOfSections; i++ )
	{
		if ( memcmp( section->Name, ".text", 5 ) == 0 )
		{
			start = (uintptr_t)dos + section->VirtualAddress;
			end = start + section->Misc.VirtualSize;
			break;
		}
		section++;
	}

	if ( start == 0 || end == 0 )
		return 0;

	// now for the scanning
	int found_pointers = 0;
	for ( uintptr_t addr = start; addr <= end - sizeof( uintptr_t ); addr++ )
	{
		if ( *(uintptr_t*)addr == findPointer )
		{
			WriteBytes( addr, desiredPointer );
			found_pointers++;

			if ( foundPointers )
				foundPointers->push_back( addr );
		}
	}
	return found_pointers;
}

// -----------------------------------------------------------------------------

// very much needed because we need to enable hooks in bulk, to prevent crashes
std::vector< safetyhook::InlineHook > o_Hooks;
#define SETUP_HOOK( funcname, sig, mask ) do { \
	CSigScan sig_##funcname; \
	sig_##funcname.Init( sig, mask, sizeof( mask ) - 1 ); \
	o_Hooks.push_back( safetyhook::create_inline( sig_##funcname##.sig_addr, funcname, safetyhook::InlineHook::StartDisabled ) ); \
} while (0)

// interfaces
ICvar* cvar = NULL;
ISoundServices* g_pSoundServices = NULL;
IClientEntityList* entitylist = NULL;
IVEngineClient* engineclient = NULL;
IAudioDevice** pg_AudioDevice = NULL; // i believe audio device can change during runtime

// global variables
int* ptotal_channels = NULL;
Vector* plistener_origin = NULL;
Vector* plistener_forward = NULL;
Vector* plistener_right = NULL;
Vector* plistener_up = NULL;
grouprule_t* pg_grouprules = NULL;
int* pg_cgrouprules = NULL;
int* pg_mapMixgroupidToGrouprulesid = NULL;
float* pg_DuckScale = NULL;
soundfade_t* psoundfade = NULL;
CUtlMap< FileNameHandle_t, SfxDictEntry >* ps_Sounds = NULL;
bool* ps_bOnLoadScreen = NULL;
double* pg_LastSoundFrame = NULL;
double* pg_LastMixTime = NULL;
float* pg_EstFrameTime = NULL;
float* pg_DashboardMusicMixValue = NULL;
float* pg_DashboardMusicMixTarget = NULL;
bool* ps_bIsListenerUnderwater = NULL;
int* pg_snd_trace_count = NULL;
int* pg_paintedtime = NULL;
CThreadMutex* pg_SndMutex = NULL;
bool* pg_bDspOff = NULL;
paintbuffer_t** pg_paintBuffers = NULL;
portable_samplepair_t** pg_curpaintbuffer = NULL;
portable_samplepair_t** pg_currearpaintbuffer = NULL;
portable_samplepair_t** pg_curcenterpaintbuffer = NULL;
bool* pg_bdirectionalfx = NULL;
int* pg_snd_profile_type = NULL;
float* pg_dsp_volume = NULL;
int* pidsp_facingaway = NULL;
int* pidsp_speaker = NULL;
int* pidsp_automatic = NULL;
int* pidsp_room = NULL;
int* pidsp_player = NULL;
int* pidsp_water = NULL;
int* pidsp_spatial = NULL;

// functions
CAudioSource* (*S_LoadSound)(CSfxTable * pSfx, channel_t * ch) = NULL;
void (*S_SyncClockAdjust)() = NULL;
void (*DSP_ClearState)() = NULL;
void (*MIX_ClearAllPaintBuffers)(int SampleCount, bool clearFilters) = NULL;
void (*CDebugOverlay_AddTextOverlay)(const Vector& origin, float flDuration, const char* text) = NULL;
void (*S_Update_)( float mixAheadTime ) = NULL;
void (*SND_Spatialize)(channel_t* ch) = NULL;
void (*S_UpdateSoundFade)(void) = NULL;
void (*SND_SetSpatialDelays)(void) = NULL;
void (*DAS_CheckNewRoomDSP)(void) = NULL;
void (*S_UpdateVoiceDuck)(int voiceChannelCount, int voiceChannelMaxVolume, float frametime) = NULL;
void (*SND_ActivateChannel)(channel_t* pChannel) = NULL;
void (*VOX_LoadSound)(channel_t * pchan, const char* pszin) = NULL;
void (*MXR_GetMixGroupFromSoundsource)(channel_t* pchan, SoundSource soundsource, soundlevel_t soundlevel) = NULL;
void (*MIX_MixPaintbuffers)(int ibuf1, int ibuf2, int ibuf3, int count, float fgain_out) = NULL;
void (*MXR_SetCurrentSoundMixer)( const char* szsoundmixer ) = NULL;
void (*CheckNewDspPresets)() = NULL;
void (*MIX_ScalePaintBuffer)( int bufferIndex, int count, float fgain ) = NULL;
bool (*DSP_RoomDSPIsOff)() = NULL;

// some important globals
channel_t* pchannels/*[OLD_MAX_CHANNELS]*/;
CActiveChannels<OLD_MAX_CHANNELS>* pg_ActiveChannels;
// we replace some global stuff with these
channel_t channels[MAX_CHANNELS];
CActiveChannels<MAX_CHANNELS> g_ActiveChannels;

// forward def
struct SndInfo_t;

bool CEmptyServerPlugin::Load( CreateInterfaceFn interfaceFactory, CreateInterfaceFn gameServerFactory )
{
	// interfaces
	cvar = (ICvar*)interfaceFactory( CVAR_INTERFACE_VERSION, NULL );
	entitylist = (IClientEntityList*)interfaceFactory( VCLIENTENTITYLIST_INTERFACE_VERSION, NULL );
	engineclient = (IVEngineClient*)interfaceFactory( VENGINE_CLIENT_INTERFACE_VERSION, NULL );

	// set count to 0
	g_ActiveChannels.Init();

	// signature scanning
	{
		CSigScan::GetDllMemInfo( "engine.dll" );

		// We need the mutex as fast as possible, so we can hook without worrying that some of the code runs,
		// before we even get to hooking the functions
		{
			// mov ecx, offset g_SndMutex
			// "\xB9\xCC\xCC\xCC\xCC\xFF\x15\xCC\xCC\xCC\xCC\xE8\xCC\xCC\xCC\xCC\x8B\xB4\x24\xC4\x01\x00\x00", "x????xx????x????xxxxxxx"
			CSigScan sig_g_SndMutex;
			sig_g_SndMutex.Init( "\xB9\xCC\xCC\xCC\xCC\xFF\x15\xCC\xCC\xCC\xCC\xE8\xCC\xCC\xCC\xCC\x8B\xB4\x24\xC4\x01\x00\x00", "x????xx????x????xxxxxxx", 23 );
			pg_SndMutex = *(CThreadMutex**)((uintptr_t)sig_g_SndMutex.sig_addr + 1);
		}
		AUTO_LOCK( *pg_SndMutex );

		// S_LoadSound
		CSigScan sig_S_LoadSound;
		sig_S_LoadSound.Init( "\x83\xEC\x08\x56\x8B\x74\x24\x10\x8B\x4E\x08", "xxxxxxxxxxx", 11 );
		S_LoadSound = decltype(S_LoadSound)(sig_S_LoadSound.sig_addr);

		// S_SyncClockAdjust
		CSigScan sig_S_SyncClockAdjust;
		sig_S_SyncClockAdjust.Init( "\x0F\x57\xC0\xF3\x0F\x11\x05\xCC\xCC\xCC\xCC\xF3\x0F\x11\x05\xCC\xCC\xCC\xCC\xC7\x05", "xxxxxxx????xxxx????xx", 21 );
		S_SyncClockAdjust = decltype(S_SyncClockAdjust)(sig_S_SyncClockAdjust.sig_addr);

		// MIX_ClearAllPaintBuffers
		CSigScan sig_MIX_ClearAllPaintBuffers;
		sig_MIX_ClearAllPaintBuffers.Init( "\xA1\xCC\xCC\xCC\xCC\x53\x33\xDB\x3B\xC3\x0F\x84\x0F\x01\x00\x00", "x????xxxxxxxxxxx", 16 );
		MIX_ClearAllPaintBuffers = decltype(MIX_ClearAllPaintBuffers)(sig_MIX_ClearAllPaintBuffers.sig_addr);

		// DSP_ClearState
		CSigScan sig_DSP_ClearState;
		sig_DSP_ClearState.Init( "\x53\x33\xDB\x38\x1D\xCC\xCC\xCC\xCC\x0F\x85\xBD\x00\x00\x00", "xxxxx????xxxxxx", 15 );
		DSP_ClearState = decltype(DSP_ClearState)(sig_DSP_ClearState.sig_addr);

		// CDebugOverlay::AddTextOverlay
		CSigScan sig_CDebugOverlay_AddTextOverlay;
		sig_CDebugOverlay_AddTextOverlay.Init( "\xE8\xCC\xCC\xCC\xCC\x84\xC0\x0F\x85\xDC\x00\x00\x00", "x????xxxxxxxx", 13 );
		CDebugOverlay_AddTextOverlay = decltype(CDebugOverlay_AddTextOverlay)(sig_CDebugOverlay_AddTextOverlay.sig_addr);

		// S_Update_
		CSigScan sig_S_Update_;
		sig_S_Update_.Init( "\xA1\xCC\xCC\xCC\xCC\x83\x78\x30\x00\x75\x44", "x????xxxxxx", 11 );
		S_Update_ = decltype(S_Update_)(sig_S_Update_.sig_addr);

		// SND_Spatialize
		CSigScan sig_SND_Spatialize;
		sig_SND_Spatialize.Init( "\x81\xEC\xEC\x00\x00\x00\x0F\x57\xC0", "xxxxxxxxx", 9 );
		SND_Spatialize = decltype(SND_Spatialize)(sig_SND_Spatialize.sig_addr);

		// S_UpdateSoundFade
		CSigScan sig_S_UpdateSoundFade;
		sig_S_UpdateSoundFade.Init( "\xA1\xCC\xCC\xCC\xCC\x83\x78\x30\x00\x75\x44", "x????xxxxxx", 11 );
		S_UpdateSoundFade = decltype(S_UpdateSoundFade)(sig_S_UpdateSoundFade.sig_addr);

		// SND_SetSpatialDelays
		CSigScan sig_SND_SetSpatialDelays;
		sig_SND_SetSpatialDelays.Init( "\xA1\xCC\xCC\xCC\xCC\x83\x78\x30\x00\x75\x44", "x????xxxxxx", 11 );
		SND_SetSpatialDelays = decltype(SND_SetSpatialDelays)(sig_SND_SetSpatialDelays.sig_addr);

		// DAS_CheckNewRoomDSP
		CSigScan sig_DAS_CheckNewRoomDSP;
		sig_DAS_CheckNewRoomDSP.Init( "\xA1\xCC\xCC\xCC\xCC\x83\x78\x30\x00\x75\x44", "x????xxxxxx", 11 );
		DAS_CheckNewRoomDSP = decltype(DAS_CheckNewRoomDSP)(sig_DAS_CheckNewRoomDSP.sig_addr);

		// S_UpdateVoiceDuck
		CSigScan sig_S_UpdateVoiceDuck;
		sig_S_UpdateVoiceDuck.Init( "\x83\xEC\x08\x8B\x0D\xCC\xCC\xCC\xCC\xD9\x41\x2C", "xxxxx????xxx", 12 );
		S_UpdateVoiceDuck = decltype(S_UpdateVoiceDuck)(sig_S_UpdateVoiceDuck.sig_addr);

		// SND_ActivateChannel
		CSigScan sig_SND_ActivateChannel;
		sig_SND_ActivateChannel.Init( "\x56\x8B\x74\x24\x08\x68\x30\x01\x00\x00", "xxxxxxxxxx", 10 );
		SND_ActivateChannel = decltype(SND_ActivateChannel)(sig_SND_ActivateChannel.sig_addr);

		// VOX_LoadSound
		CSigScan sig_VOX_LoadSound;
		sig_VOX_LoadSound.Init( "\x81\xEC\x14\x0F\x00\x00", "xxxxxx", 6 );
		VOX_LoadSound = decltype(VOX_LoadSound)(sig_VOX_LoadSound.sig_addr);

		// MXR_GetMixGroupFromSoundsource
		CSigScan sig_MXR_GetMixGroupFromSoundsource;
		sig_MXR_GetMixGroupFromSoundsource.Init( "\x81\xEC\x20\x02\x00\x00\x53\x83\xC8\xFF", "xxxxxxxxxx", 10 );
		MXR_GetMixGroupFromSoundsource = decltype(MXR_GetMixGroupFromSoundsource)(sig_MXR_GetMixGroupFromSoundsource.sig_addr);

		// MIX_MixPaintbuffers
		CSigScan sig_MIX_MixPaintbuffers;
		sig_MIX_MixPaintbuffers.Init( "\x83\xEC\x34\x8B\x54\x24\x38", "xxxxxxx", 7 );
		MIX_MixPaintbuffers = decltype(MIX_MixPaintbuffers)(sig_MIX_MixPaintbuffers.sig_addr);

		// MXR_SetCurrentSoundMixer
		CSigScan sig_MXR_SetCurrentSoundMixer;
		sig_MXR_SetCurrentSoundMixer.Init( "\x53\x8B\x5C\x24\x08\x68\xCC\xCC\xCC\xCC\x53\xE8\xCC\xCC\xCC\xCC\x83\xC4\x08\x85\xC0\x74", "xxxxxx????xx????xxxxxx", 22 );
		MXR_SetCurrentSoundMixer = decltype(MXR_SetCurrentSoundMixer)(sig_MXR_SetCurrentSoundMixer.sig_addr);

		// CheckNewDspPresets
		CSigScan sig_CheckNewDspPresets;
		sig_CheckNewDspPresets.Init( "\xA1\xCC\xCC\xCC\xCC\x83\xEC\x10\x53", "x????xxxx", 9 );
		CheckNewDspPresets = decltype(CheckNewDspPresets)(sig_CheckNewDspPresets.sig_addr);
		
		// MIX_ScalePaintBuffer
		CSigScan sig_MIX_ScalePaintBuffer;
		sig_MIX_ScalePaintBuffer.Init( "\x51\xA1\xCC\xCC\xCC\xCC\xF3\x0F\x10\x44\x24\x10", "xx????xxxxxx", 12 );
		MIX_ScalePaintBuffer = decltype(MIX_ScalePaintBuffer)(sig_MIX_ScalePaintBuffer.sig_addr);
		
		// DSP_PresetIsOff
		CSigScan sig_DSP_RoomDSPIsOff;
		sig_DSP_RoomDSPIsOff.Init( "\xA1\xCC\xCC\xCC\xCC\x83\x78\x30\x01\xA1\xCC\xCC\xCC\xCC\x75\x05\xA1\xCC\xCC\xCC\xCC\x83\xF8\x1F", "x????xxxxx????xxx????xxx", 24 );
		DSP_RoomDSPIsOff = decltype(DSP_RoomDSPIsOff)(sig_DSP_RoomDSPIsOff.sig_addr);

		// MIX_PaintChannels
		{
			// mov g_bDspOff, al
			// "\xA2\xCC\xCC\xCC\xCC\xE8\xCC\xCC\xCC\xCC\x8B\x15\xCC\xCC\xCC\xCC\xF3\x0F\x10\x42\x2C", "x????x????xx????xxxxx"
			CSigScan sig_g_bDspOff;
			sig_g_bDspOff.Init( "\xA2\xCC\xCC\xCC\xCC\xE8\xCC\xCC\xCC\xCC\x8B\x15\xCC\xCC\xCC\xCC\xF3\x0F\x10\x42\x2C", "x????x????xx????xxxxx", 21 );
			pg_bDspOff = *(bool**)( (uintptr_t)sig_g_bDspOff.sig_addr + 1 );

			// mov ecx, g_paintBuffers_m_Memory
			// "\x8B\x0D\xCC\xCC\xCC\xCC\x0F\x95\x44\x24\x02", "xx????xxxxx"
			CSigScan sig_g_paintBuffers;
			sig_g_paintBuffers.Init( "\x8B\x0D\xCC\xCC\xCC\xCC\x0F\x95\x44\x24\x02", "xx????xxxxx", 11 );
			pg_paintBuffers = *(paintbuffer_t***)( (uintptr_t)sig_g_paintBuffers.sig_addr + 2 );

			// mov g_bdirectionalfx, 0
			// "\xC6\x05\xCC\xCC\xCC\xCC\xCC\x57\xE8", "xx?????xx"
			CSigScan sig_g_bdirectionalfx;
			sig_g_bdirectionalfx.Init( "\xC6\x05\xCC\xCC\xCC\xCC\xCC\x57\xE8", "xx?????xx", 9 );
			pg_bdirectionalfx = *(bool**)( (uintptr_t)sig_g_bdirectionalfx.sig_addr + 2 );

			// mov g_snd_profile_type, ecx
			// "\x89\x0D\xCC\xCC\xCC\xCC\x83\x7A\x30\x00\x8D\x4C\x24\x1C", "xx????xxxxxxxx"
			CSigScan sig_g_snd_profile_type;
			sig_g_snd_profile_type.Init( "\x89\x0D\xCC\xCC\xCC\xCC\x83\x7A\x30\x00\x8D\x4C\x24\x1C", "xx????xxxxxxxx", 14 );
			pg_snd_profile_type = *(int**)( (uintptr_t)sig_g_snd_profile_type.sig_addr + 2 );

			// movss g_dsp_volume, xmm0
			// "\xF3\x0F\x11\x05\xCC\xCC\xCC\xCC\x8B\x01\x8B\x50\x74", "xxxx????xxxxx"
			CSigScan sig_g_dsp_volume;
			sig_g_dsp_volume.Init( "\xF3\x0F\x11\x05\xCC\xCC\xCC\xCC\x8B\x01\x8B\x50\x74", "xxxx????xxxxx", 13 );
			pg_dsp_volume = *(float**)( (uintptr_t)sig_g_dsp_volume.sig_addr + 4 );
		}

		// AllocDsps
		{
			// mov idsp_facingaway, eax
			// "\xA3\xCC\xCC\xCC\xCC\xA1\xCC\xCC\xCC\xCC\xD9\x05", "x????x????xx"
			CSigScan sig_idsp_facingaway;
			sig_idsp_facingaway.Init( "\xA3\xCC\xCC\xCC\xCC\xA1\xCC\xCC\xCC\xCC\xD9\x05", "x????x????xx", 12 );
			pidsp_facingaway = *(int**)( (uintptr_t)sig_idsp_facingaway.sig_addr + 1 );

			// mov idsp_speaker, eax
			// "\xA3\xCC\xCC\xCC\xCC\xD9\x1C\x24\x8B\x41\x30\x50\xE8\xCC\xCC\xCC\xCC\x8B\x15", "x????xxxxxxxx????xx"
			CSigScan sig_idsp_speaker;
			sig_idsp_speaker.Init( "\xA3\xCC\xCC\xCC\xCC\xD9\x1C\x24\x8B\x41\x30\x50\xE8\xCC\xCC\xCC\xCC\x8B\x15", "x????xxxxxxxx????xx", 19 );
			pidsp_speaker = *(int**)( (uintptr_t)sig_idsp_speaker.sig_addr + 1 );

			// mov idsp_automatic, eax
			// "\xA3\xCC\xCC\xCC\xCC\x8B\x41\x30", "x????xxx"
			CSigScan sig_idsp_automatic;
			sig_idsp_automatic.Init( "\xA3\xCC\xCC\xCC\xCC\x8B\x41\x30", "x????xxx", 8 );
			pidsp_automatic = *(int**)( (uintptr_t)sig_idsp_automatic.sig_addr + 1 );

			// mov idsp_room, eax
			// "\xA3\xCC\xCC\xCC\xCC\xD9\x1C\x24\xA3", "x????xxxx"
			CSigScan sig_idsp_room;
			sig_idsp_room.Init( "\xA3\xCC\xCC\xCC\xCC\xD9\x1C\x24\xA3", "x????xxxx", 9 );
			pidsp_room = *(int**)( (uintptr_t)sig_idsp_room.sig_addr + 1 );

			// mov idsp_player, eax
			// "\xA3\xCC\xCC\xCC\xCC\xD9\x1C\x24\x8B\x42\x30\x50\xE8\xCC\xCC\xCC\xCC\x83\xC4\x0C", "x????xxxxxxxx????xxx"
			CSigScan sig_idsp_player;
			sig_idsp_player.Init( "\xA3\xCC\xCC\xCC\xCC\xD9\x1C\x24\x8B\x42\x30\x50\xE8\xCC\xCC\xCC\xCC\x83\xC4\x0C", "x????xxxxxxxx????xxx", 20 );
			pidsp_player = *(int**)( (uintptr_t)sig_idsp_player.sig_addr + 1 );

			// mov idsp_water, eax
			// "\xA3\xCC\xCC\xCC\xCC\xD9\x1C\x24\x8B\x41\x30\x50\xE8\xCC\xCC\xCC\xCC\xD9\x05", "x????xxxxxxxx????xx"
			CSigScan sig_idsp_water;
			sig_idsp_water.Init( "\xA3\xCC\xCC\xCC\xCC\xD9\x1C\x24\x8B\x41\x30\x50\xE8\xCC\xCC\xCC\xCC\xD9\x05", "x????xxxxxxxx????xx", 19 );
			pidsp_water = *(int**)((uintptr_t)sig_idsp_water.sig_addr + 1);

			// mov idsp_spatial, eax
			// "\xA3\xCC\xCC\xCC\xCC\x8B\x4A\x30\x8B\x15", "x????xxxxx"
			CSigScan sig_idsp_spatial;
			sig_idsp_spatial.Init( "\xA3\xCC\xCC\xCC\xCC\x8B\x4A\x30\x8B\x15", "x????xxxxx", 10 );
			pidsp_spatial = *(int**)((uintptr_t)sig_idsp_spatial.sig_addr + 1);
		}
		
		// MIX_SetCurrentPaintbuffer
		{
			// mov g_curpaintbuffer, edx
			// "\x89\x15\xCC\xCC\xCC\xCC\x38\x48\x01", "xx????xxx"
			CSigScan sig_g_curpaintbuffer;
			sig_g_curpaintbuffer.Init( "\x89\x15\xCC\xCC\xCC\xCC\x38\x48\x01", "xx????xxx", 9 );
			pg_curpaintbuffer = *(portable_samplepair_t***)( (uintptr_t)sig_g_curpaintbuffer.sig_addr + 2 );

			// mov g_currearpaintbuffer, edx
			// "\x89\x15\xCC\xCC\xCC\xCC\x89\x0D\xCC\xCC\xCC\xCC\x38\x48\x02", "xx????xx????xxx"
			CSigScan sig_g_currearpaintbuffer;
			sig_g_currearpaintbuffer.Init( "\x89\x15\xCC\xCC\xCC\xCC\x89\x0D\xCC\xCC\xCC\xCC\x38\x48\x02", "xx????xx????xxx", 15 );
			pg_currearpaintbuffer = *(portable_samplepair_t***)( (uintptr_t)sig_g_currearpaintbuffer.sig_addr + 2 );

			// mov g_curcenterpaintbuffer, ecx
			// "\x89\x15\xCC\xCC\xCC\xCC\x38\x48\x01", "xx????xxx"
			CSigScan sig_g_curcenterpaintbuffer;
			sig_g_curcenterpaintbuffer.Init( "\x89\x0D\xCC\xCC\xCC\xCC\x38\x48\x02", "xx????xxx", 9 );
			pg_curcenterpaintbuffer = *(portable_samplepair_t***)( (uintptr_t)sig_g_curcenterpaintbuffer.sig_addr + 2 );
		}

		// MIX_MixChannelsToPaintbuffer
		{
			// sub ecx, g_paintedtime
			// "\x2B\x0D\xCC\xCC\xCC\xCC\x56", "xx????x"
			CSigScan sig_g_paintedtime;
			sig_g_paintedtime.Init( "\x2B\x0D\xCC\xCC\xCC\xCC\x56", "xx????x", 7 );
			pg_paintedtime = *(int**)( (uintptr_t)sig_g_paintedtime.sig_addr + 2 );
		}

		// S_Update
		{
			// mov ecx, g_AudioDevice
			// "\x8B\x0D\xCC\xCC\xCC\xCC\x8B\x01\x8B\x10\x81\xEC\xB0\x01\x00\x00", "xx????xxxxxxxxxx"
			CSigScan sig_S_Update;
			sig_S_Update.Init( "\x8B\x0D\xCC\xCC\xCC\xCC\x8B\x01\x8B\x10\x81\xEC\xB0\x01\x00\x00", "xx????xxxxxxxxxx", 16 );
			pg_AudioDevice = *(IAudioDevice***)( (uintptr_t)sig_S_Update.sig_addr + 2 );

			// cmp s_bOnLoadScreen, 0
			// "\x80\x3D\xCC\xCC\xCC\xCC\xCC\x5F\x5E\x5D", "xx?????xxx"
			CSigScan sig_s_bOnLoadScreen;
			sig_s_bOnLoadScreen.Init( "\x80\x3D\xCC\xCC\xCC\xCC\xCC\x5F\x5E\x5D", "xx?????xxx", 10 );
			ps_bOnLoadScreen = *(bool**)( (uintptr_t)sig_s_bOnLoadScreen.sig_addr + 2 );

			// fst g_LastSoundFrame ; +2
			// mov edx, [ecx]
			// fstp g_LastMixTime ; +10
			// mov eax, [edx+18h]
			// call eax
			// fmul ds:dword_102FA690
			// fld g_EstFrameTime ; +27
			// "\xDD\x15\xCC\xCC\xCC\xCC\x8B\x11", "xx????xx"
			CSigScan sig_g_LastSoundFrame;
			sig_g_LastSoundFrame.Init( "\xDD\x15\xCC\xCC\xCC\xCC\x8B\x11", "xx????xx", 8 );
			pg_LastSoundFrame = *(double**)( (uintptr_t)sig_g_LastSoundFrame.sig_addr + 2 );
			pg_LastMixTime = *(double**)( (uintptr_t)sig_g_LastSoundFrame.sig_addr + 10 );
			pg_EstFrameTime = *(float**)( (uintptr_t)sig_g_LastSoundFrame.sig_addr + 27 );

			// fld g_DashboardMusicMixValue
			// "\xD9\x05\xCC\xCC\xCC\xCC\xD9\x1C\x24\x51\xD9\x05", "xx????xxxxxx"
			CSigScan sig_g_DashboardMusicMixValue;
			sig_g_DashboardMusicMixValue.Init( "\xD9\x05\xCC\xCC\xCC\xCC\xD9\x1C\x24\x51\xD9\x05", "xx????xxxxxx", 12 );
			pg_DashboardMusicMixValue = *(float**)( (uintptr_t)sig_g_DashboardMusicMixValue.sig_addr + 2 );
			pg_DashboardMusicMixTarget = pg_DashboardMusicMixValue + 1;

			// mov s_bIsListenerUnderwater, cl
			// "\x88\x0D\xCC\xCC\xCC\xCC\xEB\x7E", "xx????xx"
			CSigScan sig_s_bIsListenerUnderwater;
			sig_s_bIsListenerUnderwater.Init( "\x88\x0D\xCC\xCC\xCC\xCC\xEB\x7E", "xx????xx", 8 );
			ps_bIsListenerUnderwater = *(bool**)( (uintptr_t)sig_s_bIsListenerUnderwater.sig_addr + 2 );

			// mov g_snd_trace_count, ebx
			// "\x89\x1D\xCC\xCC\xCC\xCC\xE8\xCC\xCC\xCC\xCC\xE8", "xx????x????x"
			CSigScan sig_g_snd_trace_count;
			sig_g_snd_trace_count.Init( "\x89\x1D\xCC\xCC\xCC\xCC\xE8\xCC\xCC\xCC\xCC\xE8", "xx????x????x", 12 );
			pg_snd_trace_count = *(int**)( (uintptr_t)sig_g_snd_trace_count.sig_addr + 2 );
		}

		// S_SoundList
		{
			// mov ecx, offset s_Sounds
			// "\xB9\xCC\xCC\xCC\xCC\xC7\x44\x24\x04\x00\x00\x00\x00", "x????xxxxxxxx"
			CSigScan sig_s_Sounds;
			sig_s_Sounds.Init( "\xB9\xCC\xCC\xCC\xCC\xC7\x44\x24\x04\x00\x00\x00\x00", "x????xxxxxxxx", 13 );
			ps_Sounds = *(decltype(ps_Sounds)*)( (uintptr_t)sig_s_Sounds.sig_addr + 1 );
		}

		// S_SoundFade
		{
			// movss soundfade.initial_percent, xmm0
			// "\xF3\x0F\x11\x05\xCC\xCC\xCC\xCC\xF3\x0F\x10\x44\x24\x10", "xxxx????xxxxxx"
			CSigScan sig_soundfade;
			sig_soundfade.Init( "\xF3\x0F\x11\x05\xCC\xCC\xCC\xCC\xF3\x0F\x10\x44\x24\x10", "xxxx????xxxxxx", 14 );
			psoundfade = *(soundfade_t**)( (uintptr_t)sig_soundfade.sig_addr + 4 );
		}

		// MXR_LoadAllSoundMixers
		{
			// mov esi, g_cgrouprules
			// imul esi, 74h
			// add esi, offset g_grouprules
			// "\x8B\x35\xCC\xCC\xCC\xCC\x6B\xF6\x74", "xx????xxx"
			CSigScan sig_grouprules;
			sig_grouprules.Init( "\x8B\x35\xCC\xCC\xCC\xCC\x6B\xF6\x74", "xx????xxx", 9 );
			pg_cgrouprules = *(int**)((uintptr_t)sig_grouprules.sig_addr + 2);
			pg_grouprules = *(grouprule_t**)( (uintptr_t)sig_grouprules.sig_addr + 11 );
		}

		// MXR_UpdateAllDuckerVolumes
		{
			// mov eax, [ecx + 0B4h]
			// test eax, eax
			// jl short loc_101BF78C
			// mov eax, g_mapMixgroupidToGrouprulesid[eax * 4]
			// "\x8B\x81\xB4\x00\x00\x00\x85\xC0\x7C\x31", "xxxxxxxxxx"
			CSigScan sig_g_mapMixgroupidToGrouprulesid;
			sig_g_mapMixgroupidToGrouprulesid.Init( "\x8B\x81\xB4\x00\x00\x00\x85\xC0\x7C\x31", "xxxxxxxxxx", 10 );
			pg_mapMixgroupidToGrouprulesid = *(int**)( (uintptr_t)sig_g_mapMixgroupidToGrouprulesid.sig_addr + 13 );

			// movss xmm2, g_DuckScale
			// "\xF3\x0F\x10\x15\xCC\xCC\xCC\xCC\xF3\x0F\x10\x0D\xCC\xCC\xCC\xCC\xBA", "xxxx????xxxx????x"
			CSigScan sig_g_DuckScale;
			sig_g_DuckScale.Init( "\xF3\x0F\x10\x15\xCC\xCC\xCC\xCC\xF3\x0F\x10\x0D\xCC\xCC\xCC\xCC\xBA", "xxxx????xxxx????x", 17 );
			pg_DuckScale = *(float**)( (uintptr_t)sig_g_DuckScale.sig_addr + 12 );
		}

		// SND_StealDynamicChannel
		{
			// subss xmm0, listener_origin.x
			// "\xF3\x0F\x5C\x05\xCC\xCC\xCC\xCC\x0F\x28\xE2", "xxxx????xxx"
			CSigScan sig_listener_origin;
			sig_listener_origin.Init( "\xF3\x0F\x5C\x05\xCC\xCC\xCC\xCC\x0F\x28\xE2", "xxxx????xxx", 11 );
			plistener_origin = *(Vector**)( (uintptr_t)sig_listener_origin.sig_addr + 4 );
			plistener_forward = plistener_origin + 1;
			plistener_right = plistener_origin + 2;
			plistener_up = plistener_origin + 3;
		}

		// S_StartDynamicSound
		{
			// mov ecx, g_pSoundServices
			// "\x8B\x0D\xCC\xCC\xCC\xCC\x8B\x11\x8B\x42\x10\xFF\xD0\x8B\x4F\x34\x8B\x57\x0C\x8B\x47\x08\x83\xEC\x10", "xx????xxxxxxxxxxxxxxxxxxx"
			CSigScan sig_g_pSoundServices;
			sig_g_pSoundServices.Init( "\x8B\x0D\xCC\xCC\xCC\xCC\x8B\x11\x8B\x42\x10\xFF\xD0\x8B\x4F\x34\x8B\x57\x0C\x8B\x47\x08\x83\xEC\x10", "xx????xxxxxxxxxxxxxxxxxxx", 25 );
			g_pSoundServices = **(ISoundServices***)( (uintptr_t)sig_g_pSoundServices.sig_addr + 2 );
		}

		// S_StopAllSounds
		{
			// mov total_channels, 24
			// (24 = MAX_DYNAMIC_CHANNELS)
			// "\xC7\x05\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xE8\xCC\xCC\xCC\xCC\xA1\xCC\xCC\xCC\xCC\x85\xC0\x8B\xD8\x89\x5C\x24\x08", "xx????????x????x????xxxxxxxx"
			CSigScan sig_MAX_DYNAMIC_CHANNELS;
			sig_MAX_DYNAMIC_CHANNELS.Init( "\xC7\x05\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xCC\xE8\xCC\xCC\xCC\xCC\xA1\xCC\xCC\xCC\xCC\x85\xC0\x8B\xD8\x89\x5C\x24\x08", "xx????????x????x????xxxxxxxx", 28 );

			// push 38912
			// (38912 = MAX_CHANNELS * sizeof(channel_t))
			// "\x7C\xC4\x5E\x5D\x68\x00\x98\x00\x00", "xxxxxxxxx"
			CSigScan sig_MAX_CHANNELS;
			sig_MAX_CHANNELS.Init( "\x7C\xC4\x5E\x5D\x68\x00\x98\x00\x00", "xxxxxxxxx", 9 );

			WriteInt( (uintptr_t)sig_MAX_DYNAMIC_CHANNELS.sig_addr + 6, MAX_DYNAMIC_CHANNELS );
			WriteInt( (uintptr_t)sig_MAX_CHANNELS.sig_addr + 5, MAX_CHANNELS * sizeof( channel_t ) );
		}

		// SND_PickStaticChannel
		{
			CSigScan sig_total_channels;
			sig_total_channels.Init( "\x8B\x0D\xCC\xCC\xCC\xCC\xB8\x18\x00\x00\x00", "xx????xxxxx", 11 );
			ptotal_channels = *(int**)( (uintptr_t)sig_total_channels.sig_addr + 2 );
		}

		SETUP_HOOK( MIX_PaintChannels, "\x81\xEC\xA0\x01\x00", "xxxxx" );
		SETUP_HOOK( MIX_BuildChannelList, "\x81\xEC\x90\x06\x00\x00", "xxxxxx" );
		SETUP_HOOK( S_AlterChannel, "\x81\xEC\x8C\x01\x00\x00\x53\x56", "xxxxxxxx" );
		SETUP_HOOK( S_StopSound, "\x81\xEC\x88\x01\x00\x00\x56", "xxxxxxx" );
		SETUP_HOOK( SND_PickStaticChannel, "\x8B\x0D\xCC\xCC\xCC\xCC\xB8\x18\x00\x00\x00", "xx????xxxxx" );
		SETUP_HOOK( SND_StealDynamicChannel, "\x83\xEC\x7C\x53\x55\x33\xDB", "xxxxxxx" );
		SETUP_HOOK( S_FindChannelByGuid, "\x81\xEC\x88\x01\x00\x00\xA1", "xxxxxxx" );
		SETUP_HOOK( MXR_UpdateAllDuckerVolumes, "\xA1\xCC\xCC\xCC\xCC\x81\xEC\x90\x01\x00\x00", "x????xxxxxx" );
		SETUP_HOOK( snd_dumpclientsounds, "\x55\x8B\xEC\x83\xE4\xC0\x81\xEC\xB0\x01\x00\x00", "xxxxxxxxxxxx" );
		SETUP_HOOK( MXR_DebugShowMixVolumes, "\xA1\xCC\xCC\xCC\xCC\x81\xEC\x88\x05\x00\x00", "x????xxxxxx" );
		SETUP_HOOK( S_StopAllSounds, "\x81\xEC\x88\x01\x00\x00\xB9", "xxxxxxx" );
		SETUP_HOOK( S_GetActiveSounds, "\x81\xEC\xBC\x01\x00\x00", "xxxxxx" );
		SETUP_HOOK( S_Update, "\x8B\x0D\xCC\xCC\xCC\xCC\x8B\x01\x8B\x10\x81\xEC\xB0\x01\x00\x00", "xx????xxxxxxxxxx" );
		SETUP_HOOK( S_StartStaticSound, "\x55\x8B\xEC\x83\xE4\xC0\x81\xEC\x34\x03\x00\x00", "xxxxxxxxxxxx" );
		SETUP_HOOK( CEngineSoundClient_StopAllSounds, "\x81\xEC\x90\x01\x00\x00\xB9", "xxxxxxx" );
		SETUP_HOOK( S_FreeChannel, "\x56\x8B\x74\x24\x08\x8A\x86\x2D\x01\x00\x00", "xxxxxxxxxxx" );
		SETUP_HOOK( MIX_MixChannelsToPaintbuffer, "\xB8\x44\xAC\x00\x00\x99", "xxxxxx" );
		
		// SND_PickStaticChannel
		{
			// add eax, offset channels
			// "\x05\xCC\xCC\xCC\xCC\xC3\x8B\xC1", "x????xxx"
			CSigScan sig_channels;
			sig_channels.Init( "\x05\xCC\xCC\xCC\xCC\xC3\x8B\xC1", "x????xxx", 8 );
			pchannels = *(channel_t**)( (uintptr_t)sig_channels.sig_addr + 1 );

			// copy data over! ~~hopefully the channels havent been touched by another thread~~
			// (it shouldn't be modified now, as i added a mutex lock as there was a chance for it to happen before)
			memcpy( channels, pchannels, sizeof( *pchannels ) * OLD_MAX_CHANNELS );

			FindPointerAndReplaceWith( "engine.dll", (uintptr_t)pchannels, (uintptr_t)( &channels ) );
		}

		// SND_ActivateChannel
		{
			// mov eax, g_ActiveChannels.m_count
			// "\xA1\xCC\xCC\xCC\xCC\x83\xC0\x01\xA3\xCC\xCC\xCC\xCC\x66\x89\x86\x1C\x01\x00\x00", "x????xxxx????xxxxxxx"
			CSigScan sig_g_ActiveChannels;
			sig_g_ActiveChannels.Init( "\xA1\xCC\xCC\xCC\xCC\x83\xC0\x01\xA3\xCC\xCC\xCC\xCC\x66\x89\x86\x1C\x01\x00\x00", "x????xxxx????xxxxxxx", 20 );
			pg_ActiveChannels = *(CActiveChannels<OLD_MAX_CHANNELS>**)( (uintptr_t)sig_g_ActiveChannels.sig_addr + 1 );
			
			// copy data over! ~~hopefully the channels havent been touched by another thread~~
			// (it shouldn't be modified now, as i added a mutex lock as there was a chance for it to happen before)
			memcpy( g_ActiveChannels.m_list, pg_ActiveChannels->m_list, sizeof( pg_ActiveChannels->m_list ) );
			g_ActiveChannels.m_count = pg_ActiveChannels->m_count;

			FindPointerAndReplaceWith( "engine.dll", (uintptr_t)&( pg_ActiveChannels->m_count ), (uintptr_t)&( g_ActiveChannels.m_count ) );
			FindPointerAndReplaceWith( "engine.dll", (uintptr_t)&( pg_ActiveChannels->m_list ), (uintptr_t)&( g_ActiveChannels.m_list ) );
		}

		for ( safetyhook::InlineHook& hook : o_Hooks )
			std::ignore = hook.enable();
	}

	// Fully initialized!
	ConColorMsg(Color(0, 122, 122), "Initialized P3SoundChannelsExpanded plugin.\n");
	
	return true;
}

void CEmptyServerPlugin::Unload( void )
{
	
}