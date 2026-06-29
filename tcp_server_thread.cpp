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

#include <cstring>
#include <list>
#include <shared_mutex>
#include <vector>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/can.h>

#include "cannelloni.h"
#include "inet_address.h"
#include "logging.h"
#include "parser.h"
#include "peer.h"
#include "peerregistry.h"
#include "router.h"
#include "tcpthread.h"

using namespace cannelloni;

/*
 * Per-peer TX backpressure bounds (Phase 4b, cannelloni-84a.4.2). A peer that
 * stops reading must neither grow the hub's memory without limit nor stall the
 * shared net thread or the other peers. Writes are already non-blocking, so a
 * slow peer only ever parks bytes in its own txPending queue; these caps bound
 * that queue:
 *
 *   - At SOFT we stop feeding the peer new frames. They stay in its egress ring
 *     buffer, which drops the oldest under overload -- the standard cannelloni
 *     overload policy -- so a momentarily slow peer loses stale frames, not the
 *     hub's memory, and catches up once it drains.
 *   - At HARD the peer has not drained even the bytes already committed to its
 *     stream; it is a hopeless laggard and is disconnected. A disconnected peer
 *     can reconnect, whereas a corrupted stream cannot be partially dropped.
 */
static const size_t TX_BACKLOG_SOFT_BYTES = 256 * 1024;
static const size_t TX_BACKLOG_HARD_BYTES = 1024 * 1024;

TCPServerThread::TCPServerThread(const struct debugOptions_t &debugOptions,
                                 const struct TCPServerThreadParams &params)
  : TCPThread(debugOptions, params.toTCPThreadParams())
  , m_checkPeerConnect(params.checkPeer)
  , m_epollFd(-1)
  , m_registry(nullptr)
  , m_maxPeers(0)
  , m_nextPeerId(FIRST_NET_PEER_ID)
{
}

void TCPServerThread::setRegistry(PeerRegistry *registry, size_t maxPeers) {
  m_registry = registry;
  m_maxPeers = maxPeers;
}

void TCPServerThread::setEgressPoolSize(size_t frames, size_t max) {
  m_egressPoolFrames = frames;
  m_egressPoolMax = max;
}

int TCPServerThread::start() {
  m_serverSocket = socket(m_addressFamily, SOCK_STREAM, 0);
  if (m_serverSocket < 0) {
    lerror << "socket error" << std::endl;
    return -1;
  }

  const int option = 1;
  setsockopt(m_serverSocket, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

  if (m_addressFamily == AF_INET && bind(m_serverSocket, (struct sockaddr *)&m_localAddr, sizeof(sockaddr_in)) < 0) {
    lerror << "Could not bind to address" << std::endl;
    return -1;
  } else if (m_addressFamily == AF_INET6 && bind(m_serverSocket, (struct sockaddr *)&m_localAddr, sizeof(sockaddr_in6)) < 0) {
    lerror << "Could not bind to address" << std::endl;
    return -1;
  } else if (m_addressFamily != AF_INET && m_addressFamily != AF_INET6) {
    lerror << "Invalid address family" << m_addressFamily <<  std::endl;
    return -1;
  }

  /* Non-blocking listen socket: the epoll loop accepts in a drain-until-EAGAIN
   * loop, so a spurious readiness never blocks the shared net thread. */
  int flags = fcntl(m_serverSocket, F_GETFL, 0);
  if (flags < 0 || fcntl(m_serverSocket, F_SETFL, flags | O_NONBLOCK) < 0) {
    lerror << "Could not set listen socket non-blocking" << std::endl;
    return -1;
  }
  if (listen(m_serverSocket, SOMAXCONN) < 0) {
    lerror << "listen error" << std::endl;
    return -1;
  }

  return TCPThread::start();
}

void TCPServerThread::stop() {
  Thread::stop();
  /* m_started is now false; wake the epoll loop so it can observe it. */
  m_blockTimer.fire();
}

bool TCPServerThread::attempt_connect() {
  /* Unused: the multi-peer server drives accept() from its own epoll loop in
   * run(); the single-peer connect state machine of the base class is bypassed. */
  return false;
}

bool TCPServerThread::epollAdd(int fd, uint32_t events) {
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.fd = fd;
  if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &ev) < 0) {
    lerror << "epoll_ctl ADD failed" << std::endl;
    return false;
  }
  return true;
}

bool TCPServerThread::epollMod(int fd, uint32_t events) {
  struct epoll_event ev;
  memset(&ev, 0, sizeof(ev));
  ev.events = events;
  ev.data.fd = fd;
  if (epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &ev) < 0) {
    lerror << "epoll_ctl MOD failed" << std::endl;
    return false;
  }
  return true;
}

void TCPServerThread::epollDel(int fd) {
  epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, nullptr);
}

void TCPServerThread::run() {
  m_epollFd = epoll_create1(EPOLL_CLOEXEC);
  if (m_epollFd < 0) {
    lerror << "epoll_create1 failed" << std::endl;
    return;
  }
  /*
   * Single wake pipe for the whole thread: transmitFrame() (CAN thread or this
   * thread reflecting peer->peer) enqueues onto a peer's egress and writes one
   * byte here so the net thread leaves epoll_wait and flushes whoever has data.
   */
  if (pipe(m_framebufferHasDataPipe) == -1) {
    lerror << "could not initialize signal pipe" << std::endl;
    close(m_epollFd);
    return;
  }
  fcntl(m_framebufferHasDataPipe[SIGNAL_PIPE_READ], F_SETFL, O_NONBLOCK);
  fcntl(m_framebufferHasDataPipe[SIGNAL_PIPE_WRITE], F_SETFL, O_NONBLOCK);

  /* Periodic catch-up flush, mirroring the single-peer block timer. */
  m_blockTimer.adjust(SELECT_TIMEOUT, SELECT_TIMEOUT);

  epollAdd(m_serverSocket, EPOLLIN);
  epollAdd(m_blockTimer.getFd(), EPOLLIN);
  epollAdd(m_framebufferHasDataPipe[SIGNAL_PIPE_READ], EPOLLIN);

  linfo << "TCPServerThread up and running" << std::endl;

  const int MAX_EVENTS = 64;
  struct epoll_event events[MAX_EVENTS];
  while (m_started) {
    int n = epoll_wait(m_epollFd, events, MAX_EVENTS, -1);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      lerror << "epoll_wait error" << std::endl;
      break;
    }
    for (int i = 0; i < n; ++i) {
      int fd = events[i].data.fd;
      uint32_t ev = events[i].events;

      if (fd == m_serverSocket) {
        acceptClients();
        continue;
      }
      if (fd == m_blockTimer.getFd()) {
        m_blockTimer.read();
        flushAllPeers();
        continue;
      }
      if (fd == m_framebufferHasDataPipe[SIGNAL_PIPE_READ]) {
        int sig;
        while (read(m_framebufferHasDataPipe[SIGNAL_PIPE_READ], &sig, sizeof(sig)) > 0) {
        }
        flushAllPeers();
        continue;
      }

      /* A peer fd. It may already have been evicted earlier in this batch. */
      auto it = m_fdIndex.find(fd);
      if (it == m_fdIndex.end())
        continue;
      TcpPeer *peer = it->second;
      if (ev & EPOLLOUT) {
        if (!drainPending(*peer)) {
          evictPeer(peer, "write error");
          continue;
        }
      }
      if (ev & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
        if (!handlePeerReadable(*peer)) {
          evictPeer(peer, "disconnect");
          continue;
        }
      }
    }
  }

  /*
   * Tear down every peer before returning. This thread is stopped and joined
   * before the CAN thread (see main), so the CAN thread may still route() to a
   * TCP peer right now: evictPeer() deregisters under the registry write lock,
   * which drains in-flight routers before the egress buffer is freed.
   */
  while (!m_peers.empty())
    evictPeer(m_peers.back().get(), "shutdown");

  linfo << "Shutting down. TCP Transmission Summary: TX: " << m_txCount << " RX: " << m_rxCount << std::endl;
  close(m_framebufferHasDataPipe[SIGNAL_PIPE_READ]);
  close(m_framebufferHasDataPipe[SIGNAL_PIPE_WRITE]);
  close(m_epollFd);
  m_epollFd = -1;
  cleanup();
}

/* True if `addr` is the operator-configured remote (used only with peer check). */
static bool matchesRemote(int family, const struct sockaddr_storage &addr,
                          const struct sockaddr_storage &remote) {
  if (family == AF_INET)
    return memcmp(&((const struct sockaddr_in *)&addr)->sin_addr,
                  &((const struct sockaddr_in *)&remote)->sin_addr,
                  sizeof(struct in_addr)) == 0;
  if (family == AF_INET6)
    return memcmp(&((const struct sockaddr_in6 *)&addr)->sin6_addr,
                  &((const struct sockaddr_in6 *)&remote)->sin6_addr,
                  sizeof(struct in6_addr)) == 0;
  return false;
}

void TCPServerThread::acceptClients() {
  uint8_t versionBuffer[] = CANNELLONI_CONNECT_V1_STRING;
  const size_t versionLen = sizeof(versionBuffer) - 1;

  for (;;) {
    struct sockaddr_storage connAddr;
    socklen_t connAddrLen = sizeof(connAddr);
    int fd = accept(m_serverSocket, (struct sockaddr *)&connAddr, &connAddrLen);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break; /* all pending connections drained */
      if (errno == EINTR)
        continue;
      lerror << "Error while accepting." << std::endl;
      break;
    }

    /*
     * Peer check: when enabled, only the operator-configured remote (-R) may
     * connect, exactly as the single-peer server did. Hubs that accept many
     * distinct clients run with -p (no peer check), as the nix module emits.
     */
    if (m_checkPeerConnect && !matchesRemote(m_addressFamily, connAddr, m_remoteAddr)) {
      lwarn << "Got a connection attempt from " << formatSocketAddress(getSocketAddress(&connAddr))
            << ", which is not set as a remote. Restart with -p argument to override." << std::endl;
      close(fd);
      continue;
    }

    if (m_maxPeers != 0 && m_peers.size() >= m_maxPeers) {
      lwarn << "TCP client limit (" << m_maxPeers << ") reached, rejecting "
            << formatSocketAddress(getSocketAddress(&connAddr)) << std::endl;
      close(fd);
      continue;
    }

    /* Non-blocking + same socket tuning the single-peer path used. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      lerror << "Could not set client socket non-blocking" << std::endl;
      close(fd);
      continue;
    }
    const int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    /* Announce our protocol version; the fresh socket buffer always has room. */
    ssize_t res = send(fd, versionBuffer, versionLen, MSG_NOSIGNAL);
    if (res != (ssize_t)versionLen) {
      lerror << "Could not announce protocol to "
             << formatSocketAddress(getSocketAddress(&connAddr)) << std::endl;
      close(fd);
      continue;
    }

    PeerId id = m_nextPeerId++;
    auto peer = std::make_unique<TcpPeer>(id, fd, connAddr,
                                          m_egressPoolFrames, m_egressPoolMax);
    TcpPeer *raw = peer.get();
    m_fdIndex[fd] = raw;
    m_peers.push_back(std::move(peer));
    if (!epollAdd(fd, EPOLLIN)) {
      evictPeer(raw, "epoll add failed");
      continue;
    }
    linfo << "Got a connection from " << formatSocketAddress(getSocketAddress(&connAddr))
          << " (participant " << id << ")" << std::endl;
  }
}

bool TCPServerThread::handlePeerReadable(TcpPeer &peer) {
  uint8_t chunk[4096];
  for (;;) {
    ssize_t n = read(peer.fd, chunk, sizeof(chunk));
    if (n > 0) {
      peer.rxBuf.insert(peer.rxBuf.end(), chunk, chunk + n);
      continue;
    }
    if (n == 0)
      return false; /* peer closed the connection */
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      break; /* socket drained */
    if (errno == EINTR)
      continue;
    lerror << "read error on participant " << peer.id << std::endl;
    return false;
  }
  return decodePeerStream(peer);
}

bool TCPServerThread::decodePeerStream(TcpPeer &peer) {
  uint8_t *buf = peer.rxBuf.data();
  size_t avail = peer.rxBuf.size();
  size_t off = 0;

  /*
   * Handshake: the first bytes of every client are the cannelloni version
   * string. Only once it matches is the peer published to the Router, so frames
   * never fan out to a half-open or non-cannelloni connection.
   */
  if (peer.connectState == CONNECTED) {
    const size_t versionLen = sizeof(CANNELLONI_CONNECT_V1_STRING) - 1;
    if (avail - off < versionLen)
      return true; /* wait for the rest of the handshake */
    if (memcmp(buf + off, CANNELLONI_CONNECT_V1_STRING, versionLen) != 0) {
      lwarn << "Invalid protocol detected from participant " << peer.id << std::endl;
      return false;
    }
    off += versionLen;
    peer.connectState = NEGOTIATED;
    publishPeer(peer);
  }

  /*
   * Pull complete frames out of the stream. The decoder is a "read exactly N
   * bytes" state machine; we feed it from the accumulator and stop when fewer
   * than N bytes remain, keeping the partial-frame tail for the next read. The
   * Router copies tempFrame into each target's pool, so it is reusable at once.
   */
  for (;;) {
    if (peer.decoder.expectedBytes == 0) {
      /* STATE_INIT (fresh, or just completed a frame): size the next field. */
      peer.decoder.expectedBytes =
          decodeFrame(nullptr, 0, &peer.decoder.tempFrame, &peer.decoder.state);
    }
    size_t need = static_cast<size_t>(peer.decoder.expectedBytes);
    if (avail - off < need)
      break;
    ssize_t r = decodeFrame(buf + off, need, &peer.decoder.tempFrame, &peer.decoder.state);
    off += need;
    if (r < 0) {
      lerror << "Decoder error on participant " << peer.id << std::endl;
      return false;
    } else if (r == 0) {
      m_router->route(&peer.decoder.tempFrame, peer.id);
      peer.rxCount++;
      peer.decoder.expectedBytes = 0; /* re-prime on the next iteration */
    } else {
      peer.decoder.expectedBytes = r;
    }
  }

  if (off > 0)
    peer.rxBuf.erase(peer.rxBuf.begin(), peer.rxBuf.begin() + off);
  return true;
}

void TCPServerThread::publishPeer(TcpPeer &peer) {
  /*
   * Publish the registry entry and the id index together under the registry
   * write lock: the CAN thread reads m_peerIndex in transmitFrame while holding
   * the registry's shared lock, so a route() sees this peer in both or neither
   * (same invariant as dynamic UDP discovery).
   */
  {
    std::unique_lock<std::shared_mutex> lock(m_registry->mutex());
    m_peerIndex[peer.id] = &peer;
    m_registry->addLocked(Peer{ peer.id, this, peer.egress });
    peer.published = true;
  }
  linfo << "TCP peer " << formatSocketAddress(getSocketAddress(&peer.remoteAddr))
        << " negotiated as participant " << peer.id << std::endl;
}

void TCPServerThread::evictPeer(TcpPeer *peer, const char *reason) {
  PeerId id = peer->id;
  int fd = peer->fd;
  bool published = peer->published;

  linfo << "Disconnecting participant " << id << " (" << reason << ")" << std::endl;

  epollDel(fd);
  m_fdIndex.erase(fd);
  /* Fold the peer's counters into the aggregate for the shutdown summary. */
  m_txCount += peer->txCount;
  m_rxCount += peer->rxCount;

  auto erasePeer = [this, peer]() {
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
      if (it->get() == peer) {
        m_peers.erase(it);
        return;
      }
    }
  };

  if (published) {
    /*
     * Deregister and free under the registry write lock. Acquiring it waits for
     * every in-flight route() to finish, so no router can hold this peer's
     * egress when erasePeer() destroys the TcpPeer (freeing the egress buffer).
     */
    std::unique_lock<std::shared_mutex> lock(m_registry->mutex());
    m_registry->removeLocked(id);
    m_peerIndex.erase(id);
    erasePeer();
  } else {
    /* Never published => never a routing target => no router ever held it. */
    erasePeer();
  }
  close(fd);
}

void TCPServerThread::transmitFrame(canfd_frame *frame, PeerId target) {
  /*
   * Called from route() (CAN thread, or this thread reflecting peer->peer) under
   * the registry shared lock. That lock excludes publishPeer/evictPeer, so the
   * m_peerIndex lookup races nothing; the Router only delivers to published
   * peers, so the id is always present.
   */
  auto it = m_peerIndex.find(target);
  if (it == m_peerIndex.end()) {
    lerror << "transmitFrame: no such TCP peer id " << target << std::endl;
    return;
  }
  TcpPeer *peer = it->second;
  peer->egress->insertFrame(frame);
  int signal = 1;
  ssize_t res = write(m_framebufferHasDataPipe[SIGNAL_PIPE_WRITE], &signal, sizeof(signal));
  if (res != sizeof(signal)) {
    /* The net thread may simply be slower than the producer at draining the
     * pipe; a full pipe is not an error (it is already going to flush). */
    if (errno != EWOULDBLOCK && errno != EAGAIN)
      lwarn << "could not write to pipe " << res << std::endl;
  }
}

void TCPServerThread::flushPeer(TcpPeer &peer) {
  if (peer.connectState != NEGOTIATED)
    return;

  /*
   * Backpressure: if this peer is already sitting on a large unsent backlog it
   * is not keeping up, so stop moving more frames out of its egress ring. The
   * ring then drops its oldest frames under continued overload (drop-oldest),
   * which bounds memory and isolates the slow peer without touching the others.
   */
  if (peer.txPending.size() - peer.txSent >= TX_BACKLOG_SOFT_BYTES)
    return;

  /* Move every queued egress frame into the peer's outbound byte stream. */
  uint8_t enc[MAX_TRANSMIT_BUFFER_SIZE_BYTES];
  peer.egress->swapBuffers();
  std::list<canfd_frame *> *frames = peer.egress->getIntermediateBuffer();
  for (canfd_frame *frame : *frames) {
    size_t encodedBytes = encodeFrame(enc, frame);
    peer.txPending.insert(peer.txPending.end(), enc, enc + encodedBytes);
    peer.txCount++;
  }
  peer.egress->unlockIntermediateBuffer();
  peer.egress->mergeIntermediateBuffer();
}

bool TCPServerThread::drainPending(TcpPeer &peer) {
  while (peer.txSent < peer.txPending.size()) {
    size_t remaining = peer.txPending.size() - peer.txSent;
    ssize_t w = send(peer.fd, peer.txPending.data() + peer.txSent, remaining, MSG_NOSIGNAL);
    if (w > 0) {
      peer.txSent += static_cast<size_t>(w);
      continue;
    }
    if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break; /* socket send buffer full: park the rest on EPOLLOUT */
    if (w < 0 && errno == EINTR)
      continue;
    return false; /* fatal write error: drop this peer */
  }

  if (peer.txSent >= peer.txPending.size()) {
    /* Fully drained: clear the queue and stop watching for writability. */
    peer.txPending.clear();
    peer.txSent = 0;
    if (peer.epollOut) {
      epollMod(peer.fd, EPOLLIN);
      peer.epollOut = false;
    }
  } else {
    /* Still backed up: compact the consumed prefix and watch for writability. */
    if (peer.txSent > 0) {
      peer.txPending.erase(peer.txPending.begin(), peer.txPending.begin() + peer.txSent);
      peer.txSent = 0;
    }
    if (!peer.epollOut) {
      epollMod(peer.fd, EPOLLIN | EPOLLOUT);
      peer.epollOut = true;
    }
    /*
     * Still backed up after a full send attempt: if the parked backlog has
     * grown past the hard cap the peer is a hopeless laggard (its receive
     * window has been stuck long enough that even SOFT-gated egress could not
     * keep it bounded). Drop it so it stops consuming memory; it may reconnect.
     */
    if (peer.txPending.size() - peer.txSent >= TX_BACKLOG_HARD_BYTES) {
      lwarn << "Participant " << peer.id << " TX backlog exceeds "
            << TX_BACKLOG_HARD_BYTES << " bytes; dropping laggard" << std::endl;
      return false;
    }
  }
  return true;
}

void TCPServerThread::flushAllPeers() {
  std::vector<TcpPeer *> dead;
  for (auto &p : m_peers) {
    if (p->connectState != NEGOTIATED)
      continue;
    if (p->egress->getFrameBufferSize() == 0 && p->txPending.empty())
      continue;
    flushPeer(*p);
    if (!drainPending(*p))
      dead.push_back(p.get());
  }
  /* Evict laggards after the iteration so erase() never invalidates it. */
  for (TcpPeer *p : dead)
    evictPeer(p, "write error");
}

void TCPServerThread::cleanup() {
  if (m_serverSocket >= 0)
    close(m_serverSocket);
}
