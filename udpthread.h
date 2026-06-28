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

#pragma once

#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>

#include "connection.h"
#include "framebuffer.h"
#include "peer.h"
#include "timer.h"


namespace cannelloni {

#define IPv4_HEADER_SIZE 20
#define IPv6_HEADER_SIZE 40
#define UDP_HEADER_SIZE 8

/* Block select max. for 500ms */
#define SELECT_TIMEOUT 500000

struct UDPThreadParams {
  struct sockaddr_storage &remoteAddr;
  struct sockaddr_storage &localAddr;
  int addressFamily;
  bool sortFrames;
  bool checkPeer;
  uint16_t linkMtuSize;
};

/*
 * Per-peer egress state for the multiplexed UDP hub. A single UDPThread owns
 * one bound socket and N of these, one per network peer. Each holds the peer's
 * own egress buffer, sequence number and flush timer so every peer sees an
 * independent, standard cannelloni stream (Phase 2, cannelloni-84a.2). The
 * Timer wraps a timerfd and must not be copied, hence UdpPeer is stored behind
 * a unique_ptr and is non-copyable.
 */
struct UdpPeer {
  UdpPeer(PeerId id, const struct sockaddr_storage &remoteAddr, FrameBuffer *egress)
    : id(id), remoteAddr(remoteAddr), egress(egress)
    , seqNo(0), transmitTimer(), txCount(0), rxCount(0)
  {}
  UdpPeer(const UdpPeer &) = delete;
  UdpPeer &operator=(const UdpPeer &) = delete;

  PeerId id;
  struct sockaddr_storage remoteAddr;
  FrameBuffer *egress;     /* not owned; lives in main, shared with the registry */
  uint8_t seqNo;
  Timer transmitTimer;
  uint64_t txCount;
  uint64_t rxCount;
};

class UDPThread : public ConnectionThread {
  public:
    UDPThread(const struct debugOptions_t &debugOptions,
              const struct UDPThreadParams &params);

    virtual int start();
    virtual void stop();
    virtual void run();
    bool parsePacket(uint8_t *buf, uint16_t len, struct sockaddr_storage *clientAddr);
    virtual void transmitFrame(canfd_frame *frame, PeerId target);

    /*
     * Register a network peer served by this (multiplexed) thread. Called once
     * per peer at startup, before start(); the egress buffer is the same one
     * registered with the PeerRegistry/Router for this peer.
     */
    void addPeer(PeerId id, const struct sockaddr_storage &remoteAddr, FrameBuffer *egress);

    void setTimeout(uint32_t timeout);
    uint32_t getTimeout();

    void setTimeoutTable(std::map<uint32_t,uint32_t> &timeoutTable);
    std::map<uint32_t,uint32_t>& getTimeoutTable();

  protected:
    void prepareBuffer();
    virtual ssize_t sendBuffer(uint8_t *buffer, uint16_t len);

    /* Flush one peer's egress buffer as a single UDP datagram to its address. */
    void flushPeer(UdpPeer &peer);
    /* Enqueue + (re)arm a flush timer, honouring the per-CAN-id timeout table. */
    void armTransmit(FrameBuffer &buffer, Timer &timer, const canfd_frame *frame);
    /* Map an incoming datagram's source address to a configured peer. */
    UdpPeer *resolveOrigin(const struct sockaddr_storage *clientAddr);
    /* Parse a received datagram and route each frame with the resolved origin. */
    void handleDatagram(uint8_t *buffer, uint16_t len, struct sockaddr_storage *clientAddr);

  protected:
    struct debugOptions_t m_debugOptions;
    bool m_sort;
    bool m_checkPeer;
    int m_socket;
    int m_addressFamily;
    Timer m_blockTimer;
    Timer m_transmitTimer;

    struct sockaddr_storage m_localAddr;
    struct sockaddr_storage m_remoteAddr;

    uint8_t m_sequenceNumber;
    /* Timeout variables */
    uint32_t m_timeout;
    std::map<uint32_t,uint32_t> m_timeoutTable;
    /* Performance Counters */
    uint64_t m_rxCount;
    uint64_t m_txCount;

    uint32_t m_linkMtuSize; // mtu of the network interface
    uint32_t m_payloadSize; // payload usable by cannelloni

    /*
     * The network peers served by this thread. Empty for the single-peer
     * base-class path reused by SCTPThread (which keeps using m_frameBuffer /
     * m_sequenceNumber / m_transmitTimer / prepareBuffer() / parsePacket()).
     * The UDP hub always populates this (>= 1 entry) and uses it exclusively.
     */
    std::vector<std::unique_ptr<UdpPeer>> m_peers;
    std::unordered_map<PeerId, UdpPeer*> m_peerIndex;
};

}
