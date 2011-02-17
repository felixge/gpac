/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Copyright (c) ENST 2009-
 *				Authors: Jean Le Feuvre
 *					All rights reserved
 *
 *	Created by NGO Van Luyen / ARTEMIS / Telecom SudParis /Institut TELECOM on Oct, 2010
 * nvluyen81@gmail.com
 *
 *  This file is part of GPAC / Wrapper
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *   
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *   
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA. 
 *
 */

#include "javaenv.h"

#include <gpac/modules/audio_out.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#define STREAM_MUSIC 3
#define CHANNEL_CONFIGURATION_MONO 2
#define CHANNEL_CONFIGURATION_STEREO 3
#define ENCODING_PCM_8BIT 3
#define ENCODING_PCM_16BIT 2
#define MODE_STREAM 1
#define CHANNEL_OUT_MONO 4
#define CHANNEL_IN_STEREO 12
#define CHANNEL_IN_MONO 16


/*for channel codes*/
#include <gpac/constants.h>

static const char android_device[] = "Android Default";

static jclass cAudioTrack = NULL;
static jobject mtrack = NULL;

static jmethodID mAudioTrack;
static jmethodID mGetMinBufferSize;
static jmethodID mPlay;
static jmethodID mStop;
static jmethodID mRelease;
static jmethodID mWrite;
static jmethodID mFlush;


typedef struct 
{
	JNIEnv* env;

	jobject mtrack;

	u32 num_buffers;
	
	u32 vol, pan;
	u32 delay, total_length_ms;

	Bool force_config;
	u32 cfg_num_buffers, cfg_duration;
	
	u32 sampleRateInHz;
	u32 channelConfig; //AudioFormat.CHANNEL_OUT_MONO
	u32 audioFormat; //AudioFormat.ENCODING_PCM_16BIT
	s32 mbufferSizeInBytes;
	
	jarray buff;
} DroidContext;

//----------------------------------------------------------------------
//----------------------------------------------------------------------
// Called by the main thread
static GF_Err WAV_Setup(GF_AudioOutput *dr, void *os_handle, u32 num_buffers, u32 total_duration)
{
	DroidContext *ctx = (DroidContext *)dr->opaque;
	JNIEnv* env = GetEnv();
    	int channels;
    	int bytes;
	
	GF_LOG(GF_LOG_DEBUG, GF_LOG_CORE, ("[Android Audio] Setup\n"));

	ctx->force_config = (num_buffers && total_duration) ? 1 : 0;
	ctx->cfg_num_buffers = num_buffers;
	if (ctx->cfg_num_buffers <= 1) ctx->cfg_num_buffers = 2;
	ctx->cfg_duration = total_duration;
	if (!ctx->force_config) ctx->num_buffers = 1;

	if (!cAudioTrack){
		cAudioTrack = (*env)->FindClass(env, "android/media/AudioTrack");
        	if (!cAudioTrack) {
            		return GF_NOT_SUPPORTED;
        	}
		
		cAudioTrack = (*env)->NewGlobalRef(env, cAudioTrack);

		mAudioTrack = (*env)->GetMethodID(env, cAudioTrack, "<init>", "(IIIIII)V");
		mGetMinBufferSize = (*env)->GetStaticMethodID(env, cAudioTrack, "getMinBufferSize", "(III)I");
		mPlay = (*env)->GetMethodID(env, cAudioTrack, "play", "()V");
		mStop = (*env)->GetMethodID(env, cAudioTrack, "stop", "()V");
		mRelease = (*env)->GetMethodID(env, cAudioTrack, "release", "()V");
		mWrite = (*env)->GetMethodID(env, cAudioTrack, "write", "([BII)I");
		mFlush = (*env)->GetMethodID(env, cAudioTrack, "flush", "()V");
	}

	return GF_OK;
}
//----------------------------------------------------------------------
// Called by the audio thread
static void WAV_Shutdown(GF_AudioOutput *dr)
{
	DroidContext *ctx = (DroidContext *)dr->opaque;
	JNIEnv* env = ctx->env;

	(*env)->CallNonvirtualVoidMethod(env, mtrack, cAudioTrack, mStop);
	(*env)->CallNonvirtualVoidMethod(env, mtrack, cAudioTrack, mRelease);
	
	(*env)->PopLocalFrame(env, NULL);
	
	(*GetJavaVM())->DetachCurrentThread(GetJavaVM());
}


/*we assume what was asked is what we got*/
/* Called by the audio thread */
static GF_Err WAV_ConfigureOutput(GF_AudioOutput *dr, u32 *SampleRate, u32 *NbChannels, u32 *nbBitsPerSample, u32 channel_cfg)
{	
	JNIEnv* env = NULL;
	u32 i;
	DroidContext *ctx = (DroidContext *)dr->opaque;

	GF_LOG(GF_LOG_DEBUG, GF_LOG_CORE, ("[Android Audio] Configure Output\n"));

	if (!ctx) return GF_BAD_PARAM;
	
	ctx->sampleRateInHz = *SampleRate;
	ctx->channelConfig = (*NbChannels == 1) ? CHANNEL_CONFIGURATION_MONO : CHANNEL_CONFIGURATION_STEREO; //AudioFormat.CHANNEL_CONFIGURATION_MONO
	ctx->audioFormat = (*nbBitsPerSample == 8)? ENCODING_PCM_8BIT : ENCODING_PCM_16BIT; //AudioFormat.ENCODING_PCM_16BIT
	
	// Get the java environment in the new thread
	(*GetJavaVM())->AttachCurrentThread(GetJavaVM(), &env, NULL);
	ctx->env = env;
	GF_LOG(GF_LOG_DEBUG, GF_LOG_MEDIA, ("[Android Audio] SampleRate : %d \n",ctx->sampleRateInHz));
GF_LOG(GF_LOG_DEBUG, GF_LOG_MEDIA, ("[Android Audio] BitPerSample : %d \n",nbBitsPerSample));

	(*env)->PushLocalFrame(env, 2);

	ctx->num_buffers = 1;
	ctx->mbufferSizeInBytes = (*env)->CallStaticIntMethod(env, cAudioTrack, mGetMinBufferSize, 
			ctx->sampleRateInHz, ctx->channelConfig, ctx->audioFormat);

	i = 1;
	if ( ctx->channelConfig == CHANNEL_CONFIGURATION_STEREO )
		i *= 2;
	if ( ctx->audioFormat == ENCODING_PCM_16BIT )
		i *= 2;
	
	ctx->total_length_ms =  1000 * ctx->num_buffers * ctx->mbufferSizeInBytes / i / ctx->sampleRateInHz;
	
	/*initial delay is full buffer size*/
	ctx->delay = ctx->total_length_ms;

	mtrack = (*env)->NewObject(env, cAudioTrack, mAudioTrack, STREAM_MUSIC, ctx->sampleRateInHz, 
		ctx->channelConfig, ctx->audioFormat, ctx->mbufferSizeInBytes, MODE_STREAM); //AudioTrack.MODE_STREAM
	mtrack = (*env)->NewGlobalRef(env, mtrack);
	
	ctx->mtrack = mtrack;	

	if (mtrack == NULL){ 
		GF_LOG(GF_LOG_DEBUG, GF_LOG_CORE, ("[Android Audio] mtrack is NULL\n"));
		return GF_NOT_SUPPORTED;
	} else (*env)->CallNonvirtualVoidMethod(env, mtrack, cAudioTrack, mPlay);

	ctx->buff = (*env)->NewByteArray(env, ctx->mbufferSizeInBytes);
	
	return GF_OK;
}

/* Called by the audio thread */
static void WAV_WriteAudio(GF_AudioOutput *dr)
{
	DroidContext *ctx = (DroidContext *)dr->opaque;
	JNIEnv* env = ctx->env;
	u32 written;
	void* pBuffer;
	
	pBuffer = (*env)->GetPrimitiveArrayCritical(env, ctx->buff, NULL);
	if (pBuffer)
	{
		written = dr->FillBuffer(dr->audio_renderer, pBuffer, ctx->mbufferSizeInBytes);
		(*env)->ReleasePrimitiveArrayCritical(env, ctx->buff, pBuffer, 0);
		if (written)  
		{
			(*env)->CallNonvirtualIntMethod(env, mtrack, cAudioTrack, mWrite, ctx->buff, 0, ctx->mbufferSizeInBytes);
		}
	}
	else
	{
		GF_LOG(GF_LOG_ERROR, GF_LOG_CORE, ("[Android Audio] Failed to get pointer to array bytes\n"));
	}
}

/* Called by the main thread */
static void WAV_Play(GF_AudioOutput *dr, u32 PlayType)
{
	DroidContext *ctx = (DroidContext *)dr->opaque;
	JNIEnv* env = GetEnv();

	GF_LOG(GF_LOG_DEBUG, GF_LOG_CORE, ("[Android Audio] Play: %d\n", PlayType));

	switch ( PlayType )
	{
	case 0:
		// Stop playing
		(*env)->CallNonvirtualVoidMethod(env, mtrack, cAudioTrack, mStop);
		// Clear the internal buffers
		(*env)->CallNonvirtualVoidMethod(env, mtrack, cAudioTrack, mFlush);
		break;
	case 1:
	case 2:
		(*env)->CallNonvirtualVoidMethod(env, mtrack, cAudioTrack, mPlay);
		break;
	}
}

static void WAV_SetVolume(GF_AudioOutput *dr, u32 Volume) { }
static void WAV_SetPan(GF_AudioOutput *dr, u32 Pan) { }

/* Called by the audio thread */
static GF_Err WAV_QueryOutputSampleRate(GF_AudioOutput *dr, u32 *desired_samplerate, u32 *NbChannels, u32 *nbBitsPerSample)
{	
	DroidContext *ctx = (DroidContext *)dr->opaque;
	JNIEnv* env = ctx->env;
	u32 sampleRateInHz, channelConfig, audioFormat;

	GF_LOG(GF_LOG_DEBUG, GF_LOG_CORE, ("[Android Audio] Query sample\n"));

#ifdef TEST_QUERY_SAMPLE
	sampleRateInHz = *desired_samplerate;
	channelConfig = (*NbChannels == 1) ? CHANNEL_CONFIGURATION_MONO : CHANNEL_CONFIGURATION_STEREO;
	audioFormat = (*nbBitsPerSample == 8)? ENCODING_PCM_8BIT : ENCODING_PCM_16BIT;

	GF_LOG(GF_LOG_DEBUG, GF_LOG_CORE, ("[Android Audio] Query: SampleRate ChannelConfig AudioFormat: %d %d %d \n",
		sampleRateInHz, 
		(channelConfig == CHANNEL_CONFIGURATION_MONO)? 1 : 2, 
		(ctx->audioFormat == ENCODING_PCM_8BIT)? 8 : 16));

	switch (*desired_samplerate) {
	case 11025:
		*desired_samplerate = 11025;
		if ( (*env)->CallStaticIntMethod(env, cAudioTrack, mGetMinBufferSize, 
			*desired_samplerate, channelConfig, audioFormat) > 0 )
			return GF_OK;
	case 22050:
		*desired_samplerate = 22050;
		if ( (*env)->CallStaticIntMethod(env, cAudioTrack, mGetMinBufferSize, 
			*desired_samplerate, channelConfig, audioFormat) > 0 )
			return GF_OK;
		break;
	case 8000:
		*desired_samplerate = 8000;
		if ( (*env)->CallStaticIntMethod(env, cAudioTrack, mGetMinBufferSize, 
			*desired_samplerate, channelConfig, audioFormat) > 0 )
			return GF_OK;
	case 16000:
		*desired_samplerate = 16000;
		if ( (*env)->CallStaticIntMethod(env, cAudioTrack, mGetMinBufferSize, 
			*desired_samplerate, channelConfig, audioFormat) > 0 )
			return GF_OK;
	case 32000:
		*desired_samplerate = 32000;
		if ( (*env)->CallStaticIntMethod(env, cAudioTrack, mGetMinBufferSize, 
			*desired_samplerate, channelConfig, audioFormat) > 0 )
			return GF_OK;
		break;
	case 24000:
		*desired_samplerate = 24000;
		if ( (*env)->CallStaticIntMethod(env, cAudioTrack, mGetMinBufferSize, 
			*desired_samplerate, channelConfig, audioFormat) > 0 )
			return GF_OK;
	case 48000:
		*desired_samplerate = 48000;
		if ( (*env)->CallStaticIntMethod(env, cAudioTrack, mGetMinBufferSize, 
			*desired_samplerate, channelConfig, audioFormat) > 0 )
			return GF_OK;
		break;
	case 44100:
		*desired_samplerate = 44100;
		if ( (*env)->CallStaticIntMethod(env, cAudioTrack, mGetMinBufferSize, 
			*desired_samplerate, channelConfig, audioFormat) > 0 )
			return GF_OK;
		break;
	default:
		break;
	}
#endif

	return GF_OK;
}

static u32 WAV_GetAudioDelay(GF_AudioOutput *dr)
{
	DroidContext *ctx = (DroidContext *)dr->opaque;

	return ctx->delay;
}

static u32 WAV_GetTotalBufferTime(GF_AudioOutput *dr)
{
	DroidContext *ctx = (DroidContext *)dr->opaque;

	return ctx->total_length_ms;
}

//----------------------------------------------------------------------
void *NewWAVRender()
{
	DroidContext *ctx;
	GF_AudioOutput *driv;
	ctx = gf_malloc(sizeof(DroidContext));
	memset(ctx, 0, sizeof(DroidContext));
	ctx->num_buffers = 1;
	ctx->pan = 50;
	ctx->vol = 100;
	driv = gf_malloc(sizeof(GF_AudioOutput));
	memset(driv, 0, sizeof(GF_AudioOutput));
	GF_REGISTER_MODULE_INTERFACE(driv, GF_AUDIO_OUTPUT_INTERFACE, "Android Audio Output", "gpac distribution")

	driv->opaque = ctx;

	driv->SelfThreaded = 0;
	driv->Setup = WAV_Setup;
	driv->Shutdown = WAV_Shutdown;
	driv->ConfigureOutput = WAV_ConfigureOutput;
	driv->GetAudioDelay = WAV_GetAudioDelay;
	driv->GetTotalBufferTime = WAV_GetTotalBufferTime;
	driv->SetVolume = WAV_SetVolume;
	driv->SetPan = WAV_SetPan;
	driv->Play = WAV_Play;
	driv->QueryOutputSampleRate = WAV_QueryOutputSampleRate;
	driv->WriteAudio = WAV_WriteAudio;

	GF_LOG(GF_LOG_DEBUG, GF_LOG_CORE, ("[Android Audio] New\n"));

	return driv;
}
//----------------------------------------------------------------------
void DeleteWAVRender(void *ifce)
{
	GF_AudioOutput *dr = (GF_AudioOutput *) ifce;
	
	GF_LOG(GF_LOG_DEBUG, GF_LOG_CORE, ("[Android Audio] Delete\n"));

	gf_free(dr);
}
//----------------------------------------------------------------------
const u32 *QueryInterfaces() 
{
	static u32 si [] = {
		GF_AUDIO_OUTPUT_INTERFACE,
		0
	};
	return si; 
}

GF_BaseInterface *LoadInterface(u32 InterfaceType)
{
	if (InterfaceType == GF_AUDIO_OUTPUT_INTERFACE) return NewWAVRender();
	return NULL;
}

void ShutdownInterface(GF_BaseInterface *ifce)
{
	switch (ifce->InterfaceType) {
	case GF_AUDIO_OUTPUT_INTERFACE:
		DeleteWAVRender((GF_AudioOutput *) ifce);
		break;
	}
}