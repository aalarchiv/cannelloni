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
#include <linux/can.h>

#include "router.h"
#include "connection.h"
#include "framebuffer.h"
#include "peerregistry.h"

using namespace cannelloni;

Router::Router(PeerRegistry &registry, bool debugBuffer)
  : m_registry(registry)
  , m_debugBuffer(debugBuffer)
{
}

void Router::route(const canfd_frame *frame, PeerId origin) {
  for (const Peer &peer : m_registry.peers()) {
    /* Origin-exclusion: never reflect a frame back to its source. */
    if (peer.id == origin)
      continue;
    /*
     * Take a slot from the target's egress pool and copy the frame into it.
     * overwriteLast=true preserves the previous newest-frame-wins behaviour
     * under overload (the ingest path used the same flag).
     */
    canfd_frame *copy = peer.egress->requestFrame(true, m_debugBuffer);
    if (copy == nullptr)
      continue;
    memcpy(copy, frame, sizeof(struct canfd_frame));
    /*
     * transmitFrame enqueues onto the target's egress buffer and triggers its
     * flush; it also enforces target-specific gating (TCP must be NEGOTIATED,
     * SCTP must be connected) exactly as the previous direct delivery did.
     */
    peer.thread->transmitFrame(copy, peer.id);
  }
}
