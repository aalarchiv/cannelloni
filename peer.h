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

#include <cstdint>

namespace cannelloni {

class ConnectionThread;
class FrameBuffer;

/*
 * Identifies a participant on the virtual shared CAN bus. The Router uses the
 * id for origin-exclusion (a frame ingested from participant X is delivered to
 * every participant except X).
 */
typedef uint32_t PeerId;

/* The local CAN bus is always participant 0. Network peers get ids >= 1. */
static const PeerId CAN_PEER_ID = 0;
static const PeerId FIRST_NET_PEER_ID = 1;

/*
 * A participant on the virtual shared CAN bus (the local CAN bus or one network
 * peer). Each participant owns an egress FrameBuffer holding the frames waiting
 * to be written to it; the ConnectionThread performs the actual egress I/O.
 *
 * Phase 1 keeps per-peer protocol state (UDP seq_no, TCP Decoder) inside the
 * ConnectionThread since a single net thread serves exactly one peer; it moves
 * into Peer in Phase 2 once one thread multiplexes several peers.
 */
struct Peer {
  PeerId id;
  ConnectionThread *thread; /* performs egress I/O for this participant */
  FrameBuffer *egress;      /* frames waiting to be written to this participant */
};

}
