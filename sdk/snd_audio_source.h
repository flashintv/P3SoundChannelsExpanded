//========= Copyright © 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
// $Workfile:     $
// $Date:         $
//
//-----------------------------------------------------------------------------
// $Log: $
//
// $NoKeywords: $
//=============================================================================//

#ifndef SND_AUDIO_SOURCE_H
#define SND_AUDIO_SOURCE_H
#pragma once

#if !defined( _X360 )
#define MP3_SUPPORT	1
#endif

#define AUDIOSOURCE_COPYBUF_SIZE	4096

struct channel_t;
class CSentence;
class CSfxTable;

class CAudioSource;
class IAudioDevice;
class CUtlBuffer;

//-----------------------------------------------------------------------------
// Purpose: This is an instance of an audio source.
//			Mixers are attached to channels and reference an audio source.
//			Mixers are specific to the sample format and source format.
//			Mixers are never re-used, so they can track instance data like
//			sample position, fractional sample, stream cache, faders, etc.
//-----------------------------------------------------------------------------
class CAudioMixer
{
public:
	virtual ~CAudioMixer( void ) {}

	// return number of samples mixed
	virtual int MixDataToDevice( IAudioDevice * pDevice, channel_t * pChannel, int sampleCount, int outputRate, int outputOffset ) = 0;
	virtual int SkipSamples( channel_t* pChannel, int sampleCount, int outputRate, int outputOffset ) = 0;
	virtual bool ShouldContinueMixing( void ) = 0;

	virtual CAudioSource* GetSource( void ) = 0;

	// get the current position (next sample to be mixed)
	virtual int GetSamplePosition( void ) = 0;

	// Allow the mixer to modulate pitch and volume. 
	// returns a floating point modulator
	virtual float ModifyPitch( float pitch ) = 0;
	virtual float GetVolumeScale( void ) = 0;

	// NOTE: Playback is optimized for linear streaming.  These calls will usually cost performance
	// It is currently optimal to call them before any playback starts, but some audio sources may not
	// guarantee this.  Also, some mixers may choose to ignore these calls for internal reasons (none do currently).

	// Move the current position to newPosition 
	// BUGBUG: THIS CALL DOES NOT SUPPORT MOVING BACKWARD, ONLY FORWARD!!!
	virtual void SetSampleStart( int newPosition ) = 0;

	// End playback at newEndPosition
	virtual void SetSampleEnd( int newEndPosition ) = 0;

	// How many samples to skip before commencing actual data reading ( to allow sub-frametime sound
	//  offsets and avoid synchronizing sounds to various 100 msec clock intervals throughout the
	//  engine and game code)
	virtual void SetStartupDelaySamples( int delaySamples ) = 0;
	virtual int GetMixSampleSize() = 0;

	// Certain async loaded sounds lazilly load into memory in the background, use this to determine
	//  if the sound is ready for mixing
	virtual bool IsReadyToMix() = 0;

	// NOTE: The "saved" position can be different than the "sample" position
	// NOTE: Allows mixer to save file offsets, loop info, etc
	virtual int GetPositionForSave() = 0;
	virtual void SetPositionFromSaved( int savedPosition ) = 0;
};

//-----------------------------------------------------------------------------
// Purpose: A source is an abstraction for a stream, cached file, or procedural
//			source of audio.
//-----------------------------------------------------------------------------
class CAudioSource
{
public:
	enum
	{
		AUDIO_SOURCE_UNK = 0,
		AUDIO_SOURCE_WAV,
		AUDIO_SOURCE_MP3,
		AUDIO_SOURCE_VOICE,

		AUDIO_SOURCE_MAXTYPE,
	};

	enum
	{
		AUDIO_NOT_LOADED = 0,
		AUDIO_IS_LOADED = 1,
		AUDIO_LOADING = 2,
	};

	virtual ~CAudioSource( void ) {}

	// Create an instance (mixer) of this audio source
	virtual CAudioMixer* CreateMixer( int initialStreamPosition = 0 ) = 0;

	// Serialization for caching
	virtual int					GetType( void ) = 0;
	virtual void				GetCacheData( class CAudioSourceCachedInfo* info ) = 0;

	// Provide samples for the mixer. You can point pData at your own data, or if you prefer to copy the data,
	// you can copy it into copyBuf and set pData to copyBuf.
	virtual int					GetOutputData( void** pData, int samplePosition, int sampleCount, char copyBuf[AUDIOSOURCE_COPYBUF_SIZE] ) = 0;

	virtual int					SampleRate( void ) = 0;

	// Returns true if the source is a voice source.
	// This affects the voice_overdrive behavior (all sounds get quieter when
	// someone is speaking).
	virtual bool				IsVoiceSource() = 0;

	// Sample size is in bytes.  It will not be accurate for compressed audio.  This is a best estimate.
	// The compressed audio mixers understand this, but in general do not assume that SampleSize() * SampleCount() = filesize
	// or even that SampleSize() is 100% accurate due to compression.
	virtual int					SampleSize( void ) = 0;

	// Total number of samples in this source.  NOTE: Some sources are infinite (mic input), they should return
	// a count equal to one second of audio at their current rate.
	virtual int					SampleCount( void ) = 0;

	virtual int					Format( void ) = 0;
	virtual int					DataSize( void ) = 0;

	virtual bool				IsLooped( void ) = 0;
	virtual bool				IsStereoWav( void ) = 0;
	virtual bool				IsStreaming( void ) = 0;
	virtual int					GetCacheStatus( void ) = 0;
	int 						IsCached( void ) { return GetCacheStatus() == AUDIO_IS_LOADED ? true : false; }
	virtual void				CacheLoad( void ) = 0;
	virtual void				CacheUnload( void ) = 0;
	virtual CSentence* GetSentence( void ) = 0;

	// these are used to find good splice/loop points.
	// If not implementing these, simply return sample
	virtual int					ZeroCrossingBefore( int sample ) = 0;
	virtual int					ZeroCrossingAfter( int sample ) = 0;

	// mixer's references
	virtual void				ReferenceAdd( CAudioMixer* pMixer ) = 0;
	virtual void				ReferenceRemove( CAudioMixer* pMixer ) = 0;

	// check reference count, return true if nothing is referencing this
	virtual bool				CanDelete( void ) = 0;

	virtual void				Prefetch() = 0;

	virtual bool				IsAsyncLoad() = 0;

	// Make sure our data is rebuilt into the per-level cache
	virtual void				CheckAudioSourceCache() = 0;

	virtual char const* GetFileName() = 0;

	virtual void				SetPlayOnce( bool ) = 0;
	virtual bool				IsPlayOnce() = 0;

	// Used to identify a word that is part of a sentence mixing operation
	virtual void				SetSentenceWord( bool bIsWord ) = 0;
	virtual bool				IsSentenceWord() = 0;

	virtual int					SampleToStreamPosition( int samplePosition ) = 0;
	virtual int					StreamToSamplePosition( int streamPosition ) = 0;
};

#endif // SND_AUDIO_SOURCE_H