/*
SoLoud audio engine
Copyright (c) 2013-2014 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/
#include "soloud.h"
#include "soloud_thread.h"

#if !defined(WITH_XAUDIO2)

namespace SoLoud
{
	result xaudio2_init(Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate, unsigned int aBuffer)
	{
		return NOT_IMPLEMENTED;
	}
};

#else

#include <windows.h>

#ifdef _MSC_VER
#include <xaudio2.h>
#pragma comment(lib, "xaudio2.lib")
#else
#include "backend/xaudio2/xaudio2.h"
#endif

namespace SoLoud
{
    static const int BUFFER_COUNT = 2;

    struct XAudio2Data
    {
        float *buffer[BUFFER_COUNT];
        IXAudio2 *xaudio2;
        IXAudio2MasteringVoice *masteringVoice;
        IXAudio2SourceVoice *sourceVoice;
        HANDLE bufferEndEvent;
        HANDLE audioProcessingDoneEvent;
		HANDLE criticalErrorEvent;
        class VoiceCallback *voiceCb;
		class EngineCallback *engineCb;
        Thread::ThreadHandle thread;
        Soloud *soloud;
        int samples;
        UINT32 bufferLengthBytes;
		unsigned int sampleRate;
    };

    class VoiceCallback : public IXAudio2VoiceCallback
    {
    public:
        VoiceCallback(HANDLE aBufferEndEvent) 
            : IXAudio2VoiceCallback(), mBufferEndEvent(aBufferEndEvent) {}
        virtual ~VoiceCallback() {}

    private:
        // Called just before this voice's processing pass begins.
        void __stdcall OnVoiceProcessingPassStart(UINT32 aBytesRequired) {}

        // Called just after this voice's processing pass ends.
        void __stdcall OnVoiceProcessingPassEnd() {}

        // Called when this voice has just finished playing a buffer stream
        // (as marked with the XAUDIO2_END_OF_STREAM flag on the last buffer).
        void __stdcall OnStreamEnd() {}

        // Called when this voice is about to start processing a new buffer.
        void __stdcall OnBufferStart(void *aBufferContext) {}

        // Called when this voice has just finished processing a buffer.
        // The buffer can now be reused or destroyed.
        void __stdcall OnBufferEnd(void *aBufferContext) 
        {
            SetEvent(mBufferEndEvent);
        }

        // Called when this voice has just reached the end position of a loop.
        void __stdcall OnLoopEnd(void *aBufferContext) {}

        // Called in the event of a critical error during voice processing,
        // such as a failing xAPO or an error from the hardware XMA decoder.
        // The voice may have to be destroyed and re-created to recover from
        // the error.  The callback arguments report which buffer was being
        // processed when the error occurred, and its HRESULT code.
        void __stdcall OnVoiceError(void *aBufferContext, HRESULT aError) {
			//printf("Got voice error: 0x%x\r\n", aError);
		}

        HANDLE mBufferEndEvent;
    };

	static void xaudio2Cleanup(Soloud *aSoloud);
	result xaudio2_init(Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate, unsigned int aBuffer, unsigned int aChannels);

	class EngineCallback : public IXAudio2EngineCallback
	{
	public:
		EngineCallback(XAudio2Data *aData) : IXAudio2EngineCallback(), mData(aData) {}
		virtual ~EngineCallback() {}

	private:
		// Called by XAudio2 just before an audio processing pass begins.
		void __stdcall OnProcessingPassStart() {}

		// Called just after an audio processing pass ends.
		void __stdcall OnProcessingPassEnd() {}

		// Called in the event of a critical system error which requires XAudio2
		// to be closed down and restarted.  The error code is given in Error.
		void __stdcall OnCriticalError(HRESULT hr) {
			// signal the audio thread that we got an error
			// TODO check HRESULT?
			SetEvent(mData->criticalErrorEvent);
		}

		XAudio2Data *mData;
	};

    static void xaudio2Thread(LPVOID aParam)
    {
        XAudio2Data *data = static_cast<XAudio2Data*>(aParam);
        int bufferIndex = 0;
		HANDLE handles[2] = { data->bufferEndEvent, data->criticalErrorEvent };
        while (WAIT_OBJECT_0 != WaitForSingleObject(data->audioProcessingDoneEvent, 0)) 
        {
            XAUDIO2_VOICE_STATE state;
            data->sourceVoice->GetState(&state);
            while (state.BuffersQueued < BUFFER_COUNT) 
            {
                data->soloud->mix(data->buffer[bufferIndex], data->samples);
                XAUDIO2_BUFFER info = {0};
                info.AudioBytes = data->bufferLengthBytes;
                info.pAudioData = reinterpret_cast<const BYTE*>(data->buffer[bufferIndex]);
                data->sourceVoice->SubmitSourceBuffer(&info);
                ++bufferIndex;
                if (bufferIndex >= BUFFER_COUNT)
                {
                    bufferIndex = 0;
                }
                data->sourceVoice->GetState(&state);
            }
			if (WAIT_OBJECT_0 + 1 == WaitForMultipleObjects(2, handles, FALSE, INFINITE)) {
				// TODO reset the wait handle
				// A critical error occurred, so tear down the audio graph:
				if (0 != data->sourceVoice)
				{
					data->sourceVoice->Stop();
					data->sourceVoice->FlushSourceBuffers();
				}
				if (0 != data->xaudio2)
				{
					data->xaudio2->StopEngine();
				}
				if (0 != data->sourceVoice)
				{
					data->sourceVoice->DestroyVoice();
				}
				if (0 != data->masteringVoice)
				{
					data->masteringVoice->DestroyVoice();
				}
				if (0 != data->xaudio2)
				{
					data->xaudio2->Release();
				}
				// Recreate the audio graph:
				WAVEFORMATEX format;
				ZeroMemory(&format, sizeof(WAVEFORMATEX));
				format.nChannels = 2;
				format.nSamplesPerSec = data->sampleRate;
				format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
				format.nAvgBytesPerSec = data->sampleRate * sizeof(float)*format.nChannels;
				format.nBlockAlign = sizeof(float)*format.nChannels;
				format.wBitsPerSample = sizeof(float) * 8;
				// TODO: DO SOMETHING WITH THE ERRORS! AAARGH!
				if (FAILED(XAudio2Create(&data->xaudio2)))
				{
					//return UNKNOWN_ERROR;
				}
				if (FAILED(data->xaudio2->CreateMasteringVoice(&data->masteringVoice,
					format.nChannels, data->sampleRate)))
				{
					//return UNKNOWN_ERROR;
				}
				if (FAILED(data->xaudio2->RegisterForCallbacks(data->engineCb)))
				{
					//return UNKNOWN_ERROR;
				}
				if (FAILED(data->xaudio2->CreateSourceVoice(&data->sourceVoice,
					&format, XAUDIO2_VOICE_NOSRC | XAUDIO2_VOICE_NOPITCH, 2.f, data->voiceCb)))
				{
					//return UNKNOWN_ERROR;
				}
				data->sourceVoice->Start();
			}
        }
    }

    static void xaudio2Cleanup(Soloud *aSoloud)
    {
        if (0 == aSoloud->mBackendData)
        {
            return;
        }
        XAudio2Data *data = static_cast<XAudio2Data*>(aSoloud->mBackendData);
        SetEvent(data->audioProcessingDoneEvent);
        SetEvent(data->bufferEndEvent);
        Thread::wait(data->thread);
        Thread::release(data->thread);
        if (0 != data->sourceVoice) 
        {
            data->sourceVoice->Stop();
            data->sourceVoice->FlushSourceBuffers();
        }
        if (0 != data->xaudio2)
        {
            data->xaudio2->StopEngine();
        }
        if (0 != data->sourceVoice)
        {
            data->sourceVoice->DestroyVoice();
        }
        if (0 != data->voiceCb)
        {
            delete data->voiceCb;
        }
		if (0 != data->engineCb)
		{
			delete data->engineCb;
		}
        if (0 != data->masteringVoice)
        {
            data->masteringVoice->DestroyVoice();
        }
        if (0 != data->xaudio2)
        {
            data->xaudio2->Release();
        }
        for (int i=0;i<BUFFER_COUNT;++i) 
        {
            if (0 != data->buffer[i])
            {
                delete[] data->buffer[i];
            }
        }
		CloseHandle(data->criticalErrorEvent);
        CloseHandle(data->bufferEndEvent);
        CloseHandle(data->audioProcessingDoneEvent);
        delete data;
        aSoloud->mBackendData = 0;
        CoUninitialize();
    }

    result xaudio2_init(Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate, unsigned int aBuffer, unsigned int aChannels)
    {
        if (FAILED(CoInitializeEx(0, COINIT_MULTITHREADED)))
        {
            return UNKNOWN_ERROR;
        }
        XAudio2Data *data = new XAudio2Data;
        ZeroMemory(data, sizeof(XAudio2Data));
        aSoloud->mBackendData = data;
        aSoloud->mBackendCleanupFunc = xaudio2Cleanup;
        data->bufferEndEvent = CreateEvent(0, FALSE, FALSE, 0);
        if (0 == data->bufferEndEvent)
        {
            return UNKNOWN_ERROR;
        }
        data->audioProcessingDoneEvent = CreateEvent(0, FALSE, FALSE, 0);
        if (0 == data->audioProcessingDoneEvent)
        {
            return UNKNOWN_ERROR;
        }
		data->criticalErrorEvent = CreateEvent(0, FALSE, FALSE, 0);
		if (0 == data->criticalErrorEvent)
		{
			return UNKNOWN_ERROR;
		}
        WAVEFORMATEX format;
        ZeroMemory(&format, sizeof(WAVEFORMATEX));
        format.nChannels = 2;
        format.nSamplesPerSec = aSamplerate;
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nAvgBytesPerSec = aSamplerate*sizeof(float)*format.nChannels;
        format.nBlockAlign = sizeof(float)*format.nChannels;
        format.wBitsPerSample = sizeof(float)*8;
        if (FAILED(XAudio2Create(&data->xaudio2)))
        {
            return UNKNOWN_ERROR;
        }
        if (FAILED(data->xaudio2->CreateMasteringVoice(&data->masteringVoice, 
                                                       format.nChannels, aSamplerate))) 
        {
            return UNKNOWN_ERROR;
        }
		data->engineCb = new EngineCallback(data);
		if (FAILED(data->xaudio2->RegisterForCallbacks(data->engineCb)))
		{
			return UNKNOWN_ERROR;
		}
        data->voiceCb = new VoiceCallback(data->bufferEndEvent);
        if (FAILED(data->xaudio2->CreateSourceVoice(&data->sourceVoice, 
                   &format, XAUDIO2_VOICE_NOSRC|XAUDIO2_VOICE_NOPITCH, 2.f, data->voiceCb))) 
        {
            return UNKNOWN_ERROR;
        }
        data->bufferLengthBytes = aBuffer * format.nChannels * sizeof(float);
        for (int i=0;i<BUFFER_COUNT;++i)
        {
            data->buffer[i] = new float[aBuffer * format.nChannels];
        }
        data->samples = aBuffer;
		data->sampleRate = aSamplerate;
        data->soloud = aSoloud;
        aSoloud->postinit(aSamplerate, aBuffer * format.nChannels, aFlags, 2);
        data->thread = Thread::createThread(xaudio2Thread, data);
        if (0 == data->thread)
        {
            return UNKNOWN_ERROR;
        }
        data->sourceVoice->Start();
        aSoloud->mBackendString = "XAudio2";
        return 0;
    }
};

#endif