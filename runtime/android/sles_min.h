// Minimal OpenSL ES 1.0.1 + Android extension declarations: only what opensles.c implements.
// Layouts and constants follow the Khronos OpenSLES.h / Android OpenSLES_Android.h (NDK) ABI.
#ifndef SLES_MIN_H
#define SLES_MIN_H
#include <stdint.h>

typedef uint8_t SLuint8;
typedef int16_t SLint16;
typedef uint16_t SLuint16;
typedef int32_t SLint32;
typedef uint32_t SLuint32;
typedef SLuint32 SLboolean;
typedef SLuint32 SLresult;
typedef SLint16 SLmillibel;
typedef SLuint32 SLmillisecond;
typedef SLint16 SLpermille;
typedef SLuint8 SLchar;

struct SLInterfaceID_ {
    SLuint32 time_low;
    SLuint16 time_mid;
    SLuint16 time_hi_and_version;
    SLuint16 clock_seq;
    SLuint8 node[6];
};
typedef const struct SLInterfaceID_ *SLInterfaceID;

typedef struct { SLuint32 feature; SLuint32 data; } SLEngineOption;
typedef struct { void *pLocator; void *pFormat; } SLDataSource;
typedef struct { void *pLocator; void *pFormat; } SLDataSink;
typedef struct { SLuint32 locatorType; SLuint32 numBuffers; } SLDataLocator_BufferQueue;  // also Android simple BQ
typedef struct { SLuint32 locatorType; void *outputMix; } SLDataLocator_OutputMix;
typedef struct {
    SLuint32 formatType, numChannels, samplesPerSec /* milliHz */, bitsPerSample, containerSize,
             channelMask, endianness;
} SLDataFormat_PCM;
typedef struct {   // SL_ANDROID_DATAFORMAT_PCM_EX
    SLuint32 formatType, numChannels, sampleRate /* milliHz */, bitsPerSample, containerSize,
             channelMask, endianness, representation;
} SLAndroidDataFormat_PCM_EX;
typedef struct { SLuint32 count; SLuint32 index; } SLBufferQueueState;  // index = playIndex

#define SL_BOOLEAN_FALSE 0u
#define SL_BOOLEAN_TRUE 1u

#define SL_RESULT_SUCCESS 0u
#define SL_RESULT_PRECONDITIONS_VIOLATED 1u
#define SL_RESULT_PARAMETER_INVALID 2u
#define SL_RESULT_MEMORY_FAILURE 3u
#define SL_RESULT_RESOURCE_ERROR 4u
#define SL_RESULT_BUFFER_INSUFFICIENT 7u
#define SL_RESULT_CONTENT_UNSUPPORTED 9u
#define SL_RESULT_FEATURE_UNSUPPORTED 12u
#define SL_RESULT_INTERNAL_ERROR 13u

#define SL_OBJECT_STATE_UNREALIZED 1u
#define SL_OBJECT_STATE_REALIZED 2u
#define SL_OBJECT_STATE_SUSPENDED 3u
#define SL_OBJECT_EVENT_ASYNC_TERMINATION 2u

#define SL_PLAYSTATE_STOPPED 1u
#define SL_PLAYSTATE_PAUSED 2u
#define SL_PLAYSTATE_PLAYING 3u
#define SL_PLAYEVENT_HEADATEND 1u
#define SL_TIME_UNKNOWN 0xFFFFFFFFu

#define SL_DATALOCATOR_OUTPUTMIX 4u
#define SL_DATALOCATOR_BUFFERQUEUE 6u
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE 0x800007BDu
#define SL_DATAFORMAT_PCM 2u
#define SL_ANDROID_DATAFORMAT_PCM_EX 4u
#define SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT 1u
#define SL_ANDROID_PCM_REPRESENTATION_UNSIGNED_INT 2u
#define SL_ANDROID_PCM_REPRESENTATION_FLOAT 3u
#define SL_BYTEORDER_BIGENDIAN 1u
#define SL_BYTEORDER_LITTLEENDIAN 2u
#define SL_SPEAKER_FRONT_LEFT 1u
#define SL_SPEAKER_FRONT_RIGHT 2u
#define SL_SPEAKER_FRONT_CENTER 4u
#define SL_MILLIBEL_MIN ((SLmillibel)-32768)

#define SL_ANDROID_KEY_STREAM_TYPE "androidPlaybackStreamType"
#define SL_ANDROID_KEY_PERFORMANCE_MODE "androidPerformanceMode"
#define SL_ANDROID_STREAM_MEDIA 3

// Interface handles are pointers to a pointer to the vtable.
typedef const struct SLObjectItf_ *const *SLObjectItf;
typedef const struct SLEngineItf_ *const *SLEngineItf;
typedef const struct SLPlayItf_ *const *SLPlayItf;
typedef const struct SLBufferQueueItf_ *const *SLBufferQueueItf;   // == SLAndroidSimpleBufferQueueItf
typedef const struct SLVolumeItf_ *const *SLVolumeItf;
typedef const struct SLOutputMixItf_ *const *SLOutputMixItf;
typedef const struct SLAndroidConfigurationItf_ *const *SLAndroidConfigurationItf;

typedef void (*slObjectCallback)(SLObjectItf caller, const void *pContext, SLuint32 event,
                                 SLresult result, SLuint32 param, void *pInterface);
typedef void (*slPlayCallback)(SLPlayItf caller, void *pContext, SLuint32 event);
typedef void (*slBufferQueueCallback)(SLBufferQueueItf caller, void *pContext);

struct SLObjectItf_ {
    SLresult (*Realize)(SLObjectItf self, SLboolean async);
    SLresult (*Resume)(SLObjectItf self, SLboolean async);
    SLresult (*GetState)(SLObjectItf self, SLuint32 *pState);
    SLresult (*GetInterface)(SLObjectItf self, const SLInterfaceID iid, void *pInterface);
    SLresult (*RegisterCallback)(SLObjectItf self, slObjectCallback callback, void *pContext);
    void (*AbortAsyncOperation)(SLObjectItf self);
    void (*Destroy)(SLObjectItf self);
    SLresult (*SetPriority)(SLObjectItf self, SLint32 priority, SLboolean preemptable);
    SLresult (*GetPriority)(SLObjectItf self, SLint32 *pPriority, SLboolean *pPreemptable);
    SLresult (*SetLossOfControlInterfaces)(SLObjectItf self, SLint16 numInterfaces,
                                           SLInterfaceID *pInterfaceIDs, SLboolean enabled);
};

typedef SLresult (*slCreateDeviceFn)(SLEngineItf, SLObjectItf *, SLuint32, SLuint32, const SLInterfaceID *, const SLboolean *);
struct SLEngineItf_ {
    slCreateDeviceFn CreateLEDDevice;
    slCreateDeviceFn CreateVibraDevice;
    SLresult (*CreateAudioPlayer)(SLEngineItf self, SLObjectItf *pPlayer, SLDataSource *pAudioSrc,
                                  SLDataSink *pAudioSnk, SLuint32 numInterfaces,
                                  const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*CreateAudioRecorder)(SLEngineItf self, SLObjectItf *pRecorder, SLDataSource *pAudioSrc,
                                    SLDataSink *pAudioSnk, SLuint32 numInterfaces,
                                    const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*CreateMidiPlayer)(SLEngineItf self, SLObjectItf *pPlayer, SLDataSource *pMIDISrc,
                                 SLDataSource *pBankSrc, SLDataSink *pAudioOutput, SLDataSink *pVibra,
                                 SLDataSink *pLEDArray, SLuint32 numInterfaces,
                                 const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*CreateListener)(SLEngineItf self, SLObjectItf *pListener, SLuint32 numInterfaces,
                               const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*Create3DGroup)(SLEngineItf self, SLObjectItf *pGroup, SLuint32 numInterfaces,
                              const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*CreateOutputMix)(SLEngineItf self, SLObjectItf *pMix, SLuint32 numInterfaces,
                                const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*CreateMetadataExtractor)(SLEngineItf self, SLObjectItf *pMetadataExtractor,
                                        SLDataSource *pDataSource, SLuint32 numInterfaces,
                                        const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*CreateExtensionObject)(SLEngineItf self, SLObjectItf *pObject, void *pParameters,
                                      SLuint32 objectID, SLuint32 numInterfaces,
                                      const SLInterfaceID *pInterfaceIds, const SLboolean *pInterfaceRequired);
    SLresult (*QueryNumSupportedInterfaces)(SLEngineItf self, SLuint32 objectID, SLuint32 *pNum);
    SLresult (*QuerySupportedInterfaces)(SLEngineItf self, SLuint32 objectID, SLuint32 index, SLInterfaceID *pId);
    SLresult (*QueryNumSupportedExtensions)(SLEngineItf self, SLuint32 *pNumExtensions);
    SLresult (*QuerySupportedExtension)(SLEngineItf self, SLuint32 index, SLchar *pName, SLint16 *pNameLength);
    SLresult (*IsExtensionSupported)(SLEngineItf self, const SLchar *pName, SLboolean *pSupported);
};

struct SLPlayItf_ {
    SLresult (*SetPlayState)(SLPlayItf self, SLuint32 state);
    SLresult (*GetPlayState)(SLPlayItf self, SLuint32 *pState);
    SLresult (*GetDuration)(SLPlayItf self, SLmillisecond *pMsec);
    SLresult (*GetPosition)(SLPlayItf self, SLmillisecond *pMsec);
    SLresult (*RegisterCallback)(SLPlayItf self, slPlayCallback callback, void *pContext);
    SLresult (*SetCallbackEventsMask)(SLPlayItf self, SLuint32 eventFlags);
    SLresult (*GetCallbackEventsMask)(SLPlayItf self, SLuint32 *pEventFlags);
    SLresult (*SetMarkerPosition)(SLPlayItf self, SLmillisecond mSec);
    SLresult (*ClearMarkerPosition)(SLPlayItf self);
    SLresult (*GetMarkerPosition)(SLPlayItf self, SLmillisecond *pMsec);
    SLresult (*SetPositionUpdatePeriod)(SLPlayItf self, SLmillisecond mSec);
    SLresult (*GetPositionUpdatePeriod)(SLPlayItf self, SLmillisecond *pMsec);
};

// SLBufferQueueItf_ and SLAndroidSimpleBufferQueueItf_ have the same layout.
struct SLBufferQueueItf_ {
    SLresult (*Enqueue)(SLBufferQueueItf self, const void *pBuffer, SLuint32 size);
    SLresult (*Clear)(SLBufferQueueItf self);
    SLresult (*GetState)(SLBufferQueueItf self, SLBufferQueueState *pState);
    SLresult (*RegisterCallback)(SLBufferQueueItf self, slBufferQueueCallback callback, void *pContext);
};

struct SLVolumeItf_ {
    SLresult (*SetVolumeLevel)(SLVolumeItf self, SLmillibel level);
    SLresult (*GetVolumeLevel)(SLVolumeItf self, SLmillibel *pLevel);
    SLresult (*GetMaxVolumeLevel)(SLVolumeItf self, SLmillibel *pMaxLevel);
    SLresult (*SetMute)(SLVolumeItf self, SLboolean mute);
    SLresult (*GetMute)(SLVolumeItf self, SLboolean *pMute);
    SLresult (*EnableStereoPosition)(SLVolumeItf self, SLboolean enable);
    SLresult (*IsEnabledStereoPosition)(SLVolumeItf self, SLboolean *pEnable);
    SLresult (*SetStereoPosition)(SLVolumeItf self, SLpermille stereoPosition);
    SLresult (*GetStereoPosition)(SLVolumeItf self, SLpermille *pStereoPosition);
};

struct SLOutputMixItf_ {
    SLresult (*GetDestinationOutputDeviceIDs)(SLOutputMixItf self, SLint32 *pNumDevices, SLuint32 *pDeviceIDs);
    SLresult (*RegisterDeviceChangeCallback)(SLOutputMixItf self, void *callback, void *pContext);
    SLresult (*ReRoute)(SLOutputMixItf self, SLint32 numOutputDevices, SLuint32 *pOutputDeviceIDs);
};

struct SLAndroidConfigurationItf_ {
    SLresult (*SetConfiguration)(SLAndroidConfigurationItf self, const SLchar *key, const void *pValue, SLuint32 size);
    SLresult (*GetConfiguration)(SLAndroidConfigurationItf self, const SLchar *key, SLuint32 *pSize, void *pValue);
    SLresult (*AcquireJavaProxy)(SLAndroidConfigurationItf self, SLuint32 proxyType, void **pProxyObj);
    SLresult (*ReleaseJavaProxy)(SLAndroidConfigurationItf self, SLuint32 proxyType);
};

extern const SLInterfaceID SL_IID_NULL, SL_IID_OBJECT, SL_IID_ENGINE, SL_IID_ENGINECAPABILITIES,
    SL_IID_OUTPUTMIX, SL_IID_PLAY, SL_IID_RECORD, SL_IID_VOLUME, SL_IID_BUFFERQUEUE,
    SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_ANDROIDCONFIGURATION, SL_IID_EFFECTSEND,
    SL_IID_ANDROIDEFFECTSEND, SL_IID_MUTESOLO, SL_IID_PLAYBACKRATE, SL_IID_SEEK,
    SL_IID_PREFETCHSTATUS, SL_IID_METADATAEXTRACTION, SL_IID_ANDROIDEFFECT,
    SL_IID_ANDROIDBUFFERQUEUESOURCE;

SLresult slCreateEngine(SLObjectItf *pEngine, SLuint32 numOptions, const SLEngineOption *pEngineOptions,
                        SLuint32 numInterfaces, const SLInterfaceID *pInterfaceIds,
                        const SLboolean *pInterfaceRequired);
SLresult slQueryNumSupportedEngineInterfaces(SLuint32 *pNumSupportedInterfaces);
SLresult slQuerySupportedEngineInterfaces(SLuint32 index, SLInterfaceID *pInterfaceId);

#endif
