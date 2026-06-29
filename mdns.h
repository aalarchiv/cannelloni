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

#include "config.h"

#ifdef AVAHI_SUPPORT

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <sys/socket.h>

namespace cannelloni {

/*
 * Optional mDNS/Avahi zeroconf peer discovery (cannelloni-84a.7). A sibling of
 * the passive traffic-learning discovery (Phase 3), not a replacement: each
 * cannellonis instance advertises a "_cannelloni._udp" service on its transport
 * port and browses the LAN for the same service, so peers learn each other's
 * address+port before any data flows -- no static config.
 *
 * THREADING: Avahi runs its own poll loop, so this owns a SEPARATE control-plane
 * thread (avahi_threaded_poll) that is deliberately NOT pinned with the bounded
 * RT data threads -- discovery is low-rate (ties to PR #84). A resolved,
 * compatible peer is delivered through the PeerFoundCallback, which MUST be
 * thread-safe: it fires on the Avahi thread, and the UDPThread hands the address
 * off to its net thread so all peer-registry mutation stays single-writer.
 *
 * The advertised TXT records (protocol version + CAN-FD capability + interface
 * name) let an instance pre-filter incompatible peers, and a unique per-instance
 * service name lets it skip its own advertisement (never peer with itself).
 *
 * All Avahi state is hidden behind a PIMPL so this header pulls in no Avahi
 * types; the whole class is compiled out unless AVAHI_SUPPORT is defined.
 */
class MdnsDiscovery {
  public:
    /* Called on the Avahi control thread with a resolved, compatible peer's
     * address+port (matching the configured address family). */
    using PeerFoundCallback = std::function<void(const struct sockaddr_storage &)>;

    MdnsDiscovery(int addressFamily, uint16_t port, std::string canInterface,
                  bool canFd, uint8_t protocolVersion, PeerFoundCallback onPeerFound);
    ~MdnsDiscovery();

    MdnsDiscovery(const MdnsDiscovery &) = delete;
    MdnsDiscovery &operator=(const MdnsDiscovery &) = delete;

    /* Begin advertising + browsing, spawning the Avahi control thread. Returns
     * false on failure (e.g. no running avahi-daemon); the caller logs and
     * continues without proactive discovery. */
    bool start();
    /* Stop the control thread and tear down all Avahi state. Idempotent. */
    void stop();

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

}

#endif /* AVAHI_SUPPORT */
