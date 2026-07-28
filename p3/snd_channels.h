//========= Copyright ? 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose:
//
//=============================================================================//

#ifndef SND_CHANNELS_H
#define SND_CHANNELS_H

#include "../sdk/vector.h"
#include "../sdk/iconvar.h"
#include "../sdk/cdll_int.h"
#include "../sdk/snd_audio_source.h"

#if defined( _WIN32 )
#pragma once
#endif

class CSentence;
class CAudioMixer;
class CAudioSourceCachedInfo;
typedef int SoundSource;

// DO NOT REORDER: indices to fvolume arrays in channel_t 

#define IFRONT_LEFT		0			// NOTE: must correspond to order of fvolume array below!
#define IFRONT_RIGHT	1
#define	IREAR_LEFT		2
#define IREAR_RIGHT		3
#define IFRONT_CENTER	4
#define IFRONT_CENTER0	5			// dummy slot - center channel is mono, but mixers reference volume[1] slot

#define IFRONT_LEFTD	6			// start of doppler right array			
#define IFRONT_RIGHTD	7
#define	IREAR_LEFTD		8
#define IREAR_RIGHTD	9
#define IFRONT_CENTERD	10
#define IFRONT_CENTERD0 11			// dummy slot - center channel is mono, but mixers reference volume[1] slot

#define CCHANVOLUMES	12

enum SoundFlags_t
{
	SND_NOFLAGS = 0,			// to keep the compiler happy
	SND_CHANGE_VOL = (1 << 0),		// change sound vol
	SND_CHANGE_PITCH = (1 << 1),		// change sound pitch
	SND_STOP = (1 << 2),		// stop the sound
	SND_SPAWNING = (1 << 3),		// we're spawning, used in some cases for ambients
	// not sent over net, only a param between dll and server.
	SND_DELAY = (1 << 4),		// sound has an initial delay
	SND_STOP_LOOPING = (1 << 5),		// stop all looping sounds on the entity.
	SND_SPEAKER = (1 << 6),		// being played again by a microphone through a speaker

	SND_SHOULDPAUSE = (1 << 7),		// this sound should be paused if the game is paused
	SND_IGNORE_PHONEMES = (1 << 8),
	SND_IGNORE_NAME = (1 << 9),		// used to change all sounds emitted by an entity, regardless of scriptname

	SND_DO_NOT_OVERWRITE_EXISTING_ON_CHANNEL = (1 << 10),
};

#define CMXRNAMEMAX 32
struct grouprule_t
{
	char			szmixgroup[CMXRNAMEMAX];	// mix group name
	int				mixgroupid;					// mix group unique id
	char			szdir[CMXRNAMEMAX];			// substring to search for in ch->sfx
	int				classId;					// index of classname
	int				chantype;					// channel type (CHAN_WEAPON, etc)
	int				soundlevel_min;				// min soundlevel
	int				soundlevel_max;				// max soundlevel

	int				priority;					// 0..100 higher priority sound groups duck all lower pri groups if enabled
	int				is_ducked;					// if 1, sound group is ducked by all higher priority 'causes_duck" sounds
	int				causes_ducking;				// if 1, sound group ducks other 'is_ducked' sounds of lower priority
	float			duck_target_pct;			// if sound group is ducked, target percent of original volume

	float			total_vol;					// total volume of all sounds in this group, if group can cause ducking
	float			ducker_threshold;			// ducking is caused by this group if total_vol > ducker_threshold
	// and causes_ducking is enabled.
	float			duck_target_vol;			// target volume while ducking	
	float			duck_ramp_val;				// current value of ramp - moves towards duck_target_vol
};

//

class CSfxTable
{
public:
	virtual const char* getname() = 0;
	FileNameHandle_t GetFileNameHandle();

public:
	int					m_namePoolIndex;
	CAudioSource*		pSource;

	bool				m_bUseErrorFilename : 1;
	bool				m_bIsUISound : 1;
	bool				m_bIsLateLoad : 1;
	bool				m_bMixGroupsCached : 1;
	byte				m_mixGroupCount;
	// UNDONE: Use a fixed bit vec here?
	byte				m_mixGroupList[8];

	// Only set in debug mode so you can see the name.
	const char*			m_pDebugName;
};

struct SfxDictEntry
{
	CSfxTable* pSfx;
};

//-----------------------------------------------------------------------------
// Purpose: Each currently playing wave is stored in a channel
//-----------------------------------------------------------------------------
// NOTE: 128bytes.  These are memset to zero at some points.  Do not add virtuals without changing that pattern.
// UNDONE: now 300 bytes...
struct channel_t
{
	int			guid;			// incremented each time a channel is allocated (to match with channel free in tools, etc.)
	int			userdata;		// user specified data for syncing to tools

	CSfxTable* sfx;			// the actual sound
	CAudioMixer* pMixer;		// The sound's instance data for this channel

	// speaker channel volumes, indexed using IFRONT_LEFT to IFRONT_CENTER.
	// NOTE: never access these fvolume[] elements directly! Use channel helpers in snd_dma.cpp.

	float		fvolume[CCHANVOLUMES];			// 0.0-255.0 current output volumes
	float		fvolume_target[CCHANVOLUMES];	// 0.0-255.0 target output volumes
	float		fvolume_inc[CCHANVOLUMES];		// volume increment, per frame, moves volume[i] to vol_target[i] (per spatialization)		

	SoundSource	soundsource;	// see iclientsound.h for description.
	int			entchannel;		// sound channel (CHAN_STREAM, CHAN_VOICE, etc.)
	int			speakerentity;  // if a sound is being played through a speaker entity (e.g., on a monitor,), this is the
	//  entity upon which to show the lips moving, if the sound has sentence data
	short		master_vol;		// 0-255 master volume
	short		basePitch;		// base pitch percent (100% is normal pitch playback)
	float		pitch;			// real-time pitch after any modulation or shift by dynamic data
	int			mixgroups[8];	// sound belongs to these mixgroups: world, actor, player weapon, explosion etc.
	int			last_mixgroupid;// last mixgroupid selected
	float		last_vol;		// last volume after spatialization

	Vector		origin;			// origin of sound effect
	Vector		direction;		// direction of the sound
	float		dist_mult;		// distance multiplier (attenuation/clipK)


	float		dspmix;			// 0 - 1.0 proportion of dsp to mix with original sound, based on distance
	float		dspface;		// -1.0 - 1.0 (1.0 = facing listener)
	float		distmix;		// 0 - 1.0 proportion based on distance from listner (1.0 - 100% wav right - far)
	float		dsp_mix_min;	// for dspmix calculation - set by current preset in SND_GetDspMix
	float		dsp_mix_max;	// for dspmix calculation - set by current preset in SND_GetDspMix

	float		radius;			// Radius of this sound effect (spatialization is different within the radius)

	float		ob_gain;		// gain drop if sound source obscured from listener
	float		ob_gain_target;	// target gain while crossfading between ob_gain & ob_gain_target
	float		ob_gain_inc;	// crossfade increment

	short		activeIndex;
	char		wavtype;		// 0 default, CHAR_DOPPLER, CHAR_DIRECTIONAL, CHAR_DISTVARIANT
	char		pad;

	char		sample_prev[8];	// last sample(s) in previous input data buffer - space for 2, 16 bit, stereo samples

	int			initialStreamPosition;

	union
	{
		unsigned int flagsword;
		struct
		{
			bool		bUpdatePositions : 1; // if true, assume sound source can move and update according to entity
			bool		isSentence : 1;		// true if playing linked sentence
			bool		bdry : 1;			// if true, bypass all dsp processing for this sound (ie: music)	
			bool		bSpeaker : 1;		// true if sound is playing through in-game speaker entity.
			bool		bstereowav : 1;		// if true, a stereo .wav file is the sample data source

			bool		delayed_start : 1;  // If true, sound had a delay and so same sound on same channel won't channel steal from it
			bool		fromserver : 1;		// for snd_show, networked sounds get colored differently than local sounds

			bool		bfirstpass : 1;		// true if this is first time sound is spatialized
			bool		bTraced : 1;		// true if channel was already checked this frame for obscuring
			bool		bfast_pitch : 1;	// true if using low quality pitch (fast, but no interpolation)

			bool		m_bIsFreeingChannel : 1;	// true when inside S_FreeChannel - prevents reentrance
			bool		m_bCompatibilityAttenuation : 1;	// True when we want to use goldsrc compatibility mode for the attenuation
			// In that case, dist_mul is set to a relatively meaningful value in StartDynamic/StartStaticSound,
			// but we interpret it totally differently in SND_GetGain.
			bool		m_bShouldPause : 1;	// if true, sound should pause when the game is paused
			bool		m_bIgnorePhonemes : 1;	// if true, we don't want to drive animation w/ phoneme data
		} flags;
	};
};


//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

// original values used in Postal 3
#define	OLD_MAX_CHANNELS			128
#define	OLD_MAX_DYNAMIC_CHANNELS	24

// new changed values by me (we dont really need to change the static number of channels, we only care about dynamic)
#define	MAX_DYNAMIC_CHANNELS	256
#define MAX_CHANNELS MAX_DYNAMIC_CHANNELS + (OLD_MAX_CHANNELS - OLD_MAX_DYNAMIC_CHANNELS)

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

extern	channel_t   channels[MAX_CHANNELS];
// 0 to MAX_DYNAMIC_CHANNELS-1	= normal entity sounds
// MAX_DYNAMIC_CHANNELS to total_channels = static sounds

extern	int*		ptotal_channels;

class CChannelList
{
public:
	int		Count();
	int		GetChannelIndex( int listIndex );
	channel_t* GetChannel( int listIndex );
	void	RemoveChannelFromList( int listIndex );
	bool	IsQuashed( int listIndex );

	int		m_count;
	short	m_list[MAX_CHANNELS];
	bool	m_quashed[MAX_CHANNELS]; // if true, the channel should be advanced, but not mixed, because it's been heuristically suppressed
	bool	m_hasSpeakerChannels : 1;
	bool	m_hasDryChannels : 1;
	bool	m_has11kChannels : 1;
	bool	m_has22kChannels : 1;
	bool	m_has44kChannels : 1;
};

inline int CChannelList::Count()
{
	return m_count;
}

inline int CChannelList::GetChannelIndex( int listIndex )
{
	return m_list[listIndex];
}
inline channel_t* CChannelList::GetChannel( int listIndex )
{
	return &channels[GetChannelIndex( listIndex )];
}

inline bool CChannelList::IsQuashed( int listIndex )
{
	return m_quashed[listIndex];
}

inline void CChannelList::RemoveChannelFromList( int listIndex )
{
	// decrease the count by one, and swap the deleted channel with
	// the last one.
	m_count--;
	if ( m_count > 0 && listIndex != m_count )
	{
		m_list[listIndex] = m_list[m_count];
		m_quashed[listIndex] = m_quashed[m_count];
	}
}

template< int size = MAX_CHANNELS >
class CActiveChannels
{
public:
	void Add( channel_t* pChannel );
	void Remove( channel_t* pChannel );

	void GetActiveChannels( CChannelList& list );

	void Init();
	int	 GetActiveCount() { return m_count; }
//private:
	int		m_count;
	short	m_list[size];
};

// Structure used for fading in and out client sound volume.
typedef struct
{
	float		initial_percent;

	// How far to adjust client's volume down by.
	float		percent;

	// GetHostTime() when we started adjusting volume
	float		starttime;

	// # of seconds to get to faded out state
	float		fadeouttime;
	// # of seconds to hold
	float		holdtime;
	// # of seconds to restore
	float		fadeintime;
} soundfade_t;

struct SndInfo_t
{
	// Sound Guid
	int			m_nGuid;
	FileNameHandle_t m_filenameHandle;		// filesystem filename handle - call IFilesystem to conver this to a string
	int			m_nSoundSource;
	int			m_nChannel;
	// If a sound is being played through a speaker entity (e.g., on a monitor,), this is the
	//  entity upon which to show the lips moving, if the sound has sentence data
	int			m_nSpeakerEntity;
	float		m_flVolume;
	float		m_flLastSpatializedVolume;
	// Radius of this sound effect (spatialization is different within the radius)
	float		m_flRadius;
	int			m_nPitch;
	Vector* m_pOrigin;
	Vector* m_pDirection;

	// if true, assume sound source can move and update according to entity
	bool		m_bUpdatePositions;
	// true if playing linked sentence
	bool		m_bIsSentence;
	// if true, bypass all dsp processing for this sound (ie: music)	
	bool		m_bDryMix;
	// true if sound is playing through in-game speaker entity.
	bool		m_bSpeaker;
	// for snd_show, networked sounds get colored differently than local sounds
	bool		m_bFromServer;
};

int ChannelGetMaxVol( channel_t* pch );
unsigned int RemainingSamples( channel_t* pChannel );
void SND_InitMouth( channel_t* pChannel );
bool SND_IsMouth( channel_t* pChannel );
bool BChannelLowVolume( channel_t* pch, int vol_min );
float ChannelLoudestCurVolume( const channel_t* RESTRICT pch );
void S_FreeChannel( channel_t* ch );
void MIX_MixChannelsToPaintbuffer( CChannelList& list, int endtime, int flags, int rate, int outputRate );
void CEngineSoundClient_StopAllSounds( bool bClearBuffers );
int S_StartStaticSound( StartSoundParams_t& params );
void S_Update( const AudioState_t* pAudioState );
void S_GetActiveSounds( CUtlVector< SndInfo_t >& sndlist );
void S_StopAllSounds( bool bClear );
void MXR_DebugShowMixVolumes( void );
void snd_dumpclientsounds( const ConCommand& args );
void MXR_UpdateAllDuckerVolumes( void );
channel_t* S_FindChannelByGuid( int guid );
channel_t* SND_StealDynamicChannel( SoundSource soundsource, int entchannel, const Vector& origin, CSfxTable* sfx );
channel_t* SND_PickStaticChannel( int soundsource, CSfxTable* pSfx );
void S_StopSound( int soundsource, int entchannel );
int S_AlterChannel( int soundsource, int entchannel, CSfxTable* sfx, int vol, int pitch, int flags );
void MIX_BuildChannelList( CChannelList& list );
void MIX_PaintChannels( int endtime, bool bIsUnderwater );

//=============================================================================

#endif // SND_CHANNELS_H