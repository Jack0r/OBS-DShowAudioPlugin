/********************************************************************************
 Copyright (C) 2012 Hugh Bailey <obs.jim@gmail.com>

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; either version 2 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
********************************************************************************/


#include "DShowAudioPlugin.h"

DWORD STDCALL PackPlanarThread(ConvertData *data);

#define NEAR_SILENT  3000
#define NEAR_SILENTf 3000.0


bool DeviceSource::Init(XElement *data)
{
    HRESULT err;
    err = CoCreateInstance(CLSID_FilterGraph, NULL, CLSCTX_INPROC_SERVER, (REFIID)IID_IFilterGraph, (void**)&graph);
    if(FAILED(err))
    {
        AppWarning(TEXT("DShowAudioPlugin: Failed to build IGraphBuilder, result = %08lX"), err);
        return false;
    }

    err = CoCreateInstance(CLSID_CaptureGraphBuilder2, NULL, CLSCTX_INPROC_SERVER, (REFIID)IID_ICaptureGraphBuilder2, (void**)&capture);
    if(FAILED(err))
    {
        AppWarning(TEXT("DShowAudioPlugin: Failed to build ICaptureGraphBuilder2, result = %08lX"), err);
        return false;
    }

    hSampleMutex = OSCreateMutex();
    if(!hSampleMutex)
    {
        AppWarning(TEXT("DShowAudioPlugin: could not create sample mutex"));
        return false;
    }

    capture->SetFiltergraph(graph);

    int numThreads = MAX(OSGetTotalCores()-2, 1);
    hConvertThreads = (HANDLE*)Allocate(sizeof(HANDLE)*numThreads);
    convertData = (ConvertData*)Allocate(sizeof(ConvertData)*numThreads);

    zero(hConvertThreads, sizeof(HANDLE)*numThreads);
    zero(convertData, sizeof(ConvertData)*numThreads);

    this->data = data;
    UpdateSettings();

    //if(!bFiltersLoaded)
    //    return false;

    Log(TEXT("Using directshow audio input"));

    return true;
}

DeviceSource::~DeviceSource()
{
    Stop();
    UnloadFilters();

    SafeRelease(capture);
    SafeRelease(graph);

    if(hConvertThreads)
        Free(hConvertThreads);

    if(convertData)
        Free(convertData);

    if(hSampleMutex)
        OSCloseMutex(hSampleMutex);
}

String DeviceSource::ChooseShader()
{
    return String();
}

void DeviceSource::SetAudioInfo(AM_MEDIA_TYPE *audioMediaType, GUID &expectedAudioType)
{
    expectedAudioType = audioMediaType->subtype;

    if(audioMediaType->formattype == FORMAT_WaveFormatEx)
    {
        WAVEFORMATEX *pFormat = reinterpret_cast<WAVEFORMATEX*>(audioMediaType->pbFormat);
        mcpy(&audioFormat, pFormat, sizeof(audioFormat));

        Log(TEXT("    device audio info - bits per sample: %u, channels: %u, samples per sec: %u, block size: %u"),
            audioFormat.wBitsPerSample, audioFormat.nChannels, audioFormat.nSamplesPerSec, audioFormat.nBlockAlign);

        //avoid local resampling if possible
        /*if(pFormat->nSamplesPerSec != 44100)
        {
            pFormat->nSamplesPerSec = 44100;
            if(SUCCEEDED(audioConfig->SetFormat(audioMediaType)))
            {
                Log(TEXT("    also successfully set samples per sec to 44.1k"));
                audioFormat.nSamplesPerSec = 44100;
            }
        }*/
    }
    else
    {
        AppWarning(TEXT("DShowAudioPlugin: Audio format was not a normal wave format"));
        soundOutputType = 0;
    }

    DeleteMediaType(audioMediaType);
}

bool DeviceSource::LoadFilters()
{
    if(bCapturing || bFiltersLoaded)
        return false;

    bool bSucceeded = false;

    List<MediaOutputInfo> outputList;
    IAMStreamConfig *config = NULL;
    bool bAddedVideoCapture = false, bAddedAudioCapture = false, bAddedDevice = false;
    GUID expectedMediaType;
    IPin *devicePin = NULL, *audioPin = NULL;
    HRESULT err;
    String strShader;

    bUseThreadedConversion = API->UseMultithreadedOptimizations() && (OSGetTotalCores() > 1);

    //------------------------------------------------
    // basic initialization vars

    bool bCheckForceAudio = data->GetInt(TEXT("forceCustomAudioDevice")) != 0;

    bUseCustomResolution = data->GetInt(TEXT("customResolution"));
    strDevice = data->GetString(TEXT("device"));
    strDeviceName = data->GetString(TEXT("deviceName"));
    strDeviceID = data->GetString(TEXT("deviceID"));
    strAudioDevice = data->GetString(TEXT("audioDevice"));
    strAudioName = data->GetString(TEXT("audioDeviceName"));
    strAudioID = data->GetString(TEXT("audioDeviceID"));
    strAudioGUID = data->GetString(TEXT("audioDeviceCLSID"));

    if(strAudioGUID.Compare(TEXT("CLSID_AudioInputDeviceCategory"))) matchGUID = CLSID_AudioInputDeviceCategory;
    if(strAudioGUID.Compare(TEXT("CLSID_VideoInputDeviceCategory"))) matchGUID = CLSID_VideoInputDeviceCategory;
    if(strAudioGUID.Compare(TEXT("CLSID_AudioRendererCategory"))) {
        //Log(TEXT("Dese are spekers.\n"));
        matchGUID = CLSID_AudioRendererCategory;
    }

    bFlipVertical = data->GetInt(TEXT("flipImage")) != 0;
    bFlipHorizontal = data->GetInt(TEXT("flipImageHorizontal")) != 0;

    opacity = data->GetInt(TEXT("opacity"), 100);

    float volume = data->GetFloat(TEXT("volume"), 1.0f);

    //------------------------------------------------
    // chrom key stuff

    bUseChromaKey = data->GetInt(TEXT("useChromaKey")) != 0;
    keyColor = data->GetInt(TEXT("keyColor"), 0xFFFFFFFF);
    keySimilarity = data->GetInt(TEXT("keySimilarity"));
    keyBlend = data->GetInt(TEXT("keyBlend"), 80);
    keySpillReduction = data->GetInt(TEXT("keySpillReduction"), 50);

    if(keyBaseColor.x < keyBaseColor.y && keyBaseColor.x < keyBaseColor.z)
        keyBaseColor -= keyBaseColor.x;
    else if(keyBaseColor.y < keyBaseColor.x && keyBaseColor.y < keyBaseColor.z)
        keyBaseColor -= keyBaseColor.y;
    else if(keyBaseColor.z < keyBaseColor.x && keyBaseColor.z < keyBaseColor.y)
        keyBaseColor -= keyBaseColor.z;

    //------------------------------------------------
    // get the device filter and pins

    if(strAudioDevice.IsValid())
    {
        audioDeviceFilter = GetDeviceByValue(matchGUID, L"FriendlyName", strAudioName, L"DevicePath", strAudioID);
        if(!audioDeviceFilter) {
            AppWarning(TEXT("DShowAudioPlugin: Invalid audio device: name '%s', path '%s'"), strAudioName.Array(), strAudioID.Array());
        }
    }

    soundOutputType = data->GetInt(TEXT("soundOutputType"));

    if(soundOutputType != 0)
    {
        if(matchGUID == CLSID_AudioRendererCategory) {
            err = capture->FindPin(audioDeviceFilter, PINDIR_OUTPUT, &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, FALSE, 0, &audioPin);
        }
        else {
            err = capture->FindPin(audioDeviceFilter, PINDIR_OUTPUT, &PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, FALSE, 0, &audioPin);
        }
        if(FAILED(err))
        {
            Log(TEXT("DShowAudioPlugin: No audio pin, result = %lX"), err);
            soundOutputType = 0;
        }
    }

    int soundTimeOffset = data->GetInt(TEXT("soundTimeOffset"));

    //GetOutputList(devicePin, outputList);

    //------------------------------------------------
    // initialize the basic video variables and data


    //------------------------------------------------
    // log audio info
    {
    String strTest;

        strTest = FormattedString(TEXT("    audio device: %s,\r\n    audio device id %s,\r\n"), strAudioDevice.Array(), strAudioID.Array());

        Log(TEXT("------------------------------------------"));
        Log(strTest.Array());
    }

    //------------------------------------------------
    // get audio pin configuration, optionally configure audio pin to 44100

    GUID expectedAudioType;

    if(soundOutputType == 1)
    {
        IAMStreamConfig *audioConfig;
        if(SUCCEEDED(audioPin->QueryInterface(IID_IAMStreamConfig, (void**)&audioConfig)))
        {
            AM_MEDIA_TYPE *audioMediaType;
            if(SUCCEEDED(err = audioConfig->GetFormat(&audioMediaType)))
            {
                SetAudioInfo(audioMediaType, expectedAudioType);
                //Log(TEXT("Fixed size samples: %s\r\n"), (audioMediaType->bFixedSizeSamples) ? "Yes" : "No");
                //Log(TEXT("Temporal Compression: %s\r\n"), (audioMediaType->bTemporalCompression) ? "Yes" : "No");
                //Log(TEXT("cbFormat: %.16X\r\n"), audioMediaType->cbFormat);
                //Log(TEXT("Sample size: %u\r\n"), audioMediaType->lSampleSize);
            }
            else if(err == E_NOTIMPL) //elgato probably
            {
                IEnumMediaTypes *audioMediaTypes;
                if(SUCCEEDED(err = audioPin->EnumMediaTypes(&audioMediaTypes)))
                {
                    ULONG i = 0;
                    if((err = audioMediaTypes->Next(1, &audioMediaType, &i)) == S_OK)
                        SetAudioInfo(audioMediaType, expectedAudioType);
                    else
                    {
                        AppWarning(TEXT("DShowAudioPlugin: audioMediaTypes->Next failed, result = %08lX"), err);
                        soundOutputType = 0;
                    }

                    audioMediaTypes->Release();
                }
                else
                {
                    AppWarning(TEXT("DShowAudioPlugin: audioMediaTypes->Next failed, result = %08lX"), err);
                    soundOutputType = 0;
                }
            }
            else
            {
                AppWarning(TEXT("DShowAudioPlugin: Could not get audio format, result = %08lX"), err);
                soundOutputType = 0;
            }

            audioConfig->Release();
        }
        else {
            soundOutputType = 0;
        }
    }

    //------------------------------------------------
    // add video capture filter if any

    captureFilter = new CaptureFilter(this, MEDIATYPE_Video, expectedMediaType);

    if(FAILED(err = graph->AddFilter(captureFilter, NULL)))
    {
        AppWarning(TEXT("DShowAudioPlugin: Failed to add video capture filter to graph, result = %08lX"), err);
        goto cleanFinish;
    }

    bAddedVideoCapture = true;

    //------------------------------------------------
    // add audio capture filter if any

    if(soundOutputType == 1)
    {
        audioFilter = new CaptureFilter(this, MEDIATYPE_Audio, expectedAudioType);
        if(!audioFilter)
        {
            AppWarning(TEXT("Failed to create audio capture filter"));
            soundOutputType = 0;
        }
    }
    else if(soundOutputType == 2)
    {
        if(FAILED(err = CoCreateInstance(CLSID_DSoundRender, NULL, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&audioFilter)))
        {
            AppWarning(TEXT("DShowPlugin: failed to create audio renderer, result = %08lX"), err);
            soundOutputType = 0;
        }

        IBasicAudio *basicAudio;
        if(SUCCEEDED(audioFilter->QueryInterface(IID_IBasicAudio, (void**)&basicAudio)))
        {
            long lVol = long((double(volume)*NEAR_SILENTf)-NEAR_SILENTf);
            if(lVol <= -NEAR_SILENT)
                lVol = -10000;
            basicAudio->put_Volume(lVol);
            basicAudio->Release();
        }
    }

    if(soundOutputType != 0)
    {
        if(FAILED(err = graph->AddFilter(audioFilter, NULL)))
        {
            AppWarning(TEXT("DShowAudioPlugin: Failed to add audio capture filter to graph, result = %08lX"), err);
            goto cleanFinish;
        }

        bAddedAudioCapture = true;
    }

    //------------------------------------------------
    // add primary device filter

    if(FAILED(err = graph->AddFilter(audioDeviceFilter, NULL)))
    {
        AppWarning(TEXT("DShowAudioPlugin: Failed to add audio device filter to graph, result = %08lX"), err);
        goto cleanFinish;
    }

    bAddedDevice = true;

    //------------------------------------------------
    // connect all pins and set up the whole capture thing

    bool bConnected;

    if(soundOutputType != 0)
    {
        bConnected = SUCCEEDED(err = capture->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Audio, audioDeviceFilter, NULL, audioFilter));
        if(!bConnected)
        {
            AppWarning(TEXT("DShowAudioPlugin: Failed to connect the audio device pin to the audio capture pin, result = %08lX"), err);
            soundOutputType = 0;
        }
    }

    if(FAILED(err = graph->QueryInterface(IID_IMediaControl, (void**)&control)))
    {
        AppWarning(TEXT("DShowAudioPlugin: Failed to get IMediaControl, result = %08lX"), err);
        goto cleanFinish;
    }

    if(soundOutputType == 1)
    {
        audioOut = new DeviceAudioSource;
        audioOut->Initialize(this);
        API->AddAudioSource(audioOut);

        audioOut->SetAudioOffset(soundTimeOffset);
        audioOut->SetVolume(volume);
    }

    bSucceeded = true;

cleanFinish:
    SafeRelease(config);
    SafeRelease(devicePin);
    SafeRelease(audioPin);

    for(UINT i=0; i<outputList.Num(); i++)
        outputList[i].FreeData();

    if(!bSucceeded)
    {
        bCapturing = false;

        if(bAddedVideoCapture)
            graph->RemoveFilter(captureFilter);
        if(bAddedAudioCapture)
            graph->RemoveFilter(audioFilter);
        if(bAddedDevice) {
            graph->RemoveFilter(audioDeviceFilter);
            graph->RemoveFilter(deviceFilter);
        }

        SafeRelease(audioDeviceFilter);
        SafeRelease(deviceFilter);
        SafeRelease(captureFilter);
        SafeRelease(audioFilter);
        SafeRelease(control);

        if(colorConvertShader)
        {
            delete colorConvertShader;
            colorConvertShader = NULL;
        }

        if(audioOut)
        {
            delete audioOut;
            audioOut = NULL;
        }

        if(lpImageBuffer)
        {
            Free(lpImageBuffer);
            lpImageBuffer = NULL;
        }

        bReadyToDraw = true;
    }
    else
        bReadyToDraw = false;

    if(!renderCX) renderCX = 32;
    if(!renderCY) renderCY = 32;

    //-----------------------------------------------------
    // create the texture regardless, will just show up as red to indicate failure
    BYTE *textureData = (BYTE*)Allocate(renderCX*renderCY*4);

    if(colorType == DeviceOutputType_RGB) //you may be confused, but when directshow outputs RGB, it's actually outputting BGR
    {
        msetd(textureData, 0xFFFF0000, renderCX*renderCY*4);
        texture = CreateTexture(renderCX, renderCY, GS_BGR, textureData, FALSE, FALSE);
    }
    else //if we're working with planar YUV, we can just use regular RGB textures instead
    {
        msetd(textureData, 0xFF0000FF, renderCX*renderCY*4);
        texture = CreateTexture(renderCX, renderCY, GS_RGB, textureData, FALSE, FALSE);
    }

    if(bSucceeded && bUseThreadedConversion)
    {
        if(colorType == DeviceOutputType_I420 || colorType == DeviceOutputType_YV12)
        {
            LPBYTE lpData;
            if(texture->Map(lpData, texturePitch))
                texture->Unmap();
            else
                texturePitch = renderCX*4;

            lpImageBuffer = (LPBYTE)Allocate(texturePitch*renderCY);
        }
    }

    Free(textureData);

    bFiltersLoaded = bSucceeded;
    return bSucceeded;
}

void DeviceSource::UnloadFilters()
{
    if(texture)
    {
        delete texture;
        texture = NULL;
    }

    int numThreads = MAX(OSGetTotalCores()-2, 1);
    for(int i=0; i<numThreads; i++)
    {
        if(hConvertThreads[i])
        {
            convertData[i].bKillThread = true;
            SetEvent(convertData[i].hSignalConvert);

            OSTerminateThread(hConvertThreads[i], 10000);
            hConvertThreads[i] = NULL;
        }

        convertData[i].bKillThread = false;

        if(convertData[i].hSignalConvert)
        {
            CloseHandle(convertData[i].hSignalConvert);
            convertData[i].hSignalConvert = NULL;
        }

        if(convertData[i].hSignalComplete)
        {
            CloseHandle(convertData[i].hSignalComplete);
            convertData[i].hSignalComplete = NULL;
        }
    }

    if(bFiltersLoaded)
    {
        graph->RemoveFilter(captureFilter);
        graph->RemoveFilter(deviceFilter);
        graph->RemoveFilter(audioDeviceFilter);

        if(audioFilter)
            graph->RemoveFilter(audioFilter);

        SafeReleaseLogRef(captureFilter);
        SafeReleaseLogRef(deviceFilter);
        SafeReleaseLogRef(audioDeviceFilter);
        SafeReleaseLogRef(audioFilter);

        bFiltersLoaded = false;
    }

    if(audioOut)
    {
        API->RemoveAudioSource(audioOut);
        delete audioOut;
        audioOut = NULL;
    }

    if(colorConvertShader)
    {
        delete colorConvertShader;
        colorConvertShader = NULL;
    }

    if(lpImageBuffer)
    {
        Free(lpImageBuffer);
        lpImageBuffer = NULL;
    }

    SafeRelease(control);
}

void DeviceSource::Start()
{
    if(bCapturing || !control)
        return;

    HRESULT err;
    if(FAILED(err = control->Run()))
    {
        AppWarning(TEXT("DShowAudioPlugin: control->Run failed, result = %08lX"), err);
        return;
    }

    bCapturing = true;
}

void DeviceSource::Stop()
{
    if(!bCapturing)
        return;

    bCapturing = false;
    control->Stop();
    FlushSamples();
}

void DeviceSource::BeginScene()
{
    Start();
}

void DeviceSource::EndScene()
{
    Stop();
}

void DeviceSource::GlobalSourceLeaveScene()
{
    if(soundOutputType == 1) {
        audioOut->SetVolume(0.0f);
    }
    if(soundOutputType == 2) {
        IBasicAudio *basicAudio;
        if(SUCCEEDED(audioFilter->QueryInterface(IID_IBasicAudio, (void**)&basicAudio)))
        {
            long lVol = long((double(0.0)*NEAR_SILENTf)-NEAR_SILENTf);
            if(lVol <= -NEAR_SILENT)
                lVol = -10000;
            basicAudio->put_Volume(lVol);
            basicAudio->Release();
        }
    }
}

void DeviceSource::GlobalSourceEnterScene()
{
    float sourceVolume = data->GetFloat(TEXT("volume"), 1.0f);

    if(soundOutputType == 1) {
        audioOut->SetVolume(sourceVolume);
    }
    if(soundOutputType == 2) {
        IBasicAudio *basicAudio;
        if(SUCCEEDED(audioFilter->QueryInterface(IID_IBasicAudio, (void**)&basicAudio)))
        {
            long lVol = long((double(sourceVolume)*NEAR_SILENTf)-NEAR_SILENTf);
            if(lVol <= -NEAR_SILENT)
                lVol = -10000;
            basicAudio->put_Volume(lVol);
            basicAudio->Release();
        }
    }
}

void DeviceSource::ReceiveMediaSample(IMediaSample *sample, bool bAudio)
{
    if (!sample)
        return;

    if (bCapturing) {
        BYTE *pointer;

        if (SUCCEEDED(sample->GetPointer(&pointer))) {
            SampleData *data = NULL;
            
            if (bUseBuffering || !bAudio) {
                data = new SampleData;
                data->bAudio = bAudio;
                data->dataLength = sample->GetActualDataLength();
                data->lpData = (LPBYTE)Allocate(data->dataLength);//pointer; //
                /*data->sample = sample;
                sample->AddRef();*/

                memcpy(data->lpData, pointer, data->dataLength);

                LONGLONG stopTime;
                sample->GetTime(&stopTime, &data->timestamp);
            }

            //Log(TEXT("timestamp: %lld, bAudio - %s"), data->timestamp, bAudio ? TEXT("true") : TEXT("false"));

            OSEnterMutex(hSampleMutex);

            if (bUseBuffering) {
                UINT id = GetSampleInsertIndex(data->timestamp);
                samples.Insert(id, data);
            } else if (bAudio) {
                if (audioOut)
                    audioOut->ReceiveAudio(pointer, sample->GetActualDataLength());
            }

            OSLeaveMutex(hSampleMutex);
        }
    }
}

DWORD STDCALL PackPlanarThread(ConvertData *data)
{
    do {
        WaitForSingleObject(data->hSignalConvert, INFINITE);
        if(data->bKillThread) break;

        PackPlanar(data->output, data->input, data->width, data->height, data->pitch, data->startY, data->endY);
        data->sample->Release();

        SetEvent(data->hSignalComplete);
    } while(!data->bKillThread);

    return 0;
}

void DeviceSource::Preprocess()
{
    if(!bCapturing)
        return;

    //----------------------------------------

    if(bRequestVolume)
    {
        if(audioOut)
            audioOut->SetVolume(fNewVol);
        else if(audioFilter)
        {
            IBasicAudio *basicAudio;
            if(SUCCEEDED(audioFilter->QueryInterface(IID_IBasicAudio, (void**)&basicAudio)))
            {
                long lVol = long((double(fNewVol)*NEAR_SILENTf)-NEAR_SILENTf);
                if(lVol <= -NEAR_SILENT)
                    lVol = -10000;
                basicAudio->put_Volume(lVol);
                basicAudio->Release();
            }
        }
        bRequestVolume = false;
    }

    //----------------------------------------

    SampleData *lastSample = NULL;

    OSEnterMutex(hSampleMutex);

    lastSample = latestVideoSample;
    latestVideoSample = NULL;

    OSLeaveMutex(hSampleMutex);

    //----------------------------------------

    int numThreads = MAX(OSGetTotalCores()-2, 1);

    if(lastSample)
    {
        /*REFERENCE_TIME refTimeStart, refTimeFinish;
        lastSample->GetTime(&refTimeStart, &refTimeFinish);

        static REFERENCE_TIME lastRefTime = 0;
        //Log(TEXT("refTimeStart: %llu, refTimeFinish: %llu, offset = %llu"), refTimeStart, refTimeFinish, refTimeStart-lastRefTime);
        lastRefTime = refTimeStart;*/

        lastSample->Release();
    }
}

UINT DeviceSource::GetSampleInsertIndex(LONGLONG timestamp)
{
    UINT index;
    for (index=0; index<samples.Num(); index++) {
        if (samples[index]->timestamp > timestamp)
            return index;
    }

    return index;
}

void DeviceSource::Render(const Vect2 &pos, const Vect2 &size)
{
}

void DeviceSource::UpdateSettings()
{
    String strNewDevice     = data->GetString(TEXT("audioDevice"));
    UINT64 newFrameInterval = data->GetInt(TEXT("frameInterval"));
    UINT newCX              = data->GetInt(TEXT("resolutionWidth"));
    UINT newCY              = data->GetInt(TEXT("resolutionHeight"));
    BOOL bNewCustom         = data->GetInt(TEXT("customResolution"));
    UINT newPreferredType   = data->GetInt(TEXT("usePreferredType")) != 0 ? data->GetInt(TEXT("preferredType")) : -1;
    UINT newSoundOutputType     = data->GetInt(TEXT("soundOutputType"));

    if(renderCX != newCX || renderCY != newCY || frameInterval != newFrameInterval || newPreferredType != preferredOutputType ||
        !strAudioDevice.CompareI(strNewDevice) || bNewCustom != bUseCustomResolution ||
        newSoundOutputType != soundOutputType)
    {
        API->EnterSceneMutex();

        bool bWasCapturing = bCapturing;
        if(bWasCapturing) Stop();

        UnloadFilters();
        LoadFilters();

        if(bWasCapturing) Start();

        API->LeaveSceneMutex();
    }
}

void DeviceSource::SetInt(CTSTR lpName, int iVal)
{
    if(bCapturing)
    {
        if(scmpi(lpName, TEXT("timeOffset")) == 0)
        {
            if(audioOut)
                audioOut->SetAudioOffset(iVal);
        }
    }
}

void DeviceSource::SetFloat(CTSTR lpName, float fValue)
{
    if(!bCapturing)
        return;

    if(scmpi(lpName, TEXT("volume")) == 0)
    {
        fNewVol = fValue;
        bRequestVolume = true;
    }
}
