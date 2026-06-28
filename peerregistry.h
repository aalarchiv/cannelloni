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

#include <cstddef>
#include <shared_mutex>
#include <vector>

#include "peer.h"

namespace cannelloni {

/*
 * Holds the set of participants on the virtual shared CAN bus.
 *
 * Phase 1 populated this once at startup and treated it as read-only. Phase 3
 * (dynamic UDP peer discovery / liveness eviction) mutates it at runtime from
 * the net thread while the CAN thread is routing, so access is synchronised by
 * an internal shared_mutex:
 *
 *   - Routing threads (Router::route) hold a SHARED (read) lock for the whole
 *     fan-out, so the Peer records and the egress buffers they point at stay
 *     alive for the duration. Several routers may fan out concurrently.
 *   - add()/remove() take an EXCLUSIVE (write) lock; remove() therefore only
 *     returns once no router is mid-fan-out, which is what lets the caller
 *     destroy the removed peer's egress buffer without a use-after-free.
 *
 * The lock is exposed via mutex() so the Router can hold a shared lock across
 * the iteration + egress access (the work it does per peer is Router logic, not
 * registry logic). Only the local CAN bus and the net thread are participants,
 * so the peer set is small and these critical sections are short.
 */
class PeerRegistry {
  public:
    /* Self-locking helpers for single-threaded startup wiring. */
    void add(const Peer &peer);
    bool remove(PeerId id);

    /*
     * Lock-free variants for the runtime hub path. The discovery thread mutates
     * the registry AND its own peer index/vector together, and the routing
     * threads read both under the same lock, so the caller takes mutex()
     * exclusively and performs all of those mutations in one critical section.
     * Caller MUST already hold mutex() exclusively.
     */
    void addLocked(const Peer &peer);
    bool removeLocked(PeerId id);

    /* Caller MUST hold at least a shared lock on mutex() while using these. */
    const std::vector<Peer> &peers() const;
    Peer *find(PeerId id);
    size_t size() const;

    std::shared_mutex &mutex() { return m_mutex; }

  private:
    std::vector<Peer> m_peers;
    std::shared_mutex m_mutex;
};

}
