/*
 *      Copyright (C) 2010 Alwin Esch (Team XBMC)
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
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/types.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/timeb.h>
#include <sys/wait.h>
#include <sys/signal.h>
#include <sys/resource.h>

#include <vdr/plugin.h>
#include <vdr/device.h>
#include <vdr/channels.h>

#include "connection.h"
#include "receiver.h"
#include "server.h"
#include "vnsicommand.h"
#include "recplayer.h"
#include "responsepacket.h"
#include "hash.h"

cConnection::cConnection(cServer *server, int fd, unsigned int id, const char *ClientAdr)
{
  m_Id                      = id;
  m_server                  = server;
  m_Streamer                = NULL;
  m_isStreaming             = false;
  m_ClientAddress           = ClientAdr;
  m_StatusInterfaceEnabled  = false;
  m_OSDInterfaceEnabled     = false;
  m_RecPlayer               = NULL;

  m_socket.set_handle(fd);

  Start();
}

cConnection::~cConnection()
{
  DEBUGLOG("%s", __FUNCTION__);
  StopChannelStreaming();
  m_socket.close(); // force closing connection
  DEBUGLOG("stopping cConnection thread ...");
  Cancel(10);
  DEBUGLOG("done");
}

void cConnection::Action(void)
{
  uint32_t kaTimeStamp;
  uint32_t channelID;
  uint32_t requestID;
  uint32_t opcode;
  uint32_t dataLength;
  uint8_t* data;

  while (Running())
  {
    if (!m_socket.read((uint8_t*)&channelID, sizeof(uint32_t))) break;
    channelID = ntohl(channelID);

    if (channelID == 1)
    {
      if (!m_socket.read((uint8_t*)&requestID, sizeof(uint32_t), 10000)) break;
      requestID = ntohl(requestID);

      if (!m_socket.read((uint8_t*)&opcode, sizeof(uint32_t), 10000)) break;
      opcode = ntohl(opcode);

      if (!m_socket.read((uint8_t*)&dataLength, sizeof(uint32_t), 10000)) break;
      dataLength = ntohl(dataLength);
      if (dataLength > 200000) // a random sanity limit
      {
        ERRORLOG("dataLength > 200000!");
        break;
      }

      if (dataLength)
      {
        data = (uint8_t*)malloc(dataLength);
        if (!data)
        {
          ERRORLOG("Extra data buffer malloc error");
          break;
        }

        if (!m_socket.read(data, dataLength, 10000))
        {
          ERRORLOG("Could not read data");
          free(data);
          break;
        }
      }
      else
      {
        data = NULL;
      }

      DEBUGLOG("Received chan=%u, ser=%u, op=%u, edl=%u", channelID, requestID, opcode, dataLength);

      if (!m_loggedIn && (opcode != 1))
      {
        ERRORLOG("Not logged in and opcode != 1");
        if (data) free(data);
        break;
      }

      /* Handle channel open and close inside this thread */
      if (opcode == VNSI_CHANNELSTREAM_OPEN)
      {
        cResponsePacket *resp = new cResponsePacket();
        if (!resp->init(requestID))
        {
          ERRORLOG("response packet init fail");
          delete resp;
          continue;
        }

        uint32_t uid = ntohl(*(uint32_t*)&data[0]);
        free(data);

        if (m_isStreaming)
          StopChannelStreaming();

        Channels.Lock(false);
        const cChannel *channel = NULL;

        // try to find channel by uid first
        channel = FindChannelByUID(uid);

        // try channelnumber
        if (channel == NULL)
          channel = Channels.GetByNumber(uid);

        if (channel != NULL)
        {
          if (StartChannelStreaming(channel, resp))
          {
            INFOLOG("Started streaming of channel %s", channel->Name());
            Channels.Unlock();
            continue;
          }
          else
          {
            DEBUGLOG("Can't stream channel %s", channel->Name());
            resp->add_U32(VNSI_RET_DATALOCKED);
          }
        }
        else
        {
          ERRORLOG("Can't find channel %08x", uid);
          resp->add_U32(VNSI_RET_DATAINVALID);
        }

        Channels.Unlock();
        resp->finalise();
        m_socket.write(resp->getPtr(), resp->getLen());
      }
      else if (opcode == VNSI_CHANNELSTREAM_CLOSE)
      {
        if (m_isStreaming)
          StopChannelStreaming();
      }
      else
      {
        cRequestPacket* req = new cRequestPacket(requestID, opcode, data, dataLength, this);
        m_cmdcontrol.recvRequest(req);
      }
    }
    else if (channelID == 3)
    {
      if (!m_socket.read((uint8_t*)&kaTimeStamp, sizeof(uint32_t), 1000)) break;
      kaTimeStamp = ntohl(kaTimeStamp);

      uint8_t buffer[8];
      *(uint32_t*)&buffer[0] = htonl(3); // KA CHANNEL
      *(uint32_t*)&buffer[4] = htonl(kaTimeStamp);
      if (!m_socket.write(buffer, 8))
      {
        ERRORLOG("Could not send back KA reply");
        break;
      }
    }
    else
    {
      ERRORLOG("Incoming channel number unknown");
      break;
    }
  }

  /* If thread is ended due to closed connection delete a
     possible running stream here */
  StopChannelStreaming();
}

bool cConnection::StartChannelStreaming(const cChannel *channel, cResponsePacket *resp)
{
  m_Streamer    = new cLiveStreamer;
  m_isStreaming = m_Streamer->StreamChannel(channel, 50, &m_socket, resp);
  return m_isStreaming;
}

void cConnection::StopChannelStreaming()
{
  m_isStreaming = false;
  if (m_Streamer)
  {
    delete m_Streamer;
    m_Streamer = NULL;
  }
}

void cConnection::TimerChange(const cTimer *Timer, eTimerChange Change)
{
  TimerChange();
}

void cConnection::TimerChange()
{
  if (m_StatusInterfaceEnabled)
  {
    cResponsePacket *resp = new cResponsePacket();
    if (!resp->initStatus(VNSI_STATUS_TIMERCHANGE))
    {
      delete resp;
      return;
    }

    resp->finalise();
    m_socket.write(resp->getPtr(), resp->getLen());
    delete resp;
  }
}

void cConnection::ChannelChange()
{
  if (!m_StatusInterfaceEnabled)
    return;

  cResponsePacket *resp = new cResponsePacket();
  if (!resp->initStatus(VNSI_STATUS_CHANNELCHANGE))
  {
    delete resp;
    return;
  }

  resp->finalise();
  m_socket.write(resp->getPtr(), resp->getLen());
  delete resp;
}

void cConnection::RecordingsChange()
{
  if (!m_StatusInterfaceEnabled)
    return;

  cResponsePacket *resp = new cResponsePacket();
  if (!resp->initStatus(VNSI_STATUS_RECORDINGSCHANGE))
  {
    delete resp;
    return;
  }

  resp->finalise();
  m_socket.write(resp->getPtr(), resp->getLen());
  delete resp;
}

//void cConnection::ChannelSwitch(const cDevice *Device, int ChannelNumber)
//{
//
//}

void cConnection::Recording(const cDevice *Device, const char *Name, const char *FileName, bool On)
{
  if (m_StatusInterfaceEnabled)
  {
    cResponsePacket *resp = new cResponsePacket();
    if (!resp->initStatus(VNSI_STATUS_RECORDING))
    {
      delete resp;
      return;
    }

    resp->add_U32(Device->CardIndex());
    resp->add_U32(On);
    if (Name)
      resp->add_String(Name);
    else
      resp->add_String("");

    if (FileName)
      resp->add_String(FileName);
    else
      resp->add_String("");

    resp->finalise();
    m_socket.write(resp->getPtr(), resp->getLen());
    delete resp;
  }
}

//void cConnection::Replaying(const cControl *Control, const char *Name, const char *FileName, bool On)
//{
//
//}
//
//void cConnection::SetVolume(int Volume, bool Absolute)
//{
//
//}
//
//void cConnection::SetAudioTrack(int Index, const char * const *Tracks)
//{
//
//}
//
//void cConnection::SetAudioChannel(int AudioChannel)
//{
//
//}
//
//void cConnection::SetSubtitleTrack(int Index, const char * const *Tracks)
//{
//
//}
//
//void cConnection::OsdClear(void)
//{
//
//}
//
//void cConnection::OsdTitle(const char *Title)
//{
//
//}

void cConnection::OsdStatusMessage(const char *Message)
{
  if (m_StatusInterfaceEnabled && Message)
  {
    /* Ignore this messages */
    if (strcasecmp(Message, trVDR("Channel not available!")) == 0) return;
    else if (strcasecmp(Message, trVDR("Delete timer?")) == 0) return;
    else if (strcasecmp(Message, trVDR("Delete recording?")) == 0) return;
    else if (strcasecmp(Message, trVDR("Press any key to cancel shutdown")) == 0) return;
    else if (strcasecmp(Message, trVDR("Press any key to cancel restart")) == 0) return;
    else if (strcasecmp(Message, trVDR("Editing - shut down anyway?")) == 0) return;
    else if (strcasecmp(Message, trVDR("Recording - shut down anyway?")) == 0) return;
    else if (strcasecmp(Message, trVDR("shut down anyway?")) == 0) return;
    else if (strcasecmp(Message, trVDR("Recording - restart anyway?")) == 0) return;
    else if (strcasecmp(Message, trVDR("Editing - restart anyway?")) == 0) return;
    else if (strcasecmp(Message, trVDR("Delete channel?")) == 0) return;
    else if (strcasecmp(Message, trVDR("Timer still recording - really delete?")) == 0) return;
    else if (strcasecmp(Message, trVDR("Delete marks information?")) == 0) return;
    else if (strcasecmp(Message, trVDR("Delete resume information?")) == 0) return;
    else if (strcasecmp(Message, trVDR("CAM is in use - really reset?")) == 0) return;
    else if (strcasecmp(Message, trVDR("Really restart?")) == 0) return;
    else if (strcasecmp(Message, trVDR("Stop recording?")) == 0) return;
    else if (strcasecmp(Message, trVDR("Cancel editing?")) == 0) return;
    else if (strcasecmp(Message, trVDR("Cutter already running - Add to cutting queue?")) == 0) return;
    else if (strcasecmp(Message, trVDR("No index-file found. Creating may take minutes. Create one?")) == 0) return;

    cResponsePacket *resp = new cResponsePacket();
    if (!resp->initStatus(VNSI_STATUS_MESSAGE))
    {
      delete resp;
      return;
    }

    resp->add_U32(0);
    resp->add_String(Message);
    resp->finalise();
    m_socket.write(resp->getPtr(), resp->getLen());
    delete resp;
  }
}

//void cConnection::OsdHelpKeys(const char *Red, const char *Green, const char *Yellow, const char *Blue)
//{
//
//}
//
//void cConnection::OsdItem(const char *Text, int Index)
//{
//
//}
//
//void cConnection::OsdCurrentItem(const char *Text)
//{
//
//}
//
//void cConnection::OsdTextItem(const char *Text, bool Scroll)
//{
//
//}
//
//void cConnection::OsdChannel(const char *Text)
//{
//
//}
//
//void cConnection::OsdProgramme(time_t PresentTime, const char *PresentTitle, const char *PresentSubtitle, time_t FollowingTime, const char *FollowingTitle, const char *FollowingSubtitle)
//{
//
//}
