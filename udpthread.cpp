/*
 * This file is part of cannelloni, a SocketCAN over Ethernet tunnel.
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

#include <cstdint>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/timerfd.h>
#include <sys/socket.h>

#include <net/if.h>
#include <arpa/inet.h>

#include "inet_address.h"
#include "udpthread.h"
#include "logging.h"
#include "make_unique.h"
#include "parser.h"
#include "peerregistry.h"
#include "router.h"

/* Monotonic clock for peer-liveness bookkeeping (immune to wall-clock steps). */
static uint64_t nowMonoNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

UDPThread::UDPThread(const struct debugOptions_t &debugOptions,
                     const struct UDPThreadParams &params)
  : ConnectionThread()
  , m_sort(params.sortFrames)
  , m_checkPeer(params.checkPeer)
  , m_socket(0)
  , m_addressFamily(params.addressFamily)
  , m_sequenceNumber(0)
  , m_timeout(100)
  , m_rxCount(0)
  , m_txCount(0)
{
  memcpy(&m_debugOptions, &debugOptions, sizeof(struct debugOptions_t));
  memcpy(&m_remoteAddr, &params.remoteAddr, sizeof(struct sockaddr_storage));
  memcpy(&m_localAddr, &params.localAddr, sizeof(struct sockaddr_storage));

  m_linkMtuSize = params.linkMtuSize;

  if (params.addressFamily == AF_INET) {
    m_payloadSize = m_linkMtuSize - IPv4_HEADER_SIZE - UDP_HEADER_SIZE;
  } else {
    m_payloadSize = m_linkMtuSize - IPv6_HEADER_SIZE - UDP_HEADER_SIZE;
  }
}

int UDPThread::start() {
  /* Setup our connection */
  m_socket = socket(m_addressFamily, SOCK_DGRAM, 0);
  if (m_socket < 0) {
    lerror << "socket Error" << std::endl;
    return -1;
  }

  /* Setup broadcast option */
  int broadcastEnable = 1;
  if(setsockopt(m_socket,SOL_SOCKET,SO_BROADCAST,&broadcastEnable,sizeof(broadcastEnable)) < 0)
  {
      lerror <<"Error in setting Broadcast option"<< std::endl;
      close(m_socket);
      return -1;
  }

  if (bind(m_socket, (struct sockaddr *)&m_localAddr, sizeof(m_localAddr)) < 0) {
    lerror << "Could not bind to address" << std::endl;
    close(m_socket);
    return -1;
  }
  return Thread::start();
}

void UDPThread::stop() {
  Thread::stop();
  /* m_started is now false, we need to wake up the thread */
  m_blockTimer.fire();
}

bool UDPThread::parsePacket(uint8_t *buffer, uint16_t len, struct sockaddr_storage *clientAddr) {
  if ((m_addressFamily == AF_INET && (memcmp(&((struct sockaddr_in *) clientAddr)->sin_addr, &((struct sockaddr_in *) &m_remoteAddr)->sin_addr, sizeof(struct in_addr)) != 0) && m_checkPeer) ||
      (m_addressFamily == AF_INET6 && (memcmp(&((struct sockaddr_in6 *) clientAddr)->sin6_addr, &((struct sockaddr_in6 *) &m_remoteAddr)->sin6_addr, sizeof(struct in6_addr)) != 0) && m_checkPeer)) {
    lwarn << "Got a connection attempt from " << formatSocketAddress(getSocketAddress(clientAddr))
          << ", which is not set as a remote. Restart with -p argument to override." << std::endl;
    return false;
  }
  if (m_debugOptions.udp) {
    linfo << "Received " << std::dec << len << " Bytes from Host " << formatSocketAddress(getSocketAddress(clientAddr)) << std::endl;
  }
  /*
   * Parse each frame into an ingress-local scratch slot, then route it. The
   * parser fills and hands off one frame at a time (the receiver runs before
   * the next allocator call), so a single reused scratch frame is safe and
   * keeps ingest off the target's pool. The Router copies it into each
   * target's egress pool.
   */
  canfd_frame scratch;
  auto allocator = [&scratch]()
  {
      return &scratch;
  };
  auto receiver = [this](canfd_frame* f, bool success)
  {
      if (!success)
      {
          return;
      }

      m_router->route(f, m_selfId);
      if (m_debugOptions.can)
      {
          printCANInfo(f);
      }
  };
  try
  {
      parseFrames(len, buffer, allocator, receiver);
      m_rxCount++;
  }
  catch(std::exception& e)
  {
      lerror << e.what();
      return true;
  }
  return false;
}

void UDPThread::run() {
  fd_set readfds;
  ssize_t receivedBytes;
  std::vector<uint8_t> bufferVector(m_linkMtuSize);
  uint8_t *buffer = bufferVector.data();
  struct sockaddr_storage clientAddr;
  socklen_t clientAddrLen;

  /*
   * The UDP hub multiplexes every peer over this one socket. Each peer has its
   * own flush timer in the select() set, so a peer's batch-fill-then-flush and
   * per-CAN-id timeout behaviour is preserved independently of the others
   * (resolves the per-peer flush scheduling slice, cannelloni-84a.2.1).
   */
  for (auto &peer : m_peers)
    peer->transmitTimer.adjust(m_timeout, m_timeout);
  m_blockTimer.adjust(SELECT_TIMEOUT, SELECT_TIMEOUT);

  linfo << "UDPThread up and running" << std::endl;
  while (m_started) {
    /* Prepare readfds */
    FD_ZERO(&readfds);
    FD_SET(m_socket, &readfds);
    FD_SET(m_blockTimer.getFd(), &readfds);
    int maxFd = std::max(m_socket, m_blockTimer.getFd());
    for (auto &peer : m_peers) {
      FD_SET(peer->transmitTimer.getFd(), &readfds);
      maxFd = std::max(maxFd, peer->transmitTimer.getFd());
    }

    int ret = select(maxFd + 1, &readfds, NULL, NULL, NULL);
    if (ret < 0) {
      lerror << "select error" << std::endl;
      break;
    }
    for (auto &peer : m_peers) {
      if (FD_ISSET(peer->transmitTimer.getFd(), &readfds)) {
        if (peer->transmitTimer.read() > 0) {
          if (peer->egress->getFrameBufferSize())
            flushPeer(*peer);
          else
            peer->transmitTimer.disable();
        }
      }
    }
    if (FD_ISSET(m_blockTimer.getFd(), &readfds)) {
      m_blockTimer.read();
      /* Add any peers an external backend (mDNS) resolved since the last wake,
       * then run the periodic liveness sweep. Both no-op unless enabled. */
      drainDiscoveredPeers();
      sweepDeadPeers();
    }
    if (FD_ISSET(m_socket, &readfds)) {
      /* Clear buffer */
      memset(buffer, 0, m_linkMtuSize);
      clientAddrLen = sizeof(clientAddr);
      receivedBytes = recvfrom(m_socket, buffer, m_linkMtuSize,
          0, (struct sockaddr *)&clientAddr, &clientAddrLen);
      if (receivedBytes < 0) {
        lerror << "recvfrom error." << std::endl;
        continue;
      } else if (receivedBytes > 0) {
        handleDatagram(buffer, receivedBytes, &clientAddr);
      }
    }
  }
  /* Seed with the aggregate so already-evicted dynamic peers are still counted. */
  uint64_t totalTx = m_txCount, totalRx = m_rxCount;
  for (auto &peer : m_peers) {
    if (m_debugOptions.buffer)
      peer->egress->debug();
    totalTx += peer->txCount;
    totalRx += peer->rxCount;
  }
  linfo << "Shutting down. UDP Transmission Summary: TX: " << totalTx << " RX: " << totalRx << std::endl;
  shutdown(m_socket, SHUT_RDWR);
  close(m_socket);
}

void UDPThread::addPeer(PeerId id, const struct sockaddr_storage &remoteAddr, FrameBuffer *egress) {
  auto peer = std::make_unique<UdpPeer>(id, remoteAddr, egress);
  m_peerIndex[id] = peer.get();
  m_peers.push_back(std::move(peer));
  /* Hand out dynamic ids past every statically-configured one. */
  if (id >= m_nextPeerId)
    m_nextPeerId = id + 1;
}

void UDPThread::enableDiscovery(PeerRegistry *registry, size_t maxPeers, uint32_t peerTimeoutSec) {
  m_registry = registry;
  m_discover = true;
  m_maxPeers = maxPeers;
  m_peerTimeoutNs = static_cast<uint64_t>(peerTimeoutSec) * 1000000000ull;
}

void UDPThread::setEgressPoolSize(size_t frames, size_t max) {
  m_egressPoolFrames = frames;
  m_egressPoolMax = max;
}

/* Compare two socket addresses by family, IP and port. */
static bool sameSockaddr(const struct sockaddr_storage &a, const struct sockaddr_storage &b) {
  if (a.ss_family != b.ss_family)
    return false;
  if (a.ss_family == AF_INET) {
    const struct sockaddr_in *x = (const struct sockaddr_in *)&a;
    const struct sockaddr_in *y = (const struct sockaddr_in *)&b;
    return x->sin_port == y->sin_port &&
           x->sin_addr.s_addr == y->sin_addr.s_addr;
  } else if (a.ss_family == AF_INET6) {
    const struct sockaddr_in6 *x = (const struct sockaddr_in6 *)&a;
    const struct sockaddr_in6 *y = (const struct sockaddr_in6 *)&b;
    return x->sin6_port == y->sin6_port &&
           memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(struct in6_addr)) == 0;
  }
  return false;
}

/*
 * Cheap check that a datagram looks like a cannelloni DATA packet (correct
 * version + opcode, big enough for the header). Discovery only learns a new
 * peer from such a packet ("valid RX"), so a stray non-cannelloni datagram
 * (port scan, wrong protocol) never spends a peer slot or an egress buffer.
 */
static bool isCannelloniData(const uint8_t *buffer, uint16_t len) {
  if (len < CANNELLONI_DATA_PACKET_BASE_SIZE)
    return false;
  const struct CannelloniDataPacket *data =
      reinterpret_cast<const struct CannelloniDataPacket *>(buffer);
  return data->version == CANNELLONI_FRAME_VERSION && data->op_code == DATA;
}

UdpPeer *UDPThread::resolveOrigin(const struct sockaddr_storage *clientAddr) {
  for (auto &peer : m_peers) {
    if (sameSockaddr(peer->remoteAddr, *clientAddr))
      return peer.get();
  }
  /*
   * Backward compatibility: a single UDP peer with peer-checking disabled (-p)
   * accepts datagrams from any source (e.g. NAT / unknown source port), exactly
   * as before. Discovery (which attributes distinct sources to distinct peers)
   * supersedes this, so it does not apply when discovery is on. With several
   * peers an unknown source cannot be attributed to a participant (no origin =>
   * no correct origin-exclusion) and is dropped.
   */
  if (!m_discover && !m_checkPeer && m_peers.size() == 1)
    return m_peers.front().get();
  return nullptr;
}

UdpPeer *UDPThread::learnPeer(const struct sockaddr_storage *clientAddr) {
  if (m_maxPeers != 0 && m_dynamicCount >= m_maxPeers) {
    lwarn << "Discovery: dynamic peer limit (" << m_maxPeers << ") reached, dropping datagram from "
          << formatSocketAddress(getSocketAddress(clientAddr)) << std::endl;
    return nullptr;
  }

  PeerId id = m_nextPeerId++;
  /*
   * A discovered peer owns its egress buffer (static peers' buffers live in
   * main). The UdpPeer is stored behind a unique_ptr, so the raw pointer we
   * hand to the registry stays valid across vector reallocation.
   */
  auto egress = std::make_unique<FrameBuffer>(m_egressPoolFrames, m_egressPoolMax);
  auto peer = std::make_unique<UdpPeer>(id, *clientAddr, egress.get());
  peer->egressOwned = std::move(egress);
  peer->dynamic = true;
  peer->lastSeenNs = nowMonoNs();
  /* Arm the per-peer flush cadence exactly as run() does for static peers. */
  peer->transmitTimer.adjust(m_timeout, m_timeout);

  UdpPeer *raw = peer.get();
  FrameBuffer *eg = raw->egress;
  /*
   * Publish atomically with respect to the routing threads. The CAN thread
   * reaches a peer's egress/timer through m_peers/m_peerIndex (in transmitFrame)
   * while iterating the registry, all under one shared lock; an unordered_map
   * rehash or vector reallocation racing that lookup would be undefined. So the
   * registry entry AND the index/vector are mutated together under the registry
   * write lock: a route() either sees the peer in all three, or in none.
   */
  {
    std::unique_lock<std::shared_mutex> lock(m_registry->mutex());
    m_peerIndex[id] = raw;
    m_peers.push_back(std::move(peer));
    m_registry->addLocked(Peer{ id, this, eg });
  }
  m_dynamicCount++;
  linfo << "Discovered UDP peer " << formatSocketAddress(getSocketAddress(clientAddr))
        << " as participant " << id << " (" << m_dynamicCount << " dynamic)" << std::endl;
  return raw;
}

void UDPThread::queueDiscoveredPeer(const struct sockaddr_storage &addr) {
  {
    std::lock_guard<std::mutex> lock(m_discoveryInboxMutex);
    m_discoveryInbox.push_back(addr);
  }
  /*
   * Wake the net thread so it drains the inbox promptly (firing the block timer
   * from another thread is just a timerfd write, the same pattern stop() uses).
   * Worst case the 500 ms block timer would pick it up anyway.
   */
  m_blockTimer.fire();
}

void UDPThread::drainDiscoveredPeers() {
  /* Swap the inbox out under the lock so the Avahi thread can keep appending
   * while we add peers (learnPeer takes the registry lock, which can block). */
  std::vector<struct sockaddr_storage> pending;
  {
    std::lock_guard<std::mutex> lock(m_discoveryInboxMutex);
    if (m_discoveryInbox.empty())
      return;
    pending.swap(m_discoveryInbox);
  }
  for (const struct sockaddr_storage &addr : pending) {
    /* Idempotent: mDNS resolves repeatedly, and the peer may also have been
     * learnt from its own traffic already. Only learn genuinely new sources. */
    if (resolveOrigin(&addr) == nullptr) {
      if (learnPeer(&addr) != nullptr)
        linfo << "mDNS: added peer " << formatSocketAddress(getSocketAddress(&addr)) << std::endl;
    }
  }
}

void UDPThread::sweepDeadPeers() {
  if (!m_discover || m_peerTimeoutNs == 0)
    return;
  uint64_t now = nowMonoNs();
  for (auto it = m_peers.begin(); it != m_peers.end(); ) {
    UdpPeer *peer = it->get();
    /* Only discovered peers age out; static peers are operator intent and are
     * left in place so traffic resumes when they recover (84a.3 decision). */
    if (peer->dynamic && now - peer->lastSeenNs > m_peerTimeoutNs) {
      linfo << "Evicting silent UDP peer " << formatSocketAddress(getSocketAddress(&peer->remoteAddr))
            << " (participant " << peer->id << ")" << std::endl;
      /* Fold the evicted peer's traffic into the aggregate (unused on the hub
       * path) so the shutdown summary still accounts for it. */
      m_txCount += peer->txCount;
      m_rxCount += peer->rxCount;
      /*
       * Unpublish and free under the registry write lock. Acquiring it waits for
       * every in-flight route() to finish, so no router can be holding this
       * peer's egress when erase() destroys the UdpPeer (closing its timerfd and
       * freeing the egress buffer). The same lock excludes the routing threads'
       * m_peers/m_peerIndex reads in transmitFrame.
       */
      std::unique_lock<std::shared_mutex> lock(m_registry->mutex());
      m_registry->removeLocked(peer->id);
      m_peerIndex.erase(peer->id);
      it = m_peers.erase(it);
      m_dynamicCount--;
    } else {
      ++it;
    }
  }
}

void UDPThread::handleDatagram(uint8_t *buffer, uint16_t len, struct sockaddr_storage *clientAddr) {
  UdpPeer *peer = resolveOrigin(clientAddr);
  /*
   * Discovery (Phase 3): an unknown source carrying a well-formed cannelloni
   * packet is learnt as a new dynamic peer, so real devices dial into the hub
   * without static config. Learning only on valid RX keeps a stray datagram
   * from spending a peer slot; the cap bounds it further.
   */
  if (peer == nullptr && m_discover && isCannelloniData(buffer, len))
    peer = learnPeer(clientAddr);
  if (peer == nullptr) {
    lwarn << "Got a datagram from " << formatSocketAddress(getSocketAddress(clientAddr))
          << ", which is not a configured peer. Restart with -p argument to override." << std::endl;
    return;
  }
  /* Liveness: a valid datagram keeps a discovered peer alive past the sweep. */
  peer->lastSeenNs = nowMonoNs();
  if (m_debugOptions.udp) {
    linfo << "Received " << std::dec << len << " Bytes from Host "
          << formatSocketAddress(getSocketAddress(clientAddr)) << std::endl;
  }
  /*
   * Parse each frame into an ingress-local scratch slot, then route it with the
   * resolved origin so the Router excludes the sender (origin-exclusion). The
   * parser fills and hands off one frame at a time (the receiver runs before
   * the next allocator call), so a single reused scratch frame is safe and
   * keeps ingest off the targets' pools. The Router copies it into each
   * target's egress pool.
   */
  PeerId origin = peer->id;
  canfd_frame scratch;
  auto allocator = [&scratch]()
  {
      return &scratch;
  };
  auto receiver = [this, origin](canfd_frame* f, bool success)
  {
      if (!success)
      {
          return;
      }

      m_router->route(f, origin);
      if (m_debugOptions.can)
      {
          printCANInfo(f);
      }
  };
  try
  {
      parseFrames(len, buffer, allocator, receiver);
      peer->rxCount++;
  }
  catch(std::exception& e)
  {
      lerror << e.what();
  }
}

void UDPThread::transmitFrame(canfd_frame *frame, PeerId target) {
  if (m_peers.empty()) {
    /*
     * Single-peer base-class path reused by SCTPThread: enqueue onto the one
     * frame buffer and arm the single transmit timer, exactly as before.
     */
    m_frameBuffer->insertFrame(frame);
    armTransmit(*m_frameBuffer, m_transmitTimer, frame);
    return;
  }
  auto it = m_peerIndex.find(target);
  if (it == m_peerIndex.end()) {
    /* The Router only delivers to registered peers, so this is a wiring bug. */
    lerror << "transmitFrame: no such peer id " << target << std::endl;
    return;
  }
  UdpPeer *peer = it->second;
  peer->egress->insertFrame(frame);
  armTransmit(*peer->egress, peer->transmitTimer, frame);
}

void UDPThread::armTransmit(FrameBuffer &buffer, Timer &timer, const canfd_frame *frame) {
  /* If we have stopped the timer, enable it */
  if (!timer.isEnabled()) {
    timer.enable();
  }
  /*
   * We want that at least this frame and next frame fits into
   * the packet. The minimum size is CANNELLONI_FRAME_BASE_SIZE,
   * which is just the ID * plus the DLC
   */
  if (buffer.getFrameBufferSize() +
      CANNELLONI_DATA_PACKET_BASE_SIZE +
      CANNELLONI_FRAME_BASE_SIZE >= m_payloadSize) {
    timer.fire();
  } else {
    /* Check whether we have custom timeout for this frame */
    std::map<uint32_t,uint32_t>::iterator it;
    uint32_t can_id;
    if (frame->can_id & CAN_EFF_FLAG)
      can_id = frame->can_id & CAN_EFF_MASK;
    else
      can_id = frame->can_id & CAN_SFF_MASK;
    it = m_timeoutTable.find(can_id);
    if (it != m_timeoutTable.end()) {
      uint32_t timeout = it->second;
      if (timeout < m_timeout) {
        if (timeout < timer.getValue()) {
          if (m_debugOptions.timer) {
            linfo << "Found timeout entry for ID " << can_id << ". Adjusting timer." << std::endl;
          }
          /* Let buffer expire in timeout ms */
          timer.adjust(m_timeout, timeout);
        }
      }
    }

  }
}

void UDPThread::setTimeout(uint32_t timeout) {
  m_timeout = timeout;
}

uint32_t UDPThread::getTimeout() {
  return m_timeout;
}

void UDPThread::setTimeoutTable(std::map<uint32_t,uint32_t> &timeoutTable) {
  m_timeoutTable = timeoutTable;
}

std::map<uint32_t,uint32_t>& UDPThread::getTimeoutTable() {
  return m_timeoutTable;
}

void UDPThread::prepareBuffer() {
  // TODO : this should be a std::array, since payloadSize is really known at
  // compile time.
  auto bufWrap = std::make_unique<uint8_t[]>(m_payloadSize);
  auto packetBuffer = bufWrap.get();

  ssize_t transmittedBytes = 0;

  m_frameBuffer->swapBuffers();
  if (m_sort)
    m_frameBuffer->sortIntermediateBuffer();

  std::list<canfd_frame*> *buffer = m_frameBuffer->getIntermediateBuffer();

  auto overflowHandler = [this](std::list<canfd_frame*>&, std::list<canfd_frame*>::iterator it)
  {
      /* Move all remaining frames back to m_buffer */
      m_frameBuffer->returnIntermediateBuffer(it);
  };

  uint8_t* data = buildPacket(m_payloadSize, packetBuffer, *buffer,
          m_sequenceNumber++, overflowHandler);

  transmittedBytes = sendBuffer(packetBuffer, data-packetBuffer);
  if (transmittedBytes != data-packetBuffer) {
    lerror << "UDP Socket error. Error while transmitting" << std::endl;
  } else {
    m_txCount++;
  }
  m_frameBuffer->unlockIntermediateBuffer();
  m_frameBuffer->mergeIntermediateBuffer();
}

void UDPThread::flushPeer(UdpPeer &peer) {
  // TODO : this should be a std::array, since payloadSize is really known at
  // compile time.
  auto bufWrap = std::make_unique<uint8_t[]>(m_payloadSize);
  auto packetBuffer = bufWrap.get();

  peer.egress->swapBuffers();
  if (m_sort)
    peer.egress->sortIntermediateBuffer();

  std::list<canfd_frame*> *buffer = peer.egress->getIntermediateBuffer();

  auto overflowHandler = [&peer](std::list<canfd_frame*>&, std::list<canfd_frame*>::iterator it)
  {
      /* Move all remaining frames back to the peer's egress buffer */
      peer.egress->returnIntermediateBuffer(it);
  };

  uint8_t* data = buildPacket(m_payloadSize, packetBuffer, *buffer,
          peer.seqNo++, overflowHandler);

  ssize_t transmittedBytes = sendto(m_socket, packetBuffer, data-packetBuffer, 0,
          (struct sockaddr *) &peer.remoteAddr, sizeof(peer.remoteAddr));
  if (transmittedBytes != data-packetBuffer) {
    lerror << "UDP Socket error. Error while transmitting" << std::endl;
  } else {
    peer.txCount++;
  }
  peer.egress->unlockIntermediateBuffer();
  peer.egress->mergeIntermediateBuffer();
}

ssize_t UDPThread::sendBuffer(uint8_t *buffer, uint16_t len) {
  return sendto(m_socket, buffer, len, 0,
               (struct sockaddr *) &m_remoteAddr, sizeof(m_remoteAddr));
}
