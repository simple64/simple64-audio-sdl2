/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-sdl2-audio - main.c                                       *
 *   Mupen64Plus homepage: http://code.google.com/p/mupen64plus/           *
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
#include <stdlib.h>
#include <string.h>

#define M64P_PLUGIN_PROTOTYPES 1
#include "m64p_common.h"
#include "m64p_config.h"
#include "m64p_plugin.h"
#include "m64p_types.h"
#include "main.h"
#include "osal_dynamiclib.h"

/* This sets default frequency what is used if rom doesn't want to change it.
   Probably only game that needs this is Zelda: Ocarina Of Time Master Quest
   *NOTICE* We should try to find out why Demos' frequencies are always wrong
   They tend to rely on a default frequency, apparently, never the same one ;)*/
#define DEFAULT_FREQUENCY 33600

/* number of bytes per sample */
#define N64_SAMPLE_BYTES 4
#define SDL_SAMPLE_BYTES 4

/* local variables */
static void (*l_DebugCallback)(void *, int, const char *) = NULL;
static void *l_DebugCallContext = NULL;
static int l_PluginInit = 0;
static m64p_handle l_ConfigAudio;

static SDL_AudioDeviceID dev;
/* Read header for type definition */
static AUDIO_INFO AudioInfo;
/* The hardware specifications we are using */
static SDL_AudioSpec *hardware_spec;
/* Pointer to the primary audio buffer */
static unsigned char primaryBuffer[0x40000];
/* Audio frequency, this is usually obtained from the game, but for compatibility we set default value */
static int GameFreq = DEFAULT_FREQUENCY;
// If this is true then left and right channels are swapped */
static int SwapChannels = 0;
// Muted or not
static int VolIsMuted = 0;
static int ff = 0;
static int AudioDevice = -1;

// Prototype of local functions
static void InitializeAudio(int freq);
static void ReadConfig(void);
static void InitializeSDL(void);

static int critical_failure = 0;

/* definitions of pointers to Core config functions */
ptr_ConfigOpenSection      ConfigOpenSection = NULL;
ptr_ConfigDeleteSection    ConfigDeleteSection = NULL;
ptr_ConfigSaveSection      ConfigSaveSection = NULL;
ptr_ConfigSetParameter     ConfigSetParameter = NULL;
ptr_ConfigGetParameter     ConfigGetParameter = NULL;
ptr_ConfigGetParameterHelp ConfigGetParameterHelp = NULL;
ptr_ConfigSetDefaultInt    ConfigSetDefaultInt = NULL;
ptr_ConfigSetDefaultFloat  ConfigSetDefaultFloat = NULL;
ptr_ConfigSetDefaultBool   ConfigSetDefaultBool = NULL;
ptr_ConfigSetDefaultString ConfigSetDefaultString = NULL;
ptr_ConfigGetParamInt      ConfigGetParamInt = NULL;
ptr_ConfigGetParamFloat    ConfigGetParamFloat = NULL;
ptr_ConfigGetParamBool     ConfigGetParamBool = NULL;
ptr_ConfigGetParamString   ConfigGetParamString = NULL;

/* Global functions */
static void DebugMessage(int level, const char *message, ...)
{
  char msgbuf[1024];
  va_list args;

  if (l_DebugCallback == NULL)
      return;

  va_start(args, message);
  vsprintf(msgbuf, message, args);

  (*l_DebugCallback)(l_DebugCallContext, level, msgbuf);

  va_end(args);
}

/* Mupen64Plus plugin functions */
EXPORT m64p_error CALL PluginStartup(m64p_dynlib_handle CoreLibHandle, void *Context,
                                   void (*DebugCallback)(void *, int, const char *))
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
    CoreAPIVersionFunc = (ptr_CoreGetAPIVersions) osal_dynlib_getproc(CoreLibHandle, "CoreGetAPIVersions");
    if (CoreAPIVersionFunc == NULL)
    {
        DebugMessage(M64MSG_ERROR, "Core emulator broken; no CoreAPIVersionFunc() function found.");
        return M64ERR_INCOMPATIBLE;
    }

    (*CoreAPIVersionFunc)(&ConfigAPIVersion, &DebugAPIVersion, &VidextAPIVersion, NULL);
    if ((ConfigAPIVersion & 0xffff0000) != (CONFIG_API_VERSION & 0xffff0000))
    {
        DebugMessage(M64MSG_ERROR, "Emulator core Config API (v%i.%i.%i) incompatible with plugin (v%i.%i.%i)",
                VERSION_PRINTF_SPLIT(ConfigAPIVersion), VERSION_PRINTF_SPLIT(CONFIG_API_VERSION));
        return M64ERR_INCOMPATIBLE;
    }

    /* Get the core config function pointers from the library handle */
    ConfigOpenSection = (ptr_ConfigOpenSection) osal_dynlib_getproc(CoreLibHandle, "ConfigOpenSection");
    ConfigDeleteSection = (ptr_ConfigDeleteSection) osal_dynlib_getproc(CoreLibHandle, "ConfigDeleteSection");
    ConfigSaveSection = (ptr_ConfigSaveSection) osal_dynlib_getproc(CoreLibHandle, "ConfigSaveSection");
    ConfigSetParameter = (ptr_ConfigSetParameter) osal_dynlib_getproc(CoreLibHandle, "ConfigSetParameter");
    ConfigGetParameter = (ptr_ConfigGetParameter) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParameter");
    ConfigSetDefaultInt = (ptr_ConfigSetDefaultInt) osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultInt");
    ConfigSetDefaultFloat = (ptr_ConfigSetDefaultFloat) osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultFloat");
    ConfigSetDefaultBool = (ptr_ConfigSetDefaultBool) osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultBool");
    ConfigSetDefaultString = (ptr_ConfigSetDefaultString) osal_dynlib_getproc(CoreLibHandle, "ConfigSetDefaultString");
    ConfigGetParamInt = (ptr_ConfigGetParamInt) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamInt");
    ConfigGetParamFloat = (ptr_ConfigGetParamFloat) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamFloat");
    ConfigGetParamBool = (ptr_ConfigGetParamBool) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamBool");
    ConfigGetParamString = (ptr_ConfigGetParamString) osal_dynlib_getproc(CoreLibHandle, "ConfigGetParamString");

    if (!ConfigOpenSection || !ConfigDeleteSection || !ConfigSetParameter || !ConfigGetParameter ||
        !ConfigSetDefaultInt || !ConfigSetDefaultFloat || !ConfigSetDefaultBool || !ConfigSetDefaultString ||
        !ConfigGetParamInt   || !ConfigGetParamFloat   || !ConfigGetParamBool   || !ConfigGetParamString)
        return M64ERR_INCOMPATIBLE;

    /* ConfigSaveSection was added in Config API v2.1.0 */
    if (ConfigAPIVersion >= 0x020100 && !ConfigSaveSection)
        return M64ERR_INCOMPATIBLE;

    /* get a configuration section handle */
    if (ConfigOpenSection("Audio-SDL2", &l_ConfigAudio) != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_ERROR, "Couldn't open config section 'Audio-SDL2'");
        return M64ERR_INPUT_NOT_FOUND;
    }

    /* check the section version number */
    bSaveConfig = 0;
    if (ConfigGetParameter(l_ConfigAudio, "Version", M64TYPE_FLOAT, &fConfigParamsVersion, sizeof(float)) != M64ERR_SUCCESS)
    {
        DebugMessage(M64MSG_WARNING, "No version number in 'Audio-SDL2' config section. Setting defaults.");
        ConfigDeleteSection("Audio-SDL2");
        ConfigOpenSection("Audio-SDL2", &l_ConfigAudio);
        bSaveConfig = 1;
    }
    else if (((int) fConfigParamsVersion) != ((int) CONFIG_PARAM_VERSION))
    {
        DebugMessage(M64MSG_WARNING, "Incompatible version %.2f in 'Audio-SDL2' config section: current is %.2f. Setting defaults.", fConfigParamsVersion, (float) CONFIG_PARAM_VERSION);
        ConfigDeleteSection("Audio-SDL2");
        ConfigOpenSection("Audio-SDL2", &l_ConfigAudio);
        bSaveConfig = 1;
    }
    else if ((CONFIG_PARAM_VERSION - fConfigParamsVersion) >= 0.0001f)
    {
        /* handle upgrades */
        float fVersion = CONFIG_PARAM_VERSION;
        ConfigSetParameter(l_ConfigAudio, "Version", M64TYPE_FLOAT, &fVersion);
        DebugMessage(M64MSG_INFO, "Updating parameter set version in 'Audio-SDL2' config section to %.2f", fVersion);
        bSaveConfig = 1;
    }

    /* set the default values for this plugin */
    ConfigSetDefaultFloat(l_ConfigAudio, "Version",             CONFIG_PARAM_VERSION,  "Mupen64Plus SDL Audio Plugin config parameter version number");
    ConfigSetDefaultInt(l_ConfigAudio, "DEFAULT_FREQUENCY",     DEFAULT_FREQUENCY,     "Frequency which is used if rom doesn't want to change it");
    ConfigSetDefaultBool(l_ConfigAudio, "SWAP_CHANNELS",        0,                     "Swaps left and right channels");
    ConfigSetDefaultInt(l_ConfigAudio, "AUDIO_DEVICE", -1, "ID of audio playback device, -1 for default");

    if (bSaveConfig && ConfigAPIVersion >= 0x020100)
        ConfigSaveSection("Audio-SDL2");

    l_PluginInit = 1;
    ff = 0;
    VolIsMuted = 0;

    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginShutdown(void)
{
    if (!l_PluginInit)
        return M64ERR_NOT_INIT;

    /* reset some local variables */
    l_DebugCallback = NULL;
    l_DebugCallContext = NULL;

    l_PluginInit = 0;
    return M64ERR_SUCCESS;
}

EXPORT m64p_error CALL PluginGetVersion(m64p_plugin_type *PluginType, int *PluginVersion, int *APIVersion, const char **PluginNamePtr, int *Capabilities)
{
    /* set version info */
    if (PluginType != NULL)
        *PluginType = M64PLUGIN_AUDIO;

    if (PluginVersion != NULL)
        *PluginVersion = SDL2_AUDIO_PLUGIN_VERSION;

    if (APIVersion != NULL)
        *APIVersion = AUDIO_PLUGIN_API_VERSION;

    if (PluginNamePtr != NULL)
        *PluginNamePtr = "Mupen64Plus SDL2 Audio Plugin";

    if (Capabilities != NULL)
    {
        *Capabilities = 0;
    }

    return M64ERR_SUCCESS;
}

/* ----------- Audio Functions ------------- */
EXPORT void CALL AiDacrateChanged( int SystemType )
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


EXPORT void CALL AiLenChanged( void )
{
    unsigned int LenReg;
    unsigned char *p;

    if (critical_failure == 1)
        return;
    if (!l_PluginInit)
        return;

    LenReg = *AudioInfo.AI_LEN_REG;
    p = AudioInfo.RDRAM + (*AudioInfo.AI_DRAM_ADDR_REG & 0xFFFFFF);

    unsigned int i;

    for ( i = 0 ; i < LenReg ; i += 4 )
    {
        if(SwapChannels == 0)
        {
            // Left channel
            primaryBuffer[ i ] = p[ i + 2 ];
            primaryBuffer[ i + 1 ] = p[ i + 3 ];

            // Right channel
            primaryBuffer[ i + 2 ] = p[ i ];
            primaryBuffer[ i + 3 ] = p[ i + 1 ];
        } else {
            // Left channel
            primaryBuffer[ i ] = p[ i ];
            primaryBuffer[ i + 1 ] = p[ i + 1 ];

            // Right channel
            primaryBuffer[ i + 2 ] = p[ i + 2];
            primaryBuffer[ i + 3 ] = p[ i + 3 ];
        }
    }
    if (!VolIsMuted && !ff)
    {
        unsigned int audio_queue = SDL_GetQueuedAudioSize(dev);
        unsigned int acceptable_lag = (GameFreq * 0.150) * N64_SAMPLE_BYTES;
        unsigned int diff = 0;
        if (audio_queue > acceptable_lag)
        {
            diff = audio_queue - acceptable_lag;
            diff &= ~3;
        }
        if (LenReg > diff)
            SDL_QueueAudio(dev, primaryBuffer, LenReg - diff);
    }
}

EXPORT int CALL InitiateAudio( AUDIO_INFO Audio_Info )
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

static void InitializeSDL(void)
{
    DebugMessage(M64MSG_INFO, "Initializing SDL2 audio subsystem...");

    if(SDL_Init(SDL_INIT_AUDIO) < 0)
    {
        DebugMessage(M64MSG_ERROR, "Failed to initialize SDL2 audio subsystem; forcing exit.\n");
        critical_failure = 1;
        return;
    }
    critical_failure = 0;

}

static void InitializeAudio(int freq)
{
    SDL_AudioSpec *desired, *obtained;

    if(SDL_WasInit(SDL_INIT_AUDIO) == (SDL_INIT_AUDIO))
    {
        DebugMessage(M64MSG_VERBOSE, "InitializeAudio(): SDL2 Audio sub-system already initialized.");
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
    if(hardware_spec != NULL) free(hardware_spec);

    // Allocate space for SDL_AudioSpec
    desired = malloc(sizeof(SDL_AudioSpec));
    obtained = malloc(sizeof(SDL_AudioSpec));

    desired->freq = GameFreq;

    DebugMessage(M64MSG_VERBOSE, "Requesting frequency: %iHz.", desired->freq);
    /* 16-bit signed audio */
    desired->format=AUDIO_S16SYS;
    DebugMessage(M64MSG_VERBOSE, "Requesting format: %i.", desired->format);
    /* Stereo */
    desired->channels=2;
    desired->samples = 1024;
    desired->callback = NULL;
    desired->userdata = NULL;

    int i, count = SDL_GetNumAudioDevices(0);

    for (i = 0; i < count; ++i)
        DebugMessage(M64MSG_INFO, "Audio device %d: %s", i, SDL_GetAudioDeviceName(i, 0));

    /* Open the audio device */
    const char *dev_name = NULL;
    if (AudioDevice >= 0)
        dev_name = SDL_GetAudioDeviceName(AudioDevice, 0);

    dev = SDL_OpenAudioDevice(dev_name, 0, desired, obtained, SDL_AUDIO_ALLOW_FORMAT_CHANGE);
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
    if (desired->freq != obtained->freq)
    {
        DebugMessage(M64MSG_WARNING, "Obtained frequency differs from requested.");
    }

    /* desired spec is no longer needed */
    free(desired);
    hardware_spec=obtained;

    DebugMessage(M64MSG_VERBOSE, "Frequency: %i", hardware_spec->freq);
    DebugMessage(M64MSG_VERBOSE, "Format: %i", hardware_spec->format);
    DebugMessage(M64MSG_VERBOSE, "Channels: %i", hardware_spec->channels);
    DebugMessage(M64MSG_VERBOSE, "Silence: %i", hardware_spec->silence);
    DebugMessage(M64MSG_VERBOSE, "Samples: %i", hardware_spec->samples);
    DebugMessage(M64MSG_VERBOSE, "Size: %i", hardware_spec->size);

    SDL_PauseAudioDevice(dev, 0);
}
EXPORT void CALL RomClosed( void )
{
    if (!l_PluginInit)
        return;
   if (critical_failure == 1)
       return;
    DebugMessage(M64MSG_VERBOSE, "Cleaning up SDL sound plugin...");

    // Shut down SDL Audio output
    SDL_CloseAudioDevice(dev);

    // Delete the hardware spec struct
    if(hardware_spec != NULL) free(hardware_spec);
    hardware_spec = NULL;

    // Shutdown the respective subsystems
    if(SDL_WasInit(SDL_INIT_AUDIO) != 0) SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

EXPORT void CALL ProcessAList(void)
{
}

EXPORT void CALL SetSpeedFactor(int percentage)
{
    if (percentage > 100)
        ff = 1;
    else
        ff = 0;
}

static void ReadConfig(void)
{
    /* read the configuration values into our static variables */
    GameFreq = ConfigGetParamInt(l_ConfigAudio, "DEFAULT_FREQUENCY");
    SwapChannels = ConfigGetParamBool(l_ConfigAudio, "SWAP_CHANNELS");
    AudioDevice = ConfigGetParamInt(l_ConfigAudio, "AUDIO_DEVICE");
}

EXPORT void CALL VolumeMute(void)
{
    if (!l_PluginInit)
        return;

    // Toogle mute
    VolIsMuted = !VolIsMuted;
}

EXPORT void CALL VolumeUp(void)
{
}

EXPORT void CALL VolumeDown(void)
{
}

EXPORT int CALL VolumeGetLevel(void)
{
    return VolIsMuted ? 0 : 100;
}

EXPORT void CALL VolumeSetLevel(int level)
{
}

EXPORT const char * CALL VolumeGetString(void)
{
    static char VolumeString[32];

    if (VolIsMuted)
    {
        strcpy(VolumeString, "Mute");
    }
    else
    {
        sprintf(VolumeString, "%i%%", 100);
    }

    return VolumeString;
}

