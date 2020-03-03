/*
 *      Copyright (C) 2016-2016 peak3d
 *      http://www.peak3d.de
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  <http://www.gnu.org/licenses/>.
 *
 */

#include "main.h"

#include <iostream>
#include <stdio.h>
#include <string.h>
#include <sstream>

#include "libXBMC_addon.h"
#include "kodi_vfs_types.h"

#include "aes_decrypter.h"
#include "helpers.h"
#include "log.h"
#include "parser/DASHTree.h"
#include "parser/SmoothTree.h"
#include "parser/HLSTree.h"
#include "parser/TTML.h"
#include "TSReader.h"

#include "Ap4Utils.h"

#ifdef _WIN32                   // windows
#include "p8-platform/windows/dlfcn-win32.h"
#else // windows
#include <dlfcn.h>              // linux+osx
#endif

#define DVD_TIME_BASE 1000000
#define DVD_NOPTS_VALUE 0xFFF0000000000000ULL

#undef CreateDirectory

ADDON::CHelper_libXBMC_addon *xbmc = 0;

#define SAFE_DELETE(p)       do { delete (p);     (p)=NULL; } while (0)

void Log(const LogLevel loglevel, const char* format, ...)
{
  char buffer[16384];
  va_list args;
  va_start(args, format);
  vsprintf(buffer, format, args);
  va_end(args);
  xbmc->Log(static_cast<ADDON::addon_log_t>(loglevel), "%s", buffer);
}

static const AP4_Track::Type TIDC[adaptive::AdaptiveTree::STREAM_TYPE_COUNT] = {
  AP4_Track::TYPE_UNKNOWN,
  AP4_Track::TYPE_VIDEO,
  AP4_Track::TYPE_AUDIO,
  AP4_Track::TYPE_SUBTITLES };

/*******************************************************
kodi host - interface for decrypter libraries
********************************************************/
class KodiHost : public SSD::SSD_HOST
{
public:
  virtual const char *GetLibraryPath() const override
  {
    return m_strLibraryPath.c_str();
  };

  virtual const char *GetProfilePath() const override
  {
    return m_strProfilePath.c_str();
  };

  virtual void* CURLCreate(const char* strURL) override
  {
    return xbmc->CURLCreate(strURL);
  };

  virtual bool CURLAddOption(void* file, CURLOPTIONS opt, const char* name, const char * value)override
  {
    const XFILE::CURLOPTIONTYPE xbmcmap[] = { XFILE::CURL_OPTION_PROTOCOL, XFILE::CURL_OPTION_HEADER };
    return xbmc->CURLAddOption(file, xbmcmap[opt], name, value);
  }

  virtual bool CURLOpen(void* file)override
  {
    return xbmc->CURLOpen(file, XFILE::READ_NO_CACHE);
  };

  virtual size_t ReadFile(void* file, void* lpBuf, size_t uiBufSize)override
  {
    return xbmc->ReadFile(file, lpBuf, uiBufSize);
  };

  virtual void CloseFile(void* file)override
  {
    return xbmc->CloseFile(file);
  };

  virtual bool CreateDirectory(const char *dir)override
  {
    return xbmc->CreateDirectory(dir);
  };

  virtual void Log(LOGLEVEL level, const char *msg)override
  {
    const ADDON::addon_log_t xbmcmap[] = { ADDON::LOG_DEBUG, ADDON::LOG_INFO, ADDON::LOG_ERROR };
    return xbmc->Log(xbmcmap[level], msg);
  };

  void SetLibraryPath(const char *libraryPath)
  {
    m_strLibraryPath = libraryPath;

    const char *pathSep(libraryPath[0] && libraryPath[1] == ':' && isalpha(libraryPath[0]) ? "\\" : "/");

    if (m_strLibraryPath.size() && m_strLibraryPath.back() != pathSep[0])
      m_strLibraryPath += pathSep;
  }

  void SetProfilePath(const char *profilePath)
  {
    m_strProfilePath = profilePath;

    const char *pathSep(profilePath[0] && profilePath[1] == ':' && isalpha(profilePath[0]) ? "\\" : "/");

    if (m_strProfilePath.size() && m_strProfilePath.back() != pathSep[0])
      m_strProfilePath += pathSep;

    //let us make cdm userdata out of the addonpath and share them between addons
    m_strProfilePath.resize(m_strProfilePath.find_last_of(pathSep[0], m_strProfilePath.length() - 2));
    m_strProfilePath.resize(m_strProfilePath.find_last_of(pathSep[0], m_strProfilePath.length() - 1));
    m_strProfilePath.resize(m_strProfilePath.find_last_of(pathSep[0], m_strProfilePath.length() - 1) + 1);

    xbmc->CreateDirectory(m_strProfilePath.c_str());
    m_strProfilePath += "cdm";
    m_strProfilePath += pathSep;
    xbmc->CreateDirectory(m_strProfilePath.c_str());
  }

  virtual bool GetBuffer(void* instance, SSD::SSD_PICTURE &picture) override
  {
    return false;
  }

  virtual void ReleaseBuffer(void* instance, void *buffer) override
  {
  }

private:
  std::string m_strProfilePath, m_strLibraryPath;
}kodihost;

/*******************************************************
Bento4 Streams
********************************************************/

class AP4_DASHStream : public AP4_ByteStream
{
public:
  // Constructor
  AP4_DASHStream(adaptive::AdaptiveStream *stream) :stream_(stream){};

  // AP4_ByteStream methods
  AP4_Result ReadPartial(void*    buffer,
    AP4_Size  bytesToRead,
    AP4_Size& bytesRead) override
  {
    bytesRead = stream_->read(buffer, bytesToRead);
    return bytesRead > 0 ? AP4_SUCCESS : AP4_ERROR_READ_FAILED;
  };
  AP4_Result WritePartial(const void* buffer,
    AP4_Size    bytesToWrite,
    AP4_Size&   bytesWritten) override
  {
    /* unimplemented */
    return AP4_ERROR_NOT_SUPPORTED;
  };
  AP4_Result Seek(AP4_Position position) override
  {
    return stream_->seek(position) ? AP4_SUCCESS : AP4_ERROR_NOT_SUPPORTED;
  };
  AP4_Result Tell(AP4_Position& position) override
  {
    position = stream_->tell();
    return AP4_SUCCESS;
  };
  AP4_Result GetSize(AP4_LargeSize& size) override
  {
    /* unimplemented */
    return AP4_ERROR_NOT_SUPPORTED;
  };
  // AP4_Referenceable methods
  void AddReference() override {};
  void Release()override      {};
protected:
  // members
  adaptive::AdaptiveStream *stream_;
};

/*******************************************************
Kodi Streams implementation
********************************************************/

bool adaptive::AdaptiveTree::download(const char* url, const std::map<std::string, std::string> &manifestHeaders)
{
  // open the file
  void* file = xbmc->CURLCreate(url);
  if (!file)
    return false;
  xbmc->CURLAddOption(file, XFILE::CURL_OPTION_PROTOCOL, "seekable", "0");
  xbmc->CURLAddOption(file, XFILE::CURL_OPTION_PROTOCOL, "acceptencoding", "gzip");

  for (const auto &entry : manifestHeaders)
  {
    xbmc->CURLAddOption(file, XFILE::CURL_OPTION_HEADER, entry.first.c_str(), entry.second.c_str());
  }

  xbmc->CURLOpen(file, XFILE::READ_CHUNKED | XFILE::READ_NO_CACHE);

  // read the file
  static const unsigned int CHUNKSIZE = 16384;
  char buf[CHUNKSIZE];
  size_t nbRead;
  while ((nbRead = xbmc->ReadFile(file, buf, CHUNKSIZE)) > 0 && ~nbRead && write_data(buf, nbRead));
  xbmc->CloseFile(file);

  xbmc->Log(ADDON::LOG_DEBUG, "Download %s finished", url);

  return nbRead == 0;
}

bool KodiAdaptiveStream::download(const char* url, const std::map<std::string, std::string> &mediaHeaders)
{
  // open the file
  void* file = xbmc->CURLCreate(url);
  if (!file)
    return false;
  xbmc->CURLAddOption(file, XFILE::CURL_OPTION_PROTOCOL, "seekable", "0");
  xbmc->CURLAddOption(file, XFILE::CURL_OPTION_PROTOCOL, "acceptencoding", "gzip");
  xbmc->CURLAddOption(file, XFILE::CURL_OPTION_PROTOCOL, "Connection", "keep-alive");

  for (const auto &entry : mediaHeaders)
  {
    xbmc->CURLAddOption(file, XFILE::CURL_OPTION_HEADER, entry.first.c_str(), entry.second.c_str());
  }

  xbmc->CURLOpen(file, XFILE::READ_CHUNKED | XFILE::READ_NO_CACHE);

  // read the file
  char *buf = (char*)malloc(32*1024);
  size_t nbRead, nbReadOverall = 0;
  while ((nbRead = xbmc->ReadFile(file, buf, 32 * 1024)) > 0 && ~nbRead && write_data(buf, nbRead)) nbReadOverall+= nbRead;
  free(buf);

  if (!nbReadOverall)
  {
    xbmc->Log(ADDON::LOG_ERROR, "Download %s doesn't provide any data: invalid", url);
    return false;
  }

  double current_download_speed_ = xbmc->GetFileDownloadSpeed(file);
  //Calculate the new downloadspeed to 1MB
  static const size_t ref_packet = 1024 * 1024;
  if (nbReadOverall >= ref_packet)
    set_download_speed(current_download_speed_);
  else
  {
    double ratio = (double)nbReadOverall / ref_packet;
    set_download_speed((get_download_speed() * (1.0 - ratio)) + current_download_speed_*ratio);
  }

  xbmc->CloseFile(file);

  xbmc->Log(ADDON::LOG_DEBUG, "Download %s finished, average download speed: %0.4lf", url, get_download_speed());

  return nbRead == 0;
}

bool KodiAdaptiveStream::parseIndexRange()
{
  // open the file
  xbmc->Log(ADDON::LOG_DEBUG, "Downloading %s for SIDX generation", getRepresentation()->url_.c_str());

  // open the file
  void* file = xbmc->CURLCreate(getRepresentation()->url_.c_str());
  if (!file)
    return false;
  xbmc->CURLAddOption(file, XFILE::CURL_OPTION_PROTOCOL, "seekable", "0");

  char rangebuf[64];
  sprintf(rangebuf, "bytes=%u-%u", getRepresentation()->indexRangeMin_, getRepresentation()->indexRangeMax_);
  xbmc->CURLAddOption(file, XFILE::CURL_OPTION_HEADER, "Range", rangebuf);


  if (!xbmc->CURLOpen(file, XFILE::READ_CHUNKED | XFILE::READ_NO_CACHE | XFILE::READ_AUDIO_VIDEO))
  {
    xbmc->Log(ADDON::LOG_ERROR, "Download SIDX retrieval failed");
    return false;
  }

  // read the file into AP4_MemoryByteStream
  AP4_MemoryByteStream byteStream;

  char buf[16384];
  size_t nbRead, nbReadOverall = 0;
  while((nbRead = xbmc->ReadFile(file, buf, 16384)) > 0 && ~nbRead && AP4_SUCCEEDED(byteStream.Write(buf, nbRead))) nbReadOverall += nbRead;
  xbmc->CloseFile(file);

  if (nbReadOverall != getRepresentation()->indexRangeMax_ - getRepresentation()->indexRangeMin_ +1)
  {
    xbmc->Log(ADDON::LOG_ERROR, "Size of downloaded SIDX section differs from expected");
    return false;
  }
  byteStream.Seek(0);

  adaptive::AdaptiveTree::Representation *rep(const_cast<adaptive::AdaptiveTree::Representation*>(getRepresentation()));
  adaptive::AdaptiveTree::AdaptationSet *adp(const_cast<adaptive::AdaptiveTree::AdaptationSet*>(getAdaptationSet()));

  if (!getRepresentation()->indexRangeMin_)
  {
    AP4_File f(byteStream, AP4_DefaultAtomFactory::Instance, true);
    AP4_Movie* movie = f.GetMovie();
    if (movie == NULL)
    {
      xbmc->Log(ADDON::LOG_ERROR, "No MOOV in stream!");
      return false;
    }
    rep->flags_ |= adaptive::AdaptiveTree::Representation::INITIALIZATION;
    rep->initialization_.range_begin_ = 0;
    AP4_Position pos;
    byteStream.Tell(pos);
    rep->initialization_.range_end_ = pos - 1;
  }

  adaptive::AdaptiveTree::Segment seg;
  seg.startPTS_ = 0;
  unsigned int numSIDX(1);

  do
  {
    AP4_Atom *atom(NULL);
    if (AP4_FAILED(AP4_DefaultAtomFactory::Instance.CreateAtomFromStream(byteStream, atom)))
    {
      xbmc->Log(ADDON::LOG_ERROR, "Unable to create SIDX from IndexRange bytes");
      return false;
    }

    if (atom->GetType() == AP4_ATOM_TYPE_MOOF)
    {
      delete atom;
      break;
    }
    else if (atom->GetType() != AP4_ATOM_TYPE_SIDX)
    {
      delete atom;
      continue;
    }

    AP4_SidxAtom *sidx(AP4_DYNAMIC_CAST(AP4_SidxAtom, atom));
    const AP4_Array<AP4_SidxAtom::Reference> &refs(sidx->GetReferences());
    if (refs[0].m_ReferenceType == 1)
    {
      numSIDX = refs.ItemCount();
      delete atom;
      continue;
    }
    AP4_Position pos;
    byteStream.Tell(pos);
    seg.range_end_ = pos + getRepresentation()->indexRangeMin_ + sidx->GetFirstOffset() - 1;
    rep->timescale_ = sidx->GetTimeScale();
    rep->SetScaling();

    for (unsigned int i(0); i < refs.ItemCount(); ++i)
    {
      seg.range_begin_ = seg.range_end_ + 1;
      seg.range_end_ = seg.range_begin_ + refs[i].m_ReferencedSize - 1;
      rep->segments_.data.push_back(seg);
      if (adp->segment_durations_.data.size() < rep->segments_.data.size())
        adp->segment_durations_.data.push_back(refs[i].m_SubsegmentDuration);
      seg.startPTS_ += refs[i].m_SubsegmentDuration;
    }
    delete atom;
    --numSIDX;
  } while (numSIDX);
  return true;
}

/*******************************************************
|   CodecHandler
********************************************************/

class CodecHandler
{
public:
  CodecHandler(AP4_SampleDescription *sd)
    : sample_description(sd)
    , naluLengthSize(0)
    , pictureId(0)
    , pictureIdPrev(0xFF)
  {};
  virtual ~CodecHandler() {};

  virtual void UpdatePPSId(AP4_DataBuffer const&){};
  virtual bool GetInformation(INPUTSTREAM_INFO &info)
  {
    AP4_GenericAudioSampleDescription* asd(nullptr);
    if (sample_description && (asd = dynamic_cast<AP4_GenericAudioSampleDescription*>(sample_description)))
    {
      if (asd->GetChannelCount() != info.m_Channels
        || asd->GetSampleRate() != info.m_SampleRate
        || asd->GetSampleSize() != info.m_BitsPerSample)
      {
        info.m_Channels = asd->GetChannelCount();
        info.m_SampleRate = asd->GetSampleRate();
        info.m_BitsPerSample = asd->GetSampleSize();
        return true;
      }
    }
    return false;
  };
  virtual bool ExtraDataToAnnexB() { return false; };
  //virtual STREAMCODEC_PROFILE GetProfile() { return STREAMCODEC_PROFILE::CodecProfileNotNeeded; };
  virtual bool Transform(AP4_DataBuffer &buf, AP4_UI64 timescale, AP4_UI64 offSet) { return false; };
  virtual bool ReadNextSample(AP4_Sample &sample, AP4_DataBuffer &buf) { return false; };
  virtual bool TimeSeek(AP4_UI64 seekPos) { return true; };

  AP4_SampleDescription *sample_description;
  AP4_DataBuffer extra_data;
  AP4_UI08 naluLengthSize;
  AP4_UI08 pictureId, pictureIdPrev;
};

/***********************   AVC   ************************/

class AVCCodecHandler : public CodecHandler
{
public:
  AVCCodecHandler(AP4_SampleDescription *sd)
    : CodecHandler(sd)
    , countPictureSetIds(0)
    , needSliceInfo(false)
  {
    unsigned int width(0), height(0);
    if (AP4_VideoSampleDescription *video_sample_description = AP4_DYNAMIC_CAST(AP4_VideoSampleDescription, sample_description))
    {
      width = video_sample_description->GetWidth();
      height = video_sample_description->GetHeight();
    }
    if (AP4_AvcSampleDescription *avc = AP4_DYNAMIC_CAST(AP4_AvcSampleDescription, sample_description))
    {
      extra_data.SetData(avc->GetRawBytes().GetData(), avc->GetRawBytes().GetDataSize());
      countPictureSetIds = avc->GetPictureParameters().ItemCount();
      naluLengthSize = avc->GetNaluLengthSize();
      needSliceInfo = (countPictureSetIds > 1 || !width || !height);
      /*switch (avc->GetProfile())
      {
      case AP4_AVC_PROFILE_BASELINE:
        codecProfile = STREAMCODEC_PROFILE::H264CodecProfileBaseline;
        break;
      case AP4_AVC_PROFILE_MAIN:
        codecProfile = STREAMCODEC_PROFILE::H264CodecProfileMain;
        break;
      case AP4_AVC_PROFILE_EXTENDED:
        codecProfile = STREAMCODEC_PROFILE::H264CodecProfileExtended;
        break;
      case AP4_AVC_PROFILE_HIGH:
        codecProfile = STREAMCODEC_PROFILE::H264CodecProfileHigh;
        break;
      case AP4_AVC_PROFILE_HIGH_10:
        codecProfile = STREAMCODEC_PROFILE::H264CodecProfileHigh10;
        break;
      case AP4_AVC_PROFILE_HIGH_422:
        codecProfile = STREAMCODEC_PROFILE::H264CodecProfileHigh422;
        break;
      case AP4_AVC_PROFILE_HIGH_444:
        codecProfile = STREAMCODEC_PROFILE::H264CodecProfileHigh444Predictive;
        break;
      default:
        codecProfile = STREAMCODEC_PROFILE::CodecProfileUnknown;
        break;
      }*/
    }
  }

  virtual bool ExtraDataToAnnexB() override
  {
    if (AP4_AvcSampleDescription *avc = AP4_DYNAMIC_CAST(AP4_AvcSampleDescription, sample_description))
    {
      //calculate the size for annexb
      size_t sz(0);
      AP4_Array<AP4_DataBuffer>& pps(avc->GetPictureParameters());
      for (unsigned int i(0); i < pps.ItemCount(); ++i)
        sz += 4 + pps[i].GetDataSize();
      AP4_Array<AP4_DataBuffer>& sps(avc->GetSequenceParameters());
      for (unsigned int i(0); i < sps.ItemCount(); ++i)
        sz += 4 + sps[i].GetDataSize();

      extra_data.SetDataSize(sz);
      uint8_t *cursor(extra_data.UseData());

      for (unsigned int i(0); i < sps.ItemCount(); ++i)
      {
        cursor[0] = cursor[1] = cursor[2] = 0; cursor[3] = 1;
        memcpy(cursor + 4, sps[i].GetData(), sps[i].GetDataSize());
        cursor += sps[i].GetDataSize() + 4;
      }
      for (unsigned int i(0); i < pps.ItemCount(); ++i)
      {
        cursor[0] = cursor[1] = cursor[2] = 0; cursor[3] = 1;
        memcpy(cursor + 4, pps[i].GetData(), pps[i].GetDataSize());
        cursor += pps[i].GetDataSize() + 4;
      }
      return true;
    }
    return false;
  }

  virtual void UpdatePPSId(AP4_DataBuffer const &buffer) override
  {
    if (!needSliceInfo)
      return;

    //Search the Slice header NALU
    const AP4_UI08 *data(buffer.GetData());
    unsigned int data_size(buffer.GetDataSize());
    for (; data_size;)
    {
      // sanity check
      if (data_size < naluLengthSize)
        break;

      // get the next NAL unit
      AP4_UI32 nalu_size;
      switch (naluLengthSize) {
      case 1:nalu_size = *data++; data_size--; break;
      case 2:nalu_size = AP4_BytesToInt16BE(data); data += 2; data_size -= 2; break;
      case 4:nalu_size = AP4_BytesToInt32BE(data); data += 4; data_size -= 4; break;
      default: data_size = 0; nalu_size = 1; break;
      }
      if (nalu_size > data_size)
        break;

      // Stop further NALU processing
      if (countPictureSetIds < 2)
        needSliceInfo = false;

      unsigned int nal_unit_type = *data & 0x1F;

      if (
        //nal_unit_type == AP4_AVC_NAL_UNIT_TYPE_CODED_SLICE_OF_NON_IDR_PICTURE ||
        nal_unit_type == AP4_AVC_NAL_UNIT_TYPE_CODED_SLICE_OF_IDR_PICTURE //||
        //nal_unit_type == AP4_AVC_NAL_UNIT_TYPE_CODED_SLICE_DATA_PARTITION_A ||
        //nal_unit_type == AP4_AVC_NAL_UNIT_TYPE_CODED_SLICE_DATA_PARTITION_B ||
        //nal_unit_type == AP4_AVC_NAL_UNIT_TYPE_CODED_SLICE_DATA_PARTITION_C
      ) {

        AP4_DataBuffer unescaped(data, data_size);
        AP4_NalParser::Unescape(unescaped);
        AP4_BitReader bits(unescaped.GetData(), unescaped.GetDataSize());

        bits.SkipBits(8); // NAL Unit Type

        AP4_AvcFrameParser::ReadGolomb(bits); // first_mb_in_slice
        AP4_AvcFrameParser::ReadGolomb(bits); // slice_type
        pictureId = AP4_AvcFrameParser::ReadGolomb(bits); //picture_set_id
      }
      // move to the next NAL unit
      data += nalu_size;
      data_size -= nalu_size;
    }
  }

  virtual bool GetInformation(INPUTSTREAM_INFO &info) override
  {
    if (pictureId == pictureIdPrev)
      return false;
    pictureIdPrev = pictureId;

    if (AP4_AvcSampleDescription *avc = AP4_DYNAMIC_CAST(AP4_AvcSampleDescription, sample_description))
    {
      AP4_Array<AP4_DataBuffer>& ppsList(avc->GetPictureParameters());
      AP4_AvcPictureParameterSet pps;
      for (unsigned int i(0); i < ppsList.ItemCount(); ++i)
      {
        if (AP4_SUCCEEDED(AP4_AvcFrameParser::ParsePPS(ppsList[i].GetData(), ppsList[i].GetDataSize(), pps)) && pps.pic_parameter_set_id == pictureId)
        {
          AP4_Array<AP4_DataBuffer>& spsList = avc->GetSequenceParameters();
          AP4_AvcSequenceParameterSet sps;
          for (unsigned int i(0); i < spsList.ItemCount(); ++i)
          {
            if (AP4_SUCCEEDED(AP4_AvcFrameParser::ParseSPS(spsList[i].GetData(), spsList[i].GetDataSize(), sps)) && sps.seq_parameter_set_id == pps.seq_parameter_set_id)
            {
              bool ret = sps.GetInfo(info.m_Width, info.m_Height);
              ret = sps.GetVUIInfo(info.m_FpsRate, info.m_FpsScale, info.m_Aspect) || ret;
              return ret;
            }
          }
          break;
        }
      }
    }
    return false;
  };

  /*virtual STREAMCODEC_PROFILE GetProfile() override
  {
    return codecProfile;
  };*/
private:
  unsigned int countPictureSetIds;
  //STREAMCODEC_PROFILE codecProfile;
  bool needSliceInfo;
};

/***********************   HEVC   ************************/

class HEVCCodecHandler : public CodecHandler
{
public:
  HEVCCodecHandler(AP4_SampleDescription *sd)
    :CodecHandler(sd)
  {
    if (AP4_HevcSampleDescription *hevc = AP4_DYNAMIC_CAST(AP4_HevcSampleDescription, sample_description))
    {
      extra_data.SetData(hevc->GetRawBytes().GetData(), hevc->GetRawBytes().GetDataSize());
      naluLengthSize = hevc->GetNaluLengthSize();
    }
  }
};

/***********************   MPEG   ************************/

class MPEGCodecHandler : public CodecHandler
{
public:
  MPEGCodecHandler(AP4_SampleDescription *sd)
    :CodecHandler(sd)
  {
    if (AP4_MpegSampleDescription *aac = AP4_DYNAMIC_CAST(AP4_MpegSampleDescription, sample_description))
      extra_data.SetData(aac->GetDecoderInfo().GetData(), aac->GetDecoderInfo().GetDataSize());
  }

  virtual bool GetInformation(INPUTSTREAM_INFO &info) override
  {
    AP4_AudioSampleDescription *asd;
    if (sample_description && (asd = AP4_DYNAMIC_CAST(AP4_AudioSampleDescription, sample_description)))
    {
      if (asd->GetChannelCount() != info.m_Channels
        || asd->GetSampleRate() != info.m_SampleRate
        || asd->GetSampleSize() != info.m_BitsPerSample)
      {
        info.m_Channels = asd->GetChannelCount();
        info.m_SampleRate = asd->GetSampleRate();
        info.m_BitsPerSample = asd->GetSampleSize();
        return true;
      }
    }
    return false;
  }
};

/***********************   TTML   ************************/

class TTMLCodecHandler : public CodecHandler
{
public:
  TTMLCodecHandler(AP4_SampleDescription *sd)
    :CodecHandler(sd)
    ,ptsOffset(0)
  {};

  virtual bool Transform(AP4_DataBuffer &buf, AP4_UI64 timescale, AP4_UI64 offset) override
  {
    return m_ttml.Parse(buf.GetData(), buf.GetDataSize(), timescale, offset);
  }

  virtual bool ReadNextSample(AP4_Sample &sample, AP4_DataBuffer &buf) override
  {
    uint64_t pts;
    uint32_t dur;

    if (m_ttml.Prepare(pts, dur))
    {
      buf.SetData(static_cast<const AP4_Byte*>(m_ttml.GetData()), m_ttml.GetDataSize());
      sample.SetDts(pts);
      sample.SetCtsDelta(0);
      sample.SetDuration(dur);
      return true;
    }
    else
      buf.SetDataSize(0);
    return false;
  }

  virtual bool TimeSeek(AP4_UI64 seekPos) override
  {
    return m_ttml.TimeSeek(seekPos);
  };

private:
  TTML2SRT m_ttml;
  AP4_UI64 ptsOffset;
};

/*******************************************************
|   SampleReader
********************************************************/

class SampleReader
{
public:
  virtual ~SampleReader() = default;
  virtual bool EOS()const = 0;
  virtual uint64_t  DTS()const = 0;
  virtual uint64_t  PTS()const = 0;
  virtual uint64_t  Elapsed(uint64_t basePTS) = 0;
  virtual AP4_Result Start(bool &bStarted) = 0;
  virtual AP4_Result ReadSample() = 0;
  virtual void Reset(bool bEOS) = 0;
  virtual bool GetInformation(INPUTSTREAM_INFO &info) = 0;
  virtual bool TimeSeek(uint64_t pts, bool preceeding) = 0;
  virtual void SetPTSOffset(uint64_t offset) = 0;
  virtual bool GetNextFragmentInfo(uint64_t &ts, uint64_t &dur) = 0;
  virtual uint32_t GetTimeScale()const = 0;
  virtual AP4_UI32 GetStreamId()const = 0;
  virtual AP4_Size GetSampleDataSize()const = 0;
  virtual const AP4_Byte *GetSampleData()const = 0;
  virtual uint64_t GetDuration()const = 0;
  virtual bool IsEncrypted()const = 0;
  virtual void AddStreamType(INPUTSTREAM_INFO::STREAM_TYPE type, uint16_t sid) {};
  virtual void SetStreamType(INPUTSTREAM_INFO::STREAM_TYPE type, uint16_t sid) {};
  virtual bool RemoveStreamType(INPUTSTREAM_INFO::STREAM_TYPE type) { return true; };
};


/*******************************************************
|   FragmentedSampleReader
********************************************************/
class FragmentedSampleReader : public SampleReader, public AP4_LinearReader
{
public:

  FragmentedSampleReader(AP4_ByteStream *input, AP4_Movie *movie, AP4_Track *track, AP4_UI32 streamId,
    AP4_CencSingleSampleDecrypter *ssd, const SSD::SSD_DECRYPTER::SSD_CAPS &dcaps)
    : AP4_LinearReader(*movie, input)
    , m_track(track)
    , m_streamId(streamId)
    , m_sampleDescIndex(1)
    , m_bSampleDescChanged(false)
    , m_decrypterCaps(dcaps)
    , m_failCount(0)
    , m_eos(false)
    , m_started(false)
    , m_dts(0)
    , m_pts(0)
    , m_ptsDiff(0)
    , m_ptsOffs(~0ULL)
    , m_codecHandler(0)
    , m_defaultKey(0)
    , m_protectedDesc(0)
    , m_singleSampleDecryptor(ssd)
    , m_decrypter(0)
    , m_nextDuration(0)
    , m_nextTimestamp(0)
  {
    EnableTrack(m_track->GetId());

    AP4_SampleDescription *desc(m_track->GetSampleDescription(0));
    if (desc->GetType() == AP4_SampleDescription::TYPE_PROTECTED)
    {
      m_protectedDesc = static_cast<AP4_ProtectedSampleDescription*>(desc);

      AP4_ContainerAtom *schi;
      if (m_protectedDesc->GetSchemeInfo() && (schi = m_protectedDesc->GetSchemeInfo()->GetSchiAtom()))
      {
        AP4_TencAtom* tenc(AP4_DYNAMIC_CAST(AP4_TencAtom, schi->GetChild(AP4_ATOM_TYPE_TENC, 0)));
        if (tenc)
          m_defaultKey = tenc->GetDefaultKid();
        else
        {
          AP4_PiffTrackEncryptionAtom* piff(AP4_DYNAMIC_CAST(AP4_PiffTrackEncryptionAtom, schi->GetChild(AP4_UUID_PIFF_TRACK_ENCRYPTION_ATOM, 0)));
          if (piff)
            m_defaultKey = piff->GetDefaultKid();
        }
      }
    }
    if (m_singleSampleDecryptor)
      m_poolId = m_singleSampleDecryptor->AddPool();

    m_timeBaseExt = DVD_TIME_BASE;
    m_timeBaseInt = m_track->GetMediaTimeScale();

    while (m_timeBaseExt > 1)
      if ((m_timeBaseInt / 10) * 10 == m_timeBaseInt)
      {
        m_timeBaseExt /= 10;
        m_timeBaseInt /= 10;
      }
      else
        break;

    //We need this to fill extradata
    UpdateSampleDescription();
  }

  ~FragmentedSampleReader()
  {
    if (m_singleSampleDecryptor)
      m_singleSampleDecryptor->RemovePool(m_poolId);
    delete m_decrypter;
    delete m_codecHandler;
  }

  virtual AP4_Result Start(bool &bStarted) override
  {
    bStarted = false;
    if (m_started)
      return AP4_SUCCESS;
    m_started = true;
    bStarted = true;
    return ReadSample();
  }

  virtual AP4_Result ReadSample() override
  {
    AP4_Result result;
    if (!m_codecHandler || !m_codecHandler->ReadNextSample(m_sample, m_sampleData))
    {
      bool useDecryptingDecoder = m_protectedDesc && (m_decrypterCaps.flags & SSD::SSD_DECRYPTER::SSD_CAPS::SSD_SECURE_PATH) != 0;
      bool decrypterPresent(m_decrypter != nullptr);

      if (AP4_FAILED(result = ReadNextSample(m_track->GetId(), m_sample, (m_decrypter || useDecryptingDecoder) ? m_encrypted : m_sampleData)))
      {
        if (result == AP4_ERROR_EOS)
          m_eos = true;
        return result;
      }

      //AP4_AvcSequenceParameterSet sps;
      //AP4_AvcFrameParser::ParseFrameForSPS(m_sampleData.GetData(), m_sampleData.GetDataSize(), 4, sps);

      //Protection could have changed in ProcessMoof
      if (!decrypterPresent && m_decrypter != nullptr && !useDecryptingDecoder)
        m_encrypted.SetData(m_sampleData.GetData(), m_sampleData.GetDataSize());
      else if (decrypterPresent && m_decrypter == nullptr && !useDecryptingDecoder)
        m_sampleData.SetData(m_encrypted.GetData(), m_encrypted.GetDataSize());

      if (m_decrypter)
      {
        // Make sure that the decrypter is NOT allocating memory!
        // If decrypter and addon are compiled with different DEBUG / RELEASE
        // options freeing HEAP memory will fail.
        m_sampleData.Reserve(m_encrypted.GetDataSize() + 4096);
        if (AP4_FAILED(result = m_decrypter->DecryptSampleData(m_poolId, m_encrypted, m_sampleData, NULL)))
        {
          xbmc->Log(ADDON::LOG_ERROR, "Decrypt Sample returns failure!");
          if (++m_failCount > 50)
          {
            Reset(true);
            return result;
          }
          else
            m_sampleData.SetDataSize(0);
        }
        else
          m_failCount = 0;
      }
      else if (useDecryptingDecoder)
      {
        m_sampleData.Reserve(m_encrypted.GetDataSize() + 1024);
        m_singleSampleDecryptor->DecryptSampleData(m_poolId, m_encrypted, m_sampleData, nullptr, 0, nullptr, nullptr);
      }

      if (m_codecHandler->Transform(m_sampleData, m_track->GetMediaTimeScale(), (m_ptsOffs * m_timeBaseInt) / m_timeBaseExt))
        m_codecHandler->ReadNextSample(m_sample, m_sampleData);
    }

    m_dts = (m_sample.GetDts() * m_timeBaseExt) / m_timeBaseInt;
    m_pts = (m_sample.GetCts() * m_timeBaseExt) / m_timeBaseInt;

    if (~m_ptsOffs)
    {
      m_ptsDiff = m_pts - m_ptsOffs;
      m_ptsOffs = ~0ULL;
    }

    m_codecHandler->UpdatePPSId(m_sampleData);

    return AP4_SUCCESS;
  };

  virtual void Reset(bool bEOS) override
  {
    AP4_LinearReader::Reset();
    m_eos = bEOS;
  }

  virtual bool EOS() const  override { return m_eos; };
  virtual uint64_t DTS()const override { return m_dts; };
  virtual uint64_t  PTS()const override { return m_pts; };

  virtual uint64_t  Elapsed(uint64_t basePTS)
  {
    uint64_t manifestPTS = (m_pts > m_ptsDiff) ? m_pts - m_ptsDiff : 0;
    return manifestPTS > basePTS ? manifestPTS - basePTS : 0;
  };

  virtual AP4_UI32 GetStreamId()const override { return m_streamId; };
  virtual AP4_Size GetSampleDataSize()const override { return m_sampleData.GetDataSize(); };
  virtual const AP4_Byte *GetSampleData()const override { return m_sampleData.GetData(); };
  virtual uint64_t GetDuration()const override { return (m_sample.GetDuration() * m_timeBaseExt) / m_timeBaseInt; };
  virtual bool IsEncrypted()const override { return (m_decrypterCaps.flags & SSD::SSD_DECRYPTER::SSD_CAPS::SSD_SECURE_PATH) != 0 && m_decrypter != nullptr; };
  virtual bool GetInformation(INPUTSTREAM_INFO &info) override
  {
    if (!m_codecHandler)
      return false;

    bool edchanged(false);
    if (m_bSampleDescChanged && m_codecHandler->extra_data.GetDataSize()
      && (info.m_ExtraSize != m_codecHandler->extra_data.GetDataSize()
      || memcmp(info.m_ExtraData, m_codecHandler->extra_data.GetData(), info.m_ExtraSize)))
    {
      free((void*)(info.m_ExtraData));
      info.m_ExtraSize = m_codecHandler->extra_data.GetDataSize();
      info.m_ExtraData = (const uint8_t*)malloc(info.m_ExtraSize);
      memcpy((void*)info.m_ExtraData, m_codecHandler->extra_data.GetData(), info.m_ExtraSize);
      edchanged = true;
    }

    m_bSampleDescChanged = false;

    if (m_codecHandler->GetInformation(info))
      return true;

    return edchanged;
  }

  virtual bool TimeSeek(uint64_t  pts, bool preceeding) override
  {
    AP4_Ordinal sampleIndex;
    AP4_UI64 seekPos(static_cast<AP4_UI64>(((pts + m_ptsDiff) * m_timeBaseInt) / m_timeBaseExt));
    if (AP4_SUCCEEDED(SeekSample(m_track->GetId(), seekPos, sampleIndex, preceeding)))
    {
      if (m_decrypter)
        m_decrypter->SetSampleIndex(sampleIndex);
      if (m_codecHandler)
        m_codecHandler->TimeSeek(seekPos);
      m_started = true;
      return AP4_SUCCEEDED(ReadSample());
    }
    return false;
  };

  virtual void SetPTSOffset(uint64_t offset) override
  {
    FindTracker(m_track->GetId())->m_NextDts = (offset * m_timeBaseInt) / m_timeBaseExt;
    m_ptsOffs = offset;
  };

  virtual bool GetNextFragmentInfo(uint64_t &ts, uint64_t &dur) override
  {
    if (m_nextDuration)
    {
      dur = m_nextDuration;
      ts = m_nextTimestamp;
    }
    else
    {
      dur = dynamic_cast<AP4_FragmentSampleTable*>(FindTracker(m_track->GetId())->m_SampleTable)->GetDuration();
      ts = 0;
    }
    return true;
  };
  virtual uint32_t GetTimeScale()const override { return m_track->GetMediaTimeScale(); };

protected:
  virtual AP4_Result ProcessMoof(AP4_ContainerAtom* moof,
    AP4_Position       moof_offset,
    AP4_Position       mdat_payload_offset) override
  {
    AP4_Result result;

    if (AP4_SUCCEEDED((result = AP4_LinearReader::ProcessMoof(moof, moof_offset, mdat_payload_offset))))
    {
      AP4_ContainerAtom *traf = AP4_DYNAMIC_CAST(AP4_ContainerAtom, moof->GetChild(AP4_ATOM_TYPE_TRAF, 0));

      //For ISM Livestreams we have an UUID atom with one / more following fragment durations
      m_nextDuration = m_nextTimestamp = 0;
      AP4_Atom *atom;
      unsigned int atom_pos(0);
      const uint8_t uuid[16] = { 0xd4, 0x80, 0x7e, 0xf2, 0xca, 0x39, 0x46, 0x95, 0x8e, 0x54, 0x26, 0xcb, 0x9e, 0x46, 0xa7, 0x9f };
      while ((atom = traf->GetChild(AP4_ATOM_TYPE_UUID, atom_pos++))!= nullptr)
      {
        AP4_UuidAtom *uuid_atom(AP4_DYNAMIC_CAST(AP4_UuidAtom, atom));
        if (memcmp(uuid_atom->GetUuid(), uuid, 16) == 0)
        {
          //verison(8) + flags(24) + numpairs(8) + pairs(ts(64)/dur(64))*numpairs
          const AP4_DataBuffer &buf(AP4_DYNAMIC_CAST(AP4_UnknownUuidAtom, uuid_atom)->GetData());
          if (buf.GetDataSize() >= 21)
          {
            const uint8_t *data(buf.GetData());
            m_nextTimestamp = AP4_BytesToUInt64BE(data + 5);
            m_nextDuration = AP4_BytesToUInt64BE(data + 13);
          }
          break;
        }
      }

      //Check if the sample table description has changed
      AP4_TfhdAtom *tfhd = AP4_DYNAMIC_CAST(AP4_TfhdAtom, traf->GetChild(AP4_ATOM_TYPE_TFHD, 0));
      if ((tfhd && tfhd->GetSampleDescriptionIndex() != m_sampleDescIndex) || (!tfhd && (m_sampleDescIndex = 1)))
      {
        m_sampleDescIndex = tfhd->GetSampleDescriptionIndex();
        UpdateSampleDescription();
      }

      if (m_protectedDesc)
      {
        //Setup the decryption
        AP4_CencSampleInfoTable *sample_table;
        AP4_UI32 algorithm_id = 0;

        delete m_decrypter;
        m_decrypter = 0;

        AP4_ContainerAtom *traf = AP4_DYNAMIC_CAST(AP4_ContainerAtom, moof->GetChild(AP4_ATOM_TYPE_TRAF, 0));

        if (!m_protectedDesc || !traf)
          return AP4_ERROR_INVALID_FORMAT;

        if (AP4_FAILED(result = AP4_CencSampleInfoTable::Create(m_protectedDesc, traf, algorithm_id, *m_FragmentStream, moof_offset, sample_table)))
          // we assume unencrypted fragment here
          goto SUCCESS;

        if (AP4_FAILED(result = AP4_CencSampleDecrypter::Create(sample_table, algorithm_id, 0, 0, 0, m_singleSampleDecryptor, m_decrypter)))
          return result;
      }
    }
SUCCESS:
    if (m_singleSampleDecryptor && m_codecHandler)
      m_singleSampleDecryptor->SetFragmentInfo(m_poolId, m_defaultKey, m_codecHandler->naluLengthSize, m_codecHandler->extra_data, m_decrypterCaps.flags);

    return AP4_SUCCESS;
  }

private:

  void UpdateSampleDescription()
  {
    if (m_codecHandler)
      delete m_codecHandler;
    m_codecHandler = 0;
    m_bSampleDescChanged = true;

    AP4_SampleDescription *desc(m_track->GetSampleDescription(m_sampleDescIndex - 1));
    if (desc->GetType() == AP4_SampleDescription::TYPE_PROTECTED)
    {
      m_protectedDesc = static_cast<AP4_ProtectedSampleDescription*>(desc);
      desc = m_protectedDesc->GetOriginalSampleDescription();
    }
    switch (desc->GetFormat())
    {
    case AP4_SAMPLE_FORMAT_AVC1:
    case AP4_SAMPLE_FORMAT_AVC2:
    case AP4_SAMPLE_FORMAT_AVC3:
    case AP4_SAMPLE_FORMAT_AVC4:
      m_codecHandler = new AVCCodecHandler(desc);
      break;
    case AP4_SAMPLE_FORMAT_HEV1:
    case AP4_SAMPLE_FORMAT_HVC1:
      m_codecHandler = new HEVCCodecHandler(desc);
      break;
    case AP4_SAMPLE_FORMAT_MP4A:
      m_codecHandler = new MPEGCodecHandler(desc);
      break;
    case AP4_SAMPLE_FORMAT_STPP:
      m_codecHandler = new TTMLCodecHandler(desc);
      break;
    default:
      m_codecHandler = new CodecHandler(desc);
      break;
    }

    if ((m_decrypterCaps.flags & SSD::SSD_DECRYPTER::SSD_CAPS::SSD_ANNEXB_REQUIRED) != 0)
      m_codecHandler->ExtraDataToAnnexB();
  }

private:
  AP4_Track *m_track;
  AP4_UI32 m_streamId;
  AP4_UI32 m_sampleDescIndex;
  bool m_bSampleDescChanged;
  SSD::SSD_DECRYPTER::SSD_CAPS m_decrypterCaps;
  unsigned int m_failCount;
  AP4_UI32 m_poolId;

  bool m_eos, m_started;
  int64_t m_dts, m_pts, m_ptsDiff;
  AP4_UI64 m_ptsOffs;

  uint64_t m_timeBaseExt, m_timeBaseInt;

  AP4_Sample     m_sample;
  AP4_DataBuffer m_encrypted, m_sampleData;

  CodecHandler *m_codecHandler;
  const AP4_UI08 *m_defaultKey;

  AP4_ProtectedSampleDescription *m_protectedDesc;
  AP4_CencSingleSampleDecrypter *m_singleSampleDecryptor;
  AP4_CencSampleDecrypter *m_decrypter;
  uint64_t m_nextDuration, m_nextTimestamp;
};

/*******************************************************
|   SubtitleSampleReader
********************************************************/

class SubtitleSampleReader : public SampleReader
{
public:
  SubtitleSampleReader(const std::string &url, AP4_UI32 streamId)
    : m_pts(0)
    , m_streamId(streamId)
    , m_eos(false)
    , m_codecHandler(nullptr)
  {
    // open the file
    void* file = xbmc->CURLCreate(url.c_str());
    if (!file)
      return;
    xbmc->CURLAddOption(file, XFILE::CURL_OPTION_PROTOCOL, "seekable", "0");
    xbmc->CURLAddOption(file, XFILE::CURL_OPTION_PROTOCOL, "acceptencoding", "gzip");

    xbmc->CURLOpen(file, 0);

    AP4_DataBuffer result;

    // read the file
    static const unsigned int CHUNKSIZE = 16384;
    AP4_Byte buf[CHUNKSIZE];
    size_t nbRead;
    while ((nbRead = xbmc->ReadFile(file, buf, CHUNKSIZE)) > 0 && ~nbRead)
      result.AppendData(buf, nbRead);
    xbmc->CloseFile(file);

    m_codecHandler.Transform(result, 1000, 0);
  };

  virtual bool EOS()const override { return m_eos; };
  virtual uint64_t DTS()const override { return m_pts; };
  virtual uint64_t PTS()const override { return m_pts; };
  virtual uint64_t  Elapsed(uint64_t basePTS) { return m_pts > basePTS ? m_pts - basePTS : 0; };
  virtual AP4_Result Start(bool &bStarted) override { m_eos = false; return AP4_SUCCESS; };
  virtual AP4_Result ReadSample() override
  {
    if (m_codecHandler.ReadNextSample(m_sample, m_sampleData))
    {
      m_pts = m_sample.GetCts() * 1000;
      return AP4_SUCCESS;
    }
    m_eos = true;
    return AP4_ERROR_EOS;
  }
  virtual void Reset(bool bEOS) override {};
  virtual bool GetInformation(INPUTSTREAM_INFO &info) override { return false; };
  virtual bool TimeSeek(uint64_t  pts, bool preceeding) override
  {
    if (m_codecHandler.TimeSeek(pts / 1000))
      return AP4_SUCCEEDED(ReadSample());
    return false;
  };
  virtual void SetPTSOffset(uint64_t offset) override {};
  virtual bool GetNextFragmentInfo(uint64_t &ts, uint64_t &dur) override { return false; };
  virtual uint32_t GetTimeScale()const override { return 1000; };
  virtual AP4_UI32 GetStreamId()const override { return m_streamId; };
  virtual AP4_Size GetSampleDataSize()const override { return m_sampleData.GetDataSize(); };
  virtual const AP4_Byte *GetSampleData()const override { return m_sampleData.GetData(); };
  virtual uint64_t GetDuration()const override { return m_sample.GetDuration() * 1000; };
  virtual bool IsEncrypted()const override { return false; };
private:
  uint64_t m_pts;
  AP4_UI32 m_streamId;
  bool m_eos;

  TTMLCodecHandler m_codecHandler;

  AP4_Sample m_sample;
  AP4_DataBuffer m_sampleData;
};

/*******************************************************
|   TSSampleReader
********************************************************/
class TSSampleReader : public SampleReader, public TSReader
{
public:
  TSSampleReader(AP4_ByteStream *input, INPUTSTREAM_INFO::STREAM_TYPE type, AP4_UI32 streamId, uint32_t requiredMask)
    : TSReader(input, requiredMask)
    , m_typeMask(1 << type)
  {
    m_typeMap[type] = streamId;
  };

  virtual void AddStreamType(INPUTSTREAM_INFO::STREAM_TYPE type, uint16_t sid) override
  {
    m_typeMap[type] = sid;
    m_typeMask |= (1 << type);
    if (m_started)
      StartStreaming(m_typeMask);
  };

  virtual void SetStreamType(INPUTSTREAM_INFO::STREAM_TYPE type, uint16_t sid) override
  {
    m_typeMap[type] = sid;
    m_typeMask = (1 << type);
  };

  virtual bool RemoveStreamType(INPUTSTREAM_INFO::STREAM_TYPE type) override
  {
    m_typeMask &= ~(1 << type);
    StartStreaming(m_typeMask);
    return m_typeMask == 0;
  };

  virtual bool EOS()const override { return m_eos; }
  virtual uint64_t DTS()const override { return m_dts; }
  virtual uint64_t PTS()const override { return m_pts; }
  virtual uint64_t  Elapsed(uint64_t basePTS)
  {
    // TSReader::GetPTSDiff() is the difference between playlist PTS and real PTS relative to current segment
    uint64_t playlistPTS = (static_cast<int64_t>(m_pts) > m_ptsDiff) ? m_pts - m_ptsDiff : 0;
    return playlistPTS > basePTS ? playlistPTS - basePTS : 0;
  };

  virtual AP4_Result Start(bool &bStarted) override
  {
    bStarted = false;
    if (m_started)
      return AP4_SUCCESS;

    if (!StartStreaming(m_typeMask))
    {
      m_eos = true;
      return AP4_ERROR_CANNOT_OPEN_FILE;
    }

    m_started = bStarted = true;
    return ReadSample();
  }

  virtual AP4_Result ReadSample() override
  {
    if (ReadPacket())
    {
      m_dts = (GetDts() == PTS_UNSET) ? DVD_NOPTS_VALUE : (GetDts() * 100) / 9;
      m_pts = (GetPts() == PTS_UNSET) ? DVD_NOPTS_VALUE : (GetPts() * 100) / 9;

      if (~m_ptsOffs)
      {
        m_ptsDiff = m_pts - m_ptsOffs;
        m_ptsOffs = ~0ULL;
      }
      return AP4_SUCCESS;
    }
    m_eos = true;
    return AP4_ERROR_EOS;
  }

  virtual void Reset(bool bEOS) override
  {
    TSReader::Reset();
    m_eos = bEOS;
  }

  virtual bool GetInformation(INPUTSTREAM_INFO &info) override
  {
    return TSReader::GetInformation(info);
  }

  virtual bool TimeSeek(uint64_t pts, bool preceeding) override
  {
    if (!StartStreaming(m_typeMask))
      return false;

    AP4_UI64 seekPos(((pts + m_ptsDiff ) * 9) / 100);
    if (TSReader::SeekTime(seekPos, preceeding))
    {
      m_started = true;
      return AP4_SUCCEEDED(ReadSample());
    }
    return AP4_ERROR_EOS;
  }

  virtual void SetPTSOffset(uint64_t offset) override
  {
    m_ptsOffs = offset;
  }

  virtual bool GetNextFragmentInfo(uint64_t &ts, uint64_t &dur) override { return false; }
  virtual uint32_t GetTimeScale()const override { return 90000; }
  virtual AP4_UI32 GetStreamId()const override { return m_typeMap[GetStreamType()]; }
  virtual AP4_Size GetSampleDataSize()const override { return GetPacketSize(); }
  virtual const AP4_Byte *GetSampleData()const override { return GetPacketData(); }
  virtual uint64_t GetDuration()const override { return (TSReader::GetDuration() * 100) / 9; }
  virtual bool IsEncrypted()const override { return false; };

private:
  uint32_t m_typeMask; //Bit representation of INPUTSTREAM_INFO::STREAM_TYPES
  uint16_t m_typeMap[16];
  bool m_eos = false;
  bool m_started = false;

  uint64_t m_pts = 0;
  uint64_t m_dts = 0;
  int64_t m_ptsDiff = 0;
  uint64_t m_ptsOffs = ~0ULL;
};

/*******************************************************
Main class Session
********************************************************/

void Session::STREAM::disable()
{
  if (enabled)
  {
    stream_.stop();
    SAFE_DELETE(reader_);
    SAFE_DELETE(input_file_);
    SAFE_DELETE(input_);
    enabled = encrypted = false;
    mainId_ = 0;
  }
}

Session::Session(MANIFEST_TYPE manifestType, const char *strURL, const char *strUpdateParam, const char *strLicType, const char* strLicKey, const char* strLicData, const char* strCert,
  const std::map<std::string, std::string> &manifestHeaders, const std::map<std::string, std::string> &mediaHeaders, const char* profile_path, uint16_t display_width, uint16_t display_height)
  : manifest_type_(manifestType)
  , mpdFileURL_(strURL)
  , mpdUpdateParam_(strUpdateParam)
  , license_key_(strLicKey)
  , license_type_(strLicType)
  , license_data_(strLicData)
  , media_headers_(mediaHeaders)
  , profile_path_(profile_path)
  , decrypterModule_(0)
  , decrypter_(0)
  , secure_video_session_(false)
  , adaptiveTree_(0)
  , width_(display_width)
  , height_(display_height)
  , changed_(false)
  , manual_streams_(false)
  , elapsed_time_(0)
{
  switch (manifest_type_)
  {
  case MANIFEST_TYPE_MPD:
    adaptiveTree_ = new adaptive::DASHTree;
    break;
  case MANIFEST_TYPE_ISM:
    adaptiveTree_ = new adaptive::SmoothTree;
    break;
  case MANIFEST_TYPE_HLS:
    adaptiveTree_ = new adaptive::HLSTree(new AESDecrypter(license_key_));
    break;
  default:;
  };

  std::string fn(profile_path_ + "bandwidth.bin");
  FILE* f = fopen(fn.c_str(), "rb");
  if (f)
  {
    double val;
    size_t sz(fread(&val, sizeof(double), 1, f));
    if (sz)
    {
      adaptiveTree_->bandwidth_ = static_cast<uint32_t>(val * 8);
      adaptiveTree_->set_download_speed(val);
    }
    fclose(f);
  }
  else
    adaptiveTree_->bandwidth_ = 4000000;
  xbmc->Log(ADDON::LOG_DEBUG, "Initial bandwidth: %u ", adaptiveTree_->bandwidth_);

  int buf;
  xbmc->GetSetting("MAXRESOLUTION", (char*)&buf), max_resolution_ = buf;
  xbmc->Log(ADDON::LOG_DEBUG, "MAXRESOLUTION selected: %d ", max_resolution_);

  xbmc->GetSetting("MAXRESOLUTIONSECURE", (char*)&buf), max_secure_resolution_ = buf;
  xbmc->Log(ADDON::LOG_DEBUG, "MAXRESOLUTIONSECURE selected: %d ", max_secure_resolution_);

  xbmc->GetSetting("STREAMSELECTION", (char*)&buf);
  xbmc->Log(ADDON::LOG_DEBUG, "STREAMSELECTION selected: %d ", buf);
  manual_streams_ = buf != 0;

  xbmc->GetSetting("MEDIATYPE", (char*)&buf);
  switch (buf)
  {
  case 1:
    media_type_mask_ = static_cast<uint8_t>(1U) << adaptive::AdaptiveTree::AUDIO;
    break;
  case 2:
    media_type_mask_ = static_cast<uint8_t>(1U) << adaptive::AdaptiveTree::VIDEO;
    break;
  default:
    media_type_mask_ = static_cast<uint8_t>(~0);
  }
  if (*strCert)
  {
    unsigned int sz(strlen(strCert)), dstsz((sz * 3) / 4);
    server_certificate_.SetDataSize(dstsz);
    b64_decode(strCert, sz, server_certificate_.UseData(), dstsz);
    server_certificate_.SetDataSize(dstsz);
  }
  adaptiveTree_->manifest_headers_ = manifestHeaders;

}

Session::~Session()
{
  xbmc->Log(ADDON::LOG_DEBUG, "Session::~Session()");
  for (std::vector<STREAM*>::iterator b(streams_.begin()), e(streams_.end()); b != e; ++b)
    SAFE_DELETE(*b);
  streams_.clear();

  DisposeDecrypter();

  std::string fn(profile_path_ + "bandwidth.bin");
  FILE* f = fopen(fn.c_str(), "wb");
  if (f)
  {
    double val(adaptiveTree_->get_average_download_speed());
    fwrite((const char*)&val, sizeof(double), 1, f);
    fclose(f);
  }
  delete adaptiveTree_;
  adaptiveTree_ = nullptr;
}

void Session::GetSupportedDecrypterURN(std::string &key_system)
{
  typedef SSD::SSD_DECRYPTER *(*CreateDecryptorInstanceFunc)(SSD::SSD_HOST *host, uint32_t version);

  char specialpath[1024];
  if (!xbmc->GetSetting("DECRYPTERPATH", specialpath))
  {
    xbmc->Log(ADDON::LOG_DEBUG, "DECRYPTERPATH not specified in settings.xml");
    return;
  }
  kodihost.SetLibraryPath(xbmc->TranslateSpecialProtocol(specialpath));

  std::vector<std::string> searchPaths(2);
#ifdef ANDROID
  searchPaths[0] = xbmc->TranslateSpecialProtocol("special://xbmcbinaddons/");
#else
  searchPaths[0] = xbmc->TranslateSpecialProtocol("special://xbmcbinaddons/inputstream.adaptive/");
#endif
  xbmc->GetSetting("__addonpath__", specialpath);
  searchPaths[1] = specialpath;

  VFSDirEntry *items(0);
  unsigned int num_items(0);

  for (std::vector<std::string>::const_iterator path(searchPaths.begin()); !decrypter_ && path != searchPaths.end(); ++path)
  {
    xbmc->Log(ADDON::LOG_DEBUG, "Searching for decrypters in: %s", path->c_str());

    if (!xbmc->GetDirectory(path->c_str(), "", &items, &num_items))
      continue;

    for (unsigned int i(0); i < num_items; ++i)
    {
      if (strncmp(items[i].label, "ssd_", 4) && strncmp(items[i].label, "libssd_", 7))
        continue;

      void * mod(dlopen(items[i].path, RTLD_LAZY));
      if (mod)
      {
        CreateDecryptorInstanceFunc startup;
        if ((startup = (CreateDecryptorInstanceFunc)dlsym(mod, "CreateDecryptorInstance")))
        {
          SSD::SSD_DECRYPTER *decrypter = startup(&kodihost, SSD::SSD_HOST::version);
          const char *suppUrn(0);

          if (decrypter && (suppUrn = decrypter->SelectKeySytem(license_type_.c_str())))
          {
            xbmc->Log(ADDON::LOG_DEBUG, "Found decrypter: %s", items[i].path);
            decrypterModule_ = mod;
            decrypter_ = decrypter;
            key_system = suppUrn;
            break;
          }
        }
        dlclose(mod);
      }
      else
      {
        xbmc->Log(ADDON::LOG_DEBUG, "%s", dlerror());
      }
    }
  }
}

void Session::DisposeDecrypter()
{
  if (!decrypterModule_)
    return;

  for (std::vector<CDMSESSION>::iterator b(cdm_sessions_.begin()), e(cdm_sessions_.end()); b != e; ++b)
    if (!b->shared_single_sample_decryptor_)
      decrypter_->DestroySingleSampleDecrypter(b->single_sample_decryptor_);

  typedef void (*DeleteDecryptorInstanceFunc)(SSD::SSD_DECRYPTER *);
  DeleteDecryptorInstanceFunc disposefn((DeleteDecryptorInstanceFunc)dlsym(decrypterModule_, "DeleteDecryptorInstance"));

  if (disposefn)
    disposefn(decrypter_);

  dlclose(decrypterModule_);
  decrypterModule_ = 0;
  decrypter_ = 0;
}

/*----------------------------------------------------------------------
|   initialize
+---------------------------------------------------------------------*/

bool Session::initialize()
{
  if (!adaptiveTree_)
    return false;

  // Get URN's wich are supported by this addon
  if (!license_type_.empty())
  {
    GetSupportedDecrypterURN(adaptiveTree_->supportedKeySystem_);
    xbmc->Log(ADDON::LOG_DEBUG, "Supported URN: %s", adaptiveTree_->supportedKeySystem_.c_str());
  }

  // Open mpd file
  if (!adaptiveTree_->open(mpdFileURL_.c_str(), mpdUpdateParam_.c_str()) || adaptiveTree_->empty())
  {
    xbmc->Log(ADDON::LOG_ERROR, "Could not open / parse mpdURL (%s)", mpdFileURL_.c_str());
    return false;
  }
  xbmc->Log(ADDON::LOG_INFO, "Successfully parsed .mpd file. #Streams: %d Download speed: %0.4f Bytes/s", adaptiveTree_->periods_[0]->adaptationSets_.size(), adaptiveTree_->download_speed_);

  if (adaptiveTree_->encryptionState_ == adaptive::AdaptiveTree::ENCRYTIONSTATE_ENCRYPTED)
  {
    xbmc->Log(ADDON::LOG_ERROR, "Unable to handle decryption. Unsupported!");
    return false;
  }

  uint32_t min_bandwidth(0), max_bandwidth(0);
  {
    int buf;
    xbmc->GetSetting("MINBANDWIDTH", (char*)&buf), min_bandwidth = buf;
    xbmc->GetSetting("MAXBANDWIDTH", (char*)&buf), max_bandwidth = buf;
  }

  // create SESSION::STREAM objects. One for each AdaptationSet
  unsigned int i(0);
  const adaptive::AdaptiveTree::AdaptationSet *adp;

  for (std::vector<STREAM*>::iterator b(streams_.begin()), e(streams_.end()); b != e; ++b)
    SAFE_DELETE(*b);
  streams_.clear();
  cdm_sessions_.resize(adaptiveTree_->psshSets_.size());
  memset(&cdm_sessions_.front(), 0, sizeof(CDMSESSION));

  // Try to initialize an SingleSampleDecryptor
  if (adaptiveTree_->encryptionState_)
  {
    if (license_key_.empty())
      license_key_ = adaptiveTree_->license_url_;

    xbmc->Log(ADDON::LOG_DEBUG, "Entering encryption sectiom");

    if (license_key_.empty())
    {
      xbmc->Log(ADDON::LOG_ERROR, "Invalid license_key");
      return false;
    }

    if (!decrypter_)
    {
      xbmc->Log(ADDON::LOG_ERROR, "No decrypter found for encrypted stream");
      return false;
    }

    if (!decrypter_->OpenDRMSystem(license_key_.c_str(), server_certificate_))
    {
      xbmc->Log(ADDON::LOG_ERROR, "OpenDRMSystem failed");
      return false;
    }

    for (size_t ses(1); ses < cdm_sessions_.size(); ++ses)
    {
      AP4_DataBuffer init_data;
      const char *optionalKeyParameter(nullptr);

      if (adaptiveTree_->psshSets_[ses].pssh_ == "FILE")
      {
         xbmc->Log(ADDON::LOG_DEBUG, "Searching PSSH data in FILE");

        if (license_data_.empty())
        {
          std::string strkey(adaptiveTree_->supportedKeySystem_.substr(9));
          size_t pos;
          while ((pos = strkey.find('-')) != std::string::npos)
            strkey.erase(pos, 1);
          if (strkey.size() != 32)
          {
            xbmc->Log(ADDON::LOG_ERROR, "Key system mismatch (%s)!", adaptiveTree_->supportedKeySystem_.c_str());
            return false;
          }

          unsigned char key_system[16];
          AP4_ParseHex(strkey.c_str(), key_system, 16);

          Session::STREAM stream(*adaptiveTree_, adaptiveTree_->GetAdaptationSet(0)->type_);
          stream.stream_.prepare_stream(adaptiveTree_->GetAdaptationSet(0), 0, 0, 0, 0, 0, 0, 0, std::map<std::string, std::string>());

          stream.enabled = true;
          stream.stream_.start_stream(0, width_, height_);
          stream.stream_.select_stream(true, false, stream.info_.m_pID >> 16);

          stream.input_ = new AP4_DASHStream(&stream.stream_);
          stream.input_file_ = new AP4_File(*stream.input_, AP4_DefaultAtomFactory::Instance, true);
          AP4_Movie* movie = stream.input_file_->GetMovie();
          if (movie == NULL)
          {
            xbmc->Log(ADDON::LOG_ERROR, "No MOOV in stream!");
            stream.disable();
            return false;
          }
          AP4_Array<AP4_PsshAtom*>& pssh = movie->GetPsshAtoms();

          for (unsigned int i = 0; !init_data.GetDataSize() && i < pssh.ItemCount(); i++)
          {
            if (memcmp(pssh[i]->GetSystemId(), key_system, 16) == 0)
            {
              init_data.AppendData(pssh[i]->GetData().GetData(), pssh[i]->GetData().GetDataSize());
              if (adaptiveTree_->psshSets_[ses].defaultKID_.empty())
              {
                if (pssh[i]->GetKid(0))
                  adaptiveTree_->psshSets_[ses].defaultKID_ = std::string((const char*)pssh[i]->GetKid(0), 16);
                else if (AP4_Track *track = movie->GetTrack(TIDC[stream.stream_.get_type()]))
                {
                  AP4_ProtectedSampleDescription *m_protectedDesc = static_cast<AP4_ProtectedSampleDescription*>(track->GetSampleDescription(0));
                  AP4_ContainerAtom *schi;
                  if (m_protectedDesc->GetSchemeInfo() && (schi = m_protectedDesc->GetSchemeInfo()->GetSchiAtom()))
                  {
                    AP4_TencAtom* tenc(AP4_DYNAMIC_CAST(AP4_TencAtom, schi->GetChild(AP4_ATOM_TYPE_TENC, 0)));
                    if (tenc)
                      adaptiveTree_->psshSets_[ses].defaultKID_ = std::string((const char*)tenc->GetDefaultKid());
                    else
                    {
                      AP4_PiffTrackEncryptionAtom* piff(AP4_DYNAMIC_CAST(AP4_PiffTrackEncryptionAtom, schi->GetChild(AP4_UUID_PIFF_TRACK_ENCRYPTION_ATOM, 0)));
                      if (piff)
                        adaptiveTree_->psshSets_[ses].defaultKID_ = std::string((const char*)piff->GetDefaultKid());
                    }
                  }
                }
              }
            }
          }

          if (!init_data.GetDataSize())
          {
            xbmc->Log(ADDON::LOG_ERROR, "Could not extract license from video stream (PSSH not found)");
            stream.disable();
            return false;
          }
          stream.disable();
        }
        else if (!adaptiveTree_->psshSets_[ses].defaultKID_.empty())
        {
          init_data.SetData((AP4_Byte*)adaptiveTree_->psshSets_[ses].defaultKID_.data(), 16);

          uint8_t ld[1024];
          unsigned int ld_size(1014);
          b64_decode(license_data_.c_str(), license_data_.size(), ld, ld_size);

          uint8_t *uuid((uint8_t*)strstr((const char*)ld, "{KID}"));
          if (uuid)
          {
            memmove(uuid + 11, uuid, ld_size - (uuid - ld));
            memcpy(uuid, init_data.GetData(), init_data.GetDataSize());
            init_data.SetData(ld, ld_size + 11);
          }
          else
            init_data.SetData(ld, ld_size);
        }
        else
          return false;
      }
      else
      {
        if (manifest_type_ == MANIFEST_TYPE_ISM)
        {
          if (license_type_ == "com.widevine.alpha")
            create_ism_license(adaptiveTree_->psshSets_[ses].defaultKID_, license_data_, init_data);
          else
          {
            init_data.SetData(reinterpret_cast<const uint8_t*>(adaptiveTree_->psshSets_[ses].pssh_.data()), adaptiveTree_->psshSets_[ses].pssh_.size());
            optionalKeyParameter = license_data_.empty() ? nullptr : license_data_.c_str();
          }
        }
        else
        {
          init_data.SetBufferSize(1024);
          unsigned int init_data_size(1024);
          b64_decode(adaptiveTree_->psshSets_[ses].pssh_.data(), adaptiveTree_->psshSets_[ses].pssh_.size(), init_data.UseData(), init_data_size);
          init_data.SetDataSize(init_data_size);
        }
      }

      CDMSESSION &session(cdm_sessions_[ses]);
      const char *defkid = adaptiveTree_->psshSets_[ses].defaultKID_.empty() ? nullptr : adaptiveTree_->psshSets_[ses].defaultKID_.data();
      session.single_sample_decryptor_ = nullptr;
      session.shared_single_sample_decryptor_ = false;

      if (decrypter_ && defkid)
      {
        char hexkid[36];
        AP4_FormatHex(reinterpret_cast<const AP4_UI08*>(defkid), 16, hexkid), hexkid[32]=0;
         xbmc->Log(ADDON::LOG_DEBUG, "Initializing stream with KID: %s", hexkid);

        for (unsigned int i(1); i < ses; ++i)
          if (decrypter_ && decrypter_->HasLicenseKey(cdm_sessions_[i].single_sample_decryptor_, (const uint8_t *)defkid))
          {
            session.single_sample_decryptor_ = cdm_sessions_[i].single_sample_decryptor_;
            session.shared_single_sample_decryptor_ = true;
          }
      }
      else if (!defkid)
         xbmc->Log(ADDON::LOG_DEBUG, "Initializing stream with unknown KID!");

      if (decrypter_ && init_data.GetDataSize() >= 4 && (session.single_sample_decryptor_
        || (session.single_sample_decryptor_ = decrypter_->CreateSingleSampleDecrypter(init_data, optionalKeyParameter)) != 0))
      {

        decrypter_->GetCapabilities(
          session.single_sample_decryptor_,
          (const uint8_t *)defkid,
          adaptiveTree_->psshSets_[ses].media_,
          session.decrypter_caps_);

        if (session.decrypter_caps_.flags & SSD::SSD_DECRYPTER::SSD_CAPS::SSD_SECURE_PATH)
        {
          session.cdm_session_str_ = session.single_sample_decryptor_->GetSessionId();
          secure_video_session_ = true;
          // Override this setting by information passed in manifest
          if (!adaptiveTree_->need_secure_decoder_)
            session.decrypter_caps_.flags &= ~SSD::SSD_DECRYPTER::SSD_CAPS::SSD_SECURE_DECODER;
        }
      }
      else
      {
        xbmc->Log(ADDON::LOG_ERROR, "Initialize failed (SingleSampleDecrypter)");
        for (unsigned int i(ses); i < cdm_sessions_.size(); ++i)
          cdm_sessions_[i].single_sample_decryptor_ = nullptr;
        return false;
      }
    }
  }

  while ((adp = adaptiveTree_->GetAdaptationSet(i++)))
  {
    size_t repId = manual_streams_ ? adp->repesentations_.size() : 0;

    do {
      streams_.push_back(new STREAM(*adaptiveTree_, adp->type_));
      STREAM &stream(*streams_.back());
      const SSD::SSD_DECRYPTER::SSD_CAPS &caps(GetDecrypterCaps(adp->repesentations_[0]->get_psshset()));

      uint32_t hdcpLimit(caps.hdcpLimit);
      uint16_t hdcpVersion(caps.hdcpVersion);

      bool buf;
      xbmc->GetSetting("HDCPOVERRIDE", (char*)&buf);
      if (buf)
      {
        hdcpLimit = 0;
        hdcpVersion = 99;
      }

      stream.stream_.prepare_stream(adp, GetVideoWidth(), GetVideoHeight(), hdcpLimit, hdcpVersion, min_bandwidth, max_bandwidth, repId, media_headers_);

      switch (adp->type_)
      {
      case adaptive::AdaptiveTree::VIDEO:
        stream.info_.m_streamType = INPUTSTREAM_INFO::TYPE_VIDEO;
        break;
      case adaptive::AdaptiveTree::AUDIO:
        stream.info_.m_streamType = INPUTSTREAM_INFO::TYPE_AUDIO;
        break;
      case adaptive::AdaptiveTree::SUBTITLE:
        stream.info_.m_streamType = INPUTSTREAM_INFO::TYPE_SUBTITLE;
        break;
      default:
        break;
      }
      stream.info_.m_pID = i | (repId << 16);
      strcpy(stream.info_.m_language, adp->language_.c_str());
      stream.info_.m_ExtraData = nullptr;
      stream.info_.m_ExtraSize = 0;
      //stream.info_.m_features = 0;
      stream.stream_.set_observer(dynamic_cast<adaptive::AdaptiveStreamObserver*>(this));

      UpdateStream(stream, caps);

    } while (repId--);
  }
  return true;
}

void Session::UpdateStream(STREAM &stream, const SSD::SSD_DECRYPTER::SSD_CAPS &caps)
{
  const adaptive::AdaptiveTree::Representation *rep(stream.stream_.getRepresentation());

  stream.info_.m_Width = rep->width_;
  stream.info_.m_Height = rep->height_;
  stream.info_.m_Aspect = rep->aspect_;

  if (stream.info_.m_Aspect == 0.0f && stream.info_.m_Height)
    stream.info_.m_Aspect = (float)stream.info_.m_Width / stream.info_.m_Height;
  stream.encrypted = rep->get_psshset() > 0;

  if (!stream.info_.m_ExtraSize && rep->codec_private_data_.size())
  {
    std::string annexb;
    const std::string *res(&annexb);

    if ((caps.flags & SSD::SSD_DECRYPTER::SSD_CAPS::SSD_ANNEXB_REQUIRED)
      && stream.info_.m_streamType == INPUTSTREAM_INFO::TYPE_VIDEO)
    {
      xbmc->Log(ADDON::LOG_DEBUG, "UpdateStream: Convert avc -> annexb");
      annexb = avc_to_annexb(rep->codec_private_data_);
    }
    else
      res = &rep->codec_private_data_;

    stream.info_.m_ExtraSize = res->size();
    stream.info_.m_ExtraData = (const uint8_t*)malloc(stream.info_.m_ExtraSize);
    memcpy((void*)stream.info_.m_ExtraData, res->data(), stream.info_.m_ExtraSize);
  }

  // we currently use only the first track!
  std::string::size_type pos = rep->codecs_.find(",");
  if (pos == std::string::npos)
    pos = rep->codecs_.size();

  strncpy(stream.info_.m_codecInternalName, rep->codecs_.c_str(), pos);
  stream.info_.m_codecInternalName[pos] = 0;

  if (rep->codecs_.find("mp4a") == 0
  || rep->codecs_.find("aac") == 0)
    strcpy(stream.info_.m_codecName, "aac");
  else if (rep->codecs_.find("ec-3") == 0 || rep->codecs_.find("ac-3") == 0)
    strcpy(stream.info_.m_codecName, "eac3");
  else if (rep->codecs_.find("avc") == 0
  || rep->codecs_.find("h264") == 0)
    strcpy(stream.info_.m_codecName, "h264");
  else if (rep->codecs_.find("hev") == 0 || rep->codecs_.find("hvc") == 0)
    strcpy(stream.info_.m_codecName, "hevc");
  else if (rep->codecs_.find("vp9") == 0)
    strcpy(stream.info_.m_codecName, "vp9");
  else if (rep->codecs_.find("opus") == 0)
    strcpy(stream.info_.m_codecName, "opus");
  else if (rep->codecs_.find("vorbis") == 0)
    strcpy(stream.info_.m_codecName, "vorbis");
  else if (rep->codecs_.find("stpp") == 0 || rep->codecs_.find("ttml") == 0)
    strcpy(stream.info_.m_codecName, "srt");

  stream.info_.m_FpsRate = rep->fpsRate_;
  stream.info_.m_FpsScale = rep->fpsScale_;
  stream.info_.m_SampleRate = rep->samplingRate_;
  stream.info_.m_Channels = rep->channelCount_;
  stream.info_.m_BitRate = rep->bandwidth_;
}

AP4_Movie *Session::PrepareStream(STREAM *stream)
{
  if (!adaptiveTree_->prepareRepresentation(const_cast<adaptive::AdaptiveTree::Representation *>(stream->stream_.getRepresentation())))
    return nullptr;

  if (stream->stream_.getRepresentation()->containerType_ == adaptive::AdaptiveTree::CONTAINERTYPE_MP4
    && (stream->stream_.getRepresentation()->flags_ & adaptive::AdaptiveTree::Representation::INITIALIZATION_PREFIXED) == 0
    && stream->stream_.getRepresentation()->get_initialization() == nullptr)
  {
    //We'll create a Movie out of the things we got from manifest file
    //note: movie will be deleted in destructor of stream->input_file_
    AP4_Movie *movie = new AP4_Movie();

    AP4_SyntheticSampleTable* sample_table = new AP4_SyntheticSampleTable();

    AP4_SampleDescription *sample_descryption;
    if (strcmp(stream->info_.m_codecName, "h264") == 0)
    {
      const std::string &extradata(stream->stream_.getRepresentation()->codec_private_data_);
      AP4_MemoryByteStream ms((const uint8_t*)extradata.data(), extradata.size());
      AP4_AvccAtom *atom = AP4_AvccAtom::Create(AP4_ATOM_HEADER_SIZE + extradata.size(), ms);
      sample_descryption = new AP4_AvcSampleDescription(AP4_SAMPLE_FORMAT_AVC1, stream->info_.m_Width, stream->info_.m_Height, 0, nullptr, atom);
    }
    else if (strcmp(stream->info_.m_codecName, "srt") == 0)
      sample_descryption = new AP4_SampleDescription(AP4_SampleDescription::TYPE_SUBTITLES, AP4_SAMPLE_FORMAT_STPP, 0);
    else
      sample_descryption = new AP4_SampleDescription(AP4_SampleDescription::TYPE_UNKNOWN, 0, 0);

    if (stream->stream_.getRepresentation()->get_psshset() > 0)
    {
      AP4_ContainerAtom schi(AP4_ATOM_TYPE_SCHI);
      schi.AddChild(new AP4_TencAtom(AP4_CENC_ALGORITHM_ID_CTR, 8, GetDefaultKeyId(stream->stream_.getRepresentation()->get_psshset())));
      sample_descryption = new AP4_ProtectedSampleDescription(0, sample_descryption, 0, AP4_PROTECTION_SCHEME_TYPE_PIFF, 0, "", &schi);
    }
    sample_table->AddSampleDescription(sample_descryption);

    movie->AddTrack(new AP4_Track(TIDC[stream->stream_.get_type()], sample_table, ~0, stream->stream_.getRepresentation()->timescale_, 0, stream->stream_.getRepresentation()->timescale_, 0, "", 0, 0));
    //Create a dumy MOOV Atom to tell Bento4 its a fragmented stream
    AP4_MoovAtom *moov = new AP4_MoovAtom();
    moov->AddChild(new AP4_ContainerAtom(AP4_ATOM_TYPE_MVEX));
    movie->SetMoovAtom(moov);
    return movie;
  }
  return nullptr;
}

SampleReader *Session::GetNextSample()
{
  STREAM *res(0);
  for (std::vector<STREAM*>::const_iterator b(streams_.begin()), e(streams_.end()); b != e; ++b)
  {
    bool bStarted(false);
    if ((*b)->enabled && (*b)->reader_ && !(*b)->reader_->EOS() && AP4_SUCCEEDED((*b)->reader_->Start(bStarted))
      && (!res || (*b)->reader_->DTS() < res->reader_->DTS()))
      res = *b;

    if (bStarted && ((*b)->reader_->GetInformation((*b)->info_)))
      changed_ = true;
  }

  if (res)
  {
    CheckFragmentDuration(*res);
    if (res->reader_->GetInformation(res->info_))
      changed_ = true;
    if (res->reader_->PTS() != DVD_NOPTS_VALUE)
      elapsed_time_ = res->reader_->Elapsed(res->stream_.GetStartPTS());
    return res->reader_;
  }
  return 0;
}

bool Session::SeekTime(double seekTime, unsigned int streamId, bool preceeding)
{
  bool ret(false);

  //we don't have pts < 0 here and work internally with uint64
  if (seekTime < 0)
    seekTime = 0;

  if (adaptiveTree_->has_timeshift_buffer_ && seekTime > (static_cast<double>(GetTotalTimeMs()) / 1000) - 12)
  {
    seekTime = (static_cast<double>(GetTotalTimeMs()) / 1000) - 12;
    preceeding = true;
  }

  for (std::vector<STREAM*>::const_iterator b(streams_.begin()), e(streams_.end()); b != e; ++b)
    if ((*b)->enabled && (*b)->reader_ && (streamId == 0 || (*b)->info_.m_pID == streamId))
    {
      bool bReset;
      uint64_t seekTimeCorrected = static_cast<uint64_t>(seekTime * DVD_TIME_BASE) + (*b)->stream_.GetStartPTS();
      if ((*b)->stream_.seek_time(static_cast<double>(seekTimeCorrected) / DVD_TIME_BASE, preceeding, bReset))
      {
        if (bReset)
          (*b)->reader_->Reset(false);
        if (!(*b)->reader_->TimeSeek(seekTimeCorrected, preceeding))
          (*b)->reader_->Reset(true);
        else
        {
          double destTime(static_cast<double>((*b)->reader_->Elapsed((*b)->stream_.GetStartPTS())) / DVD_TIME_BASE);
          xbmc->Log(ADDON::LOG_INFO, "seekTime(%0.1lf) for Stream:%d continues at %0.1lf", seekTime, (*b)->info_.m_pID, destTime);
          if ((*b)->info_.m_streamType == INPUTSTREAM_INFO::TYPE_VIDEO)
            seekTime = destTime, preceeding = false;
          ret = true;
        }
      }
      else
        (*b)->reader_->Reset(true);
    }
  return ret;
}

void Session::OnSegmentChanged(adaptive::AdaptiveStream *stream)
{
  for (std::vector<STREAM*>::iterator s(streams_.begin()), e(streams_.end()); s != e; ++s)
    if (&(*s)->stream_ == stream)
    {
      if((*s)->reader_)
        (*s)->reader_->SetPTSOffset((*s)->stream_.GetPTSOffset());
      (*s)->segmentChanged = true;
      break;
    }
}

void Session::OnStreamChange(adaptive::AdaptiveStream *stream, uint32_t segment)
{
}

void Session::CheckFragmentDuration(STREAM &stream)
{
  uint64_t nextTs, nextDur;
  if (stream.segmentChanged && stream.reader_->GetNextFragmentInfo(nextTs, nextDur))
    adaptiveTree_->SetFragmentDuration(
      stream.stream_.getAdaptationSet(),
      stream.stream_.getRepresentation(),
      stream.stream_.getSegmentPos(),
      nextTs,
      static_cast<uint32_t>(nextDur),
      stream.reader_->GetTimeScale());
  stream.segmentChanged = false;
}

const AP4_UI08 *Session::GetDefaultKeyId(const uint16_t index) const
{
  static const AP4_UI08 default_key[16] = { 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
  if (adaptiveTree_->psshSets_[index].defaultKID_.size() == 16)
    return reinterpret_cast<const AP4_UI08 *>(adaptiveTree_->psshSets_[index].defaultKID_.data());
  return default_key;
}

std::uint16_t Session::GetVideoWidth() const
{
  std::uint16_t ret(width_);
  bool displayOverride;
  xbmc->GetSetting("IGNOREDISPLAY", (char*)&displayOverride);
  if (displayOverride)
    ret = 8192;
  switch (secure_video_session_ ? max_secure_resolution_ : max_resolution_)
  {
  case 1:
    if (ret > 640) ret = 640;
    break;
  case 2:
    if (ret > 1280) ret = 1280;
    break;
  case 3:
    if (ret > 1920) ret = 1920;
    break;
  default:
    ;
  }
  return ret;
}

std::uint16_t Session::GetVideoHeight() const
{
  std::uint16_t ret(height_);
  bool displayOverride;
  xbmc->GetSetting("IGNOREDISPLAY", (char*)&displayOverride);
  if (displayOverride)
    ret = 8182;
  switch (secure_video_session_ ? max_secure_resolution_ : max_resolution_)
  {
  case 1:
    if (ret > 480) ret = 480;
    break;
  case 2:
    if (ret > 720) ret = 720;
    break;
  case 3:
    if (ret > 1080) ret = 1080;
    break;
  default:
    ;
  }
  return ret;
}

AP4_CencSingleSampleDecrypter *Session::GetSingleSampleDecrypter(std::string sessionId)
{
  for (std::vector<CDMSESSION>::iterator b(cdm_sessions_.begin() + 1), e(cdm_sessions_.end()); b != e; ++b)
    if (b->cdm_session_str_ && sessionId == b->cdm_session_str_)
      return b->single_sample_decryptor_;
  return nullptr;
}

uint32_t Session::GetIncludedStreamMask() const
{
  const INPUTSTREAM_INFO::STREAM_TYPE adp2ips[] = { INPUTSTREAM_INFO::TYPE_NONE, INPUTSTREAM_INFO::TYPE_VIDEO, INPUTSTREAM_INFO::TYPE_AUDIO, INPUTSTREAM_INFO::TYPE_SUBTITLE};
  uint32_t res(0);
  for (unsigned int i(0); i < 4; ++i)
    if (adaptiveTree_->included_types_ & (1U << i))
      res |= (1U << adp2ips[i]);
  return res;
}

/***************************  Interface *********************************/

#include "kodi_inputstream_dll.h"
#include "libKODI_inputstream.h"

CHelper_libKODI_inputstream *ipsh = 0;

Session* m_session;
int m_width, m_height;
uint16_t m_IncludedStreams[16];

extern "C" {

  ADDON_STATUS curAddonStatus = ADDON_STATUS_UNKNOWN;

  /***********************************************************
  * Standard AddOn related public library functions
  ***********************************************************/

  ADDON_STATUS ADDON_Create(void* hdl, void* props)
  {
    // initialize globals
    m_session = nullptr;
    m_width = 1280;
    m_height = 720;

    memset(m_IncludedStreams, 0, sizeof(m_IncludedStreams));

    if (!hdl)
      return ADDON_STATUS_UNKNOWN;

    xbmc = new ADDON::CHelper_libXBMC_addon;
    if (!xbmc->RegisterMe(hdl))
    {
      SAFE_DELETE(xbmc);
      return ADDON_STATUS_PERMANENT_FAILURE;
    }
    xbmc->Log(ADDON::LOG_DEBUG, "libXBMC_addon successfully loaded");

    ipsh = new CHelper_libKODI_inputstream;
    if (!ipsh->RegisterMe(hdl))
    {
      SAFE_DELETE(xbmc);
      SAFE_DELETE(ipsh);
      return ADDON_STATUS_PERMANENT_FAILURE;
    }

    xbmc->Log(ADDON::LOG_DEBUG, "ADDON_Create()");

    curAddonStatus = ADDON_STATUS_OK;
    return curAddonStatus;
  }

  ADDON_STATUS ADDON_GetStatus()
  {
    return curAddonStatus;
  }

  void ADDON_Destroy()
  {
    SAFE_DELETE(m_session);
    if (xbmc)
    {
      xbmc->Log(ADDON::LOG_DEBUG, "ADDON_Destroy()");
      SAFE_DELETE(xbmc);
    }
    SAFE_DELETE(ipsh);
  }

  bool ADDON_HasSettings()
  {
    xbmc->Log(ADDON::LOG_DEBUG, "ADDON_HasSettings()");
    return false;
  }

  unsigned int ADDON_GetSettings(ADDON_StructSetting ***sSet)
  {
    xbmc->Log(ADDON::LOG_DEBUG, "ADDON_GetSettings()");
    return 0;
  }

  ADDON_STATUS ADDON_SetSetting(const char *settingName, const void *settingValue)
  {
    xbmc->Log(ADDON::LOG_DEBUG, "ADDON_SetSettings()");
    return ADDON_STATUS_OK;
  }

  void ADDON_Stop()
  {
  }

  void ADDON_FreeSettings()
  {
  }

  void ADDON_Announce(const char *flag, const char *sender, const char *message, const void *data)
  {
  }

  /***********************************************************
  * InputSteam Client AddOn specific public library functions
  ***********************************************************/

  bool Open(INPUTSTREAM& props)
  {
    xbmc->Log(ADDON::LOG_DEBUG, "Open()");

    const char *lt(""), *lk(""), *ld(""), *lsc(""), *mfup("");
    std::map<std::string, std::string> manh, medh;
    std::string mpd_url = props.m_strURL;
    MANIFEST_TYPE manifest(MANIFEST_TYPE_UNKNOWN);
    for (unsigned int i(0); i < props.m_nCountInfoValues; ++i)
    {
      if (strcmp(props.m_ListItemProperties[i].m_strKey, "inputstream.adaptive.license_type") == 0)
      {
        xbmc->Log(ADDON::LOG_DEBUG, "found inputstream.adaptive.license_type: %s", props.m_ListItemProperties[i].m_strValue);
        lt = props.m_ListItemProperties[i].m_strValue;
      }
      else if (strcmp(props.m_ListItemProperties[i].m_strKey, "inputstream.adaptive.license_key") == 0)
      {
        xbmc->Log(ADDON::LOG_DEBUG, "found inputstream.adaptive.license_key: [not shown]");
        lk = props.m_ListItemProperties[i].m_strValue;
      }
      else if (strcmp(props.m_ListItemProperties[i].m_strKey, "inputstream.adaptive.license_data") == 0)
      {
        xbmc->Log(ADDON::LOG_DEBUG, "found inputstream.adaptive.license_data: [not shown]");
        ld = props.m_ListItemProperties[i].m_strValue;
      }
      else if (strcmp(props.m_ListItemProperties[i].m_strKey, "inputstream.adaptive.server_certificate") == 0)
      {
        xbmc->Log(ADDON::LOG_DEBUG, "found inputstream.adaptive.server_certificate: [not shown]");
        lsc = props.m_ListItemProperties[i].m_strValue;
      }
      else if (strcmp(props.m_ListItemProperties[i].m_strKey, "inputstream.adaptive.manifest_type") == 0)
      {
        xbmc->Log(ADDON::LOG_DEBUG, "found inputstream.adaptive.manifest_type: %s", props.m_ListItemProperties[i].m_strValue);
        if (strcmp(props.m_ListItemProperties[i].m_strValue, "mpd") == 0)
          manifest = MANIFEST_TYPE_MPD;
        else if (strcmp(props.m_ListItemProperties[i].m_strValue, "ism") == 0)
          manifest = MANIFEST_TYPE_ISM;
        else if (strcmp(props.m_ListItemProperties[i].m_strValue, "hls") == 0)
          manifest = MANIFEST_TYPE_HLS;
      }
      else if (strcmp(props.m_ListItemProperties[i].m_strKey, "inputstream.adaptive.manifest_update_parameter") == 0)
      {
        mfup = props.m_ListItemProperties[i].m_strValue;
        xbmc->Log(ADDON::LOG_DEBUG, "found inputstream.adaptive.manifest_update_parameter: %s", mfup);
      }
      else if (strcmp(props.m_ListItemProperties[i].m_strKey, "inputstream.adaptive.stream_headers") == 0)
      {
        xbmc->Log(ADDON::LOG_DEBUG, "found inputstream.adaptive.stream_headers: %s", props.m_ListItemProperties[i].m_strValue);
        parseheader(manh, props.m_ListItemProperties[i].m_strValue);
        medh = manh;
        mpd_url = mpd_url.substr(0, mpd_url.find("|"));
      }
    }

    if (manifest == MANIFEST_TYPE_UNKNOWN)
    {
      xbmc->Log(ADDON::LOG_ERROR, "Invalid / not given inputstream.adaptive.manifest_type");
      return false;
    }

    std::string::size_type posHeader(mpd_url.find("|"));
    if (posHeader != std::string::npos)
    {
      manh.clear();
      parseheader(manh, mpd_url.substr(posHeader + 1).c_str());
      mpd_url = mpd_url.substr(0, posHeader);
    }

    kodihost.SetProfilePath(props.m_profileFolder);

    m_session = new Session(manifest, mpd_url.c_str(), mfup, lt, lk, ld, lsc, manh, medh, props.m_profileFolder, m_width, m_height);
    m_session->SetVideoResolution(m_width, m_height);

    if (!m_session->initialize())
    {
      SAFE_DELETE(m_session);
      return false;
    }
    return true;
  }

  void Close(void)
  {
    xbmc->Log(ADDON::LOG_DEBUG, "Close()");
    SAFE_DELETE(m_session);
  }

  const char* GetPathList(void)
  {
    return "";
  }

  struct INPUTSTREAM_IDS GetStreamIds()
  {
    xbmc->Log(ADDON::LOG_DEBUG, "GetStreamIds()");
    INPUTSTREAM_IDS iids;

    if (m_session)
    {
      iids.m_streamCount = 0;
      for (unsigned int i(1); i <= m_session->GetStreamCount(); ++i)
        if (m_session->GetMediaTypeMask() & static_cast<uint8_t>(1) << m_session->GetStream(i)->stream_.get_type())
          iids.m_streamIds[iids.m_streamCount++] = i;
    }
    else
      iids.m_streamCount = 0;
    return iids;
  }

  struct INPUTSTREAM_CAPABILITIES GetCapabilities()
  {
    xbmc->Log(ADDON::LOG_DEBUG, "GetCapabilities()");
    INPUTSTREAM_CAPABILITIES caps;
    caps.m_supportsIDemux = true;
    caps.m_supportsIPosTime = true;
    caps.m_supportsIDisplayTime = true;
    caps.m_supportsSeek = true;// m_session && !m_session->IsLive();
    caps.m_supportsPause = true;// caps.m_supportsSeek;
    return caps;
  }

  struct INPUTSTREAM_INFO GetStream(int streamid)
  {
    static struct INPUTSTREAM_INFO dummy_info = {
      INPUTSTREAM_INFO::TYPE_NONE, "", "", 0, 0, 0, 0, "",
      0, 0, 0, 0, 0.0f,
      0, 0, 0, 0, 0 };

    xbmc->Log(ADDON::LOG_DEBUG, "GetStream(%d)", streamid);

    Session::STREAM *stream(m_session->GetStream(streamid));

    if (stream)
    {
      uint16_t cdmId(stream->stream_.getRepresentation()->pssh_set_);
#ifdef ANDROID
      if (stream->encrypted)
      {
        static AP4_DataBuffer tmp;
        tmp.SetData(reinterpret_cast<const AP4_Byte*>("CRYPTO"), 6);
        const char* sessionId(m_session->GetCDMSession(cdmId));
        uint8_t sessionIdSize = static_cast<uint8_t>(strlen(sessionId));
        uint16_t cryptosize = 6 + 2 + (1 + sessionIdSize) + 16;
        tmp.AppendData(reinterpret_cast<const AP4_Byte*>(&cryptosize), sizeof(cryptosize));
        tmp.AppendData(&sessionIdSize, 1);
        tmp.AppendData((const AP4_Byte*)sessionId, sessionIdSize);
        uint8_t keysystem[16] = { 0xed, 0xef, 0x8b, 0xa9, 0x79, 0xd6, 0x4a, 0xce, 0xa3, 0xc8, 0x27, 0xdc, 0xd5, 0x1d, 0x21, 0xed };
        tmp.AppendData(keysystem, 16);
        tmp.AppendData(stream->info_.m_ExtraData, stream->info_.m_ExtraSize);
        INPUTSTREAM_INFO tmpInfo = stream->info_;
        tmpInfo.m_ExtraData = tmp.GetData();
        tmpInfo.m_ExtraSize = tmp.GetDataSize();
        return tmpInfo;
      }
#endif
      return stream->info_;
    }
    return dummy_info;
  }

  void EnableStream(int streamid, bool enable)
  {
    xbmc->Log(ADDON::LOG_DEBUG, "EnableStream(%d: %s)", streamid, enable ? "true" : "false");

    if (!m_session)
      return;

    Session::STREAM *stream(m_session->GetStream(streamid));

    if (!stream)
      return;

    if (enable)
    {
      if (stream->enabled)
        return;

      stream->enabled = true;

      stream->stream_.start_stream(~0, m_session->GetVideoWidth(), m_session->GetVideoHeight());
      const adaptive::AdaptiveTree::Representation *rep(stream->stream_.getRepresentation());

      // If we select a dummy (=inside video) stream, open the video part
      // Dummy streams will be never enabled, they will only enable / activate audio track.
      if (rep->flags_ & adaptive::AdaptiveTree::Representation::INCLUDEDSTREAM)
      {
        Session::STREAM *mainStream;
        stream->mainId_ = 0;
        while ((mainStream = m_session->GetStream(++stream->mainId_)))
          if (mainStream->info_.m_streamType == INPUTSTREAM_INFO::TYPE_VIDEO && mainStream->enabled)
            break;
        if (mainStream)
        {
          mainStream->reader_->AddStreamType(stream->info_.m_streamType, streamid);
          mainStream->reader_->GetInformation(stream->info_);
        }
        else
          stream->mainId_ = 0;
        m_IncludedStreams[stream->info_.m_streamType] = streamid;
        return;
      }

      xbmc->Log(ADDON::LOG_DEBUG, "Selecting stream with conditions: w: %u, h: %u, bw: %u",
        stream->stream_.getWidth(), stream->stream_.getHeight(), stream->stream_.getBandwidth());

      if (!stream->stream_.select_stream(true, false, stream->info_.m_pID >> 16))
      {
        xbmc->Log(ADDON::LOG_ERROR, "Unable to select stream!");
        return stream->disable();
      }

      if (rep != stream->stream_.getRepresentation())
      {
        m_session->UpdateStream(*stream, m_session->GetDecrypterCaps(stream->stream_.getRepresentation()->pssh_set_));
        m_session->CheckChange(true);
      }

      if (rep->flags_ & adaptive::AdaptiveTree::Representation::SUBTITLESTREAM)
      {
        stream->reader_ = new SubtitleSampleReader(rep->url_, streamid);
        return;
      }

      AP4_Movie* movie(m_session->PrepareStream(stream));

      // We load fragments on PrepareTime for HLS manifests and have to reevaluate the start-segment
      if (m_session->GetManifestType() == MANIFEST_TYPE_HLS)
        stream->stream_.restart_stream();

      if (rep->containerType_ == adaptive::AdaptiveTree::CONTAINERTYPE_TS)
      {
        stream->input_ = new AP4_DASHStream(&stream->stream_);
        stream->reader_ = new TSSampleReader(stream->input_, stream->info_.m_streamType, streamid,
          (1U << stream->info_.m_streamType) | m_session->GetIncludedStreamMask());
        if (!static_cast<TSSampleReader*>(stream->reader_)->Initialize())
          return stream->disable();
        else
          m_session->CheckChange(true);
      }
      else if (rep->containerType_ == adaptive::AdaptiveTree::CONTAINERTYPE_MP4)
      {
        stream->input_ = new AP4_DASHStream(&stream->stream_);
        stream->input_file_ = new AP4_File(*stream->input_, AP4_DefaultAtomFactory::Instance, true, movie);
        movie = stream->input_file_->GetMovie();

        if (movie == NULL)
        {
          xbmc->Log(ADDON::LOG_ERROR, "No MOOV in stream!");
          return stream->disable();
        }

        AP4_Track *track = movie->GetTrack(TIDC[stream->stream_.get_type()]);
        if (!track)
        {
          xbmc->Log(ADDON::LOG_ERROR, "No suitable track found in stream");
          return stream->disable();
        }

        stream->reader_ = new FragmentedSampleReader(stream->input_, movie, track, streamid,
          m_session->GetSingleSampleDecryptor(stream->stream_.getRepresentation()->pssh_set_),
          m_session->GetDecrypterCaps(stream->stream_.getRepresentation()->pssh_set_));
      }
      else
        return stream->disable();

      if (stream->info_.m_streamType == INPUTSTREAM_INFO::TYPE_VIDEO)
      {
        for (uint16_t i(0); i < 16; ++i)
          if (m_IncludedStreams[i])
          {
            stream->reader_->AddStreamType(static_cast<INPUTSTREAM_INFO::STREAM_TYPE>(i), m_IncludedStreams[i]);
            if(stream->reader_->GetInformation(m_session->GetStream(m_IncludedStreams[i])->info_))
              m_session->CheckChange(true);
          }
      }

      if (stream->reader_->GetInformation(stream->info_))
        m_session->CheckChange(true);

      return;
    }
    else if (stream->enabled)
    {
      if (stream->mainId_)
      {
        Session::STREAM *mainStream(m_session->GetStream(stream->mainId_));
        if (mainStream->reader_)
          mainStream->reader_->RemoveStreamType(stream->info_.m_streamType);
      }
      const adaptive::AdaptiveTree::Representation *rep(stream->stream_.getRepresentation());
      if (rep->flags_ & adaptive::AdaptiveTree::Representation::INCLUDEDSTREAM)
        m_IncludedStreams[stream->info_.m_streamType] = 0;
      stream->disable();
      return stream->disable();
    }
  }

  int ReadStream(unsigned char*, unsigned int)
  {
    return -1;
  }

  int64_t SeekStream(int64_t, int)
  {
    return -1;
  }

  int64_t PositionStream(void)
  {
    return -1;
  }

  int64_t LengthStream(void)
  {
    return -1;
  }

  void DemuxReset(void)
  {
  }

  void DemuxAbort(void)
  {
  }

  void DemuxFlush(void)
  {
  }

/*****************************************************************************************************/

  DemuxPacket* __cdecl DemuxRead(void)
  {
    if (!m_session)
      return NULL;

    SampleReader *sr(m_session->GetNextSample());

    if (m_session->CheckChange())
    {
      DemuxPacket *p = ipsh->AllocateDemuxPacket(0);
      p->iStreamId = DMX_SPECIALID_STREAMCHANGE;
      xbmc->Log(ADDON::LOG_DEBUG, "DMX_SPECIALID_STREAMCHANGE");
      return p;
    }

    if (sr)
    {
      DemuxPacket *p = ipsh->AllocateDemuxPacket(sr->GetSampleDataSize());
      p->dts = static_cast<double>(sr->DTS());
      p->pts = static_cast<double>(sr->PTS());
      p->duration = static_cast<double>(sr->GetDuration());
      p->iStreamId = sr->GetStreamId();
      p->iGroupId = 0;
      p->iSize = sr->GetSampleDataSize();
      memcpy(p->pData, sr->GetSampleData(), p->iSize);

      //xbmc->Log(ADDON::LOG_DEBUG, "DTS: %0.4f, PTS:%0.4f, ID: %u SZ: %d", p->dts, p->pts, p->iStreamId, p->iSize);

      sr->ReadSample();
      return p;
    }
    return NULL;
  }

  // Accurate search (PTS based)
  bool DemuxSeekTime(double time, bool backwards, double *startpts)
  {
    return true;
  }

  void DemuxSetSpeed(int speed)
  {

  }

  //callback - will be called from kodi
  void SetVideoResolution(int width, int height)
  {
    xbmc->Log(ADDON::LOG_INFO, "SetVideoResolution (%d x %d)", width, height);
    if (m_session)
      m_session->SetVideoResolution(width, height);
    else
    {
      m_width = width;
      m_height = height;
    }
  }

  int GetTotalTime()
  {
    if (!m_session)
      return 0;

    return static_cast<int>(m_session->GetTotalTimeMs());
  }

  int GetTime()
  {
    if (!m_session)
      return 0;

    return static_cast<int>(m_session->GetElapsedTimeMs());
  }

  bool PosTime(int ms)
  {
    if (!m_session)
      return false;

    xbmc->Log(ADDON::LOG_INFO, "PosTime (%d)", ms);

    return m_session->SeekTime(static_cast<double>(ms) * 0.001f, 0, false);
  }

  bool CanPauseStream(void)
  {
    return true;
  }

  bool CanSeekStream(void)
  {
    return true;
  }

  void SetSpeed(int)
  {
  }

  void PauseStream(double)
  {
  }

  bool IsRealTimeStream()
  {
    return m_session && m_session->IsLive();
  }

}//extern "C"