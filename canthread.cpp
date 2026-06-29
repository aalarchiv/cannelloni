/*
 * This file is part of cannelloni, a SocketCAN over ethernet tunnel.
 *
 * Copyright (C) 2014-2017 Maximilian Güntner <code@sourcediver.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>

#include <linux/can/raw.h>
#include <net/if.h>
#include <sys/ioctl.h>

#include "canthread.h"
#include "cannelloni.h"
#include "logging.h"
#include "router.h"

using namespace cannelloni;

CANThread::CANThread(const struct debugOptions_t &debugOptions, const std::string &canInterfaceName)
  : ConnectionThread()
  , m_canSocket(0)
  , m_canfd(false)
  , m_canInterfaceName(canInterfaceName)
  , m_rxCount(0)
  , m_txCount(0)
  , m_rxDropCount(0)
{
  memcpy(&m_debugOptions, &debugOptions, sizeof(struct debugOptions_t));
}

CANThread::~CANThread() {}

int CANThread::start() {
  struct ifreq canInterface;
  uint32_t canfd_on = 1;
  /* Setup our socket */
  m_canSocket = socket(PF_CAN, SOCK_RAW, CAN_RAW);
  if (m_canSocket < 0) {
    lerror << "socket Error" << std::endl;
    return -1;
  }
  /* Determine the index of m_canInterfaceName */
  strncpy(canInterface.ifr_name, m_canInterfaceName.c_str(), IFNAMSIZ-1);
  canInterface.ifr_name[IF_NAMESIZE-1] = '\0';
  if (ioctl(m_canSocket, SIOCGIFINDEX, &canInterface) < 0) {
    lerror << "Could get index of interface >" << m_canInterfaceName << "<" << std::endl;
    return -1;
  }
  struct sockaddr_can localAddr;
  memset(&localAddr, 0, sizeof(localAddr));
  localAddr.can_ifindex = canInterface.ifr_ifindex;
  localAddr.can_family = AF_CAN;
  /* Check MTU of interface */
  if (ioctl(m_canSocket, SIOCGIFMTU, &canInterface) < 0) {
    lerror << "Could get MTU of interface >" << m_canInterfaceName << "<" <<  std::endl;
  }
  /* Check whether CAN_FD is possible */
  if (canInterface.ifr_mtu == CANFD_MTU) {
    /* Try to switch into CAN_FD mode */
    if (setsockopt(m_canSocket, SOL_CAN_RAW, CAN_RAW_FD_FRAMES, &canfd_on, sizeof(canfd_on))) {
      lerror << "Could not enable CAN_FD." << std::endl;
    } else {
      m_canfd = true;
    }

  } else {
    lerror << "CAN_FD is not supported on >" << m_canInterfaceName << "<" << std::endl;
  }

  /*
   * As a hub fans every ingested CAN frame out to N peers inline before the
   * next recv(), the CAN RX critical path lengthens with the peer count and the
   * kernel socket receive buffer can overflow under high bus load = silent
   * frame loss at ingress, distinct from the bounded drop-oldest at egress
   * (epic caveat 8). Mitigate by requesting a larger receive buffer (the kernel
   * clamps to net.core.rmem_max), and enable SO_RXQ_OVFL so any overflow is
   * surfaced via ancillary data on recvmsg() instead of being lost silently.
   */
  int rcvbuf = CAN_RX_BUFFER_BYTES;
  if (setsockopt(m_canSocket, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0)
    lwarn << "Could not enlarge CAN socket receive buffer." << std::endl;
  int rxq_ovfl = 1;
  if (setsockopt(m_canSocket, SOL_SOCKET, SO_RXQ_OVFL, &rxq_ovfl, sizeof(rxq_ovfl)) < 0)
    lwarn << "Could not enable SO_RXQ_OVFL on the CAN socket; ingress drops will not be reported." << std::endl;

  if (bind(m_canSocket, (struct sockaddr *)&localAddr, sizeof(localAddr)) < 0) {
    lerror << "Could not bind to interface" << std::endl;
    return -1;
  }

  return Thread::start();
}

void CANThread::stop() {
  Thread::stop();
  fireTimer();
}

void CANThread::run() {
  fd_set readfds;
  ssize_t receivedBytes;

  linfo << "CANThread up and running" << std::endl;

  m_timer.adjust(CAN_TIMEOUT, CAN_TIMEOUT);

  while (m_started) {
    /* Prepare readfds */
    FD_ZERO(&readfds);
    FD_SET(m_canSocket, &readfds);
    FD_SET(m_timer.getFd(), &readfds);

    int ret = select(std::max(m_canSocket,m_timer.getFd())+1, &readfds, NULL, NULL, NULL);
    if (ret < 0) {
      lerror << "select error" << std::endl;
      break;
    }
    if (FD_ISSET(m_timer.getFd(), &readfds)) {
      if (m_timer.read() > 0) {
        /* We transmit our buffer */
        if (m_frameBuffer->getFrameBufferSize())
          transmitBuffer();
      }
    }
    if (FD_ISSET(m_canSocket, &readfds)) {
      /* Read into an ingress-local frame; the Router copies it into each
       * target's egress pool (the frame is never shared between buffers).
       * recvmsg (not recv) so the SO_RXQ_OVFL ancillary counter rides along:
       * it tells us how many frames the kernel dropped at ingress while the
       * inline fan-out kept this loop from draining the socket (caveat 8). */
      struct canfd_frame frame;
      struct iovec iov = { &frame, sizeof(frame) };
      char ctrl[CMSG_SPACE(sizeof(uint32_t))];
      struct msghdr msg;
      memset(&msg, 0, sizeof(msg));
      msg.msg_iov = &iov;
      msg.msg_iovlen = 1;
      msg.msg_control = ctrl;
      msg.msg_controllen = sizeof(ctrl);
      receivedBytes = recvmsg(m_canSocket, &msg, 0);
      if (receivedBytes < 0) {
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
          /* Timeout occurred */
          continue;
        } else {
          lerror << "CAN read error" << std::endl;
          break;
        }
      } else if (receivedBytes == CAN_MTU || receivedBytes == CANFD_MTU) {
        m_rxCount++;
        /* Surface any ingress overrun reported alongside this frame. The
         * counter is cumulative since socket creation, so log the delta. */
        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {
          if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_RXQ_OVFL) {
            uint32_t dropped;
            memcpy(&dropped, CMSG_DATA(cmsg), sizeof(dropped));
            if (dropped > m_rxDropCount) {
              lwarn << "CAN ingress overrun: kernel dropped "
                    << (dropped - m_rxDropCount) << " frame(s) (total " << dropped
                    << ") -- socket RX buffer overflowed under load." << std::endl;
              m_rxDropCount = dropped;
            }
          }
        }
        /* If it is a CAN FD frame, encode this in len */
        if (receivedBytes == CANFD_MTU) {
          frame.len |= CANFD_FRAME;
        } else {
          frame.len &= ~(CANFD_FRAME);
        }
        m_router->route(&frame, m_selfId);
        if (m_debugOptions.can) {
          printCANInfo(&frame);
        }
      } else {
        lwarn << "Incomplete/Invalid CAN frame" << std::endl;
      }
    }
  }
  if (m_debugOptions.buffer) {
    m_frameBuffer->debug();
  }
  linfo << "Shutting down. CAN Transmission Summary: TX: " << m_txCount << " RX: " << m_rxCount
        << " ingress drops: " << m_rxDropCount << std::endl;
  shutdown(m_canSocket, SHUT_RDWR);
  close(m_canSocket);
}

void CANThread::transmitFrame(canfd_frame* frame, PeerId /*target*/) {
  /* There is exactly one CAN participant, so the target id is irrelevant. */
  m_frameBuffer->insertFrame(frame);
  fireTimer();
}

void CANThread::transmitBuffer() {
  ssize_t transmittedBytes = 0;
  /* Loop here until buffer is empty or we cannot write anymore */
  while(1) {
    canfd_frame *frame = m_frameBuffer->requestBufferFront();
    bool frameIsCANFD = false;
    if (frame == NULL)
      break;
    /* Check whether we are operating on a CAN FD socket */
    if (m_canfd) {
      if (frame->len & CANFD_FRAME) {
        frameIsCANFD = true;
        /* Clear the CANFD_FRAME bit in len */
        frame->len &= ~(CANFD_FRAME);
        transmittedBytes = write(m_canSocket, frame, CANFD_MTU);
      } else {
        frame->len &= ~(CANFD_FRAME);
        transmittedBytes = write(m_canSocket, frame, CAN_MTU);
      }
    } else {
      /* First check the length of the frame */
      if (frame->len & CANFD_FRAME) {
        /* Something is wrong with the setup */
        lwarn << "Received a CAN FD for a socket that only supports (CAN 2.0)." << std::endl;
        frame->len &= ~(CANFD_FRAME);
        m_frameBuffer->insertFramePool(frame);
        continue;
      } else {
        /* No CAN FD socket, use legacy MTU */
        transmittedBytes = write(m_canSocket, frame, CAN_MTU);
      }
    }
    if (transmittedBytes == CANFD_MTU || transmittedBytes == CAN_MTU) {
      /* Put frame back into pool */
      m_frameBuffer->insertFramePool(frame);
      m_txCount++;
    } else {
      /* If it was a CAN FD frame, encode this in len again before putting it back into buffer */
      if (frameIsCANFD) {
        frame->len |= CANFD_FRAME;
      }
      /* Put frame back into buffer */
      m_frameBuffer->returnFrame(frame);
      /* Revisit this function after 25 us */
      m_timer.adjust(CAN_TIMEOUT, 25);
      if (m_debugOptions.can)
        linfo << "CAN write failed." << std::endl;
      break;
    }
  }
}

void CANThread::fireTimer() {
  /* Instant expiry (so 1us) */
  m_timer.adjust(CAN_TIMEOUT, 1);
}
