// Darwin stand-in (dm/android/compat): AudioToolbox's types. The output AudioUnit is implemented on AAudio (aaudio_unit.c), so hle_audio.c's real output, watchdog and
// background handling run unchanged; the AudioConverter (AAC in movies) is MediaCodec's AAC decoder (audio_converter.c)
#pragma once
#include <stdint.h>
#include <pthread/qos.h>
typedef int32_t OSStatus;
typedef uint32_t UInt32;
typedef double Float64;
typedef uint32_t AudioUnitRenderActionFlags, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, AudioConverterPropertyID;
#define noErr 0
enum { dm_at_unsupported = -50 };
typedef struct { Float64 mSampleRate; UInt32 mFormatID, mFormatFlags, mBytesPerPacket, mFramesPerPacket, mBytesPerFrame, mChannelsPerFrame, mBitsPerChannel, mReserved; } AudioStreamBasicDescription;
typedef struct { int64_t mStartOffset; UInt32 mVariableFramesInPacket, mDataByteSize; } AudioStreamPacketDescription;
typedef struct { UInt32 mNumberChannels, mDataByteSize; void *mData; } AudioBuffer;
typedef struct { UInt32 mNumberBuffers; AudioBuffer mBuffers[1]; } AudioBufferList;
typedef struct { Float64 mSampleTime; uint64_t mHostTime; } AudioTimeStamp;
typedef struct { UInt32 componentType, componentSubType, componentManufacturer, componentFlags, componentFlagsMask; } AudioComponentDescription;
typedef struct dm_audio_component *AudioComponent, *AudioComponentInstance, *AudioUnit;
typedef struct dm_audio_converter *AudioConverterRef;
typedef OSStatus (*AURenderCallback)(void *, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32, AudioBufferList *);
typedef OSStatus (*AudioConverterComplexInputDataProc)(AudioConverterRef, UInt32 *, AudioBufferList *, AudioStreamPacketDescription **, void *);
typedef struct { AURenderCallback inputProc; void *inputProcRefCon; } AURenderCallbackStruct;
enum { kAudioUnitType_Output = 1, kAudioUnitSubType_DefaultOutput = 2, kAudioUnitSubType_RemoteIO = 3, kAudioUnitManufacturer_Apple = 4,
       kAudioUnitProperty_StreamFormat = 8, kAudioUnitProperty_SetRenderCallback = 23, kAudioDevicePropertyBufferFrameSize = 24, kAudioUnitScope_Global = 0, kAudioUnitScope_Input = 1,
       kAudioFormatLinearPCM = 0x6c70636d, kAudioFormatMPEG4AAC = 0x61616320, kLinearPCMFormatFlagIsFloat = 1, kLinearPCMFormatFlagIsSignedInteger = 4, kLinearPCMFormatFlagIsPacked = 8,
       kAudioConverterDecompressionMagicCookie = 0x646d6763 };
AudioComponent AudioComponentFindNext(AudioComponent c, const AudioComponentDescription *d);
OSStatus AudioComponentInstanceNew(AudioComponent c, AudioComponentInstance *o);
OSStatus AudioComponentInstanceDispose(AudioComponentInstance i);
OSStatus AudioUnitSetProperty(AudioUnit u, AudioUnitPropertyID p, AudioUnitScope s, AudioUnitElement e, const void *d, UInt32 n);
OSStatus AudioUnitInitialize(AudioUnit u);
static inline OSStatus AudioUnitUninitialize(AudioUnit u) { (void)u; return noErr; }
OSStatus AudioOutputUnitStart(AudioUnit u);
OSStatus AudioOutputUnitStop(AudioUnit u);
OSStatus AudioConverterNew(const AudioStreamBasicDescription *i, const AudioStreamBasicDescription *o, AudioConverterRef *c);
OSStatus AudioConverterDispose(AudioConverterRef c);
OSStatus AudioConverterSetProperty(AudioConverterRef c, AudioConverterPropertyID p, UInt32 n, const void *d);
OSStatus AudioConverterFillComplexBuffer(AudioConverterRef c, AudioConverterComplexInputDataProc f, void *u, UInt32 *n, AudioBufferList *o, AudioStreamPacketDescription *d);
OSStatus AudioConverterReset(AudioConverterRef c);
