//========= Copyright © 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: Portable code to mix sounds for snd_dma.cpp.
//
//=============================================================================//

#include "../sdk/icliententitylist.h"
#include "../sdk/icliententity.h"
#include "../sdk/icommandline.h"
#include "../sdk/soundservice.h"
#include "../sdk/snd_mix_buf.h"
#include "../sdk/snd_device.h"
#include "../sdk/mouthinfo.h"
#include "../sdk/cdll_int.h"
#include "../sdk/icvar.h"

#include "common.h"
#include "snd_channels.h"
#include <algorithm>

extern ICvar* cvar;
extern ISoundServices* g_pSoundServices;
extern IAudioDevice** pg_AudioDevice;

extern int* ptotal_channels;
extern Vector* plistener_origin;
extern Vector* plistener_forward;
extern Vector* plistener_right;
extern Vector* plistener_up;
extern int* pg_paintedtime;
extern bool* pg_bDspOff;
extern paintbuffer_t** pg_paintBuffers;
extern portable_samplepair_t** pg_curpaintbuffer;
extern portable_samplepair_t** pg_currearpaintbuffer;
extern portable_samplepair_t** pg_curcenterpaintbuffer;
extern bool* pg_bdirectionalfx;
extern int* pg_snd_profile_type;
extern float* pg_dsp_volume;
extern int* pidsp_facingaway;
extern int* pidsp_speaker;
extern int* pidsp_automatic;
extern int* pidsp_room;
extern int* pidsp_player;
extern int* pidsp_water;
extern int* pidsp_spatial;

extern channel_t channels[MAX_CHANNELS];
extern CActiveChannels<MAX_CHANNELS> g_ActiveChannels;

extern CAudioSource* (*S_LoadSound)(CSfxTable* pSfx, channel_t* ch);
extern void (*S_SyncClockAdjust)();
extern void (*MIX_MixPaintbuffers)(int ibuf1, int ibuf2, int ibuf3, int count, float fgain_out);
extern void (*MXR_SetCurrentSoundMixer)(const char* szsoundmixer);
extern void (*CheckNewDspPresets)();
extern void (*MIX_ScalePaintBuffer)(int bufferIndex, int count, float fgain);
extern bool (*DSP_RoomDSPIsOff)();

#define IPAINTBUFFER		0
#define IROOMBUFFER			1
#define IFACINGBUFFER		2
#define IFACINGAWAYBUFFER	3
#define IDRYBUFFER			4
#define ISPEAKERBUFFER		5

#define FILTERTYPE_NONE		0
#define FILTERTYPE_LINEAR	1
#define FILTERTYPE_CUBIC	2

// from snd_dma, don't think it's ever not 1.0;
extern float	DSP_ROOM_MIX;
extern float	DSP_NOROOM_MIX;

// Helper class for determining whether a given channel number should be culled from
// mixing, if snd_cull_duplicates is enabled (psychoacoustic quashing).
class CChannelCullList
{
public:
	// default constructor
	CChannelCullList() : m_numChans( 0 ) {};

	// call if you plan on culling channels - and not otherwise, it's a little expensive
	// (that's why it's not in the constructor)
	void Initialize( CChannelList& list );

	// returns true if a given channel number has been marked for culling
	inline bool ShouldCull( int channelNum )
	{
		return (m_numChans > channelNum) ? m_bShouldCull[channelNum] : false;
	}

	// an array of sound names and their volumes
	// TODO: there may be a way to do this faster on 360 (eg, pad to 128bit, use SIMD)
	struct sChannelVolData
	{
		int m_channelNum;
		int m_vol; // max volume of sound. -1 means "do not cull, ever, do not even do the math"
		unsigned int m_nameHash; // a unique id for a sound file
	};
protected:
	sChannelVolData m_channelInfo[MAX_CHANNELS];

	bool m_bShouldCull[MAX_CHANNELS]; // in ChannelList order, not sorted order
	int m_numChans;
};

// comparator for qsort as used below (eg a lambda)
// returns < 0 if a should come before b, > 0 if a should come after, 0 otherwise
static int __cdecl ChannelVolComparator( const void* a, const void* b )
{
	// greater numbers come first.
	return static_cast<const CChannelCullList::sChannelVolData*>(b)->m_vol - static_cast<const CChannelCullList::sChannelVolData*>(a)->m_vol;
}

void CChannelCullList::Initialize( CChannelList& list )
{
	// First, build a sorted list of channels by decreasing volume, and by a hash of their wavname.
	m_numChans = list.Count();

	for ( int i = m_numChans - 1; i >= 0; --i )
	{
		channel_t* ch = list.GetChannel( i );
		m_channelInfo[i].m_channelNum = i;
		if ( ch && ch->pMixer->IsReadyToMix() )
		{
			m_channelInfo[i].m_vol = ChannelLoudestCurVolume( ch );
			AssertMsg( m_channelInfo[i].m_vol >= 0, "Sound channel has a negative volume?" );
			m_channelInfo[i].m_nameHash = (unsigned int)ch->sfx;
		}
		else
		{
			m_channelInfo[i].m_vol = -1;
			m_channelInfo[i].m_nameHash = NULL; // doesn't matter
		}
	}

	// set the unused channels to invalid data
	for ( int i = m_numChans; i < MAX_CHANNELS; ++i )
	{
		m_channelInfo[i].m_channelNum = -1;
		m_channelInfo[i].m_vol = -1;
	}

	// Sort the list.
	qsort( m_channelInfo, MAX_CHANNELS, sizeof( sChannelVolData ), ChannelVolComparator );

	// Then, determine if the given sound is less than the nth loudest of its hash. If so, mark its flag
	// for removal.
	// TODO: use an actual algorithm rather than this bogus quadratic technique.
	// (I'm using it for now because we don't have convenient/fast hash table 
	// classes, which would be the linear-time way to deal with this).
	static ConVar* snd_cull_duplicates = cvar->FindVar( "snd_cull_duplicates" );
	const int cutoff = snd_cull_duplicates->GetInt();
	for ( int i = 0; i < m_numChans; ++i ) // i is index in original channel list
	{
		channel_t* ch = list.GetChannel( i );
		// for each sound, determine where it ranks in loudness
		int howManyLouder = 0;
		for ( int j = 0;
			m_channelInfo[j].m_channelNum != i && m_channelInfo[j].m_vol >= 0 && j < MAX_CHANNELS;
			++j )
		{
			// j steps through the sorted list until we find ourselves:
			if ( m_channelInfo[j].m_nameHash == (unsigned int)(ch->sfx) )
			{
				// that's another channel playing this sound but louder than me
				++howManyLouder;
			}
		}
		if ( howManyLouder >= cutoff )
		{
			// this sound should be culled
			m_bShouldCull[i] = true;
		}
		else
		{
			// this sound should not be culled
			m_bShouldCull[i] = false;
		}
	}
}


//===============================================================================
// Client entity mouth movement code.  Set entity mouthopen variable, based
// on the sound envelope of the voice channel playing.
// KellyB 10/22/97
//===============================================================================
// 
// called when voice channel is first opened on this entity
static CMouthInfo* GetMouthInfoForChannel( channel_t* pChannel )
{
	int mouthentity = pChannel->speakerentity == -1 ? pChannel->soundsource : pChannel->speakerentity;

	IClientEntity* pClientEntity = entitylist->GetClientEntity( mouthentity );

	if ( !pClientEntity )
		return NULL;

	return pClientEntity->GetMouth();
}

// called when channel stops

void SND_CloseMouth( channel_t* pChannel )
{
	if ( SND_IsMouth( pChannel ) )
	{
		CMouthInfo* pMouth = GetMouthInfoForChannel( pChannel );
		if ( pMouth )
		{
			// shut mouth
			int idx = pMouth->GetIndexForSource( pChannel->sfx->pSource );

			if ( idx != UNKNOWN_VOICE_SOURCE )
			{
				pMouth->RemoveSourceByIndex( idx );
			}
			else
			{
				pMouth->ClearVoiceSources();
			}
			pMouth->mouthopen = 0;
		}
	}
}

/*
===============================================================================

CHANNEL MIXING

===============================================================================
*/


// free channel so that it may be allocated by the
// next request to play a sound.  If sound is a 
// word in a sentence, release the sentence.
// Works for static, dynamic, sentence and stream sounds

void S_FreeChannel( channel_t* ch )
{
	// Don't reenter in here (can happen inside voice code).
	if ( ch->flags.m_bIsFreeingChannel )
		return;
	ch->flags.m_bIsFreeingChannel = true;

	SND_CloseMouth( ch );

	g_pSoundServices->OnSoundStopped( ch->guid, ch->soundsource, ch->entchannel, ch->sfx->getname() );

	ch->flags.isSentence = false;
	//	Msg("End sound %s\n", ch->sfx->getname() );

	delete ch->pMixer;
	ch->pMixer = NULL;
	ch->sfx = NULL;

	// zero all data in channel
	g_ActiveChannels.Remove( ch );
	memset( ch, 0, sizeof( channel_t ) );
}

#define CAVGSAMPLES 10
void SND_MoveMouth8( channel_t* ch, CAudioSource* pSource, int count )
{
	int 	data;
	char* pdata = NULL;
	int		i;
	int		savg;
	int		scount;

	CMouthInfo* pMouth = GetMouthInfoForChannel( ch );

	if ( !pMouth )
		return;

	if ( pSource->GetSentence() )
	{
		int idx = pMouth->GetIndexForSource( pSource );

		if ( idx == UNKNOWN_VOICE_SOURCE )
		{
			if ( pMouth->AddSource( pSource, ch->flags.m_bIgnorePhonemes ) == NULL )
			{
				DevMsg( 1, "out of voice sources, won't lipsync %s\n", ch->sfx->getname() );
#if 0
				for ( int i = 0; i < pMouth->GetNumVoiceSources(); i++ )
				{
					CVoiceData* pVoice = pMouth->GetVoiceSource( i );
					CAudioSourceWave* pWave = dynamic_cast<CAudioSourceWave*>( pVoice->m_pAudioSource );
					const char* pName = "unknown";
					if ( pWave && pWave->GetName() )
						pName = pWave->GetName();
					Msg( "Playing %s...\n", pName );
				}
#endif
			}
		}
		else
		{
			// Update elapsed time from mixer
			CVoiceData* vd = pMouth->GetVoiceSource( idx );
			Assert( vd );
			if ( vd )
			{
				Assert( pSource->SampleRate() > 0 );

				float elapsed = (float)ch->pMixer->GetSamplePosition() / (float)pSource->SampleRate();

				vd->SetElapsedTime( elapsed );
			}
		}
	}

	if ( pMouth->NeedsEnvelope() )
	{
		int availableSamples = pSource->GetOutputData( (void**)&pdata, ch->pMixer->GetSamplePosition(), count, NULL );

		if ( pdata == NULL )
			return;

		i = 0;
		scount = pMouth->sndcount;
		savg = 0;

		while ( i < availableSamples && scount < CAVGSAMPLES )
		{
			data = pdata[i];
			savg += abs( data );

			i += 80 + ((byte)data & 0x1F);
			scount++;
		}

		pMouth->sndavg += savg;
		pMouth->sndcount = (byte)scount;

		if ( pMouth->sndcount >= CAVGSAMPLES )
		{
			pMouth->mouthopen = pMouth->sndavg / CAVGSAMPLES;
			pMouth->sndavg = 0;
			pMouth->sndcount = 0;
		}
	}
	else
	{
		pMouth->mouthopen = 0;
	}
}

// Mix all channels into active paintbuffers until paintbuffer is full or 'endtime' is reached.
// endtime: time in 44khz samples to mix
// rate: ignore samples which are not natively at this rate (for multipass mixing/filtering)
//		 if rate == SOUND_ALL_RATES then mix all samples this pass
// flags: if SOUND_MIX_DRY, then mix only samples with channel flagged as 'dry'
// outputRate: target mix rate for all samples.  Note, if outputRate = SOUND_DMA_SPEED, then
//		 this routine will fill the paintbuffer to endtime.  Otherwise, fewer samples are mixed.
//		 if (endtime - paintedtime) is not aligned on boundaries of 4, 
//		 we'll miss data if outputRate < SOUND_DMA_SPEED!
void MIX_MixChannelsToPaintbuffer( CChannelList& list, int endtime, int flags, int rate, int outputRate )
{
	int		i;
	int		sampleCount;

	// mix each channel into paintbuffer
	// validate parameters
	Assert( outputRate <= SOUND_DMA_SPEED );
	Assert( !((endtime - (*pg_paintedtime)) & 0x3) || (outputRate == SOUND_DMA_SPEED) ); // make sure we're not discarding data

	// 44k: try to mix this many samples at outputRate
	sampleCount = (endtime - (*pg_paintedtime)) / (SOUND_DMA_SPEED / outputRate);
	if ( sampleCount <= 0 )
		return;

	for ( i = list.Count(); --i >= 0; )
	{
		channel_t* ch = list.GetChannel( i );
		Assert( ch->sfx );
		// must never have a 'dry' and 'speaker' set - causes double mixing & double data reading
		Assert( !(ch->flags.bdry && ch->flags.bSpeaker) );

		// if mixing with SOUND_MIX_DRY flag, ignore (don't even load) all channels not flagged as 'dry'
		if ( flags == SOUND_MIX_DRY )
		{
			if ( !ch->flags.bdry )
				continue;
		}

		// if mixing with SOUND_MIX_WET flag, ignore (don't even load) all channels flagged as 'dry' or 'speaker'
		if ( flags == SOUND_MIX_WET )
		{
			if ( ch->flags.bdry || ch->flags.bSpeaker )
				continue;
		}

		// if mixing with SOUND_MIX_SPEAKER flag, ignore (don't even load) all channels not flagged as 'speaker'
		if ( flags == SOUND_MIX_SPEAKER )
		{
			if ( !ch->flags.bSpeaker )
				continue;
		}

		// multipass mixing - only mix samples of specified sample rate
		switch ( rate )
		{
		case SOUND_11k:
		case SOUND_22k:
		case SOUND_44k:
			if ( rate != ch->sfx->pSource->SampleRate() )
				continue;
			break;
		default:
		case SOUND_ALL_RATES:
			break;
		}

		bool bIsMouth = SND_IsMouth( ch );
		if ( bIsMouth && g_pSoundServices->IsGamePaused() )
		{
			continue;
		}

		if ( bIsMouth )
		{
			if ( entitylist->GetClientEntity( ch->soundsource ) ||
				(ch->flags.bSpeaker && entitylist->GetClientEntity( ch->speakerentity )) )
			{
				// UNDONE: recode this as a member function of CAudioMixer
				SND_MoveMouth8( ch, ch->sfx->pSource, sampleCount );
			}
		}

		// mix channel to all active paintbuffers:
		// mix 'dry' sounds only to dry paintbuffer.
		// mix 'speaker' sounds only to speaker paintbuffer.
		// mix all other sounds between room, facing & facingaway paintbuffers
		// NOTE: must be called once per channel only - consecutive calls retrieve additional data.
		if ( list.IsQuashed( i ) )
		{
			// If the sound has been silenced as a performance heuristic, quash it.
			ch->pMixer->SkipSamples( ch, sampleCount, outputRate, 0 );
			// DevMsg("Quashed channel %d (%s)\n", i, ch->sfx->GetFileName());
		}
		else
		{
			ch->pMixer->MixDataToDevice( (*pg_AudioDevice), ch, sampleCount, outputRate, 0 );
		}

		if ( !ch->pMixer->ShouldContinueMixing() )
		{
			S_FreeChannel( ch );
			list.RemoveChannelFromList( i );
		}
	}
}

//-----------------------------------------------------------------------------
// Purpose: 
// Input  : *pChannel - 
// Output : Returns true on success, false on failure.
//-----------------------------------------------------------------------------
bool SND_ShouldPause( channel_t* pChannel )
{
	return pChannel->flags.m_bShouldPause;
}

// build a list of channels that will actually do mixing in this update
// remove all active channels that won't mix for some reason
void MIX_BuildChannelList( CChannelList& list )
{
	static ConVar* snd_cull_duplicates = cvar->FindVar( "snd_cull_duplicates" );

	g_ActiveChannels.GetActiveChannels( list );
	list.m_hasDryChannels = false;
	list.m_hasSpeakerChannels = false;
	list.m_has11kChannels = false;
	list.m_has22kChannels = false;
	list.m_has44kChannels = false;
	bool delayStart = false;
	bool bPaused = g_pSoundServices->IsGamePaused();

	CChannelCullList cullList;
	if ( snd_cull_duplicates->GetInt() > 0 )
	{
		cullList.Initialize( list );
	}

	// int numQuashed = 0;
	for ( int i = list.Count(); --i >= 0; )
	{
		channel_t* ch = list.GetChannel( i );
		bool bRemove = false;
		// Certain async loaded sounds lazily load into memory in the background, use this to determine
		//  if the sound is ready for mixing
		CAudioSource* pSource = NULL;
		if ( ch->pMixer->IsReadyToMix() )
		{
			pSource = S_LoadSound( ch->sfx, ch );

			// Don't mix sound data for sounds with 'zero' volume. If it's a non-looping sound, 
			// just remove the sound when its volume goes to zero. If it's a 'dry' channel sound (ie: music)
			// then assume bZeroVolume is fade in - don't restart

			// To be 'zero' volume, all target volume and current volume values must all be less than 5

			bool bZeroVolume = BChannelLowVolume( ch, 1 );

			if ( !pSource || (bZeroVolume && !pSource->IsLooped() && !ch->flags.bdry) )
			{
				// NOTE: Since we've loaded the sound, check to see if it's a sentence.  Play them at zero anyway
				// to keep the character's lips moving and the captions happening.
				if ( !pSource || pSource->GetSentence() == NULL )
				{
					S_FreeChannel( ch );
					bRemove = true;
				}
			}
			else if ( bZeroVolume )
			{
				bRemove = true;
			}
			// If the sound wants to stop when the game pauses, do so
			if ( bPaused && SND_ShouldPause( ch ) )
			{
				bRemove = true;
			}
			// On lowend, aggressively cull duplicate sounds.
			if ( !bRemove && snd_cull_duplicates->GetInt() > 0 )
			{
				// We can't simply remove them, because then sounds will pile up waiting to finish later.
				// We need to flag them for not mixing.
				list.m_quashed[i] = cullList.ShouldCull( i );
				/*
				if (list.m_quashed[i])
				{
					numQuashed++;
					// Msg("removed %i\n", i);
				}
				*/
			}
			else
			{
				list.m_quashed[i] = false;
			}
		}
		else
		{
			bRemove = true;
		}

		if ( bRemove )
		{
			list.RemoveChannelFromList( i );
			continue;
		}
		if ( ch->flags.bSpeaker )
		{
			list.m_hasSpeakerChannels = true;
		}
		if ( ch->flags.bdry )
		{
			list.m_hasDryChannels = true;
		}
		int rate = pSource->SampleRate();
		if ( rate == SOUND_11k )
		{
			list.m_has11kChannels = true;
		}
		else if ( rate == SOUND_22k )
		{
			list.m_has22kChannels = true;
		}
		else if ( rate == SOUND_44k )
		{
			list.m_has44kChannels = true;
		}
		if ( ch->flags.delayed_start && !SND_IsMouth( ch ) )
			delayStart = true;

		// get playback pitch
		ch->pitch = ch->pMixer->ModifyPitch( ch->basePitch * 0.01f );
	}
	// DevMsg( "%d channels quashed.\n", numQuashed );

	// Huh? For some reason Postal 3 calls this unconditionally, whereas Source normally doesn't.
	//if ( !delayStart || bPaused || (*phost_frametime_unbounded > *phost_frametime) )
	{
		S_SyncClockAdjust();
	}
}

void SND_InitMouth( channel_t* pChannel )
{
	if ( SND_IsMouth( pChannel ) )
	{
		CMouthInfo* pMouth = GetMouthInfoForChannel( pChannel );
		// init mouth movement vars
		if ( pMouth )
		{
			pMouth->mouthopen = 0;
			pMouth->sndavg = 0;
			pMouth->sndcount = 0;
			if ( pChannel->sfx->pSource && pChannel->sfx->pSource->GetSentence() )
			{
				pMouth->AddSource( pChannel->sfx->pSource, pChannel->flags.m_bIgnorePhonemes );
			}
		}
	}
}

inline void MIX_ResetPaintbufferFilterCounters( void )
{
	int i;
	for ( i = 0; i < CPAINTBUFFERS; i++ )
		(*pg_paintBuffers)[i].ifilter = 0;
}

inline void MIX_ResetPaintbufferFilterCounter( int ipaintbuffer )
{
	Assert( ipaintbuffer < CPAINTBUFFERS );
	(*pg_paintBuffers)[ipaintbuffer].ifilter = 0;
}

inline void MIX_DeactivateAllPaintbuffers( void )
{
	int i;
	for ( i = 0; i < CPAINTBUFFERS; i++ )
		(*pg_paintBuffers)[i].factive = false;
}

inline void MIX_ActivatePaintbuffer( int ipaintbuffer )
{
	Assert( ipaintbuffer < CPAINTBUFFERS );
	(*pg_paintBuffers)[ipaintbuffer].factive = true;
}

// return index to current paintbuffer
inline int MIX_GetCurrentPaintbufferIndex( void )
{
	int i;

	for ( i = 0; i < CPAINTBUFFERS; i++ )
	{
		if ( (*pg_curpaintbuffer) == (*pg_paintBuffers)[i].pbuf )
			return i;
	}

	return 0;
}

// return pointer to current paintbuffer struct
inline paintbuffer_t* MIX_GetCurrentPaintbufferPtr( void )
{
	int ipaint = MIX_GetCurrentPaintbufferIndex();

	Assert( ipaint < CPAINTBUFFERS );

	return &(*pg_paintBuffers)[ipaint];
}

// return pointer to front paintbuffer pbuf, given index
inline portable_samplepair_t* MIX_GetPFrontFromIPaint( int ipaintbuffer )
{
	return (*pg_paintBuffers)[ipaintbuffer].pbuf;
}

inline paintbuffer_t* MIX_GetPPaintFromIPaint( int ipaint )
{
	Assert( ipaint < CPAINTBUFFERS );

	return &(*pg_paintBuffers)[ipaint];
}

// return pointer to rear buffer, given index.
// returns null if fsurround is false;
inline portable_samplepair_t* MIX_GetPRearFromIPaint( int ipaintbuffer )
{
	if ( (*pg_paintBuffers)[ipaintbuffer].fsurround )
		return (*pg_paintBuffers)[ipaintbuffer].pbufrear;

	return NULL;
}

// return pointer to center buffer, given index.
// returns null if fsurround_center is false;
inline portable_samplepair_t* MIX_GetPCenterFromIPaint( int ipaintbuffer )
{
	if ( (*pg_paintBuffers)[ipaintbuffer].fsurround_center )
		return (*pg_paintBuffers)[ipaintbuffer].pbufcenter;

	return NULL;
}

// return index to paintbuffer, given buffer pointer
inline int MIX_GetIPaintFromPFront( portable_samplepair_t* pbuf )
{
	int i;

	for ( i = 0; i < CPAINTBUFFERS; i++ )
	{
		if ( pbuf == (*pg_paintBuffers)[i].pbuf )
			return i;
	}

	return 0;
}

// return pointer to paintbuffer struct, given ptr to buffer data
inline paintbuffer_t* MIX_GetPPaintFromPFront( portable_samplepair_t* pbuf )
{
	int i;
	i = MIX_GetIPaintFromPFront( pbuf );

	return &(*pg_paintBuffers)[i];
}

#define DSP_AUTOMATIC	1		// corresponds to Generic preset
bool DSP_CheckDspAutoEnabled( void )
{
	static ConVar* dsp_room = cvar->FindVar( "dsp_room" );
	return (dsp_room->GetInt() == DSP_AUTOMATIC);
}

int Get_idsp_room( void )
{
	// if dsp_automatic is not enabled, get room
	if ( !DSP_CheckDspAutoEnabled() )
		return (*pidsp_room);

	// automatic room detection is on, return dsp_automatic preset instead of dsp_room preset
	return (*pidsp_automatic);
}

// Set current paintbuffer to pbuf.  
// The set paintbuffer is used by all subsequent mixing, upsampling and dsp routines.
// Also sets the rear paintbuffer if paintbuffer has fsurround true.
// (otherwise, rearpaintbuffer is NULL)

void MIX_SetCurrentPaintbuffer( int ipaintbuffer )
{
	// set front and rear paintbuffer

	Assert( ipaintbuffer < CPAINTBUFFERS );

	(*pg_curpaintbuffer) = (*pg_paintBuffers)[ipaintbuffer].pbuf;

	if ( (*pg_paintBuffers)[ipaintbuffer].fsurround )
	{
		(*pg_currearpaintbuffer) = (*pg_paintBuffers)[ipaintbuffer].pbufrear;

		(*pg_curcenterpaintbuffer) = NULL;

		if ( (*pg_paintBuffers)[ipaintbuffer].fsurround_center )
			(*pg_curcenterpaintbuffer) = (*pg_paintBuffers)[ipaintbuffer].pbufcenter;
	}
	else
	{
		(*pg_currearpaintbuffer) = NULL;
		(*pg_curcenterpaintbuffer) = NULL;
	}

	Assert( (*pg_curpaintbuffer) != NULL );
}

void MIX_CompressPaintbuffer( int ipaint, int count )
{
	//VPROF( "CompressPaintbuffer" );
	int i;
	paintbuffer_t* ppaint = MIX_GetPPaintFromIPaint( ipaint );
	portable_samplepair_t* pbf;
	portable_samplepair_t* pbr;
	portable_samplepair_t* pbc;

	pbf = ppaint->pbuf;
	pbr = ppaint->pbufrear;
	pbc = ppaint->pbufcenter;

	for ( i = 0; i < count; i++ )
	{
		pbf->left = CLIP( pbf->left );
		pbf->right = CLIP( pbf->right );
		pbf++;
	}

	if ( ppaint->fsurround )
	{
		Assert( pbr );

		for ( i = 0; i < count; i++ )
		{
			pbr->left = CLIP( pbr->left );
			pbr->right = CLIP( pbr->right );
			pbr++;
		}
	}

	if ( ppaint->fsurround_center )
	{
		Assert( pbc );

		for ( i = 0; i < count; i++ )
		{
			pbc->left = CLIP( pbc->left );
			//pbc->right = CLIP(pbc->right); mono center channel
			pbc++;
		}
	}
}

inline void MIX_ConvertBufferToSurround( int ipaintbuffer )
{
	paintbuffer_t* ppaint = &(*pg_paintBuffers)[ipaintbuffer];

	// duplicate channel data as needed

	if ( (*pg_AudioDevice)->IsSurround() )
	{
		// set buffer flags

		ppaint->fsurround = (*pg_AudioDevice)->IsSurround();
		ppaint->fsurround_center = (*pg_AudioDevice)->IsSurroundCenter();

		portable_samplepair_t* pfront = MIX_GetPFrontFromIPaint( ipaintbuffer );
		portable_samplepair_t* prear = MIX_GetPRearFromIPaint( ipaintbuffer );
		portable_samplepair_t* pcenter = MIX_GetPCenterFromIPaint( ipaintbuffer );

		// copy front to rear
		memcpy( prear, pfront, sizeof( portable_samplepair_t ) * PAINTBUFFER_SIZE );

		// copy front to center
		if ( (*pg_AudioDevice)->IsSurroundCenter() )
			memcpy( pcenter, pfront, sizeof( portable_samplepair_t ) * PAINTBUFFER_SIZE );
	}
}

// mix and upsample channels to 44khz 'ipaintbuffer'
// mix channels matching 'flags' (SOUND_MIX_DRY, SOUND_MIX_WET, SOUND_MIX_SPEAKER) into specified paintbuffer
// upsamples 11khz, 22khz channels to 44khz.

// NOTE: only call this on channels that will be mixed into only 1 paintbuffer
// and that will not be mixed until the next mix pass! otherwise, MIX_MixChannelsToPaintbuffer
// will advance any internal pointers on mixed channels; subsequent calls will be at 
// incorrect offset.

void MIX_MixUpsampleBuffer( CChannelList& list, int ipaintbuffer, int end, int count, int flags )
{
	//VPROF( "MixUpsampleBuffer" );
	int ipaintcur = MIX_GetCurrentPaintbufferIndex(); // save current paintbuffer

	// reset paintbuffer upsampling filter index
	MIX_ResetPaintbufferFilterCounter( ipaintbuffer );

	// prevent other paintbuffers from being mixed
	MIX_DeactivateAllPaintbuffers();

	MIX_ActivatePaintbuffer( ipaintbuffer );			// operates on MIX_MixChannelsToPaintbuffer	
	MIX_SetCurrentPaintbuffer( ipaintbuffer );			// operates on MixUpSample

	// mix 11khz channels to buffer
	if ( list.m_has11kChannels )
	{
		MIX_MixChannelsToPaintbuffer( list, end, flags, SOUND_11k, SOUND_11k );

		// upsample 11khz buffer by 2x
		(*pg_AudioDevice)->MixUpsample( count / (SOUND_DMA_SPEED / SOUND_11k), FILTERTYPE_LINEAR );
	}

	if ( list.m_has22kChannels || list.m_has11kChannels )
	{
		// mix 22khz channels to buffer
		MIX_MixChannelsToPaintbuffer( list, end, flags, SOUND_22k, SOUND_22k );

#if (SOUND_DMA_SPEED > SOUND_22k)
		// upsample 22khz buffer by 2x
		(*pg_AudioDevice)->MixUpsample( count / (SOUND_DMA_SPEED / SOUND_22k), FILTERTYPE_LINEAR );
#endif
	}

	// mix 44khz channels to buffer
	MIX_MixChannelsToPaintbuffer( list, end, flags, SOUND_44k, SOUND_DMA_SPEED );

	MIX_DeactivateAllPaintbuffers();

	// restore previous paintbuffer
	MIX_SetCurrentPaintbuffer( ipaintcur );
}


void MIX_UpsampleAllPaintbuffers( CChannelList& list, int end, int count )
{
	//VPROF( "MixUpsampleAll" );

	// 'dry' and 'speaker' channel sounds mix 100% into their corresponding buffers

	// mix and upsample all 'dry' sounds (channels) to 44khz IDRYBUFFER paintbuffer

	if ( list.m_hasDryChannels )
		MIX_MixUpsampleBuffer( list, IDRYBUFFER, end, count, SOUND_MIX_DRY );

	// mix and upsample all 'speaker' sounds (channels) to 44khz ISPEAKERBUFFER paintbuffer

	if ( list.m_hasSpeakerChannels )
		MIX_MixUpsampleBuffer( list, ISPEAKERBUFFER, end, count, SOUND_MIX_SPEAKER );

	// 'room', 'facing' 'facingaway' sounds are mixed into up to 3 buffers:

	// 11khz sounds are mixed into 3 buffers based on distance from listener, and facing direction
	// These buffers are room, facing, facingaway
	// These 3 mixed buffers are then each upsampled to 22khz.

	// 22khz sounds are mixed into the 3 buffers based on distance from listener, and facing direction
	// These 3 mixed buffers are then each upsampled to 44khz.

	// 44khz sounds are mixed into the 3 buffers based on distance from listener, and facing direction
	MIX_DeactivateAllPaintbuffers();

	// set paintbuffer upsample filter indices to 0
	MIX_ResetPaintbufferFilterCounters();

	if ( !(*pg_bDspOff) )
	{
		// only mix to roombuffer if dsp fx are on KDB: perf
		MIX_ActivatePaintbuffer( IROOMBUFFER );					// operates on MIX_MixChannelsToPaintbuffer
	}

	MIX_ActivatePaintbuffer( IFACINGBUFFER );

	if ( (*pg_bdirectionalfx) )
	{
		// mix to facing away buffer only if directional presets are set

		MIX_ActivatePaintbuffer( IFACINGAWAYBUFFER );
	}

	// mix 11khz sounds: 
	// pan sounds between 3 busses: facing, facingaway and room buffers

	MIX_MixChannelsToPaintbuffer( list, end, SOUND_MIX_WET, SOUND_11k, SOUND_11k );

	// upsample all 11khz buffers by 2x
	if ( !(*pg_bDspOff) )
	{
		// only upsample roombuffer if dsp fx are on KDB: perf
		MIX_SetCurrentPaintbuffer( IROOMBUFFER );			// operates on MixUpSample
		(*pg_AudioDevice)->MixUpsample( count / (SOUND_DMA_SPEED / SOUND_11k), FILTERTYPE_LINEAR );
	}

	MIX_SetCurrentPaintbuffer( IFACINGBUFFER );
	(*pg_AudioDevice)->MixUpsample( count / (SOUND_DMA_SPEED / SOUND_11k), FILTERTYPE_LINEAR );

	if ( (*pg_bdirectionalfx) )
	{
		MIX_SetCurrentPaintbuffer( IFACINGAWAYBUFFER );
		(*pg_AudioDevice)->MixUpsample( count / (SOUND_DMA_SPEED / SOUND_11k), FILTERTYPE_LINEAR );
	}

	// mix 22khz sounds: 
	// pan sounds between 3 busses: facing, facingaway and room buffers
	MIX_MixChannelsToPaintbuffer( list, end, SOUND_MIX_WET, SOUND_22k, SOUND_22k );

	// upsample all 22khz buffers by 2x
#if ( SOUND_DMA_SPEED > SOUND_22k )
	if ( !(*pg_bDspOff) )
	{
		// only upsample roombuffer if dsp fx are on KDB: perf

		MIX_SetCurrentPaintbuffer( IROOMBUFFER );
		(*pg_AudioDevice)->MixUpsample( count / (SOUND_DMA_SPEED / SOUND_22k), FILTERTYPE_LINEAR );
	}

	MIX_SetCurrentPaintbuffer( IFACINGBUFFER );
	(*pg_AudioDevice)->MixUpsample( count / (SOUND_DMA_SPEED / SOUND_22k), FILTERTYPE_LINEAR );

	if ( (*pg_bdirectionalfx) )
	{
		MIX_SetCurrentPaintbuffer( IFACINGAWAYBUFFER );
		(*pg_AudioDevice)->MixUpsample( count / (SOUND_DMA_SPEED / SOUND_22k), FILTERTYPE_LINEAR );
	}
#endif

	// mix all 44khz sounds to all active paintbuffers
	MIX_MixChannelsToPaintbuffer( list, end, SOUND_MIX_WET, SOUND_44k, SOUND_DMA_SPEED );

	MIX_DeactivateAllPaintbuffers();

	MIX_SetCurrentPaintbuffer( IPAINTBUFFER );
}

void MIX_PaintChannels( int endtime, bool bIsUnderwater )
{
	//VPROF( "MIX_PaintChannels" );
	static ConVar* dsp_enhance_stereo = cvar->FindVar( "dsp_enhance_stereo" );

	int 	end;
	int		count;
	bool	b_spatial_delays = dsp_enhance_stereo->GetInt() != 0 ? true : false;
	bool room_fsurround_sav;
	bool room_fsurround_center_sav;
	paintbuffer_t* proom = MIX_GetPPaintFromIPaint( IROOMBUFFER );

	CheckNewDspPresets();

	static ConVar* snd_soundmixer = cvar->FindVar( "snd_soundmixer" );
	MXR_SetCurrentSoundMixer( snd_soundmixer->GetString() );

	// dsp performance tuning
	static ConVar* snd_profile = cvar->FindVar( "snd_profile" );
	(*pg_snd_profile_type) = snd_profile->GetInt();

	// dsp_off is true if no dsp processing is to run
	// directional dsp processing is enabled if dsp_facingaway is non-zero

	static ConVar* dsp_off = cvar->FindVar( "dsp_off" );
	(*pg_bDspOff) = dsp_off->GetInt() ? 1 : 0;
	CChannelList list;

	MIX_BuildChannelList( list );

	// get master dsp volume 
	static ConVar* dsp_volume = cvar->FindVar( "dsp_volume" );
	(*pg_dsp_volume) = dsp_volume->GetFloat();

	// attenuate master dsp volume by 2,4 or 5 ch settings
	if ( (*pg_AudioDevice)->IsSurround() )
	{
		static ConVar* dsp_vol_5ch = cvar->FindVar( "dsp_vol_5ch" );
		static ConVar* dsp_vol_4ch = cvar->FindVar( "dsp_vol_4ch" );
		(*pg_dsp_volume) *= ((*pg_AudioDevice)->IsSurroundCenter() ? dsp_vol_5ch->GetFloat() : dsp_vol_4ch->GetFloat());
	}
	else
	{
		static ConVar* dsp_vol_2ch = cvar->FindVar( "dsp_vol_2ch" );
		(*pg_dsp_volume) *= dsp_vol_2ch->GetFloat();
	}

	if ( !(*pg_bDspOff) )
	{
		static ConVar* dsp_facingaway = cvar->FindVar( "dsp_facingaway" );
		(*pg_bdirectionalfx) = dsp_facingaway->GetInt() ? 1 : 0;
	}
	else
	{
		(*pg_bdirectionalfx) = 0;
	}

	while ( (*pg_paintedtime) < endtime )
	{
		//VPROF( "MIX_PaintChannels inner loop" );
		// mix a full 'paintbuffer' of sound

		// clamp at paintbuffer size
		end = endtime;
		if ( endtime - (*pg_paintedtime) > PAINTBUFFER_SIZE )
		{
			end = (*pg_paintedtime) + PAINTBUFFER_SIZE;
		}

		// number of 44khz samples to mix into paintbuffer, up to paintbuffer size
		count = end - (*pg_paintedtime);

		// clear all mix buffers
		(*pg_AudioDevice)->MixBegin( count );

		// upsample all mix buffers.
		// results in 44khz versions of:
		// IROOMBUFFER, IFACINGBUFFER, IFACINGAWAYBUFFER, IDRYBUFFER, ISPEAKERBUFFER
		MIX_UpsampleAllPaintbuffers( list, end, count );

		// apply appropriate dsp fx to each buffer, remix buffers into single quad output buffer
		// apply 2 or 4ch filtering to IFACINGAWAY buffer
		if ( (*pg_bdirectionalfx) )
		{
			(*pg_AudioDevice)->ApplyDSPEffects( (*pidsp_facingaway), MIX_GetPFrontFromIPaint( IFACINGAWAYBUFFER ), MIX_GetPRearFromIPaint( IFACINGAWAYBUFFER ), MIX_GetPCenterFromIPaint( IFACINGAWAYBUFFER ), count );
		}

		if ( !(*pg_bDspOff) && list.m_hasSpeakerChannels )
		{
			// apply 1ch filtering to ISPEAKERBUFFER
			(*pg_AudioDevice)->ApplyDSPEffects( (*pidsp_speaker), MIX_GetPFrontFromIPaint( ISPEAKERBUFFER ), MIX_GetPRearFromIPaint( ISPEAKERBUFFER ), MIX_GetPCenterFromIPaint( ISPEAKERBUFFER ), count );

			// mix ISPEAKERBUFFER with IROOMBUFFER and IFACINGBUFFER
			MIX_ScalePaintBuffer( ISPEAKERBUFFER, count, 0.7 );

			MIX_MixPaintbuffers( ISPEAKERBUFFER, IFACINGBUFFER, IFACINGBUFFER, count, 1.0 );	// +70% dry speaker

			MIX_ScalePaintBuffer( ISPEAKERBUFFER, count, 0.43 );

			MIX_MixPaintbuffers( ISPEAKERBUFFER, IROOMBUFFER, IROOMBUFFER, count, 1.0 );		// +30% wet speaker
		}

		// apply dsp_room effects to room buffer
		(*pg_AudioDevice)->ApplyDSPEffects( Get_idsp_room(), MIX_GetPFrontFromIPaint( IROOMBUFFER ), MIX_GetPRearFromIPaint( IROOMBUFFER ), MIX_GetPCenterFromIPaint( IROOMBUFFER ), count );

		// save room buffer surround status, in case we upconvert it
		room_fsurround_sav = proom->fsurround;
		room_fsurround_center_sav = proom->fsurround_center;

		// apply left/center/right/lrear/rrear spatial delays to room buffer
		if ( b_spatial_delays && !(*pg_bDspOff) && !DSP_RoomDSPIsOff() )
		{
			// upgrade mono room buffer to surround status so we can apply spatial delays to all channels
			MIX_ConvertBufferToSurround( IROOMBUFFER );
			(*pg_AudioDevice)->ApplyDSPEffects( (*pidsp_spatial), MIX_GetPFrontFromIPaint( IROOMBUFFER ), MIX_GetPRearFromIPaint( IROOMBUFFER ), MIX_GetPCenterFromIPaint( IROOMBUFFER ), count );
		}

		if ( (*pg_bdirectionalfx) )		// KDB: perf
		{
			// Recombine IFACING and IFACINGAWAY buffers into IPAINTBUFFER
			MIX_MixPaintbuffers( IFACINGBUFFER, IFACINGAWAYBUFFER, IPAINTBUFFER, count, DSP_NOROOM_MIX );

			// Add in dsp room fx to paintbuffer, mix at 75%
			MIX_MixPaintbuffers( IROOMBUFFER, IPAINTBUFFER, IPAINTBUFFER, count, DSP_ROOM_MIX );
		}
		else
		{
			// Mix IFACING buffer with IROOMBUFFER
			// (IFACINGAWAYBUFFER contains no data, IFACINGBBUFFER has full dry mix based on distance from listener)
			// if dsp disabled, mix 100% facingbuffer, otherwise, mix 75% facingbuffer + roombuffer
			float mix = (*pg_bDspOff) ? 1.0 : DSP_ROOM_MIX;
			MIX_MixPaintbuffers( IROOMBUFFER, IFACINGBUFFER, IPAINTBUFFER, count, mix );
		}

		// restore room buffer surround status, in case we upconverted it 
		proom->fsurround = room_fsurround_sav;
		proom->fsurround_center = room_fsurround_center_sav;

		// Apply underwater fx dsp_water (serial in-line)
		if ( bIsUnderwater )
		{
			// BUG: if out of water, previous delays will be heard. must clear dly buffers.
			(*pg_AudioDevice)->ApplyDSPEffects( (*pidsp_water), MIX_GetPFrontFromIPaint( IPAINTBUFFER ), MIX_GetPRearFromIPaint( IPAINTBUFFER ), MIX_GetPCenterFromIPaint( IPAINTBUFFER ), count );
		}

		// Apply player fx dsp_player (serial in-line) - does nothing if dsp fx are disabled
		(*pg_AudioDevice)->ApplyDSPEffects( (*pidsp_player), MIX_GetPFrontFromIPaint( IPAINTBUFFER ), MIX_GetPRearFromIPaint( IPAINTBUFFER ), MIX_GetPCenterFromIPaint( IPAINTBUFFER ), count );

		/*
				// apply left/center/right/lrear/rrear spatial delays to paint buffer

				if ( b_spatial_delays )
					(*pg_AudioDevice)->ApplyDSPEffects( idsp_spatial, MIX_GetPFrontFromIPaint(IPAINTBUFFER),  MIX_GetPRearFromIPaint(IPAINTBUFFER), MIX_GetPCenterFromIPaint(IPAINTBUFFER), count );
		*/
		// Add dry buffer, set output gain to water * player dsp gain (both 1.0 if not active)

		MIX_MixPaintbuffers( IPAINTBUFFER, IDRYBUFFER, IPAINTBUFFER, count, 1.0 );

		// clip all values > 16 bit down to 16 bit
		// NOTE: This is required - the hardware buffer transfer routines no longer perform clipping.
		MIX_CompressPaintbuffer( IPAINTBUFFER, count );

		// transfer IPAINTBUFFER paintbuffer out to DMA buffer
		MIX_SetCurrentPaintbuffer( IPAINTBUFFER );

		(*pg_AudioDevice)->TransferSamples( end );

		(*pg_paintedtime) = end;
	}
}