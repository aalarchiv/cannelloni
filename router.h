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

#include "peer.h"

struct canfd_frame;

namespace cannelloni {

class PeerRegistry;

/*
 * The hub core. Replaces the former 1:1 ConnectionThread::m_peerThread spine.
 *
 * route() delivers a frame ingested from participant `origin` to every other
 * participant (origin-exclusion), which gives shared-bus behaviour plus 1-hop
 * loop avoidance. Pooled canfd_frame* cannot be shared between egress buffers,
 * so each target receives its own copy (request-from-target-pool + memcpy +
 * transmitFrame).
 *
 * With exactly one CAN participant and one net peer this is behaviourally
 * identical to the previous direct CAN<->peer delivery.
 */
class Router {
  public:
    Router(PeerRegistry &registry, bool debugBuffer = false);

    void route(const canfd_frame *frame, PeerId origin);

  private:
    PeerRegistry &m_registry;
    bool m_debugBuffer;
};

}
