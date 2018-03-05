/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
*   Mupen64plus-audio-sles - main.cpp                                     *
*   Mupen64Plus homepage: http://code.google.com/p/mupen64plus/           *
*   Copyright (C) 2015 Gilles Siberlin                                    *
*   Copyright (C) 2007-2009 Richard Goedeken                              *
*   Copyright (C) 2007-2008 Ebenblues                                     *
*   Copyright (C) 2003 JttL                                               *
*   Copyright (C) 2002 Hacktarux                                          *
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
*   This program is distributed in the hope that it will be useful,       *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
*   GNU General Public License for more details.                          *
*                                                                         *
*   You should have received a copy of the GNU General Public License     *
*   along with this program; if not, write to the                         *
*   Free Software Foundation, Inc.,                                       *
*   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <SDL.h>
#include <SDL_audio.h>
#include <stdarg.h>
#include <stdio.h>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <atomic>
#include <errno.h>
#include <cmath>
#include <SoundTouch.h>

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_common.h"
#include "m64p_config.h"
#include "m64p_plugin.h"
#include "m64p_types.h"
#include "m64p_frontend.h"
#include "main.h"
#include "osal_dynamiclib.h"
#include "BlockingQueue.h"

/* Default start-time size of primary buffer (in equivalent output samples).
This is the buffer where audio is loaded after it's extracted from n64's memory. */
#define PRIMARY_BUFFER_SIZE 16384

/* Size of a single secondary buffer, in output samples. This is the requested size of OpenSLES's
hardware buffer, this should be a power of two. */
#define DEFAULT_SECONDARY_BUFFER_SIZE 256

/* This sets default frequency what is used if rom doesn't want to change it.
Probably only game that needs this is Zelda: Ocarina Of Time Master Quest
*NOTICE* We should try to find out why Demos' frequencies are always wrong
They tend to rely on a default frequency, apparently, never the same one ;) */
#define DEFAULT_FREQUENCY 33600

/* This is the requested number of OpenSLES's hardware buffers */
#define SECONDARY_BUFFER_NBR 100

/* Requested number of target secondary buffers, playback speed will change to maintin this number */
#define TARGET_SECONDARY_BUFFER_NBR 20

/* number of bytes per sample */
#define N64_SAMPLE_BYTES 4

/* local variables */
static void(*l_DebugCallback)(void *, int, const char *) = nullptr;
static void *l_DebugCallContext = nullptr;
static int l_PluginInit = 0;
static m64p_handle l_ConfigAudio;

/* Read header for type definition */
static AUDIO_INFO AudioInfo;
/* Pointer to the primary audio buffer */
static unsigned char *primaryBuffer = nullptr;
/* Size of the primary buffer */
static unsigned int primaryBufferBytes = 0;
/* Size of the primary audio buffer in equivalent output samples */
static int PrimaryBufferSize = PRIMARY_BUFFER_SIZE;
/* Pointer to secondary buffers */
static unsigned char ** secondaryBuffers = nullptr;
/* Size of a single secondary audio buffer in output samples */
static int SecondaryBufferSize = DEFAULT_SECONDARY_BUFFER_SIZE;
/** Time stretched audio enabled */
static int TimeStretchEnabled = true;
/* Index of the next secondary buffer available */
static unsigned int secondaryBufferIndex = 0;
/* Number of secondary buffers */
static unsigned int SecondaryBufferNbr = SECONDARY_BUFFER_NBR;
/* Audio frequency, this is usually obtained from the game, but for compatibility we set default value */
static int GameFreq = DEFAULT_FREQUENCY;
/* SpeedFactor is used to increase/decrease game playback speed */
static unsigned int speed_factor = 100;
/* If this is true then left and right channels are swapped */
static int SwapChannels = 0;
/* Number of secondary buffers to target */
static int TargetSecondaryBuffers = TARGET_SECONDARY_BUFFER_NBR;
/* Selected samplin rate */
static int SamplingRateSelection = 0;
/* Output Audio frequency */
static int OutputFreq;
/* Indicate that the audio plugin failed to initialize, so the emulator can keep running without sound */
static int critical_failure = 0;
static int AudioDevice = -1;
static unsigned int SDL_SAMPLE_BYTES = 0;
static unsigned long totalBuffersProcessed = 0;

void processAudio(const unsigned char* buffer, unsigned int length);
void audioConsumerStretch(void);
void audioConsumerNoStretch(void);
static std::thread audioConsumerThread;
static BlockingQueue<QueueData*> audioConsumerQueue;

static std::atomic_bool shutdownThread = true;

using namespace soundtouch;
static SoundTouch soundTouch;

static SDL_AudioDeviceID dev;
/* The hardware specifications we are using */
static SDL_AudioSpec *hardware_spec;

/* Definitions of pointers to Core config functions */
ptr_ConfigOpenSection      ConfigOpenSection = nullptr;
ptr_ConfigDeleteSection    ConfigDeleteSection = nullptr;
ptr_ConfigSaveSection      ConfigSaveSection = nullptr;
ptr_ConfigSetParameter     ConfigSetParameter = nullptr;
ptr_ConfigGetParameter     ConfigGetParameter = nullptr;
ptr_ConfigGetParameterHelp ConfigGetParameterHelp = nullptr;
ptr_ConfigSetDefaultInt    ConfigSetDefaultInt = nullptr;
ptr_ConfigSetDefaultFloat  ConfigSetDefaultFloat = nullptr;
ptr_ConfigSetDefaultBool   ConfigSetDefaultBool = nullptr;
ptr_ConfigSetDefaultString ConfigSetDefaultString = nullptr;
ptr_ConfigGetParamInt      ConfigGetParamInt = nullptr;
ptr_ConfigGetParamFloat    ConfigGetParamFloat = nullptr;
ptr_ConfigGetParamBool     ConfigGetParamBool = nullptr;
ptr_ConfigGetParamString   ConfigGetParamString = nullptr;
ptr_CoreDoCommand          CoreDoCommand = nullptr;

/* Global functions */
static void DebugMessage(int level, const char *message, ...)
{
	char msgbuf[1024];
	va_list args;

	if (l_DebugCallback == nullptr)
		return;

	va_start(args, message);
	vsprintf(msgbuf, message, args);

	(*l_DebugCallback)(l_DebugCallContext, level, msgbuf);

	va_end(args);
}

static void CloseAudio(void)
{
	if (!shutdownThread)
	{
		shutdownThread = true;

		if (audioConsumerThread.joinable()) {
			audioConsumerThread.join();
		}
	}

	secondaryBufferIndex = 0;

	/* Delete Primary buffer */
	if (primaryBuffer != nullptr)
	{
		primaryBufferBytes = 0;
		free(primaryBuffer);
		primaryBuffer = nullptr;
	}

	/* Delete Secondary buffers */
	if (secondaryBuffers != nullptr)
	{
		for (unsigned int i = 0; i<SecondaryBufferNbr; i++)
		{
			if (secondaryBuffers[i] != nullptr)
			{
				free(secondaryBuffers[i]);
				secondaryBuffers[i] = nullptr;
			}
		}
		free(secondaryBuffers);
		secondaryBuffers = nullptr;
	}

	DebugMessage(M64MSG_VERBOSE, "Cleaning up SDL sound plugin...");

	SDL_ClearQueuedAudio(dev);
	// Shut down SDL Audio output
	SDL_CloseAudioDevice(dev);

	// Delete the hardware spec struct
	if (hardware_spec != nullptr) free(hardware_spec);
	hardware_spec = nullptr;

	// Shutdown the respective subsystems
	if (SDL_WasInit(SDL_INIT_AUDIO) != 0) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static int CreatePrimaryBuffer(void)
{
	unsigned int primaryBytes = (unsigned int)(PrimaryBufferSize * N64_SAMPLE_BYTES);

	DebugMessage(M64MSG_VERBOSE, "Allocating memory for primary audio buffer: %i bytes.", primaryBytes);

	primaryBuffer = (unsigned char*)malloc(primaryBytes);

	if (primaryBuffer == nullptr)
		return 0;

	memset(primaryBuffer, 0, primaryBytes);
	primaryBufferBytes = primaryBytes;

	return 1;
}

static int CreateSecondaryBuffers(void)
{
	int status = 1;
	int secondaryBytes = SecondaryBufferSize * SDL_SAMPLE_BYTES;

	DebugMessage(M64MSG_VERBOSE, "Allocating memory for %d secondary audio buffers: %i bytes.", SecondaryBufferNbr, secondaryBytes);

	/* Allocate number of secondary buffers */
	secondaryBuffers = (unsigned char**)malloc(sizeof(char*) * SecondaryBufferNbr);

	if (secondaryBuffers == nullptr)
		return 0;

	/* Allocate size of each secondary buffers */
	for (unsigned int i = 0; i<SecondaryBufferNbr; i++)
	{
		secondaryBuffers[i] = (unsigned char*)malloc((size_t)secondaryBytes);

		if (secondaryBuffers[i] == nullptr)
		{
			status = 0;
			break;
		}

		memset(secondaryBuffers[i], 0, (size_t)secondaryBytes);
	}

	return status;
}

void OnInitFailure(void)
{
	DebugMessage(M64MSG_ERROR, "Couldn't open OpenSLES audio");
	CloseAudio();
	critical_failure = 1;
}

static void InitializeSDL(void)
{
	DebugMessage(M64MSG_INFO, "Initializing SDL2 audio subsystem...");

	if (SDL_Init(SDL_INIT_AUDIO) < 0)
	{
		DebugMessage(M64MSG_ERROR, "Failed to initialize SDL2 audio subsystem; forcing exit.\n");
		critical_failure = 1;
		return;
	}
	critical_failure = 0;

	int i, count = SDL_GetNumAudioDevices(0);
	for (i = 0; i < count; ++i)
		DebugMessage(M64MSG_INFO, "Audio device %d: %s", i, SDL_GetAudioDeviceName(i, 0));
}

static void InitializeAudio(int freq)
{

	/* reload these because they gets re-assigned from data below, and InitializeAudio can be called more than once */
	GameFreq = ConfigGetParamInt(l_ConfigAudio, "DEFAULT_FREQUENCY");
	SwapChannels = ConfigGetParamBool(l_ConfigAudio, "SWAP_CHANNELS");
	PrimaryBufferSize = ConfigGetParamInt(l_ConfigAudio, "PRIMARY_BUFFER_SIZE");
	SecondaryBufferSize = ConfigGetParamInt(l_ConfigAudio, "SECONDARY_BUFFER_SIZE");
	TargetSecondaryBuffers = ConfigGetParamInt(l_ConfigAudio, "SECONDARY_BUFFER_NBR");
	SamplingRateSelection = ConfigGetParamInt(l_ConfigAudio, "SAMPLING_RATE");
	TimeStretchEnabled = ConfigGetParamBool(l_ConfigAudio, "TIME_STRETCH_ENABLED");
	AudioDevice = ConfigGetParamInt(l_ConfigAudio, "AUDIO_DEVICE");

	/* Sometimes a bad frequency is requested so ignore it */
	if (freq < 4000)
		return;

	if (critical_failure)
		return;

	/* This is important for the sync */
	GameFreq = freq;

	if (SamplingRateSelection == 0) {
		if ((freq / 1000) <= 11) {
			OutputFreq = 11025;
		}
		else if ((freq / 1000) <= 22) {
			OutputFreq = 22050;
		}
		else if ((freq / 1000) <= 32) {
			OutputFreq = 32000;
		}
		else {
			OutputFreq = 44100;
		}
	}
	else {
		OutputFreq = SamplingRateSelection;
	}

	DebugMessage(M64MSG_INFO, "Requesting frequency: %iHz.", OutputFreq);

	/* Close everything because InitializeAudio can be called more than once */
	CloseAudio();

	/* Create primary buffer */
	if (!CreatePrimaryBuffer())
	{
		OnInitFailure();
		return;
	}

	/* Create secondary buffers */
	if (!CreateSecondaryBuffers())
	{
		OnInitFailure();
		return;
	}

	if (SDL_WasInit(SDL_INIT_AUDIO) == (SDL_INIT_AUDIO))
	{
		DebugMessage(M64MSG_VERBOSE, "InitializeAudio(): SDL2 Audio sub-system already initialized.");
		SDL_ClearQueuedAudio(dev);
		SDL_CloseAudioDevice(dev);
	}
	else
	{
		DebugMessage(M64MSG_VERBOSE, "InitializeAudio(): Initializing SDL2 Audio");
		InitializeSDL();
	}


	if (critical_failure == 1)
		return;
	GameFreq = freq; // This is important for the sync
	if (hardware_spec != nullptr) free(hardware_spec);

	// Allocate space for SDL_AudioSpec
	SDL_AudioSpec* desired = new SDL_AudioSpec;
	SDL_AudioSpec* obtained = new SDL_AudioSpec;

	desired->freq = OutputFreq;
	DebugMessage(M64MSG_VERBOSE, "Requesting frequency: %iHz.", desired->freq);

	desired->format = AUDIO_F32;
	DebugMessage(M64MSG_VERBOSE, "Requesting format: %i.", desired->format);
	/* Stereo */
	desired->channels = 2;
	desired->samples = SecondaryBufferNbr;
	desired->callback = nullptr;
	desired->userdata = nullptr;

	/* Open the audio device */
	const char *dev_name = nullptr;
	if (AudioDevice >= 0)
		dev_name = SDL_GetAudioDeviceName(AudioDevice, 0);

	dev = SDL_OpenAudioDevice(dev_name, 0, desired, obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);

	if (dev == 0)
	{
		DebugMessage(M64MSG_ERROR, "Couldn't open audio: %s", SDL_GetError());
		critical_failure = 1;
		return;
	}
	if (desired->format != obtained->format)
	{
		DebugMessage(M64MSG_WARNING, "Obtained audio format differs from requested.");
	}

	OutputFreq = obtained->freq;

	/* desired spec is no longer needed */
	delete desired;
	hardware_spec = obtained;

	SDL_SAMPLE_BYTES = hardware_spec->size / hardware_spec->samples;

	DebugMessage(M64MSG_ERROR, "Frequency: %i", hardware_spec->freq);
	DebugMessage(M64MSG_ERROR, "Format: %i", hardware_spec->format);
	DebugMessage(M64MSG_ERROR, "Channels: %i", hardware_spec->channels);
	DebugMessage(M64MSG_ERROR, "Silence: %i", hardware_spec->silence);
	DebugMessage(M64MSG_ERROR, "Samples: %i", hardware_spec->samples);
	DebugMessage(M64MSG_ERROR, "Size: %i", hardware_spec->size);
	DebugMessage(M64MSG_ERROR, "Bytes per sample: %i", SDL_SAMPLE_BYTES);

	shutdownThread = false;

	SDL_PauseAudioDevice(dev, 0);

	if (TimeStretchEnabled)
	{
		audioConsumerThread = std::thread(audioConsumerStretch);
	}
	else
	{
		audioConsumerThread = std::thread(audioConsumerNoStretch);
	}
}

static void ReadConfig(void)
{
	/* read the configuration values into our static variables */
	GameFreq = ConfigGetParamInt(l_ConfigAudio, "DEFAULT_FREQUENCY");
	SwapChannels = ConfigGetParamBool(l_ConfigAudio, "SWAP_CHANNELS");
	PrimaryBufferSize = ConfigGetParamInt(l_ConfigAudio, "PRIMARY_BUFFER_SIZE");
	SecondaryBufferSize = ConfigGetParamInt(l_ConfigAudio, "SECONDARY_BUFFER_SIZE");
	TargetSecondaryBuffers = ConfigGetParamInt(l_ConfigAudio, "SECONDARY_BUFFER_NBR");
	SamplingRateSelection = ConfigGetParamInt(l_ConfigAudio, "SAMPLING_RATE");
	TimeStretchEnabled = ConfigGetParamBool(l_ConfigAudio, "TIME_STRETCH_ENABLED");
	AudioDevice = ConfigGetParamInt(l_ConfigAudio, "AUDIO_DEVICE");
}

/* Mupen64Plus plugin functions */
EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle CoreLibHandle, void *Context,
	void(*DebugCallback)(void *, int, const char *))
{
	ptr_CoreGetAPIVersions CoreAPIVersionFunc;

	int ConfigAPIVersion, DebugAPIVersion, VidextAPIVersion, bSaveConfig;
	float fConfigParamsVersion = 0.0f;

	if (l_PluginInit)
		return M64ERR_ALREADY_INIT;

	/* first thing is to set the callback function for debug info */
	l_DebugCallback = DebugCallback;
	l_DebugCallContext = Context;

	/* attach and call the CoreGetAPIVersions function, check Config API version for compatibility */
	CoreAPIVersionFunc = (ptr_CoreGetAPIVersions)osal_dynlib_getproc(CoreLibHandle, "CoreGetAPIVersions");
	if (CoreAPIVersionFunc == nullptr)
	{
		DebugMessage(M64MSG_ERROR, "Core emulator broken; no CoreAPIVersionFunc() function found.");
		return M64ERR_INCOMPATIBLE;
	}

	(*CoreAPIVersionFunc)(&ConfigAPIVersion, &DebugAPIVersion, &VidextAPIVersion, nullptr);
	if ((ConfigAPIVersion & 0xffff0000) != (CONFIG_API_VERSION & 0xffff0000))
	{
		DebugMessage(M64MSG_ERROR, "Emulator core Config API (v%i.%i.%i) incompatible with plugin (v%i.%i.%i)",
			VERSION_PRINTF_SPLIT(ConfigAPIVersion), VERSION_PRINTF_SPLIT(CONFIG_API_VERSION));
		return M64ERR_INCOMPATIBLE;
	}

	/* Get the core config function pointers from the library handle */
	ConfigOpenSection = (ptr_ConfigOpenSection)osal_dynlib_getproc(CoreLibHandle, "ConfigOpenSection");
	ConfigDeleteSection = (ptr_ConfigDeleteSection)osal_dynlib_getproc(CoreLibHandle, "ConfigDeleteSection");
	ConfigSaveSection = (ptr_ConfigSaveSection)osal_dynlib_getproc(CoreLibHandle, "ConfigSaveSection");
	ConfigSetParameter = (ptr_ConfigSetParameter)osal_dynlib_getproc(CoreLibHandle, "ConfigSetParameter");
	ConfigGetParameter = (ptr_ConfigGetParameter)osal_dynlib_getproc(CoreLibHandle, "ConfigGetParameter");
	ConfigSetDefaultInt = (ptr_ConfigSetDefaultInt)osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultInt");
	ConfigSetDefaultFloat = (ptr_ConfigSetDefaultFloat)osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultFloat");
	ConfigSetDefaultBool = (ptr_ConfigSetDefaultBool)osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultBool");
	ConfigSetDefaultString = (ptr_ConfigSetDefaultString)osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultString");
	ConfigGetParamInt = (ptr_ConfigGetParamInt)osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamInt");
	ConfigGetParamFloat = (ptr_ConfigGetParamFloat)osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamFloat");
	ConfigGetParamBool = (ptr_ConfigGetParamBool)osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamBool");
	ConfigGetParamString = (ptr_ConfigGetParamString)osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamString");
	CoreDoCommand = (ptr_CoreDoCommand)osal_dynlib_getproc(CoreLibHandle, "CoreDoCommand");

	if (!ConfigOpenSection || !ConfigDeleteSection || !ConfigSetParameter || !ConfigGetParameter ||
		!ConfigSetDefaultInt || !ConfigSetDefaultFloat || !ConfigSetDefaultBool || !ConfigSetDefaultString ||
		!ConfigGetParamInt || !ConfigGetParamFloat || !ConfigGetParamBool || !ConfigGetParamString ||
		!CoreDoCommand)
		return M64ERR_INCOMPATIBLE;

	/* ConfigSaveSection was added in Config API v2.1.0 */
	if (ConfigAPIVersion >= 0x020100 && !ConfigSaveSection)
		return M64ERR_INCOMPATIBLE;

	/* get a configuration section handle */
	if (ConfigOpenSection("audio-sdl2", &l_ConfigAudio) != M64ERR_SUCCESS)
	{
		DebugMessage(M64MSG_ERROR, "Couldn't open config section 'audio-sdl2'");
		return M64ERR_INPUT_NOT_FOUND;
	}

	/* check the section version number */
	bSaveConfig = 0;
	if (ConfigGetParameter(l_ConfigAudio, "Version", M64TYPE_FLOAT, &fConfigParamsVersion, sizeof(float)) != M64ERR_SUCCESS)
	{
		DebugMessage(M64MSG_WARNING, "No version number in 'audio-sdl2' config section. Setting defaults.");
		ConfigDeleteSection("audio-sdl2");
		ConfigOpenSection("audio-sdl2", &l_ConfigAudio);
		bSaveConfig = 1;
	}
	else if (((int)fConfigParamsVersion) != ((int)CONFIG_PARAM_VERSION))
	{
		DebugMessage(M64MSG_WARNING, "Incompatible version %.2f in 'audio-sdl2' config section: current is %.2f. Setting defaults.", fConfigParamsVersion, (float)CONFIG_PARAM_VERSION);
		ConfigDeleteSection("audio-sdl2");
		ConfigOpenSection("audio-sdl2", &l_ConfigAudio);
		bSaveConfig = 1;
	}
	else if ((CONFIG_PARAM_VERSION - fConfigParamsVersion) >= 0.0001f)
	{
		/* handle upgrades */
		float fVersion = CONFIG_PARAM_VERSION;
		ConfigSetParameter(l_ConfigAudio, "Version", M64TYPE_FLOAT, &fVersion);
		DebugMessage(M64MSG_INFO, "Updating parameter set version in 'audio-sdl2' config section to %.2f", fVersion);
		bSaveConfig = 1;
	}

	/* set the default values for this plugin */
	ConfigSetDefaultFloat(l_ConfigAudio, "Version", CONFIG_PARAM_VERSION, "Mupen64Plus SDL Audio Plugin config parameter version number");
	ConfigSetDefaultInt(l_ConfigAudio, "DEFAULT_FREQUENCY", DEFAULT_FREQUENCY, "Frequency which is used if rom doesn't want to change it");
	ConfigSetDefaultBool(l_ConfigAudio, "SWAP_CHANNELS", 0, "Swaps left and right channels");
	ConfigSetDefaultInt(l_ConfigAudio, "PRIMARY_BUFFER_SIZE", PRIMARY_BUFFER_SIZE, "Size of primary buffer in output samples. This is where audio is loaded after it's extracted from n64's memory.");
	ConfigSetDefaultInt(l_ConfigAudio, "SECONDARY_BUFFER_SIZE", DEFAULT_SECONDARY_BUFFER_SIZE, "Size of secondary buffer in output samples. This is OpenSLES's hardware buffer.");
	ConfigSetDefaultInt(l_ConfigAudio, "SECONDARY_BUFFER_NBR", TARGET_SECONDARY_BUFFER_NBR, "Number of target secondary buffers.");
	ConfigSetDefaultInt(l_ConfigAudio, "SAMPLING_RATE", 0, "Sampling rate in Hz, (0=game original, other valid values: 16000, 22050, 32000, 44100, 48000, etc");
	ConfigSetDefaultBool(l_ConfigAudio, "TIME_STRETCH_ENABLED", 1, "Enable audio time stretching to prevent crackling");
	ConfigSetDefaultInt(l_ConfigAudio, "AUDIO_DEVICE", -1, "ID of audio playback device, -1 for default");

	if (bSaveConfig && ConfigAPIVersion >= 0x020100)
		ConfigSaveSection("audio-sdl2");

	l_PluginInit = 1;

	return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void)
{
	if (!l_PluginInit)
		return M64ERR_NOT_INIT;

	CloseAudio();

	/* reset some local variables */
	l_DebugCallback = nullptr;
	l_DebugCallContext = nullptr;
	l_PluginInit = 0;

	return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *PluginType, int *PluginVersion, int *APIVersion, const char **PluginNamePtr, int *Capabilities)
{
	/* set version info */
	if (PluginType != nullptr)
		*PluginType = M64PLUGIN_AUDIO;

	if (PluginVersion != nullptr)
		*PluginVersion = SDL2_AUDIO_PLUGIN_VERSION;

	if (APIVersion != nullptr)
		*APIVersion = AUDIO_PLUGIN_API_VERSION;

	if (PluginNamePtr != nullptr)
		*PluginNamePtr = "Mupen64Plus OpenSLES Audio Plugin";

	if (Capabilities != nullptr)
	{
		*Capabilities = 0;
	}

	return M64ERR_SUCCESS;
}

/* ----------- Audio Functions ------------- */
EXPORT void CALL AiDacrateChanged(int SystemType)
{
	int f = GameFreq;

	if (!l_PluginInit)
		return;

	switch (SystemType)
	{
	case SYSTEM_NTSC:
		f = 48681812 / (*AudioInfo.AI_DACRATE_REG + 1);
		break;
	case SYSTEM_PAL:
		f = 49656530 / (*AudioInfo.AI_DACRATE_REG + 1);
		break;
	case SYSTEM_MPAL:
		f = 48628316 / (*AudioInfo.AI_DACRATE_REG + 1);
		break;
	}

	InitializeAudio(f);
}

bool isSpeedLimiterEnabled(void)
{
	int e = 1;
	CoreDoCommand(M64CMD_CORE_STATE_QUERY, M64CORE_SPEED_LIMITER, &e);
	return  e;
}

EXPORT void CALL AiLenChanged(void)
{
	static const double minSleepNeededForReset = -5.0;
	static const double minSleepNeeded = -0.1;
	static const double maxSleepNeeded = 0.5;
	static bool hasBeenReset = false;
	static unsigned long totalElapsedSamples = 0;
	static std::chrono::time_point<std::chrono::steady_clock,
		std::chrono::duration<double>> gameStartTime;
	static int lastSpeedFactor = 100;
	static bool lastSpeedLimiterEnabledState = false;
	static bool busyWait = false;
	static int busyWaitEnableCount = 0;
	static int busyWaitDisableCount = 0;
	static const int busyWaitCheck = 30;

	if (critical_failure == 1)
		return;

	if (!l_PluginInit)
		return;

	bool limiterEnabled = isSpeedLimiterEnabled();

	auto currentTime = std::chrono::steady_clock::now();

	//if this is the first time or we are resuming from pause
	if (gameStartTime.time_since_epoch().count() == 0 || !hasBeenReset || lastSpeedFactor != speed_factor || lastSpeedLimiterEnabledState != limiterEnabled)
	{
		lastSpeedLimiterEnabledState = limiterEnabled;
		gameStartTime = currentTime;
		totalElapsedSamples = 0;
		hasBeenReset = true;
		totalElapsedSamples = 0;
		totalBuffersProcessed = 0;
	}

	lastSpeedFactor = speed_factor;

	unsigned int LenReg = *AudioInfo.AI_LEN_REG;
	unsigned char * p = AudioInfo.RDRAM + (*AudioInfo.AI_DRAM_ADDR_REG & 0xFFFFFF);

	//Add data to the queue
	QueueData* theQueueData = new QueueData;
	theQueueData->data = new unsigned char[LenReg];
	theQueueData->length = LenReg; 
	std::chrono::duration<double> timeSinceStart = currentTime - gameStartTime;
	theQueueData->timeSinceStart = timeSinceStart.count();
	memcpy(theQueueData->data, p, LenReg);

	audioConsumerQueue.push(theQueueData);

	//Calculate total ellapsed game time
	totalElapsedSamples += LenReg / N64_SAMPLE_BYTES;
	double speedFactor = static_cast<double>(speed_factor) / 100.0;
	double totalElapsedGameTime = ((double)totalElapsedSamples) / (double)GameFreq / speedFactor;

	//Slow the game down if sync game to audio is enabled
	if (!limiterEnabled)
	{
		double sleepNeeded = totalElapsedGameTime - timeSinceStart.count();

		if (sleepNeeded < minSleepNeededForReset || sleepNeeded >(maxSleepNeeded / speedFactor))
		{
			hasBeenReset = false;
		}

		//We don't want to let the game get too far ahead, otherwise we may have a sudden burst of speed
		if (sleepNeeded < minSleepNeeded)
		{
			gameStartTime -= std::chrono::duration<double>(minSleepNeeded);
		}

		//Enable busywait mode if we have X callbacks of negative sleep. Don't disable busywait
		//until we have X positive callbacks
		if (sleepNeeded <= 0.0) {
			++busyWaitEnableCount;
		}
		else {
			busyWaitEnableCount = 0;
		}

		if (busyWaitEnableCount == busyWaitCheck) {
			busyWait = true;
			busyWaitEnableCount = 0;
			busyWaitDisableCount = 0;
		}

		if (busyWait) {
			if (sleepNeeded > 0) {
				++busyWaitDisableCount;
			}

			if (busyWaitDisableCount == busyWaitCheck) {
				busyWait = false;
			}
		}

		//Useful logging
		//DebugMessage(M64MSG_ERROR, "Real=%f, Game=%f, sleep=%f, start=%f, time=%f, speed=%d, sleep_before_factor=%f",
		//             totalRealTimeElapsed, totalElapsedGameTime, sleepNeeded, gameStartTime, timeDouble, speed_factor, sleepNeeded*speedFactor);
		if (sleepNeeded > 0.0 && sleepNeeded < (maxSleepNeeded / speedFactor)) {
			auto endTime = currentTime + std::chrono::duration<double>(sleepNeeded);

			if (busyWait) {
				while (std::chrono::steady_clock::now() < endTime);
			}
			else {
				std::this_thread::sleep_until(endTime);
			}
		}
	}
}

double GetAverageTime(double* feedTimes, int numTimes)
{
	double sum = 0;
	for (int index = 0; index < numTimes; ++index)
	{
		sum += feedTimes[index];
	}

	return sum / static_cast<double>(numTimes);
}

void audioConsumerStretch(void) {
	/*
	static int sequenceLenMS = 63;
	static int seekWindowMS = 16;
	static int overlapMS = 7;*/

	soundTouch.setSampleRate((uint)GameFreq);
	soundTouch.setChannels(2);
	soundTouch.setSetting(SETTING_USE_QUICKSEEK, 1);
	soundTouch.setSetting(SETTING_USE_AA_FILTER, 1);
	//soundTouch.setSetting( SETTING_SEQUENCE_MS, sequenceLenMS );
	//soundTouch.setSetting( SETTING_SEEKWINDOW_MS, seekWindowMS );
	//soundTouch.setSetting( SETTING_OVERLAP_MS, overlapMS );

	soundTouch.setRate((double)GameFreq / (double)OutputFreq);
	double speedFactor = static_cast<double>(speed_factor) / 100.0;
	soundTouch.setTempo(speedFactor);

	double bufferMultiplier = ((double)OutputFreq / DEFAULT_FREQUENCY) *
		((double)DEFAULT_SECONDARY_BUFFER_SIZE / SecondaryBufferSize);

	int bufferLimit = SecondaryBufferNbr - 20;
	int maxQueueSize = (int)((TargetSecondaryBuffers + 30.0) * bufferMultiplier);
	if (maxQueueSize > bufferLimit) {
		maxQueueSize = bufferLimit;
	}
	int minQueueSize = (int)(TargetSecondaryBuffers * bufferMultiplier);
	bool drainQueue = false;

	//Sound queue ran dry, device is running slow
	int ranDry = 0;

	//adjustment used when a device running too slow
	double slowAdjustment = 1.0;
	double currAdjustment = 1.0;

	//how quickly to return to original speed
	const double minSlowValue = 0.2;
	const double maxSlowValue = 300.0;
	const float maxSpeedUpRate = 0.5;
	const double slowRate = 0.05;
	const double defaultSampleLength = 0.01666;
	QueueData* currQueueData = nullptr;

	double prevTime = 0;

	static const int maxWindowSize = 500;

	int feedTimeWindowSize = 50;

	int feedTimeIndex = 0;
	bool feedTimesSet = false;
	double feedTimes[maxWindowSize] = {};
	double gameTimes[maxWindowSize] = {};
	double averageGameTime = defaultSampleLength;
	double averageFeedTime = defaultSampleLength;

	while (!shutdownThread) {
		int sdlQueueLength = SDL_GetQueuedAudioSize(dev)/SDL_SAMPLE_BYTES / hardware_spec->size;

		ranDry = sdlQueueLength < minQueueSize;

		QueueData* currQueueData;

		if (audioConsumerQueue.tryPop(currQueueData, std::chrono::milliseconds(1000))) {
			int threadQueueLength = audioConsumerQueue.size();

			unsigned int dataLength = currQueueData->length;
			double temp = averageGameTime / averageFeedTime;

			if (totalBuffersProcessed < SecondaryBufferNbr) {

				speedFactor = static_cast<double>(speed_factor) / 100.0;
				soundTouch.setTempo(speedFactor);

				processAudio(currQueueData->data, dataLength);
			}
			else {

				//Game is running too fast speed up audio
				if ((sdlQueueLength > maxQueueSize || drainQueue) && !ranDry) {
					drainQueue = true;
					currAdjustment = temp +
						(float)(sdlQueueLength - minQueueSize) / (float)(SecondaryBufferNbr - minQueueSize) *
						maxSpeedUpRate;
				}
				//Device can't keep up with the game
				else if (ranDry) {
					drainQueue = false;
					currAdjustment = temp - slowRate;
				//Good case
				}
				else if (!ranDry && sdlQueueLength < maxQueueSize) {
					currAdjustment = temp;
				}

				//Allow the tempo to slow quickly with no minimum value change, but restore original tempo more slowly.
				if (currAdjustment > minSlowValue && currAdjustment < maxSlowValue) {
					slowAdjustment = currAdjustment;
					static const int increments = 4;
					//Adjust tempo in x% increments so it's more steady
					double temp2 = round((slowAdjustment * 100) / increments);
					temp2 *= increments;
					slowAdjustment = (temp2) / 100;

					soundTouch.setTempo(slowAdjustment);
				}

				processAudio(currQueueData->data, dataLength);
			}

			++totalBuffersProcessed;

			//We don't want to calculate the average until we give everything a time to settle.

			//Figure out how much to slow down by
			double timeDiff = currQueueData->timeSinceStart - prevTime;

			prevTime = currQueueData->timeSinceStart;

			feedTimes[feedTimeIndex] = timeDiff;
			averageFeedTime = GetAverageTime(feedTimes, feedTimesSet ? feedTimeWindowSize : (feedTimeIndex + 1));

			gameTimes[feedTimeIndex] = (float)dataLength / (float)N64_SAMPLE_BYTES / (float)GameFreq;
			averageGameTime = GetAverageTime(gameTimes, feedTimesSet ? feedTimeWindowSize : (feedTimeIndex + 1));

			++feedTimeIndex;
			if (feedTimeIndex >= feedTimeWindowSize) {
				feedTimeIndex = 0;
				feedTimesSet = true;
			}

			//Normalize window size
			feedTimeWindowSize = static_cast<int>(defaultSampleLength / averageGameTime * 50);
			if (feedTimeWindowSize > maxWindowSize) {
				feedTimeWindowSize = maxWindowSize;
			}

			delete [] currQueueData->data;
			delete currQueueData;

			//Useful logging
			//if(sdlQueueLength == 0)
			//{
			//DebugMessage(M64MSG_ERROR, "sdl_length=%d, thread_length=%d, dry=%d, drain=%d, slow_adj=%f, curr_adj=%f, temp=%f, feed_time=%f, game_time=%f, min_size=%d, max_size=%d count=%d",
			//	sdlQueueLength, threadQueueLength, ranDry, drainQueue, slowAdjustment, currAdjustment, temp, averageFeedTime, averageGameTime, minQueueSize, maxQueueSize, totalBuffersProcessed);
			//}
		}
	}
}

void audioConsumerNoStretch(void)
{
	soundTouch.setSampleRate(GameFreq);
	soundTouch.setChannels(2);
	soundTouch.setSetting(SETTING_USE_QUICKSEEK, 1);
	soundTouch.setSetting(SETTING_USE_AA_FILTER, 1);
	double speedFactor = static_cast<double>(speed_factor) / 100.0;
	soundTouch.setTempo(speedFactor);

	soundTouch.setRate((double)GameFreq / (double)OutputFreq);
	QueueData* currQueueData = nullptr;

	int lastSpeedFactor = speed_factor;

	//How long to wait for some data
	struct timespec waitTime;
	waitTime.tv_sec = 1;
	waitTime.tv_nsec = 0;

	while (!shutdownThread)
	{
		if (audioConsumerQueue.tryPop(currQueueData, std::chrono::milliseconds(1000))) {
			int dataLength = currQueueData->length;

			if (lastSpeedFactor != speed_factor)
			{
				lastSpeedFactor = speed_factor;
				double speedFactor = static_cast<double>(speed_factor) / 100.0;
				soundTouch.setTempo(speedFactor);
			}

			processAudio(currQueueData->data, dataLength);

			delete [] currQueueData->data;
			delete currQueueData;
		}
	}
}


void processAudio(const unsigned char* buffer, unsigned int length)
{
	if (length < primaryBufferBytes)
	{
		unsigned int i;

		for (i = 0; i < length; i += 4)
		{
			if (SwapChannels == 0)
			{
				/* Left channel */
				primaryBuffer[i] = buffer[i + 2];
				primaryBuffer[i + 1] = buffer[i + 3];

				/* Right channel */
				primaryBuffer[i + 2] = buffer[i];
				primaryBuffer[i + 3] = buffer[i + 1];
			}
			else
			{
				/* Left channel */
				primaryBuffer[i] = buffer[i];
				primaryBuffer[i + 1] = buffer[i + 1];

				/* Right channel */
				primaryBuffer[i + 2] = buffer[i + 2];
				primaryBuffer[i + 3] = buffer[i + 3];
			}
		}
	}
	else
		DebugMessage(M64MSG_WARNING, "processAudio(): Audio primary buffer overflow.");


	int numSamples = length / sizeof(short);
	short* primaryBufferShort = (short*)primaryBuffer;
	std::unique_ptr<float[]> primaryBufferFloat(new float[numSamples]);

	for (int index = 0; index < numSamples; ++index)
	{
		primaryBufferFloat[index] = static_cast<float>(primaryBufferShort[index]) / 32767.0f;
	}

	soundTouch.putSamples(reinterpret_cast<SAMPLETYPE*>(primaryBufferFloat.get()), length / N64_SAMPLE_BYTES);

	int outSamples = 0;

	do
	{
		outSamples = soundTouch.receiveSamples(reinterpret_cast<SAMPLETYPE*>(secondaryBuffers[secondaryBufferIndex]), SecondaryBufferSize);

		if (outSamples != 0)
		{
			SDL_QueueAudio(dev, secondaryBuffers[secondaryBufferIndex], outSamples*SDL_SAMPLE_BYTES);

			secondaryBufferIndex++;

			if (secondaryBufferIndex > (SecondaryBufferNbr - 1))
				secondaryBufferIndex = 0;
		}
	} while (outSamples != 0);
}

EXPORT int CALL InitiateAudio(AUDIO_INFO Audio_Info)
{
	if (!l_PluginInit)
		return 0;

	AudioInfo = Audio_Info;
	return 1;
}

EXPORT int CALL RomOpen(void)
{
	if (!l_PluginInit)
		return 0;

	ReadConfig();
	InitializeAudio(GameFreq);

	return 1;
}

EXPORT void CALL RomClosed(void)
{
	if (!l_PluginInit)
		return;

	if (critical_failure == 1)
		return;

	DebugMessage(M64MSG_VERBOSE, "Cleaning up OpenSLES sound plugin...");

	CloseAudio();
}

EXPORT void CALL ProcessAList(void)
{
}

EXPORT void CALL SetSpeedFactor(int percentage)
{
	if (!l_PluginInit)
		return;
	if (percentage >= 10 && percentage <= 300)
		speed_factor = percentage;
}

EXPORT void CALL VolumeMute(void)
{
}

EXPORT void CALL VolumeUp(void)
{
}

EXPORT void CALL VolumeDown(void)
{
}

EXPORT int CALL VolumeGetLevel(void)
{
	return 100;
}

EXPORT void CALL VolumeSetLevel(int level)
{
}

EXPORT const char * CALL VolumeGetString(void)
{
	return "100%";
}

