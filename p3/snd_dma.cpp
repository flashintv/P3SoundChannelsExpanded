//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: Main control for any streaming sound output device.
//
//===========================================================================//

#include "../sdk/icliententitylist.h"
#include "../sdk/icommandline.h"
#include "../sdk/soundservice.h"
#include "../sdk/threadtools.h"
#include "../sdk/snd_device.h"
#include "../sdk/utlvector.h"
#include "../sdk/soundinfo.h"
#include "../sdk/cdll_int.h"
#include "../sdk/utlmap.h"
#include "../sdk/icvar.h"

#include "common.h"
#include "snd_channels.h"

extern ICvar* cvar;
extern ISoundServices* g_pSoundServices;
extern IAudioDevice** pg_AudioDevice;
extern IVEngineClient* engineclient;

extern int* ptotal_channels;
extern Vector* plistener_origin;
extern Vector* plistener_forward;
extern Vector* plistener_right;
extern Vector* plistener_up;
extern grouprule_t* pg_grouprules;
extern int* pg_cgrouprules;
extern int* pg_mapMixgroupidToGrouprulesid;
extern float* pg_DuckScale;
extern soundfade_t* psoundfade;
extern CUtlMap< FileNameHandle_t, SfxDictEntry >* ps_Sounds;
extern bool* ps_bOnLoadScreen;
extern double* pg_LastSoundFrame;
extern double* pg_LastMixTime;
extern float* pg_EstFrameTime;
extern float* pg_DashboardMusicMixValue;
extern float* pg_DashboardMusicMixTarget;
extern bool* ps_bIsListenerUnderwater;
extern int* pg_snd_trace_count;
extern CThreadMutex* pg_SndMutex;

extern channel_t channels[MAX_CHANNELS];
extern CActiveChannels<MAX_CHANNELS> g_ActiveChannels;

float	DSP_ROOM_MIX = 1.0;	// mix volume of dsp_room sounds when added back to 'dry' sounds
float	DSP_NOROOM_MIX = 1.0;	// mix volume of facing + facing away sounds. added to dsp_room_mix sounds

#define THREAD_LOCK_SOUND() AUTO_LOCK( *pg_SndMutex )

int ChannelGetMaxVol( channel_t* pch )
{
	float max = 0.0;

	for ( int i = 0; i < CCHANVOLUMES; i++ )
	{
		if ( pch->fvolume[i] > max )
			max = pch->fvolume[i];
	}

	return (int)max;
}

unsigned int RemainingSamples( channel_t* pChannel )
{
	if ( !pChannel || !pChannel->sfx || !pChannel->sfx->pSource )
		return 0;

	unsigned int timeleft = pChannel->sfx->pSource->SampleCount();

	if ( pChannel->sfx->pSource->IsLooped() )
	{
		return pChannel->sfx->pSource->SampleRate();
	}

	if ( pChannel->pMixer )
	{
		timeleft -= pChannel->pMixer->GetSamplePosition();
	}

	return timeleft;
}

bool SND_IsMouth( channel_t* pChannel )
{
	if ( !entitylist )
	{
		return false;
	}

	if ( pChannel->entchannel == CHAN_VOICE )
	{
		return true;
	}

	if ( pChannel->sfx &&
		pChannel->sfx->pSource &&
		pChannel->sfx->pSource->GetSentence() )
	{
		return true;
	}

	return false;
}

bool BChannelLowVolume( channel_t* pch, int vol_min )
{
	int max = -1;
	int max_target = -1;
	int vol;
	int vol_target;

	for ( int i = 0; i < CCHANVOLUMES; i++ )
	{
		vol = (int)(pch->fvolume[i]);
		vol_target = (int)(pch->fvolume_target[i]);

		if ( vol > max )
			max = vol;

		if ( vol_target > max_target )
			max_target = vol_target;
	}

	return (max <= vol_min && max_target <= vol_min);
}

// Get the loudest actual volume for a channel (not counting targets).
float ChannelLoudestCurVolume( const channel_t* RESTRICT pch )
{
	float loudest = pch->fvolume[0];
	for ( int i = 1; i < CCHANVOLUMES; i++ )
	{
		loudest = fpmax( loudest, pch->fvolume[i] );
	}
	return loudest;
}

template<>
void CActiveChannels<>::Add( channel_t* pChannel )
{
	Assert( pChannel->activeIndex == 0 );
	m_list[m_count] = pChannel - channels;
	m_count++;
	pChannel->activeIndex = m_count;
}

template<>
void CActiveChannels<>::Remove( channel_t* pChannel )
{
	if ( pChannel->activeIndex == 0 )
		return;
	int activeIndex = pChannel->activeIndex - 1;
	Assert( activeIndex >= 0 && activeIndex < m_count );
	Assert( pChannel == &channels[m_list[activeIndex]] );
	m_count--;
	// Not the last one?  Swap the last one with this one and fix its index
	if ( activeIndex < m_count )
	{
		m_list[activeIndex] = m_list[m_count];
		channels[m_list[activeIndex]].activeIndex = activeIndex + 1;
	}
	pChannel->activeIndex = 0;
}


template<>
void CActiveChannels<>::GetActiveChannels( CChannelList& list )
{
	list.m_count = m_count;
	if ( m_count )
	{
		memcpy( list.m_list, m_list, sizeof( m_list[0] ) * m_count );
	}
	list.m_hasSpeakerChannels = true;
	list.m_has11kChannels = true;
	list.m_has22kChannels = true;
	list.m_has44kChannels = true;
	list.m_hasDryChannels = true;
}

template<>
void CActiveChannels<>::Init()
{
	m_count = 0;
}

/*
=================
SND_StealDynamicChannel
Select a channel from the dynamic channel allocation area.  For the given entity,
override any other sound playing on the same channel (see code comments below for
exceptions).
=================
*/
channel_t* SND_StealDynamicChannel( SoundSource soundsource, int entchannel, const Vector& origin, CSfxTable* sfx )
{
	int canSteal[MAX_DYNAMIC_CHANNELS];
	int canStealCount = 0;

	int sameSoundCount = 0;
	unsigned int sameSoundRemaining = 0xFFFFFFFF;
	int sameSoundIndex = -1;
	int sameVol = 0xFFFF;
	int availableChannel = -1;
	bool bDelaySame = false;

	// first pass to replace sounds on same ent/channel, and search for free or stealable channels otherwise
	for ( int ch_idx = 0; ch_idx < MAX_DYNAMIC_CHANNELS; ch_idx++ )
	{
		channel_t* ch = &channels[ch_idx];

		if ( ch->activeIndex )
		{
			// channel CHAN_AUTO never overrides sounds on same channel
			if ( entchannel != CHAN_AUTO )
			{
				int checkChannel = entchannel;
				if ( checkChannel == -1 )
				{
					if ( ch->entchannel != CHAN_STREAM && ch->entchannel != CHAN_VOICE )
					{
						checkChannel = ch->entchannel;
					}
				}
				// delayed channels are never overridden
				if ( !ch->flags.delayed_start && ch->soundsource == soundsource && (soundsource != -1) && ch->entchannel == checkChannel )
					return ch;	// always override sound from same entity
			}

			// Never steal the channel of a streaming sound that is currently playing or
			// voice over IP data that is playing or any sound on CHAN_VOICE( acting )
			if ( ch->entchannel == CHAN_STREAM || ch->entchannel == CHAN_VOICE )
				continue;

			// don't let monster sounds override player sounds
			if ( g_pSoundServices->IsPlayer( ch->soundsource ) && !g_pSoundServices->IsPlayer( soundsource ) )
				continue;

			if ( ch->sfx == sfx )
			{
				bDelaySame = ch->flags.delayed_start ? true : bDelaySame;
				sameSoundCount++;
				int maxVolume = ChannelGetMaxVol( ch );
				unsigned int remaining = RemainingSamples( ch );
				if ( maxVolume < sameVol || (maxVolume == sameVol && remaining < sameSoundRemaining) )
				{
					sameSoundIndex = ch_idx;
					sameVol = maxVolume;
					sameSoundRemaining = remaining;
				}
			}
			canSteal[canStealCount++] = ch_idx;
		}
		else
		{
			if ( availableChannel < 0 )
			{
				availableChannel = ch_idx;
			}
		}
	}


	// Limit the number of times a given sfx/wave can play simultaneously
	static ConVar* voice_steal = cvar->FindVar( "voice_steal" );
	if ( voice_steal->GetInt() > 1 && sameSoundIndex >= 0 )
	{
		// if sounds of this type are normally delayed, then add an extra slot for stealing
		// NOTE: In HL2 these are usually NPC gunshot sounds - and stealing too soon will cut
		// them off early.  This is a safe heuristic to avoid that problem.  There's probably a better
		// long-term solution involving only counting channels that are actually going to play (delay included)
		// at the same time as this one.
		int maxSameSounds = bDelaySame ? 5 : 4;
		float distSqr = 0.0f;
		if ( sfx->pSource )
		{
			distSqr = origin.DistToSqr( *plistener_origin );
			if ( sfx->pSource->IsLooped() )
			{
				maxSameSounds = 3;
			}
		}

		// don't play more than N copies of the same sound, steal the quietest & closest one otherwise
		if ( sameSoundCount >= maxSameSounds )
		{
			channel_t* ch = &channels[sameSoundIndex];
			// you're already playing a closer version of this sound, don't steal
			if ( distSqr > 0.0f && ch->origin.DistToSqr( *plistener_origin ) < distSqr && entchannel != CHAN_WEAPON )
				return NULL;

			//Msg("Sound playing %d copies, stole %s (%d)\n", sameSoundCount, ch->sfx->getname(), sameVol );
			return ch;
		}
	}

	// if there's a free channel, just take that one - don't steal
	if ( availableChannel >= 0 )
		return &channels[availableChannel];

	// Still haven't found a suitable channel, so choose the one with the least amount of time left to play
	float life_left = FLT_MAX;
	int first_to_die = -1;
	bool bAllowVoiceSteal = voice_steal->GetBool();

	for ( int i = 0; i < canStealCount; i++ )
	{
		int ch_idx = canSteal[i];
		channel_t* ch = &channels[ch_idx];
		float timeleft = 0;
		if ( bAllowVoiceSteal )
		{
			int maxVolume = ChannelGetMaxVol( ch );
			if ( maxVolume < 5 )
			{
				//Msg("Sound quiet, stole %s for %s\n", ch->sfx->getname(), sfx->getname() );
				return ch;
			}

			if ( ch->sfx && ch->sfx->pSource )
			{
				unsigned int sampleCount = RemainingSamples( ch );
				timeleft = (float)sampleCount / (float)ch->sfx->pSource->SampleRate();
			}
		}
		else
		{
			// UNDONE: Kill this when voice_steal 0,1,2 has been tested
			// UNDONE: This is the old buggy code that we're trying to replace
			if ( ch->sfx )
			{
				// basically steals the first one you come to
				timeleft = 1;	//ch->end - paintedtime
			}
		}

		if ( timeleft < life_left )
		{
			life_left = timeleft;
			first_to_die = ch_idx;
		}
	}
	if ( first_to_die >= 0 )
	{
		//Msg("Stole %s, timeleft %d\n", channels[first_to_die].sfx->getname(), life_left );
		return &channels[first_to_die];
	}

	return NULL;
}

extern void (*DSP_ClearState)();
extern void (*MIX_ClearAllPaintBuffers)(int SampleCount, bool clearFilters);
void S_ClearBuffer( void )
{
	if ( !(*pg_AudioDevice) )
		return;

	(*pg_AudioDevice)->ClearBuffer();
	DSP_ClearState();
	MIX_ClearAllPaintBuffers( 1020 /*PAINTBUFFER_SIZE*/, true );
}

// Stop all sounds for entity on a channel.
void S_StopSound( int soundsource, int entchannel )
{
	THREAD_LOCK_SOUND();
	CChannelList list;
	g_ActiveChannels.GetActiveChannels( list );
	for ( int i = 0; i < list.Count(); i++ )
	{
		channel_t* pChannel = list.GetChannel( i );
		if ( pChannel->soundsource == soundsource
			&& pChannel->entchannel == entchannel )
		{
			S_FreeChannel( pChannel );
		}
	}
}

void S_StopAllSounds( bool bClear )
{
	THREAD_LOCK_SOUND();
	int		i;

	if ( !(*pg_AudioDevice) )
		return;

	if ( !(*pg_AudioDevice)->IsActive() )
		return;

	*ptotal_channels = MAX_DYNAMIC_CHANNELS;	// no statics

	CChannelList list;
	g_ActiveChannels.GetActiveChannels( list );
	for ( i = 0; i < list.Count(); i++ )
	{
		channel_t* pChannel = list.GetChannel( i );

		char nameBuf[MAX_PATH];
		const char* pName = "Unknown";
		if ( pChannel->sfx )
		{
			strncpy( nameBuf, pChannel->sfx->getname(), sizeof( nameBuf ) - 1 );
			nameBuf[sizeof( nameBuf ) - 1] = '\0';
		}

		DevMsg( 1, "Stopping: Channel:%2d %s\n", list.GetChannelIndex( i ), pName );
		S_FreeChannel( pChannel );
	}

	memset( channels, 0, MAX_CHANNELS * sizeof( channel_t ) );

	if ( bClear )
	{
		S_ClearBuffer();
	}

	// Clear any remaining soundfade
	memset( psoundfade, 0, sizeof( soundfade_t ) );

	(*pg_AudioDevice)->StopAllSounds();
	Assert( g_ActiveChannels.GetActiveCount() == 0 );
}

FileNameHandle_t CSfxTable::GetFileNameHandle()
{
	if ( (*ps_Sounds).InvalidIndex() != m_namePoolIndex )
	{
		return (*ps_Sounds).Key( m_namePoolIndex );
	}
	return NULL;
}

void S_GetActiveSounds( CUtlVector< SndInfo_t >& sndlist )
{
	CChannelList list;
	g_ActiveChannels.GetActiveChannels( list );
	for ( int i = 0; i < list.Count(); i++ )
	{
		channel_t* ch = list.GetChannel( i );

		SndInfo_t info;

		info.m_nGuid = ch->guid;
		info.m_filenameHandle = ch->sfx ? ch->sfx->GetFileNameHandle() : NULL;
		info.m_nSoundSource = ch->soundsource;
		info.m_nChannel = ch->entchannel;
		// If a sound is being played through a speaker entity (e.g., on a monitor,), this is the
		//  entity upon which to show the lips moving, if the sound has sentence data
		info.m_nSpeakerEntity = ch->speakerentity;
		info.m_flVolume = (float)ch->master_vol / 255.0f;
		info.m_flLastSpatializedVolume = ch->last_vol;
		// Radius of this sound effect (spatialization is different within the radius)
		info.m_flRadius = ch->radius;
		info.m_nPitch = ch->basePitch;
		info.m_pOrigin = &ch->origin;
		info.m_pDirection = &ch->direction;

		// if true, assume sound source can move and update according to entity
		info.m_bUpdatePositions = ch->flags.bUpdatePositions;
		// true if playing linked sentence
		info.m_bIsSentence = ch->flags.isSentence;
		// if true, bypass all dsp processing for this sound (ie: music)	
		info.m_bDryMix = ch->flags.bdry;
		// true if sound is playing through in-game speaker entity.
		info.m_bSpeaker = ch->flags.bSpeaker;
		// for snd_show, networked sounds get colored differently than local sounds
		info.m_bFromServer = ch->flags.fromserver;

		sndlist.AddToTail( info );
	}
}

channel_t* S_FindChannelByGuid( int guid )
{
	CChannelList list;
	g_ActiveChannels.GetActiveChannels( list );
	for ( int i = 0; i < list.Count(); i++ )
	{
		channel_t* pChannel = list.GetChannel( i );
		if ( pChannel->guid == guid )
		{
			return pChannel;
		}
	}
	return NULL;
}

#define SND_DUCKER_UPDATETIME	0.1		// seconds to wait between ducker updates
double g_mxr_ducktime = 0.0;			// time of last update to ducker
void MXR_UpdateAllDuckerVolumes( void )
{
	static ConVar* snd_disable_mixer_duck = cvar->FindVar( "snd_disable_mixer_duck" );
	if ( snd_disable_mixer_duck->GetInt() )
		return;

	// check timer since last update, only update at 10hz

	int i, j;
	double dtime = g_pSoundServices->GetHostTime();

	// don't update until timer expires

	if ( fabs( dtime - g_mxr_ducktime ) < SND_DUCKER_UPDATETIME )
		return;

	g_mxr_ducktime = dtime;

	// clear out all total volume values for groups

	for ( i = 0; i < (*pg_cgrouprules); i++ )
		pg_grouprules[i].total_vol = 0.0;

	// for every channel in a mix group which can cause ducking:
	// get total volume, store total in grouprule:

	CChannelList list;
	int ch_idx;

	channel_t* pchan;
	bool b_found_ducked_channel = false;

	g_ActiveChannels.GetActiveChannels( list );

	for ( i = 0; i < list.Count(); i++ )
	{
		ch_idx = list.GetChannelIndex( i );
		pchan = &channels[ch_idx];

		if ( pchan->last_vol > 0.0 )
		{
			// account for all mix groups this channel belongs to...

			for ( int j = 0; j < 8; j++ )
			{
				int imixgroup = pchan->mixgroups[j];

				if ( imixgroup < 0 )
					continue;

				int	grouprulesid = pg_mapMixgroupidToGrouprulesid[imixgroup];

				if ( pg_grouprules[grouprulesid].causes_ducking )
					pg_grouprules[grouprulesid].total_vol += pchan->last_vol;

				if ( pg_grouprules[grouprulesid].is_ducked )
					b_found_ducked_channel = true;
			}
		}
	}

	// if no channels playing which may be ducked, do nothing

	if ( !b_found_ducked_channel )
		return;

	// for all groups that can be ducked:
	// see if a higher priority sound group has a volume > threshold, 
	// if so, then duck this group by setting duck_target_vol to duck_target_pct.
	// if no sound group is causing ducking in this group, reset duck_target_vol to 1.0

	for ( i = 0; i < (*pg_cgrouprules); i++ )
	{
		if ( pg_grouprules[i].is_ducked )
		{
			int priority = pg_grouprules[i].priority;

			float duck_volume = 1.0;				// clear to 1.0 if no channel causing ducking

			// make sure we interact appropriately with global voice ducking...
			// if global voice ducking is active, skip sound group ducking and just set duck_volume target to 1.0

			if ( (*pg_DuckScale) >= 1.0 )
			{
				// check all sound groups for higher priority duck trigger

				for ( j = 0; j < (*pg_cgrouprules); j++ )
				{
					if ( pg_grouprules[j].priority > priority &&
						pg_grouprules[j].causes_ducking &&
						pg_grouprules[j].total_vol > pg_grouprules[j].ducker_threshold )
					{
						// a higher priority group is causing this group to be ducked
						// set duck volume target to the ducked group's duck target percent
						// and break

						duck_volume = pg_grouprules[i].duck_target_pct;

						// UNDONE: to prevent edge condition caused by crossing threshold, may need to have secondary
						// UNDONE: timer which allows ducking at 0.2 hz

						break;
					}
				}
			}

			pg_grouprules[i].duck_target_vol = duck_volume;
		}
	}

	// update all ducker ramps if current duck value is not target
	// if ramp is greater than duck_volume, approach at 'attack rate'
	// if ramp is less than duck_volume, approach at 'decay rate'

	for ( i = 0; i < (*pg_cgrouprules); i++ )
	{
		float target = pg_grouprules[i].duck_target_vol;
		float current = pg_grouprules[i].duck_ramp_val;

		if ( pg_grouprules[i].is_ducked && (current != target) )
		{
			static ConVar* snd_duckerattacktime = cvar->FindVar( "snd_duckerattacktime" );
			static ConVar* snd_duckerreleasetime = cvar->FindVar( "snd_duckerreleasetime" );
			float ramptime = target < current ? snd_duckerattacktime->GetFloat() : snd_duckerreleasetime->GetFloat();

			// delta is volume change per update (we can do this 
			// since we run at an approximate fixed update rate of 10hz)

			float delta = (1.0 - pg_grouprules[i].duck_target_pct);

			delta *= (SND_DUCKER_UPDATETIME / ramptime);

			if ( current > target )
				delta = -delta;

			// update ramps

			current += delta;

			if ( current < target && delta < 0 )
				current = target;
			if ( current > target && delta > 0 )
				current = target;

			pg_grouprules[i].duck_ramp_val = current;
		}
	}

}

// search through all channels for a channel that matches this
// soundsource, entchannel and sfx, and perform alteration on channel
// as indicated by 'flags' parameter. If shut down request and
// sfx contains a sentence name, shut off the sentence.
// returns TRUE if sound was altered,
// returns FALSE if sound was not found (sound is not playing)

int S_AlterChannel( int soundsource, int entchannel, CSfxTable* sfx, int vol, int pitch, int flags )
{
	THREAD_LOCK_SOUND();
	int ch_idx;

	if ( TestSoundChar( sfx->getname(), CHAR_SENTENCE ) )
	{
		// This is a sentence name.
		// For sentences: assume that the entity is only playing one sentence
		// at a time, so we can just shut off
		// any channel that has ch->isentence >= 0 and matches the
		// soundsource.

		CChannelList list;
		g_ActiveChannels.GetActiveChannels( list );
		for ( int i = 0; i < list.Count(); i++ )
		{
			ch_idx = list.GetChannelIndex( i );
			if ( channels[ch_idx].soundsource == soundsource
				&& channels[ch_idx].entchannel == entchannel
				&& channels[ch_idx].sfx != NULL )
			{

				if ( flags & SND_CHANGE_PITCH )
					channels[ch_idx].basePitch = pitch;

				if ( flags & SND_CHANGE_VOL )
					channels[ch_idx].master_vol = vol;

				if ( flags & SND_STOP )
				{
					S_FreeChannel( &channels[ch_idx] );
				}

				return TRUE;
			}
		}
		// channel not found
		return FALSE;

	}

	// regular sound or streaming sound
	CChannelList list;
	g_ActiveChannels.GetActiveChannels( list );

	bool bSuccess = false;

	for ( int i = 0; i < list.Count(); i++ )
	{
		ch_idx = list.GetChannelIndex( i );
		if ( channels[ch_idx].soundsource == soundsource &&
			((flags & SND_IGNORE_NAME) ||
				(channels[ch_idx].entchannel == entchannel && channels[ch_idx].sfx == sfx)) )
		{
			if ( flags & SND_CHANGE_PITCH )
				channels[ch_idx].basePitch = pitch;

			if ( flags & SND_CHANGE_VOL )
				channels[ch_idx].master_vol = vol;

			if ( flags & SND_STOP )
			{
				S_FreeChannel( &channels[ch_idx] );
			}

			if ( (flags & SND_IGNORE_NAME) == 0 )
				return TRUE;
			else
				bSuccess = true;
		}
	}

	return (bSuccess) ? (TRUE) : (FALSE);
}

void SND_ChannelTraceReset( void )
{
	if ( (*pg_snd_trace_count) )
		return;

	// if no tracelines performed this frame, then reset all 
	// trace flags

	for ( int i = 0; i < (*ptotal_channels); i++ )
		channels[i].flags.bTraced = false;
}

/*
============
S_Update

Called once each time through the main loop
============
*/
void S_Update( const AudioState_t* pAudioState )
{
	extern void (*SND_Spatialize)(channel_t * ch);

	int			i;
	channel_t* ch;
	channel_t* combine;
	static unsigned int s_roundrobin = 0; ///< number of times this function is called.
	///< used instead of host_frame because that number
	///< isn't necessarily available here (sez Yahn).

	if ( !(*pg_AudioDevice)->IsActive() )
		return;

	pg_SndMutex->Lock();

	// Update any client side sound fade
	extern void (*S_UpdateSoundFade)( void );
	S_UpdateSoundFade();

	if ( pAudioState )
	{
		VectorCopy( pAudioState->m_Origin, *plistener_origin );
		AngleVectors( pAudioState->m_Angles, plistener_forward, plistener_right, plistener_up );
		(*ps_bIsListenerUnderwater) = pAudioState->m_bIsUnderwater;
	}
	else
	{
		VectorCopy( vec3_origin, *plistener_origin );
		VectorCopy( vec3_origin, *plistener_forward );
		VectorCopy( vec3_origin, *plistener_right );
		VectorCopy( vec3_origin, *plistener_up );
		(*ps_bIsListenerUnderwater) = false;
	}

	(*pg_AudioDevice)->UpdateListener( *plistener_origin, *plistener_forward, *plistener_right, *plistener_up );

	combine = NULL;

	int voiceChannelCount = 0;
	int voiceChannelMaxVolume = 0;

	// reset traceline counter for this frame
	(*pg_snd_trace_count) = 0;

	// calculate distance to nearest walls, update dsp_spatial
	// updates one wall only per frame (one trace per frame)
	extern void (*SND_SetSpatialDelays)(void);
	SND_SetSpatialDelays();

	// updates dsp_room if automatic room detection enabled
	extern void (*DAS_CheckNewRoomDSP)(void);
	DAS_CheckNewRoomDSP();

	// update spatialization for static and dynamic sounds	
	CChannelList list;
	g_ActiveChannels.GetActiveChannels( list );

	static ConVar* snd_spatialize_roundrobin = cvar->FindVar( "snd_spatialize_roundrobin" );
	if ( snd_spatialize_roundrobin->GetInt() == 0 )
	{
		// spatialize each channel each time
		for ( i = 0; i < list.Count(); i++ )
		{
			ch = list.GetChannel( i );
			Assert( ch->sfx );
			Assert( ch->activeIndex > 0 );

			SND_Spatialize( ch );         // respatialize channel

			if ( ch->sfx->pSource && ch->sfx->pSource->IsVoiceSource() )
			{
				voiceChannelCount++;
				voiceChannelMaxVolume = max( voiceChannelMaxVolume, ChannelGetMaxVol( ch ) );
			}
		}
	}
	else	// lowend performance improvement: spatialize only some  channels each frame.
	{
		unsigned int robinmask = (1 << snd_spatialize_roundrobin->GetInt()) - 1;

		// now do static channels
		for ( i = 0; i < list.Count(); ++i )
		{
			ch = list.GetChannel( i );
			Assert( ch->sfx );
			Assert( ch->activeIndex > 0 );

			// need to check bfirstpass because sound tracing may have been deferred
			if ( ch->flags.bfirstpass || (robinmask & s_roundrobin) == (i & robinmask) )
			{
				SND_Spatialize( ch );         // respatialize channel
			}

			if ( ch->sfx->pSource && ch->sfx->pSource->IsVoiceSource() )
			{
				voiceChannelCount++;
				voiceChannelMaxVolume = max( voiceChannelMaxVolume, ChannelGetMaxVol( ch ) );
			}
		}

		++s_roundrobin;
	}

	SND_ChannelTraceReset();

	// set new target for voice ducking
	float frametime = g_pSoundServices->GetHostFrametime();
	extern void (*S_UpdateVoiceDuck)(int voiceChannelCount, int voiceChannelMaxVolume, float frametime);
	S_UpdateVoiceDuck( voiceChannelCount, voiceChannelMaxVolume, frametime );

	// update x360 music volume
	const float g_DashboardMusicFadeRate = 0.5f;
	(*pg_DashboardMusicMixValue) = Approach( (*pg_DashboardMusicMixTarget), (*pg_DashboardMusicMixValue), g_DashboardMusicFadeRate * frametime );

	//
	// debugging output
	//
	static ConVar* snd_show = cvar->FindVar( "snd_show" );
	if ( snd_show->GetInt() )
	{
		con_nprint_t np;
		np.time_to_live = 2.0f;
		np.fixed_width_font = true;

		int total = 0;

		CChannelList list;
		g_ActiveChannels.GetActiveChannels( list );
		for ( int i = 0; i < list.Count(); i++ )
		{
			channel_t* ch = list.GetChannel( i );
			if ( !ch->sfx )
				continue;

			np.index = total + 2;
			if ( ch->flags.fromserver )
			{
				np.color[0] = 1.0;
				np.color[1] = 0.8;
				np.color[2] = 0.1;
			}
			else
			{
				np.color[0] = 0.1;
				np.color[1] = 0.9;
				np.color[2] = 1.0;
			}

			unsigned int sampleCount = RemainingSamples( ch );
			float timeleft = (float)sampleCount / (float)ch->sfx->pSource->SampleRate();
			bool bLooping = ch->sfx->pSource->IsLooped();

			static ConVar* snd_surround = cvar->FindVar( "snd_surround_speakers" );
			if ( snd_surround->GetInt() < 4 )
			{
				// made into engineclient calls
				engineclient->Con_NXPrintf( &np, "%02i l(%03d) r(%03d) vol(%03d) ent(%03d) pos(%6d %6d %6d) timeleft(%f) looped(%d) %50s",
					total + 1,
					(int)ch->fvolume[IFRONT_LEFT],
					(int)ch->fvolume[IFRONT_RIGHT],
					ch->master_vol,
					ch->soundsource,
					(int)ch->origin[0],
					(int)ch->origin[1],
					(int)ch->origin[2],
					timeleft,
					bLooping,
					ch->sfx->getname() );
			}
			else
			{
				// made into engineclient calls
				engineclient->Con_NXPrintf( &np, "%02i l(%03d) c(%03d) r(%03d) rl(%03d) rr(%03d) vol(%03d) ent(%03d) pos(%6d %6d %6d) timeleft(%f) looped(%d) %50s",
					total + 1,
					(int)ch->fvolume[IFRONT_LEFT],
					(int)ch->fvolume[IFRONT_CENTER],
					(int)ch->fvolume[IFRONT_RIGHT],
					(int)ch->fvolume[IREAR_LEFT],
					(int)ch->fvolume[IREAR_RIGHT],
					ch->master_vol,
					ch->soundsource,
					(int)ch->origin[0],
					(int)ch->origin[1],
					(int)ch->origin[2],
					timeleft,
					bLooping,
					ch->sfx->getname() );
			}

			static ConVar* snd_visualize = cvar->FindVar( "snd_visualize" );
			if ( snd_visualize->GetInt() )
			{
				extern void (*CDebugOverlay_AddTextOverlay)(const Vector& origin, float flDuration, const char* text);
				CDebugOverlay_AddTextOverlay( ch->origin, 0.05f, ch->sfx->getname() );
			}

			total++;
		}

		while ( total <= 128 )
		{
			// made into engineclient calls
			engineclient->Con_NPrintf( total + 2, "" );
			total++;
		}
	}

	pg_SndMutex->Unlock();

	if ( *ps_bOnLoadScreen )
		return;

	static ConVar* snd_mix_minframetime = cvar->FindVar( "snd_mix_minframetime" );
	static ConVar* snd_mixahead = cvar->FindVar( "snd_mixahead" );

	// not time to update yet?
	double tNow = Plat_FloatTime();
	float delta = (tNow - (*pg_LastSoundFrame));
	if ( delta > 0 && delta < snd_mix_minframetime->GetFloat() )
		return;
	// this is the last time we ran a sound frame
	(*pg_LastSoundFrame) = tNow;
	// this is the last time we did mixing (extraupdate also advances this if it mixes)
	(*pg_LastMixTime) = tNow;
	// mix some sound
	// try to stay at least one frame + mixahead ahead in the mix.
	(*pg_EstFrameTime) = ((*pg_EstFrameTime) * 0.9f) + (g_pSoundServices->GetHostFrametime() * 0.1f);
	
	extern void (*S_Update_)(float mixAheadTime);
	S_Update_( (*pg_EstFrameTime) + snd_mixahead->GetFloat() );
}

void S_SetChannelStereo( channel_t* target_chan, CAudioSource* pSource )
{
	if ( !pSource )
	{
		target_chan->flags.bstereowav = false;
		return;
	}

	// returns true only if source data is a stereo wav file. 
	// ie: mp3, voice, sentence are all excluded.

	target_chan->flags.bstereowav = pSource->IsStereoWav();

	// Default stereo wavtype:

	// just player standard stereo wavs on player entity - no override.

	if ( target_chan->soundsource == g_pSoundServices->GetViewEntity() )
		return;

	// default wavtype for stereo wavs is OMNI - except for drymix or sounds with 0 attenuation

	if ( target_chan->flags.bstereowav && !target_chan->wavtype && !target_chan->flags.bdry && target_chan->dist_mult )
		// target_chan->wavtype = CHAR_DISTVARIANT;
		target_chan->wavtype = CHAR_OMNI;
}

void S_SetChannelWavtype( channel_t* target_chan, CSfxTable* pSfx )
{
	// if 1st or 2nd character of name is CHAR_DRYMIX, sound should be mixed dry with no dsp (ie: music)

	if ( TestSoundChar( pSfx->getname(), CHAR_DRYMIX ) )
		target_chan->flags.bdry = true;
	else
		target_chan->flags.bdry = false;

	if ( TestSoundChar( pSfx->getname(), CHAR_FAST_PITCH ) )
		target_chan->flags.bfast_pitch = true;
	else
		target_chan->flags.bfast_pitch = false;

	// get sound spatialization encoding

	target_chan->wavtype = 0;

	if ( TestSoundChar( pSfx->getname(), CHAR_DOPPLER ) )
		target_chan->wavtype = CHAR_DOPPLER;

	if ( TestSoundChar( pSfx->getname(), CHAR_DIRECTIONAL ) )
		target_chan->wavtype = CHAR_DIRECTIONAL;

	if ( TestSoundChar( pSfx->getname(), CHAR_DISTVARIANT ) )
		target_chan->wavtype = CHAR_DISTVARIANT;

	if ( TestSoundChar( pSfx->getname(), CHAR_OMNI ) )
		target_chan->wavtype = CHAR_OMNI;

	if ( TestSoundChar( pSfx->getname(), CHAR_SPATIALSTEREO ) )
		target_chan->wavtype = CHAR_SPATIALSTEREO;
}

char* MXR_GetGroupnameFromId( int mixgroupid )
{
	// scan group rules for mapping from name to id
	if ( mixgroupid < 0 )
		return NULL;

	for ( int i = 0; i < (*pg_cgrouprules); i++ )
	{
		if ( pg_grouprules[i].mixgroupid == mixgroupid )
			return pg_grouprules[i].szmixgroup;
	}

	return NULL;
}

bool S_IsMusic( channel_t* pChannel )
{
	if ( !pChannel->flags.bdry )
		return false;

	CSfxTable* sfx = pChannel->sfx;
	if ( !sfx )
		return false;

	CAudioSource* source = sfx->pSource;
	if ( !source )
		return false;

	// Don't save restore looping sounds as you can end up with an entity restarting them again and have 
	//  them accumulate, etc.
	if ( source->IsLooped() )
		return false;

	CAudioMixer* pMixer = pChannel->pMixer;
	if ( !pMixer )
		return false;

	for ( int i = 0; i < 8; i++ )
	{
		if ( pChannel->mixgroups[i] != -1 )
		{
			char* pGroupName = MXR_GetGroupnameFromId( pChannel->mixgroups[i] );
			if ( !strcmp( pGroupName, "Music" ) )
			{
				return true;
			}
		}
	}
	return false;
}

bool IsValidSampleRate( int rate )
{
	return rate == SOUND_11k || rate == SOUND_22k || rate == SOUND_44k;
}

// Take a regular sndlevel and convert it to compatibility mode.
#define SNDLEVEL_TO_COMPATIBILITY_MODE( x )		((soundlevel_t)(int)( (x) + 256 ))

// Take a compatibility-mode sndlevel and get the REAL sndlevel out of it.
#define SNDLEVEL_FROM_COMPATIBILITY_MODE( x )	((soundlevel_t)(int)( (x) - 256 ))

// Tells if the given sndlevel is marked as compatibility mode.
#define SNDLEVEL_IS_COMPATIBILITY_MODE( x )		( (x) >= soundlevel_t(256) )

float snd_refdist_GetFloat()
{
	static ConVar* snd_refdb = cvar->FindVar( "snd_refdb" );
	return snd_refdb->GetFloat(); // there's no real reason for me to just not make it static globally.
}
#define SNDLVL_TO_DIST_MULT( sndlvl ) ( sndlvl ? ((pow( 10.0f, snd_refdist_GetFloat() / 20 ) / pow( 10.0f, (float)sndlvl / 20 )) / snd_refdist_GetFloat()) : 0 )

void ChannelClearVolumes( channel_t* pch )
{
	for ( int i = 0; i < CCHANVOLUMES; i++ )
	{
		pch->fvolume[i] = 0.0;
		pch->fvolume_target[i] = 0.0;
		pch->fvolume_inc[i] = 0.0;
	}
}

void SND_SpatializeFirstFrameNoTrace( channel_t* pChannel )
{
	extern void (*SND_Spatialize)(channel_t * ch);
	static ConVar* snd_defer_trace = cvar->FindVar( "snd_defer_trace" );
	if ( snd_defer_trace->GetBool() )
	{
		// set up tracing state to be non-obstructed
		pChannel->flags.bfirstpass = false;
		pChannel->flags.bTraced = true;
		pChannel->ob_gain = 1.0;
		pChannel->ob_gain_inc = 1.0;
		pChannel->ob_gain_target = 1.0;
		// now spatialize without tracing
		SND_Spatialize( pChannel );
		// now reset tracing state to firstpass so the trace gets done on next spatialize
		pChannel->ob_gain = 0.0;
		pChannel->ob_gain_inc = 0.0;
		pChannel->ob_gain_target = 0.0;
		pChannel->flags.bfirstpass = true;
		pChannel->flags.bTraced = false;
	}
	else
	{
		pChannel->ob_gain = 0.0;
		pChannel->ob_gain_inc = 0.0;
		pChannel->ob_gain_target = 0.0;
		pChannel->flags.bfirstpass = true;
		pChannel->flags.bTraced = false;
		SND_Spatialize( pChannel );
	}
}

/*
=====================
SND_PickStaticChannel
=====================
Pick an empty channel from the static sound area, or allocate a new
channel.  Only fails if we're at max_channels (128!!!) or if
we're trying to allocate a channel for a stream sound that is
already playing.

*/
channel_t* SND_PickStaticChannel( int soundsource, CSfxTable* pSfx )
{
	int i;
	channel_t* ch = NULL;

	// Check for replacement sound, or find the best one to replace
	for ( i = MAX_DYNAMIC_CHANNELS; i < (*ptotal_channels); i++ )
		if ( channels[i].sfx == NULL )
			break;

	if ( i < (*ptotal_channels) )
	{
		// reuse an empty static sound channel
		ch = &channels[i];
	}
	else
	{
		// no empty slots, alloc a new static sound channel
		if ( (*ptotal_channels) == MAX_CHANNELS )
		{
			DevMsg( "total_channels == MAX_CHANNELS\n" );
			return NULL;
		}

		// get a channel for the static sound
		ch = &channels[(*ptotal_channels)];
		(*ptotal_channels)++;
	}

	return ch;
}

/*
=================
S_StartStaticSound
=================
Start playback of a sound, loaded into the static portion of the channel array.
Currently, this should be used for looping ambient sounds, looping sounds
that should not be interrupted until complete, non-creature sentences,
and one-shot ambient streaming sounds.  Can also play 'regular' sounds one-shot,
in case designers want to trigger regular game sounds.
Pitch changes playback pitch of wave by % above or below 100.  Ignored if pitch == 100

  NOTE: volume is 0.0 - 1.0 and attenuation is 0.0 - 1.0 when passed in.
*/

int S_StartStaticSound( StartSoundParams_t& params )
{
	Assert( params.staticsound == true );

	channel_t* ch;
	CAudioSource* pSource = NULL;

	if ( !(*pg_AudioDevice)->IsActive() )
		return 0;

	if ( !params.pSfx )
		return 0;

	// For debugging to see the actual name of the sound...
	char sndname[MAX_PATH];
	strncpy( sndname, params.pSfx->getname(), sizeof( sndname ) );
	//	Msg("Start static sound %s\n", pSfx->getname() );

	int vol = params.fvol * 255;
	if ( vol > 255 )
	{
		DevMsg( "S_StartStaticSound: %s volume > 255", sndname );
		vol = 255;
	}

	static ConVar* snd_showstart = cvar->FindVar( "snd_showstart" );
	int nSndShowStart = snd_showstart->GetInt();

	if ( (params.flags & SND_STOP) && nSndShowStart > 0 )
		DevMsg( "S_StartStaticSound: %s Stopped.\n", sndname );

	if ( (params.flags & SND_STOP) || (params.flags & SND_CHANGE_VOL) || (params.flags & SND_CHANGE_PITCH) )
	{
		if ( S_AlterChannel( params.soundsource, params.entchannel, params.pSfx, vol, params.pitch, params.flags ) || (params.flags & SND_STOP) )
			return 0;
	}

	if ( params.pitch == 0 )
	{
		DevMsg( "Warning: S_StartStaticSound Ignored, called with pitch 0\n" );
		return 0;
	}

	// First, make sure the sound source entity is even in the PVS.
	float flSoundRadius = 0.0f;

	bool looping = false;

	/*
	CAudioSource *pSource = pSfx ? pSfx->pSource : NULL;
	if ( pSource )
	{
		looping = pSource->IsLooped();
	}
	*/

	SpatializationInfo_t si;
	si.info.Set(
		params.soundsource,
		params.entchannel,
		params.pSfx ? sndname : "",
		params.origin,
		params.direction,
		vol,
		params.soundlevel,
		looping,
		params.pitch,
		*plistener_origin,
		params.speakerentity );

	si.type = SpatializationInfo_t::SI_INCREATION;

	si.pOrigin = NULL;
	si.pAngles = NULL;
	si.pflRadius = &flSoundRadius;

	g_pSoundServices->GetSoundSpatialization( params.soundsource, si );

	// pick a channel to play on from the static area
	THREAD_LOCK_SOUND();

	ch = SND_PickStaticChannel( params.soundsource, params.pSfx ); // Autolooping sounds are always fixed origin(?)
	if ( !ch )
		return 0;

	extern void (*SND_ActivateChannel)(channel_t * pChannel);
	SND_ActivateChannel( ch );

	ChannelClearVolumes( ch );

	ch->userdata = params.userdata;
	ch->initialStreamPosition = params.initialStreamPosition;

	if ( ch->userdata != 0 )
	{
		g_pSoundServices->GetToolSpatialization( ch->userdata, ch->guid, si );
	}

	int channelIndex = ch - channels;
	(*pg_AudioDevice)->ChannelReset( params.soundsource, channelIndex, ch->dist_mult );

	if ( TestSoundChar( sndname, CHAR_SENTENCE ) )
	{
		// this is a sentence. link words to play in sequence.

		// NOTE: sentence names stored in the cache lookup are
		// prepended with a '!'.  Sentence names stored in the
		// sentence file do not have a leading '!'. 

		// link all words and load the first word"
		extern void (*VOX_LoadSound)(channel_t * pchan, const char* pszin);
		VOX_LoadSound( ch, PSkipSoundChars( sndname ) );
	}
	else
	{
		// load regular or stream sound
		extern CAudioSource* (*S_LoadSound)(CSfxTable * pSfx, channel_t * ch);
		pSource = S_LoadSound( params.pSfx, ch );
		if ( pSource && !IsValidSampleRate( pSource->SampleRate() ) )
		{
			Warning( "*** Invalid sample rate (%d) for sound '%s'.\n", pSource->SampleRate(), sndname );
		}

		if ( !pSource && !params.pSfx->m_bIsLateLoad )
		{
			Warning( "Failed to load sound \"%s\", file probably missing from disk/repository\n", sndname );
		}

		ch->sfx = params.pSfx;
		ch->flags.isSentence = false;
	}

	if ( !ch->pMixer )
	{
		// couldn't load sounds' data, or sentence has 0 words (not an error)
		S_FreeChannel( ch );
		return 0;
	}

	VectorCopy( params.origin, ch->origin );
	VectorCopy( params.direction, ch->direction );

	// never update positions if source entity is 0
	ch->flags.bUpdatePositions = params.bUpdatePositions && (params.soundsource == 0 ? 0 : 1);

	ch->master_vol = vol;

	ch->flags.m_bCompatibilityAttenuation = SNDLEVEL_IS_COMPATIBILITY_MODE( params.soundlevel );
	if ( ch->flags.m_bCompatibilityAttenuation )
	{
		// Translate soundlevel from its 'encoded' value to a real soundlevel that we can use in the sound system.
		params.soundlevel = SNDLEVEL_FROM_COMPATIBILITY_MODE( params.soundlevel );
	}

	ch->dist_mult = SNDLVL_TO_DIST_MULT( params.soundlevel );

	S_SetChannelWavtype( ch, params.pSfx );

	ch->basePitch = params.pitch;
	ch->soundsource = params.soundsource;
	ch->entchannel = params.entchannel;
	ch->flags.fromserver = params.fromserver;
	ch->flags.bSpeaker = (params.flags & SND_SPEAKER) ? 1 : 0;
	ch->speakerentity = params.speakerentity;

	ch->flags.m_bShouldPause = (params.flags & SND_SHOULDPAUSE) ? 1 : 0;

	// TODO: Support looping sounds through speakers.
	// If the sound is from a speaker, and it's looping, ignore it.
	if ( ch->flags.bSpeaker )
	{
		if ( params.pSfx->pSource && params.pSfx->pSource->IsLooped() )
		{
			if ( nSndShowStart > 0 && nSndShowStart < 7 && nSndShowStart != 4 )
			{
				DevMsg( "StaticSound : Speaker ignored looping sound: %s\n", sndname );
			}

			S_FreeChannel( ch );
			return 0;
		}
	}

	// set the default radius
	ch->radius = flSoundRadius;

	S_SetChannelStereo( ch, pSource );

	// initialize dsp room mixing params
	ch->dsp_mix_min = -1;
	ch->dsp_mix_max = -1;

	if ( nSndShowStart == 5 )
	{
		snd_showstart->SetValue( 6 );		// display gain once only
		nSndShowStart = 6;
	}

	// get sound type before we spatialize
	extern void (*MXR_GetMixGroupFromSoundsource)( channel_t * pchan, SoundSource soundsource, soundlevel_t soundlevel );
	MXR_GetMixGroupFromSoundsource( ch, params.soundsource, params.soundlevel );

	// skip the trace on the first spatialization.  This channel may be stolen
	// by another sound played this frame.  Defer the trace to the mix loop
	SND_SpatializeFirstFrameNoTrace( ch );

	// Init client entity mouth movement vars
	ch->flags.m_bIgnorePhonemes = (params.flags & SND_IGNORE_PHONEMES) != 0;
	SND_InitMouth( ch );

	// Pre-startup delay.  Compute # of samples over which to mix in zeros from data source before
	// actually reading first set of samples
	if ( params.delay != 0.0f )
	{
		Assert( ch->sfx );
		Assert( ch->sfx->pSource );

		float rate = ch->sfx->pSource->SampleRate();

		int delaySamples = (int)(params.delay * rate * params.pitch * 0.01f);

		ch->pMixer->SetStartupDelaySamples( delaySamples );

		if ( params.delay > 0 )
		{
			ch->pMixer->SetStartupDelaySamples( delaySamples );
			ch->flags.delayed_start = true;
		}
		else
		{
			int skipSamples = -delaySamples;
			int totalSamples = ch->sfx->pSource->SampleCount();

			if ( ch->sfx->pSource->IsLooped() )
			{
				skipSamples = skipSamples % totalSamples;
			}

			if ( skipSamples >= totalSamples )
			{
				S_FreeChannel( ch );
				return 0;
			}

			ch->pitch = ch->basePitch * 0.01f;
			ch->pMixer->SkipSamples( ch, skipSamples, rate, 0 );
			ch->ob_gain_target = 1.0f;
			ch->ob_gain = 1.0f;
			ch->ob_gain_inc = 0.0f;
			ch->flags.bfirstpass = false;
		}
	}

	if ( S_IsMusic( ch ) )
	{
		// See if we have "music" of same name playing from "world" which means we save/restored this sound already.  If so,
		//  kill the new version and update the soundsource
		CChannelList list;
		g_ActiveChannels.GetActiveChannels( list );
		for ( int i = 0; i < list.Count(); i++ )
		{
			channel_t* pChannel = list.GetChannel( i );
			// Don't mess with the channel we just created, of course
			if ( ch == pChannel )
				continue;
			if ( ch->sfx != pChannel->sfx )
				continue;
			if ( pChannel->soundsource != SOUND_FROM_WORLD )
				continue;
			if ( !S_IsMusic( pChannel ) )
				continue;

			DevMsg( 1, "Hooking duplicate restored song track %s\n", sndname );

			// the new channel will have an updated soundsource and probably
			// has an updated pitch or volume since we are receiving this sound message
			// after the sound has started playing (usually a volume change)
			// copy that data out of the source
			pChannel->soundsource = ch->soundsource;
			pChannel->master_vol = ch->master_vol;
			pChannel->basePitch = ch->basePitch;
			pChannel->pitch = ch->pitch;
			S_FreeChannel( ch );

			return 0;
		}
	}

	g_pSoundServices->OnSoundStarted( ch->guid, params, sndname );

	if ( nSndShowStart > 0 && nSndShowStart < 7 && nSndShowStart != 4 )
	{
		DevMsg( "StaticSound %s : src %d : channel %d : %d dB : vol %.2f : radius %.0f : time %.3f\n", sndname, params.soundsource, params.entchannel, params.soundlevel, params.fvol, flSoundRadius, g_pSoundServices->GetHostTime() );
		if ( nSndShowStart == 2 || nSndShowStart == 5 )
			DevMsg( "\t dspmix %1.2f : distmix %1.2f : dspface %1.2f : lvol %1.2f : cvol %1.2f : rvol %1.2f : rlvol %1.2f : rrvol %1.2f\n",
				ch->dspmix, ch->distmix, ch->dspface,
				ch->fvolume[IFRONT_LEFT], ch->fvolume[IFRONT_CENTER], ch->fvolume[IFRONT_RIGHT], ch->fvolume[IREAR_LEFT], ch->fvolume[IREAR_RIGHT] );
		if ( nSndShowStart == 3 )
			DevMsg( "\t x: %4f y: %4f z: %4f\n", ch->origin.x, ch->origin.y, ch->origin.z );
	}

	return ch->guid;
}

void CEngineSoundClient_StopAllSounds( bool bClearBuffers )
{
	S_StopAllSounds( bClearBuffers );
}

void snd_dumpclientsounds( const ConCommand& args )
{
	DevWarning( "'snd_dumpclientsounds' command disabled temporarily, due to not being implemented. "
				"If this is in a public release, report it to Grizzle!\n" );
}

void MXR_DebugShowMixVolumes( void )
{
	DevWarning( "'MXR_DebugShowMixVolumes' function disabled temporarily, due to not being implemented. "
				"If this is in a public release, report it to Grizzle!\n" );
}