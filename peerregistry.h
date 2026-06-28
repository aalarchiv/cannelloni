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
#include <vector>

#include "peer.h"

namespace cannelloni {

/*
 * Holds the set of participants on the virtual shared CAN bus.
 *
 * Phase 1: the registry is populated once at startup, before any thread starts
 * routing, and is read-only thereafter. Dynamic add/remove (Phase 3 peer
 * discovery / Phase 5 stale-peer drop) will require synchronisation against the
 * routing threads.
 */
class PeerRegistry {
  public:
    void add(const Peer &peer);

    const std::vector<Peer> &peers() const;
    Peer *find(PeerId id);
    size_t size() const;

  private:
    std::vector<Peer> m_peers;
};

}
