// Copyright (C) 2005-2012 Team MediaPortal
// http://www.team-mediaportal.com
// 
// MediaPortal is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
// 
// MediaPortal is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
// 
// You should have received a copy of the GNU General Public License
// along with MediaPortal. If not, see <http://www.gnu.org/licenses/>.

#include "stdafx.h"
#include "Globals.h"
#include "SampleRateConverterFilter.h"

#include "alloctracing.h"

extern unsigned int gAllowedSampleRates[7];

CSampleRateConverter::CSampleRateConverter(AudioRendererSettings *pSettings)
: m_bPassThrough(false),
  m_rtInSampleTime(0),
  m_pSettings(pSettings),
  m_pSrcState(NULL),
  m_dSampleRateRation(1.0)
{
}

CSampleRateConverter::~CSampleRateConverter(void)
{
}

HRESULT CSampleRateConverter::Init()
{
  HRESULT hr = InitAllocator();
  if (FAILED(hr))
    return hr;

  return CBaseAudioSink::Init();
}

HRESULT CSampleRateConverter::Cleanup()
{
  if (m_pSrcState)
    m_pSrcState = src_delete(m_pSrcState);

  return CBaseAudioSink::Cleanup();
}

HRESULT CSampleRateConverter::NegotiateFormat(const WAVEFORMATEXTENSIBLE* pwfx, int nApplyChangesDepth)
{
  if (!pwfx)
    return VFW_E_TYPE_NOT_ACCEPTED;

  if (FormatsEqual(pwfx, m_pInputFormat))
    return S_OK;

  if (!m_pNextSink)
    return VFW_E_TYPE_NOT_ACCEPTED;

  bool bApplyChanges = (nApplyChangesDepth != 0);
  if (nApplyChangesDepth != INFINITE && nApplyChangesDepth > 0)
    nApplyChangesDepth--;

  // try passthrough
  HRESULT hr = m_pNextSink->NegotiateFormat(pwfx, nApplyChangesDepth);
  if (SUCCEEDED(hr))
  {
    if (bApplyChanges)
    {
      m_bPassThrough = true;
      SetInputFormat(pwfx);
      SetOutputFormat(pwfx);
    }
    return hr;
  }

  if (pwfx->SubFormat != KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
    return VFW_E_TYPE_NOT_ACCEPTED;

  WAVEFORMATEXTENSIBLE* pOutWfx;
  CopyWaveFormatEx(&pOutWfx, pwfx);
  pOutWfx->Format.nSamplesPerSec = 0;

  hr = VFW_E_TYPE_NOT_ACCEPTED;

  const unsigned int sampleRateCount = sizeof(gAllowedSampleRates) / sizeof(int);
  unsigned int startPoint = 0;

  // TODO test duplicate sample rates first

  // Search for the input sample rate in sample rate array
  bool foundSampleRate = false;
  for (unsigned int i = 0; i < sampleRateCount && !foundSampleRate; i++)
  {
    if (gAllowedSampleRates[i] == pwfx->Format.nSamplesPerSec)
    {
      startPoint = ++i; // select closest sample rate in ascending order 
      foundSampleRate = true;
    }
  }

  if (!foundSampleRate)
    Log("CSampleRateConverter::NegotiateFormat - sample rate (%d) not found in the source array", pwfx->Format.nSamplesPerSec);
  
  unsigned int sampleRatesTested = 0;
  for (int i = startPoint; FAILED(hr) && pOutWfx->Format.nSamplesPerSec == 0 && sampleRatesTested < sampleRateCount; i++)
  {
    if (pOutWfx->Format.nSamplesPerSec == pwfx->Format.nSamplesPerSec)
    {
      sampleRatesTested++;
      continue; // skip if same as source
    }

    pOutWfx->Format.nSamplesPerSec = gAllowedSampleRates[i];
    pOutWfx->Format.nAvgBytesPerSec = gAllowedSampleRates[i] * pOutWfx->Format.nBlockAlign;

    hr = m_pNextSink->NegotiateFormat(pOutWfx, nApplyChangesDepth);
    sampleRatesTested++;

    if (FAILED(hr))
      pOutWfx->Format.nSamplesPerSec = 0;

    // Search from the lower end
    if (i == sampleRateCount - 1)
      i = 0;
  }

  if (FAILED(hr))
  {
    SAFE_DELETE_WAVEFORMATEX(pOutWfx);
    return hr;
  }
  if (bApplyChanges)
  {
    m_bPassThrough = false;
    SetInputFormat(pwfx);
    SetOutputFormat(pOutWfx, true);
    hr = SetupConversion();
    // TODO: do something meaningfull if SetupConversion fails
    //if (FAILED(hr))
  }
  else
    SAFE_DELETE_WAVEFORMATEX(pOutWfx);

  return S_OK;
}

// Processing
HRESULT CSampleRateConverter::PutSample(IMediaSample *pSample)
{
  if (!pSample)
    return S_OK;

  AM_MEDIA_TYPE *pmt = NULL;
  bool bFormatChanged = false;
  
  HRESULT hr = S_OK;

  if (SUCCEEDED(pSample->GetMediaType(&pmt)) && pmt != NULL)
    bFormatChanged = !FormatsEqual((WAVEFORMATEXTENSIBLE*)pmt->pbFormat, m_pInputFormat);

  if (pSample->IsDiscontinuity() == S_OK)
    m_bDiscontinuity = true;

  if (bFormatChanged)
  {
    // Process any remaining input
    if (!m_bPassThrough)
      hr = ProcessData(NULL, 0, NULL);
    // Apply format change locally, 
    // next filter will evaluate the format change when it receives the sample
    Log("CSampleRateConverter::PutSample: Processing format change");
    hr = NegotiateFormat((WAVEFORMATEXTENSIBLE*)pmt->pbFormat, 1);
    if (FAILED(hr))
    {
      DeleteMediaType(pmt);
      Log("SampleRateConverter: PutSample failed to change format: 0x%08x", hr);
      return hr;
    }
  }

  if (pmt)
    DeleteMediaType(pmt);

  if (m_bPassThrough)
  {
    if (m_pNextSink)
      return m_pNextSink->PutSample(pSample);
    return S_OK; // perhaps we should return S_FALSE to indicate sample was dropped
  }

  long nOffset = 0;
  long cbSampleData = pSample->GetActualDataLength();
  BYTE *pData = NULL;
  REFERENCE_TIME rtStop;
  pSample->GetTime(&m_rtInSampleTime, &rtStop);

  hr = pSample->GetPointer(&pData);
  ASSERT(pData != NULL);

  while (nOffset < cbSampleData && SUCCEEDED(hr))
  {
    long cbProcessed = 0;
    hr = ProcessData(pData+nOffset, cbSampleData - nOffset, &cbProcessed);
    nOffset += cbProcessed;
  }
  return hr;
}

HRESULT CSampleRateConverter::EndOfStream()
{
  if (!m_bPassThrough)
    ProcessData(NULL, 0, NULL);
  return CBaseAudioSink::EndOfStream();  
}

HRESULT CSampleRateConverter::OnInitAllocatorProperties(ALLOCATOR_PROPERTIES *properties)
{
  properties->cBuffers = 4;
  properties->cbBuffer = (0x1000);
  properties->cbPrefix = 0;
  properties->cbAlign = 8;

  return S_OK;
}

HRESULT CSampleRateConverter::SetupConversion()
{
  // Only floats
  m_nBitsPerSample = 32;

  m_nFrameSize = m_pInputFormat->Format.nBlockAlign;
  m_nBytesPerSample = m_pInputFormat->Format.wBitsPerSample / 8;

  m_dSampleRateRation = (double)m_pOutputFormat->Format.nSamplesPerSec / (double)m_pInputFormat->Format.nSamplesPerSec;

  if (m_pSrcState)
    m_pSrcState = src_delete(m_pSrcState);

  int error = 0;
  m_pSrcState = src_new(m_pSettings->m_nResamplingQuality, m_pInputFormat->Format.nChannels, &error);

  // TODO better error handling
  if (error != 0)
    return S_FALSE;

  return S_OK;
}

HRESULT CSampleRateConverter::ProcessData(const BYTE *pData, long cbData, long *pcbDataProcessed)
{
  HRESULT hr = S_OK;

  if (!pData) // need to flush any existing data
  {
    if (m_pNextOutSample)
      hr = OutputNextSample();

    if (pcbDataProcessed)
      *pcbDataProcessed = 0;

    int error = src_reset(m_pSrcState);
    if (error != 0)
      return S_FALSE;

    return hr;
  }

  long bytesOutput = 0;

  while (cbData)
  {
    if (m_pNextOutSample)
    {
      // If there is not enough space in output sample, flush it
      long nOffset = m_pNextOutSample->GetActualDataLength();
      long nSize = m_pNextOutSample->GetSize();

      if (nOffset + m_nFrameSize > nSize)
        hr = OutputNextSample();
    }

    // try to get an output buffer if none available
    if (!m_pNextOutSample && FAILED(hr = RequestNextOutBuffer(m_rtInSampleTime)))
    {
      if (pcbDataProcessed)
        *pcbDataProcessed = bytesOutput + cbData; // we can't realy process the data, lie about it!
      return hr;
    }

    long nOffset = m_pNextOutSample->GetActualDataLength();
    long nSize = m_pNextOutSample->GetSize();
    BYTE *pOutData = NULL;

    if (FAILED(hr = m_pNextOutSample->GetPointer(&pOutData)))
    {
      Log("CSampleRateConverter: Failed to get output buffer pointer: 0x%08x", hr);
      return hr;
    }
    ASSERT(pOutData);
    pOutData += nOffset;

    //int framesToConvert = min(cbData / m_nFrameSize, (nSize - nOffset) / (m_nFrameSize * m_dSampleRateRation));

    SRC_DATA data;

    data.data_in = (float*)pData;
    data.data_out = (float*)pOutData;
    data.input_frames = cbData / m_nFrameSize; //framesToConvert
    data.output_frames = (nSize - nOffset) / m_nFrameSize; //framesToConvert
    data.src_ratio = m_dSampleRateRation; //96000.0 / 48000.0;
    data.end_of_input = 0;

    int ret = src_process(m_pSrcState, &data);

    //Log("input_frames_used: %d output_frames_gen: %d", data.input_frames_used, data.output_frames_gen);

    pData += data.input_frames_used * m_nFrameSize;
    bytesOutput += data.output_frames_gen * m_nFrameSize;
    cbData -= data.input_frames_used * m_nFrameSize;
    nOffset += data.output_frames_gen * m_nFrameSize;

    m_pNextOutSample->SetActualDataLength(nOffset);
    if (nOffset + m_nFrameSize > nSize)
      OutputNextSample();

    m_rtInSampleTime += data.output_frames_gen * m_nFrameSize * UNITS / m_pOutputFormat->Format.nAvgBytesPerSec;
    
    // all samples should contain an integral number of frames
    ASSERT(cbData == 0 || cbData >= m_nFrameSize);
  }
  
  if (pcbDataProcessed)
    *pcbDataProcessed = bytesOutput;
  return hr;
}


