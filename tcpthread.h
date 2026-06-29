/*
 * This file is part of cannelloni, a SocketCAN over Ethernet tunnel.
 *
 * Copyright (C) 2014-2023 Maximilian Güntner <code@mguentner.de>
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

#include "connection.h"
#include "framebuffer.h"
#include "timer.h"
#include "decoder.h"
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#define SELECT_TIMEOUT 500000
#define SIGNAL_PIPE_READ 0
#define SIGNAL_PIPE_WRITE 1

enum TCPThreadRole { TCP_SERVER, TCP_CLIENT };
/*
  DISCONNECTED: Waiting for a connection
  CONNECTED: TCP Connection established
  NEGOTIATED: A cannelloni peer has been found
 */
enum ConnectState { DISCONNECTED, CONNECTED, NEGOTIATED };

#define CANNELLONI_CONNECT_V1_STRING "CANNELLONIv1"

namespace cannelloni {

  class PeerRegistry;

  struct TCPThreadParams {
    struct sockaddr_storage &remoteAddr;
    struct sockaddr_storage &localAddr;
    int addressFamily;
  };

  class TCPThread : public ConnectionThread {
    public:
      TCPThread(const struct debugOptions_t &debugOptions,
                const struct TCPThreadParams &params);

      virtual int start();
      virtual void cleanup() = 0;
      virtual void run();

      virtual void transmitFrame(canfd_frame *frame, PeerId target);

    protected:
      bool isConnected();
      void flushFrameBuffer();
      void disconnect();
      bool setupSocket();
      bool setupPipe();
      virtual bool attempt_connect() = 0;

    protected:
      struct debugOptions_t m_debugOptions;
      int m_serverSocket;
      int m_socket;
      ConnectState m_connect_state;
      Timer m_blockTimer;
      uint64_t m_rxCount;
      uint64_t m_txCount;
      std::recursive_mutex m_socketWriteMutex;

      struct sockaddr_storage m_localAddr;
      struct sockaddr_storage m_remoteAddr;
      int m_addressFamily;

      int m_framebufferHasDataPipe[2];
      Decoder m_decoder;
  };

  struct TCPServerThreadParams {
    struct sockaddr_storage &remoteAddr;
    struct sockaddr_storage &localAddr;
    int addressFamily;
    bool checkPeer;
    
    public:

    TCPThreadParams toTCPThreadParams() const {
      return TCPThreadParams{
        .remoteAddr = remoteAddr,
        .localAddr = localAddr,
        .addressFamily = addressFamily
      };
    }
  };

  /*
   * Per-client state for the multi-peer TCP server hub (cannelloni-84a.4). One
   * TCPServerThread accepts many clients and owns one TcpPeer per client. The
   * cannelloni TCP stream is a back-to-back run of encoded frames with no packet
   * boundaries, so framing state (the Decoder + a byte accumulator) MUST be
   * per-peer. Each peer also owns its egress FrameBuffer (per-peer drop-oldest
   * under overload, like a discovered UDP peer); the PeerRegistry routing record
   * only borrows the raw pointer. TcpPeer holds a Decoder (non-copyable via its
   * members) and is reached by raw pointer from two indices, so it lives behind
   * a unique_ptr and is non-copyable.
   */
  struct TcpPeer {
    TcpPeer(PeerId id, int fd, const struct sockaddr_storage &remoteAddr,
            size_t poolFrames = DEFAULT_BUFFER_FRAMES,
            size_t poolMax = DEFAULT_BUFFER_MAX)
      : id(id), fd(fd), remoteAddr(remoteAddr)
      , egressOwned(new FrameBuffer(poolFrames, poolMax))
      , egress(egressOwned.get())
    {}
    TcpPeer(const TcpPeer &) = delete;
    TcpPeer &operator=(const TcpPeer &) = delete;

    PeerId id;
    int fd;
    struct sockaddr_storage remoteAddr;
    std::unique_ptr<FrameBuffer> egressOwned;
    FrameBuffer *egress;
    Decoder decoder;                       /* per-peer stream framing */
    /*
     * CONNECTED once accepted; NEGOTIATED once the peer echoes the cannelloni
     * version handshake. A peer is published to the Router (so frames fan out to
     * it) only after NEGOTIATED, matching the single-peer gate.
     */
    ConnectState connectState = CONNECTED;
    bool published = false;

    /* Inbound stream bytes received but not yet consumed by the decoder. */
    std::vector<uint8_t> rxBuf;

    /*
     * Phase 4b: encoded egress bytes that did not fit the socket (partial write
     * / EAGAIN). Drained on EPOLLOUT so a slow peer never blocks the net thread
     * or the other peers. txSent marks how much of txPending is already gone.
     */
    std::vector<uint8_t> txPending;
    size_t txSent = 0;
    bool epollOut = false;                 /* EPOLLOUT currently armed for fd */

    uint64_t txCount = 0;
    uint64_t rxCount = 0;
  };

  /*
   * Multi-peer TCP server hub (cannelloni-84a.4). Accepts and tracks an
   * arbitrary number of clients on a single epoll-driven net thread; each
   * accepted client becomes a participant on the virtual shared CAN bus. A
   * one-client configuration behaves exactly as the previous single-peer
   * server. Peers are learnt on accept() and evicted on disconnect, mutating the
   * shared PeerRegistry the Router reads (serialised by the registry mutex, as
   * for dynamic UDP discovery).
   */
  class TCPServerThread : public TCPThread  {
    public:
      TCPServerThread(const struct debugOptions_t &debugOptions,
                      const struct TCPServerThreadParams &params);

      virtual int start();
      virtual void stop();
      virtual void run();
      virtual void transmitFrame(canfd_frame *frame, PeerId target);
      virtual bool attempt_connect();
      virtual void cleanup();

      /*
       * Wire the server to the hub. The registry is the one the Router reads;
       * accept/evict mutate it under its shared_mutex. maxPeers caps the number
       * of simultaneously connected clients (0 = unlimited). Call before start().
       */
      void setRegistry(PeerRegistry *registry, size_t maxPeers);

      /*
       * Set the egress pool sizing applied to every accepted client's buffer
       * (frames preallocated, grown to at most max; 0 = unlimited). Call before
       * start(); defaults to DEFAULT_BUFFER_FRAMES/MAX.
       */
      void setEgressPoolSize(size_t frames, size_t max);

    private:
      /* epoll helpers (net thread only). */
      bool epollAdd(int fd, uint32_t events);
      bool epollMod(int fd, uint32_t events);
      void epollDel(int fd);

      /* Accept every pending client (listen socket is non-blocking). */
      void acceptClients();
      /* Drain a peer's socket, decode complete frames, route them. Returns
       * false if the peer must be evicted (EOF / read or decode error). */
      bool handlePeerReadable(TcpPeer &peer);
      /* Feed accumulated bytes through the per-peer decoder, routing frames.
       * Returns false on a framing/handshake error (peer must be evicted). */
      bool decodePeerStream(TcpPeer &peer);
      /* Encode a peer's egress frames into its outbound byte stream. */
      void flushPeer(TcpPeer &peer);
      /* Push a peer's pending TX bytes non-blocking; (re)arm EPOLLOUT if the
       * socket is full. Returns false on a fatal write error (evict the peer). */
      bool drainPending(TcpPeer &peer);
      /* Flush every peer that has queued egress frames. */
      void flushAllPeers();
      /* Publish a NEGOTIATED peer to the Router (registry + id index). */
      void publishPeer(TcpPeer &peer);
      /* Tear down + free a peer (deregisters first if published). */
      void evictPeer(TcpPeer *peer, const char *reason);

      bool m_checkPeerConnect;

      int m_epollFd;
      PeerRegistry *m_registry;
      size_t m_maxPeers;
      PeerId m_nextPeerId;
      /* Pool sizing for the egress buffer of each accepted client. */
      size_t m_egressPoolFrames = DEFAULT_BUFFER_FRAMES;
      size_t m_egressPoolMax = DEFAULT_BUFFER_MAX;

      /* Clients served by this thread; the unique_ptr keeps TcpPeer addresses
       * stable across vector growth so both indices stay valid. */
      std::vector<std::unique_ptr<TcpPeer>> m_peers;
      /* By PeerId: read by the CAN thread in transmitFrame, so mutated only
       * under the registry write lock (as for dynamic UDP peers). */
      std::unordered_map<PeerId, TcpPeer*> m_peerIndex;
      /* By socket fd: net thread only, no lock needed. */
      std::unordered_map<int, TcpPeer*> m_fdIndex;
  };

  class TCPClientThread : public TCPThread {
  public:
    TCPClientThread(const struct debugOptions_t &debugOptions,
                    const struct TCPThreadParams &params);
    virtual bool attempt_connect();
    virtual void cleanup();
  };
}
