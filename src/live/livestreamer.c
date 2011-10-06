/*
 *      vdr-plugin-xvdr - XBMC server plugin for VDR
 *
 *      Copyright (C) 2010 Alwin Esch (Team XBMC)
 *      Copyright (C) 2010, 2011 Alexander Pipelka
 *
 *      http://www.xbmc.org
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
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <map>
#include <vdr/remux.h>
#include <vdr/channels.h>
#include <asm/byteorder.h>

#include "config/config.h"
#include "net/cxsocket.h"
#include "net/responsepacket.h"
#include "xvdr/xvdrcommand.h"

#include "livestreamer.h"
#include "livepatfilter.h"
#include "livereceiver.h"

cLiveStreamer::cLiveStreamer(uint32_t timeout)
 : cThread("cLiveStreamer stream processor")
 , cRingBufferLinear(MEGABYTE(1), TS_SIZE, true)
 , m_scanTimeout(timeout)
{
  m_Channel         = NULL;
  m_Priority        = 0;
  m_Socket          = NULL;
  m_Device          = NULL;
  m_Receiver        = NULL;
  m_PatFilter       = NULL;
  m_Frontend        = -1;
  m_NumStreams      = 0;
  m_streamReady     = false;
  m_IsAudioOnly     = false;
  m_IsMPEGPS        = false;
  m_startup         = true;
  m_SignalLost      = false;
  m_IFrameSeen      = false;

  m_requestStreamChange = false;

  m_packetEmpty = new cResponsePacket;
  m_packetEmpty->initStream(XVDR_STREAM_MUXPKT, 0, 0, 0, 0);

  memset(&m_FrontendInfo, 0, sizeof(m_FrontendInfo));
  for (int idx = 0; idx < MAXRECEIVEPIDS; ++idx)
  {
    m_Streams[idx] = NULL;
    m_Pids[idx]    = 0;
  }

  if(m_scanTimeout == 0)
    m_scanTimeout = XVDRServerConfig.stream_timeout;

  SetTimeouts(0, 100);
}

cLiveStreamer::~cLiveStreamer()
{
  DEBUGLOG("Started to delete live streamer");

  Cancel(-1);

  if (m_Device)
  {
    if (m_Receiver)
    {
      DEBUGLOG("Detaching Live Receiver");
      m_Device->Detach(m_Receiver);
    }
    else
    {
      DEBUGLOG("No live receiver present");
    }

    if (m_PatFilter)
    {
      DEBUGLOG("Detaching Live Filter");
      m_Device->Detach(m_PatFilter);
    }
    else
    {
      DEBUGLOG("No live filter present");
    }

    for (int idx = 0; idx < MAXRECEIVEPIDS; ++idx)
    {
      if (m_Streams[idx])
      {
        DEBUGLOG("Deleting stream demuxer %i for pid=%i and type=%i", m_Streams[idx]->GetStreamIndex(), m_Streams[idx]->GetPID(), m_Streams[idx]->Type());
        DELETENULL(m_Streams[idx]);
        m_Pids[idx] = 0;
      }
    }

    if (m_Receiver)
    {
      DEBUGLOG("Deleting Live Receiver");
      DELETENULL(m_Receiver);
    }

    if (m_PatFilter)
    {
      DEBUGLOG("Deleting Live Filter");
      DELETENULL(m_PatFilter);
    }
  }
  if (m_Frontend >= 0)
  {
    close(m_Frontend);
    m_Frontend = -1;
  }

  delete m_packetEmpty;

  DEBUGLOG("Finished to delete live streamer");
}

void cLiveStreamer::RequestStreamChange()
{
  m_requestStreamChange = true;
}

void cLiveStreamer::Action(void)
{
  int size              = 0;
  int used              = 0;
  unsigned char *buf    = NULL;
  m_startup             = true;

  cTimeMs last_info;
  last_info.Set(0);

  while (Running())
  {
    size = 0;
    used = 0;
    buf = Get(size);

    if (!m_Receiver->IsAttached())
    {
      INFOLOG("returning from streamer thread, receiver is no more attached");
      break;
    }

    if(!IsStarting() && (m_last_tick.Elapsed() > (uint64_t)(m_scanTimeout*1000)) && !m_SignalLost)
    {
      INFOLOG("timeout. signal lost!");
      sendStatus(XVDR_STREAM_STATUS_SIGNALLOST);
      m_SignalLost = true;
    }

    // no data
    if (buf == NULL || size <= TS_SIZE)
      continue;

    /* Make sure we are looking at a TS packet */
    while (size > TS_SIZE)
    {
      if (buf[0] == TS_SYNC_BYTE && buf[TS_SIZE] == TS_SYNC_BYTE)
        break;
      used++;
      buf++;
      size--;
    }

    while (size >= TS_SIZE)
    {
      if(!Running())
      {
        break;
      }

      unsigned int ts_pid = TsPid(buf);

      m_FilterMutex.Lock();
      cTSDemuxer *demuxer = FindStreamDemuxer(ts_pid);
      if (demuxer)
      {
        demuxer->ProcessTSPacket(buf);
      }
      m_FilterMutex.Unlock();

      buf += TS_SIZE;
      size -= TS_SIZE;
      used += TS_SIZE;
    }
    Del(used);

    if(last_info.Elapsed() >= 10*1000 && IsReady())
    {
      last_info.Set(0);
      sendStreamInfo();
      sendSignalInfo();
    }
  }
}

bool cLiveStreamer::StreamChannel(const cChannel *channel, int priority, cxSocket *Socket, cResponsePacket *resp)
{
  if (channel == NULL)
  {
    ERRORLOG("Starting streaming of channel without valid channel");
    return false;
  }

  m_Channel   = channel;
  m_Priority  = priority;
  m_Socket    = Socket;
  m_Device    = cDevice::GetDevice(channel, m_Priority, true);

  INFOLOG("--------------------------------------");
  INFOLOG("Channel streaming request: %i - %s", m_Channel->Number(), m_Channel->Name());

  if (m_Device != NULL)
  {
    INFOLOG("Found available device %d", m_Device->CardIndex() + 1);

    if (m_Device->SwitchChannel(m_Channel, false))
    {
      if (m_Channel->Vpid())
      {
#if APIVERSNUM >= 10701
        if (m_Channel->Vtype() == 0x1B)
          m_Streams[m_NumStreams] = new cTSDemuxer(this, m_NumStreams, stH264, m_Channel->Vpid());
        else
#endif
          m_Streams[m_NumStreams] = new cTSDemuxer(this, m_NumStreams, stMPEG2VIDEO, m_Channel->Vpid());

        m_Pids[m_NumStreams] = m_Channel->Vpid();
        m_NumStreams++;
      }
      else
      {
        m_IsAudioOnly = true;
      }

      const int *APids = m_Channel->Apids();
      int index = 0;
      for ( ; *APids && m_NumStreams < MAXRECEIVEPIDS; APids++)
      {
        if (FindStreamDemuxer(*APids) == NULL)
        {
          m_Pids[m_NumStreams]    = *APids;
          m_Streams[m_NumStreams] = new cTSDemuxer(this, m_NumStreams, stMPEG2AUDIO, *APids);
          m_Streams[m_NumStreams]->SetLanguage(m_Channel->Alang(index));
          m_NumStreams++;
        }
        index++;
      }

      const int *DPids = m_Channel->Dpids();
      index = 0;
      for ( ; *DPids && m_NumStreams < MAXRECEIVEPIDS; DPids++)
      {
        if (FindStreamDemuxer(*DPids) == NULL)
        {
          m_Pids[m_NumStreams]    = *DPids;
          m_Streams[m_NumStreams] = new cTSDemuxer(this, m_NumStreams, stAC3, *DPids);
          m_Streams[m_NumStreams]->SetLanguage(m_Channel->Dlang(index));
          m_NumStreams++;
        }
        index++;
      }

      const int *SPids = m_Channel->Spids();
      if (SPids)
      {
        int index = 0;
        for ( ; *SPids && m_NumStreams < MAXRECEIVEPIDS; SPids++)
        {
          if (FindStreamDemuxer(*SPids) == NULL)
          {
            m_Pids[m_NumStreams]    = *SPids;
            m_Streams[m_NumStreams] = new cTSDemuxer(this, m_NumStreams, stDVBSUB, *SPids);
            m_Streams[m_NumStreams]->SetLanguage(m_Channel->Slang(index));
#if APIVERSNUM >= 10709
            m_Streams[m_NumStreams]->SetSubtitlingDescriptor(m_Channel->SubtitlingType(index),
                                                             m_Channel->CompositionPageId(index),
                                                             m_Channel->AncillaryPageId(index));
#endif
            m_NumStreams++;
          }
          index++;
        }
      }

      if (m_Channel->Tpid())
      {
        m_Streams[m_NumStreams] = new cTSDemuxer(this, m_NumStreams, stTELETEXT, m_Channel->Tpid());
        m_Pids[m_NumStreams]    = m_Channel->Tpid();
        cCamSlot* cam = m_Device->CamSlot();
        if(cam != NULL) 
        {
          cam->AddPid(m_Channel->Sid(), m_Channel->Tpid(), 0x06);
        }
        m_NumStreams++;
      }

      m_Streams[m_NumStreams] = NULL;
      m_Pids[m_NumStreams]    = 0;

      /* Send the OK response here, that it is before the Stream end message */
      resp->add_U32(XVDR_RET_OK);
      resp->finalise();
      m_Socket->write(resp->getPtr(), resp->getLen());

      if (m_Channel && ((m_Channel->Source() >> 24) == 'V')) m_IsMPEGPS = true;

      if (m_NumStreams > 0 && m_Socket)
      {
        DEBUGLOG("Creating new live Receiver");
        m_Receiver  = new cLiveReceiver(this, m_Channel->GetChannelID(), m_Priority, m_Pids);
        m_PatFilter = new cLivePatFilter(this, m_Channel);
        m_Device->AttachReceiver(m_Receiver);
        m_Device->AttachFilter(m_PatFilter);
      }

      INFOLOG("Successfully switched to channel %i - %s", m_Channel->Number(), m_Channel->Name());
      return true;
    }
    else
    {
      ERRORLOG("Can't switch to channel %i - %s", m_Channel->Number(), m_Channel->Name());
    }
  }
  else
  {
    ERRORLOG("Can't get device for channel %i - %s", m_Channel->Number(), m_Channel->Name());
  }
  return false;
}

cTSDemuxer *cLiveStreamer::FindStreamDemuxer(int Pid)
{
  int idx;
  for (idx = 0; idx < m_NumStreams; ++idx)
    if (m_Streams[idx] && m_Streams[idx]->GetPID() == Pid)
      return m_Streams[idx];
  return NULL;
}

int cLiveStreamer::HaveStreamDemuxer(int Pid, eStreamType streamType)
{
  int idx;
  for (idx = 0; idx < m_NumStreams; ++idx)
    if (m_Streams[idx] && (Pid == 0 || m_Streams[idx]->GetPID() == Pid) && m_Streams[idx]->Type() == streamType)
      return idx;
  return -1;
}

void cLiveStreamer::Activate(bool On)
{
  if (On)
  {
    DEBUGLOG("VDR active, sending stream start message");
    Start();
  }
  else
  {
    DEBUGLOG("VDR inactive, sending stream end message");
    Cancel(5);
  }
}

void cLiveStreamer::Attach(void)
{
  DEBUGLOG("%s", __FUNCTION__);
  if (m_Device)
  {
    if (m_Receiver)
    {
      m_Device->Detach(m_Receiver);
      m_Device->AttachReceiver(m_Receiver);
    }
  }
}

void cLiveStreamer::Detach(void)
{
  DEBUGLOG("%s", __FUNCTION__);
  if (m_Device)
  {
    if (m_Receiver)
      m_Device->Detach(m_Receiver);
  }
}

void cLiveStreamer::sendStreamPacket(sStreamPacket *pkt)
{
  if(!IsReady() || pkt == NULL || pkt->size == 0)
    return;

  if(!m_IsAudioOnly && !m_IFrameSeen && (pkt->frametype != PKT_I_FRAME))
    return;

  // Send stream information as the first packet on startup
  if (IsStarting() && IsReady())
  {
    INFOLOG("streaming of channel started");
    m_last_tick.Set(0);
    m_requestStreamChange = true;
    m_startup = false;
  }


  // send stream change on demand
  if(m_requestStreamChange)
    sendStreamChange();

  m_IFrameSeen = true;

  // get packet type
  int type = m_Streams[pkt->id]->Type();
  int streamid = m_Streams[pkt->id]->GetPID();

  m_streamHeader.channel  = htonl(XVDR_CHANNEL_STREAM);     // stream channel
  m_streamHeader.opcode   = htonl(XVDR_STREAM_MUXPKT);      // Stream packet operation code
  m_streamHeader.id       = htonl(streamid);                // Stream ID
  m_streamHeader.duration = htonl(pkt->duration);           // Duration

  uint64_t dts = __cpu_to_be64(pkt->dts);
  uint64_t pts = __cpu_to_be64(pkt->pts);
 
  m_streamHeader.dts[1] = (dts & 0x00000000FFFFFFFFLL);       // DTS LOW
  m_streamHeader.dts[0] = (dts & 0xFFFFFFFF00000000LL) >> 32; // DTS HIGH

  m_streamHeader.pts[1] = (pts & 0x00000000FFFFFFFFLL);       // PTS LOW
  m_streamHeader.pts[0] = (pts & 0xFFFFFFFF00000000LL) >> 32; // PTS HIGH

  m_streamHeader.length = htonl(pkt->size);                 // Data length
 
  // if a audio or video packet was sent, the signal is restored
  if(type > stNone && type < stDVBSUB) {
    if(m_SignalLost) {
      INFOLOG("signal restored");
      sendStatus(XVDR_STREAM_STATUS_SIGNALRESTORED);
      m_SignalLost = false;
      m_IFrameSeen = false;
      m_streamReady = false;
      m_requestStreamChange = true;
      return;
    }
    m_last_tick.Set(0);
  }
  else if(m_SignalLost)
    return;

  if(m_SignalLost)
    return;

  DEBUGLOG("sendStreamPacket (type: %i)", type);

  if(m_Socket->write(&m_streamHeader, sizeof(m_streamHeader), 2000, true) > 0)
    m_Socket->write(pkt->data, pkt->size, 2000);
}

void cLiveStreamer::sendStreamChange()
{
  cResponsePacket *resp = new cResponsePacket();
  if (!resp->initStream(XVDR_STREAM_CHANGE, 0, 0, 0, 0))
  {
    ERRORLOG("stream response packet init fail");
    delete resp;
    return;
  }

  DEBUGLOG("sendStreamChange");

  for (int idx = 0; idx < m_NumStreams; ++idx)
  {
    if (m_Streams[idx])
    {
      int streamid = m_Streams[idx]->GetPID();
      resp->add_U32(streamid);
      if (m_Streams[idx]->Type() == stMPEG2AUDIO)
      {
        resp->add_String("MPEG2AUDIO");
        resp->add_String(m_Streams[idx]->GetLanguage());
        DEBUGLOG("MPEG2AUDIO: %i (index: %i) (%s)", streamid, idx, m_Streams[idx]->GetLanguage());
      }
      else if (m_Streams[idx]->Type() == stMPEG2VIDEO)
      {
        resp->add_String("MPEG2VIDEO");
        resp->add_U32(m_Streams[idx]->GetFpsScale());
        resp->add_U32(m_Streams[idx]->GetFpsRate());
        resp->add_U32(m_Streams[idx]->GetHeight());
        resp->add_U32(m_Streams[idx]->GetWidth());
        resp->add_S32((int32_t)(m_Streams[idx]->GetAspect()*1000.0));
        DEBUGLOG("MPEG2VIDEO: %i (index: %i)", streamid, idx);
      }
      else if (m_Streams[idx]->Type() == stAC3)
      {
        resp->add_String("AC3");
        resp->add_String(m_Streams[idx]->GetLanguage());
        DEBUGLOG("AC3: %i (index: %i)", streamid, idx);
      }
      else if (m_Streams[idx]->Type() == stH264)
      {
        resp->add_String("H264");
        resp->add_U32(m_Streams[idx]->GetFpsScale());
        resp->add_U32(m_Streams[idx]->GetFpsRate());
        resp->add_U32(m_Streams[idx]->GetHeight());
        resp->add_U32(m_Streams[idx]->GetWidth());
        resp->add_S32((int32_t)(m_Streams[idx]->GetAspect()*1000.0));
        DEBUGLOG("H264: %i (index: %i)", streamid, idx);
      }
      else if (m_Streams[idx]->Type() == stDVBSUB)
      {
        resp->add_String("DVBSUB");
        resp->add_String(m_Streams[idx]->GetLanguage());
        resp->add_U32(m_Streams[idx]->CompositionPageId());
        resp->add_U32(m_Streams[idx]->AncillaryPageId());
        DEBUGLOG("DVBSUB: %i (index: %i)", streamid, idx);
      }
      else if (m_Streams[idx]->Type() == stTELETEXT)
      {
        resp->add_String("TELETEXT");
        DEBUGLOG("TELETEXT: %i (index: %i)", streamid, idx);
      }
      else if (m_Streams[idx]->Type() == stAAC)
      {
        resp->add_String("AAC");
        resp->add_String(m_Streams[idx]->GetLanguage());
        DEBUGLOG("AAC: %i (index: %i)", streamid, idx);
      }
      else if (m_Streams[idx]->Type() == stEAC3)
      {
        resp->add_String("EAC3");
        resp->add_String(m_Streams[idx]->GetLanguage());
        DEBUGLOG("EAC3: %i (index: %i)", streamid, idx);
      }
      else if (m_Streams[idx]->Type() == stDTS)
      {
        resp->add_String("DTS");
        resp->add_String(m_Streams[idx]->GetLanguage());
        DEBUGLOG("DTS: %i (index: %i)", streamid, idx);
      }
    }
  }

  resp->finaliseStream();

  m_Socket->write(resp->getPtr(), resp->getLen(), -1, true);
  delete resp;

  m_requestStreamChange = false;

  sendStreamInfo();
}

void cLiveStreamer::sendStatus(int status)
{
  cResponsePacket *resp = new cResponsePacket();
  if (!resp->initStream(XVDR_STREAM_STATUS, 0, 0, 0, 0))
  {
    ERRORLOG("stream status packet init fail");
    delete resp;
    return;
  }

  resp->add_U32(status);
  resp->finaliseStream();
  m_Socket->write(resp->getPtr(), resp->getLen(), -1, true);
  delete resp;
}

void cLiveStreamer::sendSignalInfo()
{
  /* If no frontend is found m_Frontend is set to -2, in this case
     return a empty signalinfo package */
  if (m_Frontend == -2)
  {
    cResponsePacket *resp = new cResponsePacket();
    if (!resp->initStream(XVDR_STREAM_SIGNALINFO, 0, 0, 0, 0))
    {
      ERRORLOG("stream response packet init fail");
      delete resp;
      return;
    }

    resp->add_String(*cString::sprintf("Unknown"));
    resp->add_String(*cString::sprintf("Unknown"));
    resp->add_U32(0);
    resp->add_U32(0);
    resp->add_U32(0);
    resp->add_U32(0);

    resp->finaliseStream();
    m_Socket->write(resp->getPtr(), resp->getLen(), -1, true);
    delete resp;
    return;
  }

  if (m_Channel && ((m_Channel->Source() >> 24) == 'V'))
  {
    if (m_Frontend < 0)
    {
      for (int i = 0; i < 8; i++)
      {
        m_DeviceString = cString::sprintf("/dev/video%d", i);
        m_Frontend = open(m_DeviceString, O_RDONLY | O_NONBLOCK);
        if (m_Frontend >= 0)
        {
          if (ioctl(m_Frontend, VIDIOC_QUERYCAP, &m_vcap) < 0)
          {
            ERRORLOG("cannot read analog frontend info.");
            close(m_Frontend);
            m_Frontend = -1;
            memset(&m_vcap, 0, sizeof(m_vcap));
            continue;
          }
          break;
        }
      }
      if (m_Frontend < 0)
        m_Frontend = -2;
    }

    if (m_Frontend >= 0)
    {
      cResponsePacket *resp = new cResponsePacket();
      if (!resp->initStream(XVDR_STREAM_SIGNALINFO, 0, 0, 0, 0))
      {
        ERRORLOG("stream response packet init fail");
        delete resp;
        return;
      }
      resp->add_String(*cString::sprintf("Analog #%s - %s (%s)", *m_DeviceString, (char *) m_vcap.card, m_vcap.driver));
      resp->add_String("");
      resp->add_U32(0);
      resp->add_U32(0);
      resp->add_U32(0);
      resp->add_U32(0);

      resp->finaliseStream();
      m_Socket->write(resp->getPtr(), resp->getLen(), -1, true);
      delete resp;
    }
  }
  else
  {
    if (m_Frontend < 0)
    {
      m_DeviceString = cString::sprintf(FRONTEND_DEVICE, m_Device->CardIndex(), 0);
      m_Frontend = open(m_DeviceString, O_RDONLY | O_NONBLOCK);
      if (m_Frontend >= 0)
      {
        if (ioctl(m_Frontend, FE_GET_INFO, &m_FrontendInfo) < 0)
        {
          ERRORLOG("cannot read frontend info.");
          close(m_Frontend);
          m_Frontend = -2;
          memset(&m_FrontendInfo, 0, sizeof(m_FrontendInfo));
          return;
        }
      }
    }

    if (m_Frontend >= 0)
    {
      cResponsePacket *resp = new cResponsePacket();
      if (!resp->initStream(XVDR_STREAM_SIGNALINFO, 0, 0, 0, 0))
      {
        ERRORLOG("stream response packet init fail");
        delete resp;
        return;
      }

      fe_status_t status;
      uint16_t fe_snr;
      uint16_t fe_signal;
      uint32_t fe_ber;
      uint32_t fe_unc;

      memset(&status, 0, sizeof(status));
      ioctl(m_Frontend, FE_READ_STATUS, &status);

      if (ioctl(m_Frontend, FE_READ_SIGNAL_STRENGTH, &fe_signal) == -1)
        fe_signal = -2;
      if (ioctl(m_Frontend, FE_READ_SNR, &fe_snr) == -1)
        fe_snr = -2;
      if (ioctl(m_Frontend, FE_READ_BER, &fe_ber) == -1)
        fe_ber = -2;
      if (ioctl(m_Frontend, FE_READ_UNCORRECTED_BLOCKS, &fe_unc) == -1)
        fe_unc = -2;

      switch (m_Channel->Source() & cSource::st_Mask)
      {
        case cSource::stSat:
          resp->add_String(*cString::sprintf("DVB-S%s #%d - %s", (m_FrontendInfo.caps & 0x10000000) ? "2" : "",  cDevice::ActualDevice()->CardIndex(), m_FrontendInfo.name));
          break;
        case cSource::stCable:
          resp->add_String(*cString::sprintf("DVB-C #%d - %s", cDevice::ActualDevice()->CardIndex(), m_FrontendInfo.name));
          break;
        case cSource::stTerr:
          resp->add_String(*cString::sprintf("DVB-T #%d - %s", cDevice::ActualDevice()->CardIndex(), m_FrontendInfo.name));
          break;
      }
      resp->add_String(*cString::sprintf("%s:%s:%s:%s:%s", (status & FE_HAS_LOCK) ? "LOCKED" : "-", (status & FE_HAS_SIGNAL) ? "SIGNAL" : "-", (status & FE_HAS_CARRIER) ? "CARRIER" : "-", (status & FE_HAS_VITERBI) ? "VITERBI" : "-", (status & FE_HAS_SYNC) ? "SYNC" : "-"));
      resp->add_U32(fe_snr);
      resp->add_U32(fe_signal);
      resp->add_U32(fe_ber);
      resp->add_U32(fe_unc);

      resp->finaliseStream();

      DEBUGLOG("sendSignalInfo");

      m_Socket->write(resp->getPtr(), resp->getLen(), -1, true);
      delete resp;
    }
  }
}

void cLiveStreamer::sendStreamInfo()
{
  if(m_NumStreams == 0)
  {
    return;
  }

  cResponsePacket *resp = new cResponsePacket();
  if (!resp->initStream(XVDR_STREAM_CONTENTINFO, 0, 0, 0, 0))
  {
    ERRORLOG("stream response packet init fail");
    delete resp;
    return;
  }

  for (int idx = 0; idx < m_NumStreams; ++idx)
  {
    if (m_Streams[idx])
    {
      if (m_Streams[idx]->Type() == stMPEG2AUDIO ||
          m_Streams[idx]->Type() == stAC3 ||
          m_Streams[idx]->Type() == stEAC3 ||
          m_Streams[idx]->Type() == stDTS ||
          m_Streams[idx]->Type() == stAAC)
      {
        resp->add_U32(m_Streams[idx]->GetPID());
        resp->add_String(m_Streams[idx]->GetLanguage());
        resp->add_U32(m_Streams[idx]->GetChannels());
        resp->add_U32(m_Streams[idx]->GetSampleRate());
        resp->add_U32(m_Streams[idx]->GetBlockAlign());
        resp->add_U32(m_Streams[idx]->GetBitRate());
        resp->add_U32(m_Streams[idx]->GetBitsPerSample());
      }
      else if (m_Streams[idx]->Type() == stMPEG2VIDEO || m_Streams[idx]->Type() == stH264)
      {
        resp->add_U32(m_Streams[idx]->GetPID());
        resp->add_U32(m_Streams[idx]->GetFpsScale());
        resp->add_U32(m_Streams[idx]->GetFpsRate());
        resp->add_U32(m_Streams[idx]->GetHeight());
        resp->add_U32(m_Streams[idx]->GetWidth());
        resp->add_S32((int32_t)(m_Streams[idx]->GetAspect()*1000.0));
      }
      else if (m_Streams[idx]->Type() == stDVBSUB)
      {
        resp->add_U32(m_Streams[idx]->GetPID());
        resp->add_String(m_Streams[idx]->GetLanguage());
        resp->add_U32(m_Streams[idx]->CompositionPageId());
        resp->add_U32(m_Streams[idx]->AncillaryPageId());
      }
    }
  }

  resp->finaliseStream();

  DEBUGLOG("sendStreamInfo");

  m_Socket->write(resp->getPtr(), resp->getLen());
  delete resp;
}
