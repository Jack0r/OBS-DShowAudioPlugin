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
#include <ks.h>

//todo: 1700 line file.  this is another one of those abominations.
//fix it jim

extern "C" __declspec(dllexport) bool LoadPlugin();
extern "C" __declspec(dllexport) void UnloadPlugin();
extern "C" __declspec(dllexport) CTSTR GetPluginName();
extern "C" __declspec(dllexport) CTSTR GetPluginDescription();

LocaleStringLookup *pluginLocale = NULL;
HINSTANCE hinstMain = NULL;


#define DSHOW_CLASSNAME TEXT("AudioSourceDShowCapture")


bool SourceListHasDevice(CTSTR lpDevice, XElement *sourceList)
{
    UINT numSources = sourceList->NumElements();
    for(UINT i=0; i<numSources; i++)
    {
        XElement *sourceElement = sourceList->GetElementByID(i);
        if(scmpi(sourceElement->GetString(TEXT("class")), DSHOW_CLASSNAME) == 0)
        {
            XElement *data = sourceElement->GetElement(TEXT("data"));
            if(scmpi(data->GetString(TEXT("device")), lpDevice) == 0)
                return true;
            if(scmpi(data->GetString(TEXT("audioDevice")), lpDevice) == 0)
                return true;
        }
    }

    return false;
}

bool CurrentDeviceExists(CTSTR lpDevice, bool bGlobal, bool &isGlobal)
{
    isGlobal = false;

    XElement *globalSources = API->GetGlobalSourceListElement();
    if(globalSources)
    {
        if(SourceListHasDevice(lpDevice, globalSources))
        {
            isGlobal = true;
            return true;
        }
    }

    if(bGlobal)
    {
        XElement *sceneListElement = API->GetSceneListElement();
        if(sceneListElement)
        {
            UINT numScenes = sceneListElement->NumElements();
            for(UINT i=0; i<numScenes; i++)
            {
                XElement *sceneElement = sceneListElement->GetElementByID(i);
                if(sceneElement)
                {
                    XElement *sourceListElement = sceneElement->GetElement(TEXT("sources"));
                    if(sourceListElement)
                    {
                        if(SourceListHasDevice(lpDevice, sourceListElement))
                            return true;
                    }
                }
            }
        }
    }
    else
    {
        XElement *sceneElement = API->GetSceneElement();
        if(sceneElement)
        {
            XElement *sourceListElement = sceneElement->GetElement(TEXT("sources"));
            if(sourceListElement)
            {
                if(SourceListHasDevice(lpDevice, sourceListElement))
                    return true;
            }
        }
    }

    return false;
}

IBaseFilter* GetExceptionDevice(CTSTR lpGUID)
{
    String strGUID = lpGUID;
    if(strGUID.Length() != 38)
        return NULL;

    strGUID = strGUID.Mid(1, strGUID.Length()-1);

    StringList GUIDData;
    strGUID.GetTokenList(GUIDData, '-', FALSE);

    if (GUIDData.Num() != 5)
        return NULL;

    if (GUIDData[0].Length() != 8  ||
        GUIDData[1].Length() != 4  ||
        GUIDData[2].Length() != 4  ||
        GUIDData[3].Length() != 4  ||
        GUIDData[4].Length() != 12 )
    {
        return NULL;
    }

    GUID targetGUID;
    targetGUID.Data1 = (UINT)tstring_base_to_uint(GUIDData[0], NULL, 16);
    targetGUID.Data2 = (WORD)tstring_base_to_uint(GUIDData[1], NULL, 16);
    targetGUID.Data3 = (WORD)tstring_base_to_uint(GUIDData[2], NULL, 16);
    targetGUID.Data4[0] = (BYTE)tstring_base_to_uint(GUIDData[3].Left(2), NULL, 16);
    targetGUID.Data4[1] = (BYTE)tstring_base_to_uint(GUIDData[3].Right(2), NULL, 16);
    targetGUID.Data4[2] = (BYTE)tstring_base_to_uint(GUIDData[4].Left(2), NULL, 16);
    targetGUID.Data4[3] = (BYTE)tstring_base_to_uint(GUIDData[4].Mid(2, 4), NULL, 16);
    targetGUID.Data4[4] = (BYTE)tstring_base_to_uint(GUIDData[4].Mid(4, 6), NULL, 16);
    targetGUID.Data4[5] = (BYTE)tstring_base_to_uint(GUIDData[4].Mid(6, 8), NULL, 16);
    targetGUID.Data4[6] = (BYTE)tstring_base_to_uint(GUIDData[4].Mid(8, 10), NULL, 16);
    targetGUID.Data4[7] = (BYTE)tstring_base_to_uint(GUIDData[4].Right(2), NULL, 16);

    IBaseFilter *filter;
    if(SUCCEEDED(CoCreateInstance(targetGUID, NULL, CLSCTX_INPROC_SERVER, IID_IBaseFilter, (void**)&filter)))
        return filter;

    return NULL;
}

IBaseFilter* GetDeviceByValue(const IID &enumType, WSTR lpType, CTSTR lpName, WSTR lpType2, CTSTR lpName2)
{
    //---------------------------------
    // exception devices
    if(scmpi(lpType2, L"DevicePath") == 0 && lpName2 && *lpName2 == '{')
        return GetExceptionDevice(lpName2);

    //---------------------------------

    ICreateDevEnum *deviceEnum;
    IEnumMoniker *videoDeviceEnum;

    HRESULT err;
    err = CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC, IID_ICreateDevEnum, (void**)&deviceEnum);
    if(FAILED(err))
    {
        AppWarning(TEXT("GetDeviceByName: CoCreateInstance for the device enum failed, result = %08lX"), err);
        return NULL;
    }

    err = deviceEnum->CreateClassEnumerator(enumType, &videoDeviceEnum, 0);
    if(FAILED(err))
    {
        AppWarning(TEXT("GetDeviceByName: deviceEnum->CreateClassEnumerator failed, result = %08lX"), err);
        deviceEnum->Release();
        return NULL;
    }

    SafeRelease(deviceEnum);

    if(err == S_FALSE) //no devices, so NO ENUM FO U
        return NULL;

    //---------------------------------

    IBaseFilter *bestFilter = NULL;

    IMoniker *deviceInfo;
    DWORD count;
    while(videoDeviceEnum->Next(1, &deviceInfo, &count) == S_OK)
    {
        IPropertyBag *propertyData;
        err = deviceInfo->BindToStorage(0, 0, IID_IPropertyBag, (void**)&propertyData);
        if(SUCCEEDED(err))
        {
            VARIANT valueThingy;
            VARIANT valueThingy2;
            VariantInit(&valueThingy);
            VariantInit(&valueThingy2);
            /*valueThingy.vt  = VT_BSTR;
            valueThingy.pbstrVal = NULL;

            valueThingy2.vt = VT_BSTR;
            valueThingy2.bstrVal = NULL;*/

            if(SUCCEEDED(propertyData->Read(lpType, &valueThingy, NULL)))
            {
                if(lpType2 && lpName2)
                {
                    if(FAILED(propertyData->Read(lpType2, &valueThingy2, NULL)))
                        nop();
                }

                SafeRelease(propertyData);

                String strVal1 = (CWSTR)valueThingy.bstrVal;

                if(strVal1 == lpName)
                {
                    IBaseFilter *filter;
                    err = deviceInfo->BindToObject(NULL, 0, IID_IBaseFilter, (void**)&filter);
                    if(FAILED(err))
                    {
                        AppWarning(TEXT("GetDeviceByName: deviceInfo->BindToObject failed, result = %08lX"), err);
                        continue;
                    }

                    if(!bestFilter)
                    {
                        bestFilter = filter;

                        if(!lpType2 || !lpName2)
                        {
                            SafeRelease(deviceInfo);
                            SafeRelease(videoDeviceEnum);

                            return bestFilter;
                        }
                    }
                    else if(lpType2 && lpName2)
                    {
                        String strVal2 = (CWSTR)valueThingy2.bstrVal;
                        if(strVal2 == lpName2)
                        {
                            bestFilter->Release();

                            bestFilter = filter;

                            SafeRelease(deviceInfo);
                            SafeRelease(videoDeviceEnum);

                            return bestFilter;
                        }
                    }
                    else
                        filter->Release();
                }
            }
        }

        SafeRelease(deviceInfo);
    }

    SafeRelease(videoDeviceEnum);

    return bestFilter;
}


IPin* GetOutputPin(IBaseFilter *filter, const GUID *majorType)
{
    IPin *foundPin = NULL;
    IEnumPins *pins;

    if(!filter) return NULL;
    if(FAILED(filter->EnumPins(&pins))) return NULL;

    IPin *curPin;
    ULONG num;
    while(pins->Next(1, &curPin, &num) == S_OK)
    {
        if(majorType)
        {
            AM_MEDIA_TYPE *pinMediaType;

            IEnumMediaTypes *mediaTypesEnum;
            if(FAILED(curPin->EnumMediaTypes(&mediaTypesEnum)))
            {
                SafeRelease(curPin);
                continue;
            }

            ULONG curVal = 0;
            HRESULT hRes = mediaTypesEnum->Next(1, &pinMediaType, &curVal);

            mediaTypesEnum->Release();

            if(hRes != S_OK)
            {
                SafeRelease(curPin);
                continue;
            }

            BOOL bDesiredMediaType = (pinMediaType->majortype == *majorType);
            DeleteMediaType(pinMediaType);

            if(!bDesiredMediaType)
            {
                SafeRelease(curPin);
                continue;
            }
        }

        //------------------------------

        PIN_DIRECTION pinDir;
        if(SUCCEEDED(curPin->QueryDirection(&pinDir)))
        {
            if(pinDir == PINDIR_OUTPUT)
            {
                IKsPropertySet *propertySet;
                if(SUCCEEDED(curPin->QueryInterface(IID_IKsPropertySet, (void**)&propertySet)))
                {
                    GUID pinCategory;
                    DWORD retSize;

                    PIN_INFO chi;
                    curPin->QueryPinInfo(&chi);

                    if(chi.pFilter)
                        chi.pFilter->Release();

                    if(SUCCEEDED(propertySet->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, NULL, 0, &pinCategory, sizeof(GUID), &retSize)))
                    {
                        if(pinCategory == PIN_CATEGORY_CAPTURE)
                        {
                            SafeRelease(propertySet);
                            SafeRelease(pins);

                            return curPin;
                        }
                    }

                    SafeRelease(propertySet);
                }
            }
        }

        SafeRelease(curPin);
    }

    SafeRelease(pins);

    return foundPin;
}

void AddOutput(AM_MEDIA_TYPE *pMT, BYTE *capsData, bool bAllowV2, List<MediaOutputInfo> &outputInfoList)
{
    VideoOutputType type = GetVideoOutputType(*pMT);

    if(pMT->formattype == FORMAT_VideoInfo || (bAllowV2 && pMT->formattype == FORMAT_VideoInfo2))
    {
        VIDEO_STREAM_CONFIG_CAPS *pVSCC = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS*>(capsData);
        VIDEOINFOHEADER *pVih = reinterpret_cast<VIDEOINFOHEADER*>(pMT->pbFormat);
        BITMAPINFOHEADER *bmiHeader = GetVideoBMIHeader(pMT);

        bool bUsingFourCC = false;
        if(type == VideoOutputType_None)
        {
            type = GetVideoOutputTypeFromFourCC(bmiHeader->biCompression);
            bUsingFourCC = true;
        }

        if(type != VideoOutputType_None)
        {
            MediaOutputInfo *outputInfo = outputInfoList.CreateNew();

            if(pVSCC)
            {
                outputInfo->minFrameInterval = pVSCC->MinFrameInterval;
                outputInfo->maxFrameInterval = pVSCC->MaxFrameInterval;
                outputInfo->minCX = pVSCC->MinOutputSize.cx;
                outputInfo->maxCX = pVSCC->MaxOutputSize.cx;
                outputInfo->minCY = pVSCC->MinOutputSize.cy;
                outputInfo->maxCY = pVSCC->MaxOutputSize.cy;

                //actually due to the other code in GetResolutionFPSInfo, we can have this granularity
                // back to the way it was.  now, even if it's corrupted, it will always work
                outputInfo->xGranularity = max(pVSCC->OutputGranularityX, 1);
                outputInfo->yGranularity = max(pVSCC->OutputGranularityY, 1);
            }
            else
            {
                outputInfo->minCX = outputInfo->maxCX = bmiHeader->biWidth;
                outputInfo->minCY = outputInfo->maxCY = bmiHeader->biHeight;
                if(pVih->AvgTimePerFrame != 0)
                    outputInfo->minFrameInterval = outputInfo->maxFrameInterval = pVih->AvgTimePerFrame;
                else
                    outputInfo->minFrameInterval = outputInfo->maxFrameInterval = 10000000/30; //elgato hack

                outputInfo->xGranularity = outputInfo->yGranularity = 1;
            }

            outputInfo->mediaType = pMT;
            outputInfo->videoType = type;
            outputInfo->bUsingFourCC = bUsingFourCC;

            return;
        }
    }

    DeleteMediaType(pMT);
}

void GetOutputList(IPin *curPin, List<MediaOutputInfo> &outputInfoList)
{
    HRESULT hRes;

    IAMStreamConfig *config;
    if(SUCCEEDED(curPin->QueryInterface(IID_IAMStreamConfig, (void**)&config)))
    {
        int count, size;
        if(SUCCEEDED(hRes = config->GetNumberOfCapabilities(&count, &size)))
        {
            BYTE *capsData = (BYTE*)Allocate(size);

            int priority = -1;
            for(int i=0; i<count; i++)
            {
                AM_MEDIA_TYPE *pMT;
                if(SUCCEEDED(config->GetStreamCaps(i, &pMT, capsData)))
                    AddOutput(pMT, capsData, false, outputInfoList);
            }

            Free(capsData);
        }
        else if(hRes == E_NOTIMPL) //...usually elgato.
        {
            IEnumMediaTypes *mediaTypes;
            if(SUCCEEDED(curPin->EnumMediaTypes(&mediaTypes)))
            {
                ULONG i;

                AM_MEDIA_TYPE *pMT;
                if(mediaTypes->Next(1, &pMT, &i) == S_OK)
                    AddOutput(pMT, NULL, true, outputInfoList);

                mediaTypes->Release();
            }
        }

        SafeRelease(config);
    }
}


inline bool ResolutionListHasValue(const List<SIZE> &resolutions, SIZE &size)
{
    bool bHasResolution = false;

    for(UINT i=0; i<resolutions.Num(); i++)
    {
        SIZE &testSize = resolutions[i];
        if(size.cx == testSize.cx && size.cy == testSize.cy)
        {
            bHasResolution = true;
            break;
        }
    }

    return bHasResolution;
}


struct FPSInterval
{
    inline FPSInterval(UINT64 minVal, UINT64 maxVal) : minFrameInterval(minVal), maxFrameInterval(maxVal) {}
    UINT64 minFrameInterval, maxFrameInterval;
};

struct FPSInfo
{
    List<FPSInterval> supportedIntervals;
};

bool GetClosestResolution(List<MediaOutputInfo> &outputList, SIZE &resolution, UINT64 &frameInterval)
{
    LONG width, height;
    UINT64 internalFrameInterval = 10000000/UINT64(API->GetMaxFPS());
    API->GetBaseSize((UINT&)width, (UINT&)height);

    LONG bestDistance = 0x7FFFFFFF;
    SIZE bestSize;
    UINT64 maxFrameInterval = 0;
    UINT64 bestFrameInterval = 0xFFFFFFFFFFFFFFFFLL;

    for(UINT i=0; i<outputList.Num(); i++)
    {
        MediaOutputInfo &outputInfo = outputList[i];

        LONG outputWidth  = outputInfo.minCX;
        do
        {
            LONG distWidth = width-outputWidth;
            if(distWidth < 0)
                break;

            if(distWidth > bestDistance)
            {
                outputWidth  += outputInfo.xGranularity;
                continue;
            }

            LONG outputHeight = outputInfo.minCY;
            do
            {
                LONG distHeight = height-outputHeight;
                if(distHeight < 0)
                    break;

                LONG totalDist = distHeight+distWidth;
                if((totalDist <= bestDistance) || (totalDist == bestDistance && outputInfo.minFrameInterval < bestFrameInterval))
                {
                    bestDistance = totalDist;
                    bestSize.cx = outputWidth;
                    bestSize.cy = outputHeight;
                    maxFrameInterval = outputInfo.maxFrameInterval;
                    bestFrameInterval = outputInfo.minFrameInterval;
                }

                outputHeight += outputInfo.yGranularity;
            }while((UINT)outputHeight <= outputInfo.maxCY);

            outputWidth  += outputInfo.xGranularity;
        }while((UINT)outputWidth <= outputInfo.maxCX);
    }

    if(bestDistance != 0x7FFFFFFF)
    {
        resolution.cx = bestSize.cx;
        resolution.cy = bestSize.cy;

        if(internalFrameInterval > maxFrameInterval)
            frameInterval = maxFrameInterval;
        else if(internalFrameInterval < bestFrameInterval)
            frameInterval = bestFrameInterval;
        else
            frameInterval = internalFrameInterval;
        return true;
    }

    return false;
}

struct ConfigDialogData
{
    CTSTR lpName;
    XElement *data;
    List<MediaOutputInfo> outputList;
    List<SIZE> resolutions;
    StringList deviceNameList;
    StringList deviceIDList;
    StringList audioNameList;
    StringList audioIDList;
    StringList audioGUID;
    StringList crossbarList;
    StringList crossbarIDList;
    GUID listGUID[100];
    bool bGlobalSource;
    bool bCreating;
    bool bDShowHasAudio, bForceCustomAudioDevice, bHasAudio;

    ~ConfigDialogData()
    {
        ClearOutputList();
    }

    void ClearOutputList()
    {
        for(UINT i=0; i<outputList.Num(); i++)
            outputList[i].FreeData();
        outputList.Clear();
    }

    void GetResolutions(List<SIZE> &resolutions)
    {
        resolutions.Clear();

        for(UINT i=0; i<outputList.Num(); i++)
        {
            MediaOutputInfo &outputInfo = outputList[i];
            SIZE size;

            size.cx = outputInfo.minCX;
            size.cy = outputInfo.minCY;
            if(!ResolutionListHasValue(resolutions, size))
                resolutions << size;

            size.cx = outputInfo.maxCX;
            size.cy = outputInfo.maxCY;
            if(!ResolutionListHasValue(resolutions, size))
                resolutions << size;
        }

        //sort
        for(UINT i=0; i<resolutions.Num(); i++)
        {
            SIZE &rez = resolutions[i];

            for(UINT j=i+1; j<resolutions.Num(); j++)
            {
                SIZE &testRez = resolutions[j];

                if(testRez.cy < rez.cy)
                {
                    resolutions.SwapValues(i, j);
                    j = i;
                }
            }
        }
    }

    bool GetResolutionFPSInfo(SIZE &resolution, FPSInfo &fpsInfo)
    {
        fpsInfo.supportedIntervals.Clear();

        for(UINT i=0; i<outputList.Num(); i++)
        {
            MediaOutputInfo &outputInfo = outputList[i];

            if( UINT(resolution.cx) >= outputInfo.minCX && UINT(resolution.cx) <= outputInfo.maxCX &&
                UINT(resolution.cy) >= outputInfo.minCY && UINT(resolution.cy) <= outputInfo.maxCY )
            {
                if((resolution.cx-outputInfo.minCX) % outputInfo.xGranularity || (resolution.cy-outputInfo.minCY) % outputInfo.yGranularity)
                    return false;

                fpsInfo.supportedIntervals << FPSInterval(outputInfo.minFrameInterval, outputInfo.maxFrameInterval);
            }
        }

        return fpsInfo.supportedIntervals.Num() != 0;
    }
};


#define DEV_EXCEPTION_COUNT 1
CTSTR lpExceptionNames[DEV_EXCEPTION_COUNT] = {TEXT("Elgato Game Capture HD")};
CTSTR lpExceptionGUIDs[DEV_EXCEPTION_COUNT] = {TEXT("{39F50F4C-99E1-464a-B6F9-D605B4FB5918}")};

String guidToString(GUID guid) {
    return FormattedString(TEXT("{%08X-%04hX-%04hX-%02X%02X-%02X%02X%02X%02X%02X%02X}"), guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1], guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6], guid.Data4[7]);
}

void FillOutListOfDevices(HWND hwndCombo, GUID matchGUID, StringList *deviceList, StringList *deviceIDList, StringList *GUIDList, GUID *GUID_dump)
{
    //deviceIDList->Clear();
    //deviceList->Clear();
    //if(hwndCombo != NULL) SendMessage(hwndCombo, CB_RESETCONTENT, 0, 0);

    //------------------------------------------

    for(int i=0; i<DEV_EXCEPTION_COUNT; i++)
    {
        IBaseFilter *exceptionFilter = GetExceptionDevice(lpExceptionGUIDs[i]);
        if(exceptionFilter)
        {
            deviceList->Add(lpExceptionNames[i]);
            deviceIDList->Add(lpExceptionGUIDs[i]);

            if(hwndCombo != NULL) SendMessage(hwndCombo, CB_ADDSTRING, 0, (LPARAM)lpExceptionNames[i]);

            exceptionFilter->Release();
        }
    }

    //------------------------------------------

    ICreateDevEnum *deviceEnum;
    IEnumMoniker *videoDeviceEnum;

    HRESULT err;
    err = CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC, IID_ICreateDevEnum, (void**)&deviceEnum);
    if(FAILED(err))
    {
        AppWarning(TEXT("FillOutListDevices: CoCreateInstance for the device enum failed, result = %08lX"), err);
        return;
    }

    err = deviceEnum->CreateClassEnumerator(matchGUID, &videoDeviceEnum, 0);
    if(FAILED(err))
    {
        AppWarning(TEXT("FillOutListDevices: deviceEnum->CreateClassEnumerator failed, result = %08lX"), err);
        deviceEnum->Release();
        return;
    }

    SafeRelease(deviceEnum);

    if(err == S_FALSE) //no devices
        return;

    //------------------------------------------

    IMoniker *deviceInfo;
    DWORD count;

    while(videoDeviceEnum->Next(1, &deviceInfo, &count) == S_OK)
    {
        IPropertyBag *propertyData;
        err = deviceInfo->BindToStorage(0, 0, IID_IPropertyBag, (void**)&propertyData);
        if(SUCCEEDED(err))
        {
            VARIANT friendlyNameValue, devicePathValue;
            friendlyNameValue.vt = VT_BSTR;
            friendlyNameValue.bstrVal = NULL;
            devicePathValue.vt = VT_BSTR;
            devicePathValue.bstrVal = NULL;

            err = propertyData->Read(L"FriendlyName", &friendlyNameValue, NULL);
            propertyData->Read(L"DevicePath", &devicePathValue, NULL);

            if(SUCCEEDED(err))
            {
                IBaseFilter *filter;
                err = deviceInfo->BindToObject(NULL, 0, IID_IBaseFilter, (void**)&filter);
                if(SUCCEEDED(err))
                {
                    String strDeviceName = (CWSTR)friendlyNameValue.bstrVal;
                    deviceList->Add(strDeviceName);
                    if(GUIDList != NULL) {
                        if(matchGUID == CLSID_AudioInputDeviceCategory) GUIDList->Add("CLSID_AudioInputDeviceCategory");
                        else if(matchGUID == CLSID_VideoInputDeviceCategory) GUIDList->Add("CLSID_VideoInputDeviceCategory");
                        else if(matchGUID == CLSID_AudioRendererCategory) GUIDList->Add("CLSID_AudioRendererCategory");
                        else GUIDList->Add("Unknown device category");
                        GUID_dump[deviceList->Num()] = matchGUID;
                    }

                    UINT count = 0;
                    UINT id = INVALID;
                    while((id = deviceList->FindNextValueIndexI(strDeviceName, id)) != INVALID) count++;

                    if(count > 1)
                        strDeviceName << TEXT(" (") << UIntString(count) << TEXT(")");

                    String strDeviceID = (CWSTR)devicePathValue.bstrVal;
                    if(hwndCombo != NULL) SendMessage(hwndCombo, CB_ADDSTRING, 0, (LPARAM)strDeviceName.Array());
                    deviceIDList->Add(strDeviceID);

                    SafeRelease(filter);
                }
            }

            SafeRelease(propertyData);
        }

        SafeRelease(deviceInfo);
    }

    SafeRelease(videoDeviceEnum);
}


bool GetResolution(HWND hwndResolution, SIZE &resolution, BOOL bSelChange)
{
    String strResolution;
    if(bSelChange)
        strResolution = GetCBText(hwndResolution);
    else
        strResolution = GetEditText(hwndResolution);

    if(strResolution.NumTokens('x') != 2)
        return false;

    String strCX = strResolution.GetToken(0, 'x');
    String strCY = strResolution.GetToken(1, 'x');

    if(strCX.IsEmpty() || strCX.IsEmpty() || !ValidIntString(strCX) || !ValidIntString(strCY))
        return false;

    UINT cx = strCX.ToInt();
    UINT cy = strCY.ToInt();

    if(cx < 32 || cy < 32 || cx > 4096 || cy > 4096)
        return false;

    resolution.cx = cx;
    resolution.cy = cy;

    return true;
}

struct ColorSelectionData
{
    HDC hdcDesktop;
    HDC hdcDestination;
    HBITMAP hBitmap;
    bool bValid;

    inline ColorSelectionData() : hdcDesktop(NULL), hdcDestination(NULL), hBitmap(NULL), bValid(false) {}
    inline ~ColorSelectionData() {Clear();}

    inline bool Init()
    {
        hdcDesktop = GetDC(NULL);
        if(!hdcDesktop)
            return false;

        hdcDestination = CreateCompatibleDC(hdcDesktop);
        if(!hdcDestination)
            return false;

        hBitmap = CreateCompatibleBitmap(hdcDesktop, 1, 1);
        if(!hBitmap)
            return false;

        SelectObject(hdcDestination, hBitmap);
        bValid = true;

        return true;
    }

    inline void Clear()
    {
        if(hdcDesktop)
        {
            ReleaseDC(NULL, hdcDesktop);
            hdcDesktop = NULL;
        }

        if(hdcDestination)
        {
            DeleteDC(hdcDestination);
            hdcDestination = NULL;
        }

        if(hBitmap)
        {
            DeleteObject(hBitmap);
            hBitmap = NULL;
        }

        bValid = false;
    }

    inline DWORD GetColor()
    {
        POINT p;
        if(GetCursorPos(&p))
        {
            BITMAPINFO data;
            zero(&data, sizeof(data));

            data.bmiHeader.biSize = sizeof(data.bmiHeader);
            data.bmiHeader.biWidth = 1;
            data.bmiHeader.biHeight = 1;
            data.bmiHeader.biPlanes = 1;
            data.bmiHeader.biBitCount = 24;
            data.bmiHeader.biCompression = BI_RGB;
            data.bmiHeader.biSizeImage = 4;

            if(BitBlt(hdcDestination, 0, 0, 1, 1, hdcDesktop, p.x, p.y, SRCCOPY|CAPTUREBLT))
            {
                DWORD buffer;
                if(GetDIBits(hdcDestination, hBitmap, 0, 1, &buffer, &data, DIB_RGB_COLORS))
                    return 0xFF000000|buffer;
            }
            else
            {
                int err = GetLastError();
                nop();
            }
        }

        return 0xFF000000;
    }
};

void OpenPropertyPages(HWND hwnd, String devicename, String deviceid, GUID matchGUID) {
    IBaseFilter *filter = GetDeviceByValue(matchGUID,
                                            L"FriendlyName", devicename,
                                            L"DevicePath", deviceid);
    if(filter)
    {
        ISpecifyPropertyPages *propPages;
        CAUUID cauuid;

        if(SUCCEEDED(filter->QueryInterface(IID_ISpecifyPropertyPages, (void**)&propPages)))
        {
            if(SUCCEEDED(propPages->GetPages(&cauuid)))
            {
                if(cauuid.cElems)
                {
                    OleCreatePropertyFrame(hwnd, 0, 0, NULL, 1, (LPUNKNOWN*)&filter, cauuid.cElems, cauuid.pElems, 0, 0, NULL);
                    CoTaskMemFree(cauuid.pElems);
                }
            }
            propPages->Release();
        }

        filter->Release();
    }

    return;
}

INT_PTR CALLBACK ConfigureDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    static bool bSelectingColor = false;
    static bool bMouseDown = false;
    static ColorSelectionData colorData;

    switch(message)
    {
        case WM_INITDIALOG:
            {
                SetWindowLongPtr(hwnd, DWLP_USER, (LONG_PTR)lParam);

                ConfigDialogData *configData = (ConfigDialogData*)lParam;

                HWND hwndAudioList      = GetDlgItem(hwnd, IDC_AUDIOLIST);
                HWND hwndCrossbarlist   = GetDlgItem(hwnd, IDC_CROSSBARLIST);

                //------------------------------------------

                configData->bDShowHasAudio = configData->data->GetInt(TEXT("dshowHasAudio")) != 0;

                //------------------------------------------

                String strAudioDevice = configData->data->GetString(TEXT("audioDevice"));

                LocalizeWindow(hwnd, pluginLocale);
                SendMessage(GetDlgItem(hwnd, IDC_AUDIOLIST), CB_RESETCONTENT, 0, 0);
                configData->audioIDList.Clear();
                configData->audioNameList.Clear();
                FillOutListOfDevices(GetDlgItem(hwnd, IDC_AUDIOLIST), CLSID_AudioInputDeviceCategory, &configData->audioNameList, &configData->audioIDList, &configData->audioGUID, configData->listGUID);
                FillOutListOfDevices(GetDlgItem(hwnd, IDC_AUDIOLIST), CLSID_VideoInputDeviceCategory, &configData->audioNameList, &configData->audioIDList, &configData->audioGUID, configData->listGUID);
                FillOutListOfDevices(GetDlgItem(hwnd, IDC_AUDIOLIST), CLSID_AudioRendererCategory, &configData->audioNameList, &configData->audioIDList, &configData->audioGUID, configData->listGUID);

                //FillOutListOfDevices(GetDlgItem(hwnd, IDC_AUDIOLIST), KSCATEGORY_AUDIO, &configData->audioNameList, &configData->audioIDList, &configData->audioGUID, configData->listGUID);

                //FillOutListOfDevices(GetDlgItem(hwnd, IDC_CROSSBARLIST), AM_KSCATEGORY_CROSSBAR, &configData->crossbarList, &configData->crossbarIDList, NULL, NULL);

                //SendMessage(hwndCrossbarlist, CB_SETCURSEL, 0, 0);
                //ConfigureDialogProc(hwnd, WM_COMMAND, MAKEWPARAM(IDC_CROSSBARLIST, CBN_SELCHANGE), (LPARAM)hwndCrossbarlist);

                UINT audioDeviceID = CB_ERR;
                if(strAudioDevice.IsValid())
                    audioDeviceID = (UINT)SendMessage(hwndAudioList, CB_FINDSTRINGEXACT, -1, (LPARAM)strAudioDevice.Array());


                if(audioDeviceID == CB_ERR)
                {
                    SendMessage(hwndAudioList, CB_SETCURSEL, 0, 0);
                    ConfigureDialogProc(hwnd, WM_COMMAND, MAKEWPARAM(IDC_AUDIOLIST, CBN_SELCHANGE), (LPARAM)hwndAudioList);
                }
                else
                {
                    SendMessage(hwndAudioList, CB_SETCURSEL, audioDeviceID, 0);
                    ConfigureDialogProc(hwnd, WM_COMMAND, MAKEWPARAM(IDC_AUDIOLIST, CBN_SELCHANGE), (LPARAM)hwndAudioList);
                }

                //------------------------------------------

                HWND hwndTemp;

                int soundOutputType = configData->data->GetInt(TEXT("soundOutputType"));
                switch(soundOutputType)
                {
                    case 0: hwndTemp = GetDlgItem(hwnd, IDC_NOSOUND); break;
                    case 1: hwndTemp = GetDlgItem(hwnd, IDC_OUTPUTSOUND); break;
                    case 2: hwndTemp = GetDlgItem(hwnd, IDC_PLAYDESKTOPSOUND); break;
                }
                EnableWindow(GetDlgItem(hwnd, IDC_AUDIOLIST),        true);

                SendMessage(hwndTemp, BM_SETCHECK, BST_CHECKED, 0);

                EnableWindow(GetDlgItem(hwnd, IDC_TIMEOFFSET), soundOutputType == 1);
                EnableWindow(GetDlgItem(hwnd, IDC_TIMEOFFSET_EDIT), soundOutputType == 1);
                EnableWindow(GetDlgItem(hwnd, IDC_VOLUME), soundOutputType != 0);

                //------------------------------------------

                float fVol = configData->data->GetFloat(TEXT("volume"), 1.0f);
                SetVolumeControlValue(GetDlgItem(hwnd, IDC_VOLUME), fVol);

                //------------------------------------------

                int pos = configData->data->GetInt(TEXT("soundTimeOffset"));

                SendMessage(GetDlgItem(hwnd, IDC_TIMEOFFSET), UDM_SETRANGE32, -150, 3000);
                SendMessage(GetDlgItem(hwnd, IDC_TIMEOFFSET), UDM_SETPOS32, 0, pos);

                //------------------------------------------

                return TRUE;
            }

        case WM_DESTROY:
            break;

        case WM_LBUTTONDOWN:
            break;

        case WM_MOUSEMOVE:
            break;

        case WM_LBUTTONUP:
            break;

        case WM_CAPTURECHANGED:
            break;

        case WM_COMMAND:
            switch(LOWORD(wParam))
            {
                case IDC_NOSOUND:
                case IDC_PLAYDESKTOPSOUND:
                case IDC_OUTPUTSOUND:
                    if(HIWORD(wParam) == BN_CLICKED)
                    {
                        EnableWindow(GetDlgItem(hwnd, IDC_TIMEOFFSET),      LOWORD(wParam) == IDC_OUTPUTSOUND);
                        EnableWindow(GetDlgItem(hwnd, IDC_TIMEOFFSET_EDIT), LOWORD(wParam) == IDC_OUTPUTSOUND);
                        EnableWindow(GetDlgItem(hwnd, IDC_VOLUME),          LOWORD(wParam) != IDC_NOSOUND);
                    }
                    break;

                case IDC_VOLUME:
                    if(HIWORD(wParam) == VOLN_ADJUSTING || HIWORD(wParam) == VOLN_FINALVALUE)
                    {
                        if(IsWindowEnabled((HWND)lParam))
                        {
                            float fVol = GetVolumeControlValue((HWND)lParam);

                            ConfigDialogData *configData = (ConfigDialogData*)GetWindowLongPtr(hwnd, DWLP_USER);
                            ImageSource *source = API->GetSceneImageSource(configData->lpName);
                            if(source)
                                source->SetFloat(TEXT("volume"), fVol);
                        }
                    }
                    break;

                case IDC_TIMEOFFSET_EDIT:
                    if(HIWORD(wParam) == EN_CHANGE)
                    {
                        ConfigDialogData *configData = (ConfigDialogData*)GetWindowLongPtr(hwnd, DWLP_USER);
                        if(configData)
                        {
                            ImageSource *source = API->GetSceneImageSource(configData->lpName);

                            if(source)
                            {
                                HWND hwndVal = NULL;
                                switch(LOWORD(wParam))
                                {
                                    case IDC_TIMEOFFSET_EDIT:       hwndVal = GetDlgItem(hwnd, IDC_TIMEOFFSET); break;
                                }

                                int val = (int)SendMessage(hwndVal, UDM_GETPOS32, 0, 0);
                                switch(LOWORD(wParam))
                                {
                                    case IDC_TIMEOFFSET_EDIT:       source->SetInt(TEXT("timeOffset"), val); break;
                                }
                            }
                        }
                    }
                    break;

                case IDC_REFRESH:
                    {
                        HWND hwndAudioDeviceList = GetDlgItem(hwnd, IDC_AUDIOLIST);
                        HWND hwndCrossbarList = GetDlgItem(hwnd, IDC_CROSSBARLIST);

                        ConfigDialogData *configData = (ConfigDialogData*)GetWindowLongPtr(hwnd, DWLP_USER);

                        SendMessage(GetDlgItem(hwnd, IDC_AUDIOLIST), CB_RESETCONTENT, 0, 0);
                        configData->audioIDList.Clear();
                        configData->audioNameList.Clear();
                        FillOutListOfDevices(GetDlgItem(hwnd, IDC_AUDIOLIST), CLSID_AudioInputDeviceCategory, &configData->audioNameList, &configData->audioIDList, &configData->audioGUID, configData->listGUID);
                        FillOutListOfDevices(GetDlgItem(hwnd, IDC_AUDIOLIST), CLSID_VideoInputDeviceCategory, &configData->audioNameList, &configData->audioIDList, &configData->audioGUID, configData->listGUID);
                        FillOutListOfDevices(GetDlgItem(hwnd, IDC_AUDIOLIST), CLSID_AudioRendererCategory, &configData->audioNameList, &configData->audioIDList, &configData->audioGUID, configData->listGUID);
                        //FillOutListOfDevices(GetDlgItem(hwnd, IDC_CROSSBARLIST), AM_KSCATEGORY_CROSSBAR, &configData->crossbarList, &configData->crossbarIDList, NULL);

                        SendMessage(hwndAudioDeviceList, CB_SETCURSEL, 0, 0);
                        ConfigureDialogProc(hwnd, WM_COMMAND, MAKEWPARAM(IDC_AUDIOLIST, CBN_SELCHANGE), (LPARAM)hwndAudioDeviceList);
                        SendMessage(hwndCrossbarList, CB_SETCURSEL, 0, 0);
                        ConfigureDialogProc(hwnd, WM_COMMAND, MAKEWPARAM(IDC_CROSSBARLIST, CBN_SELCHANGE), (LPARAM)hwndCrossbarList);

                        break;
                    }

                case IDC_AUDIOLIST:
                    if(HIWORD(wParam) == CBN_SELCHANGE)
                    {
                        HWND hwndDevices = (HWND)lParam;
                        UINT id = (UINT)SendMessage(hwndDevices, CB_GETCURSEL, 0, 0);

                        ConfigDialogData *configData = (ConfigDialogData*)GetWindowLongPtr(hwnd, DWLP_USER);
                        IBaseFilter *filter = GetDeviceByValue(CLSID_AudioInputDeviceCategory,
                                                                L"FriendlyName", configData->audioNameList[id],
                                                                L"DevicePath", configData->audioIDList[id]);
                        if(filter)
                        {
                            //--------------------------------
                            // get audio info

                            bool bHasAudio = false;

                            IPin *outputPin = GetOutputPin(filter, &MEDIATYPE_Audio);
                            if(outputPin)
                            {
                                bHasAudio = true;
                                outputPin->Release();
                            }

                            EnableWindow(GetDlgItem(hwnd, IDC_NOSOUND),          bHasAudio);
                            EnableWindow(GetDlgItem(hwnd, IDC_PLAYDESKTOPSOUND), bHasAudio);
                            EnableWindow(GetDlgItem(hwnd, IDC_OUTPUTSOUND),      bHasAudio);
                            EnableWindow(GetDlgItem(hwnd, IDC_VOLUME),           bHasAudio);

                            if(!bHasAudio)
                            {
                                SendMessage(GetDlgItem(hwnd, IDC_NOSOUND),          BM_SETCHECK, BST_UNCHECKED, 0);
                                SendMessage(GetDlgItem(hwnd, IDC_PLAYDESKTOPSOUND), BM_SETCHECK, BST_UNCHECKED, 0);
                                SendMessage(GetDlgItem(hwnd, IDC_OUTPUTSOUND),      BM_SETCHECK, BST_UNCHECKED, 0);

                                EnableWindow(GetDlgItem(hwnd, IDC_TIMEOFFSET),      FALSE);
                                EnableWindow(GetDlgItem(hwnd, IDC_TIMEOFFSET_EDIT), FALSE);
                            }

                            filter->Release();
                        }

                            
                    }
                    break;

                    case IDC_CONFIGAUDIO:
                    case IDC_CROSSBAR:
                    {
                        UINT id;
                        ConfigDialogData *configData = (ConfigDialogData*)GetWindowLongPtr(hwnd, DWLP_USER);
                        switch(LOWORD(wParam))
                        {
                            case IDC_CONFIGAUDIO:
                                id = (UINT)SendMessage(GetDlgItem(hwnd, IDC_AUDIOLIST), CB_GETCURSEL, 0, 0);
                                if(id != CB_ERR) OpenPropertyPages(hwnd, configData->audioNameList[id], configData->audioIDList[id], configData->listGUID[id]);
                                break;
                            case IDC_CROSSBAR:
                                id = (UINT)SendMessage(GetDlgItem(hwnd, IDC_CROSSBARLIST), CB_GETCURSEL, 0, 0);
                                if(id != CB_ERR) OpenPropertyPages(hwnd, configData->crossbarList[id], configData->crossbarIDList[id], AM_KSCATEGORY_CROSSBAR);
                                break;
                        }
                        break;
                    }

                case IDOK:
                    {
                        UINT audioDeviceID= (UINT)SendMessage(GetDlgItem(hwnd, IDC_AUDIOLIST), CB_GETCURSEL, 0, 0);
                        if(audioDeviceID == CB_ERR)
                            break;

                        ConfigDialogData *configData = (ConfigDialogData*)GetWindowLongPtr(hwnd, DWLP_USER);

                        if(!configData->bDShowHasAudio) { audioDeviceID = (UINT)SendMessage(GetDlgItem(hwnd, IDC_AUDIOLIST), CB_GETCURSEL, 0, 0);
                            if(audioDeviceID == CB_ERR)
                                break;
                        }

                        String strAudioDevice = GetCBText(GetDlgItem(hwnd, IDC_AUDIOLIST), audioDeviceID);

                        if(configData->bCreating)
                        {
                            bool bFoundGlobal;
                            if(CurrentDeviceExists(strAudioDevice, configData->bGlobalSource, bFoundGlobal))
                            {
                                if(bFoundGlobal)
                                    MessageBox(hwnd, PluginStr("DeviceSelection.GlobalExists"), NULL, 0);
                                else
                                {
                                    if(configData->bGlobalSource)
                                        MessageBox(hwnd, PluginStr("DeviceSelection.ExistsSomewhere"), NULL, 0);
                                    else
                                        MessageBox(hwnd, PluginStr("DeviceSelection.ExistsInScene"), NULL, 0);
                                }

                                break;
                            }
                        }

                        //------------------------------------------

                        configData->data->SetString(TEXT("audioDevice"), strAudioDevice);
                        configData->data->SetString(TEXT("audioDeviceName"), configData->audioNameList[audioDeviceID]);
                        configData->data->SetString(TEXT("audioDeviceID"), configData->audioIDList[audioDeviceID]);
                        configData->data->SetString(TEXT("audioDeviceCLSID"), configData->audioGUID[audioDeviceID]);

                        configData->data->SetInt(TEXT("dshowHasAudio"), configData->bDShowHasAudio);

                        //------------------------------------------

                        int soundOutputType = 0;
                        if(SendMessage(GetDlgItem(hwnd, IDC_OUTPUTSOUND), BM_GETCHECK, 0, 0) == BST_CHECKED)
                            soundOutputType = 1;
                        else if(SendMessage(GetDlgItem(hwnd, IDC_PLAYDESKTOPSOUND), BM_GETCHECK, 0, 0) == BST_CHECKED)
                            soundOutputType = 2;
                        bool bForceCustomAudioDevice = SendMessage(GetDlgItem(hwnd, IDC_FORCECUSTOMAUDIO), BM_GETCHECK, 0, 0) == BST_CHECKED;

                        configData->data->SetInt(TEXT("soundOutputType"), soundOutputType);

                        int soundTimeOffset = (int)SendMessage(GetDlgItem(hwnd, IDC_TIMEOFFSET), UDM_GETPOS32, 0, 0);
                        configData->data->SetInt(TEXT("soundTimeOffset"), soundTimeOffset);

                        float fVol = GetVolumeControlValue(GetDlgItem(hwnd, IDC_VOLUME));
                        configData->data->SetFloat(TEXT("volume"), fVol);

                    }

                case IDCANCEL:
                    if(LOWORD(wParam) == IDCANCEL)
                    {
                        ConfigDialogData *configData = (ConfigDialogData*)GetWindowLongPtr(hwnd, DWLP_USER);
                        ImageSource *source = API->GetSceneImageSource(configData->lpName);

                        if(source)
                        {
                            source->SetInt(TEXT("timeOffset"),          configData->data->GetInt(TEXT("soundTimeOffset"), 0));
                            source->SetFloat(TEXT("volume"),            configData->data->GetFloat(TEXT("volume"), 1.0f));
                        }
                    }

                    EndDialog(hwnd, LOWORD(wParam));
            }
    }

    return FALSE;
}

bool STDCALL ConfigureDShowSource(XElement *element, bool bCreating)
{
    if(!element)
    {
        AppWarning(TEXT("ConfigureDShowSource: NULL element"));
        return false;
    }

    XElement *data = element->GetElement(TEXT("data"));
    if(!data)
        data = element->CreateElement(TEXT("data"));

    ConfigDialogData *configData = new ConfigDialogData;
    configData->lpName = element->GetName();
    configData->data = data;
    configData->bGlobalSource = (scmpi(element->GetParent()->GetName(), TEXT("global sources")) == 0);
    configData->bCreating = bCreating;

    if(DialogBoxParam(hinstMain, MAKEINTRESOURCE(IDD_CONFIG), API->GetMainWindow(), ConfigureDialogProc, (LPARAM)configData) == IDOK)
    {
        element->SetInt(TEXT("cx"), data->GetInt(TEXT("resolutionWidth")));
        element->SetInt(TEXT("cy"), data->GetInt(TEXT("resolutionHeight")));

        delete configData;
        return true;
    }

    delete configData;
    return false;
}

ImageSource* STDCALL CreateDShowSource(XElement *data)
{
    DeviceSource *source = new DeviceSource;
    if(!source->Init(data))
    {
        delete source;
        return NULL;
    }

    return source;
}


bool LoadPlugin()
{
    InitVolumeControl(hinstMain);
    InitVolumeMeter(hinstMain);

    pluginLocale = new LocaleStringLookup;

    if(!pluginLocale->LoadStringFile(TEXT("plugins/DShowAudioPlugin/locale/en.txt")))
        AppWarning(TEXT("Could not open locale string file '%s'"), TEXT("plugins/DShowAudioPlugin/locale/en.txt"));

    if(scmpi(API->GetLanguage(), TEXT("en")) != 0)
    {
        String pluginStringFile;
        pluginStringFile << TEXT("plugins/DShowAudioPlugin/locale/") << API->GetLanguage() << TEXT(".txt");
        if(!pluginLocale->LoadStringFile(pluginStringFile))
            AppWarning(TEXT("Could not open locale string file '%s'"), pluginStringFile.Array());
    }

    API->RegisterImageSourceClass(DSHOW_CLASSNAME, PluginStr("ClassName"), (OBSCREATEPROC)CreateDShowSource, (OBSCONFIGPROC)ConfigureDShowSource);

    return true;
}

void UnloadPlugin()
{
    delete pluginLocale;
}

CTSTR GetPluginName()
{
    return PluginStr("Plugin.Name");
}

CTSTR GetPluginDescription()
{
    return PluginStr("Plugin.Description");
}


BOOL CALLBACK DllMain(HINSTANCE hInst, DWORD dwReason, LPVOID lpBla)
{
    if(dwReason == DLL_PROCESS_ATTACH)
        hinstMain = hInst;

    return TRUE;
}

