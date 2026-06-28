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

#include <mutex>

#include "peerregistry.h"

using namespace cannelloni;

void PeerRegistry::add(const Peer &peer) {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  addLocked(peer);
}

bool PeerRegistry::remove(PeerId id) {
  std::unique_lock<std::shared_mutex> lock(m_mutex);
  return removeLocked(id);
}

void PeerRegistry::addLocked(const Peer &peer) {
  m_peers.push_back(peer);
}

bool PeerRegistry::removeLocked(PeerId id) {
  for (auto it = m_peers.begin(); it != m_peers.end(); ++it) {
    if (it->id == id) {
      m_peers.erase(it);
      return true;
    }
  }
  return false;
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
