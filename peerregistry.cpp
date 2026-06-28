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

#include "peerregistry.h"

using namespace cannelloni;

void PeerRegistry::add(const Peer &peer) {
  m_peers.push_back(peer);
}

const std::vector<Peer> &PeerRegistry::peers() const {
  return m_peers;
}

Peer *PeerRegistry::find(PeerId id) {
  for (Peer &peer : m_peers) {
    if (peer.id == id)
      return &peer;
  }
  return nullptr;
}

size_t PeerRegistry::size() const {
  return m_peers.size();
}
