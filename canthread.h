/*
 * This file is part of cannelloni, a SocketCAN over ethernet tunnel.
 *
 * Copyright (C) 2014-2017 Maximilian Güntner <code@sourcediver.org>
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

#include <string>
#include <stdint.h>

#include "connection.h"
#include "timer.h"

namespace cannelloni {

#define CAN_TIMEOUT 2000000 /* 2 sec in us */

/*
 * Requested CAN socket receive buffer (SO_RCVBUF). The kernel clamps this to
 * net.core.rmem_max (raise that sysctl to make a larger value effective); a
 * deeper kernel queue lets the recv loop absorb the inline per-frame fan-out
 * before overflowing under a high-rate burst (epic caveat 8).
 */
#define CAN_RX_BUFFER_BYTES (4 * 1024 * 1024)

class CANThread : public ConnectionThread {
  public:
    CANThread(const struct debugOptions_t &debugOptions,
              const std::string &canInterfaceName);
    virtual ~CANThread();
    virtual int start();
    virtual void stop();
    virtual void run();

    virtual void transmitFrame(canfd_frame *frame, PeerId target);

    /* Whether the CAN interface negotiated CAN-FD (its MTU is CANFD_MTU).
     * Valid after start(); advertised over mDNS so peers can pre-filter on
     * CAN-FD capability (cannelloni-84a.7). */
    bool isCanFd() const { return m_canfd; }

  private:
    void transmitBuffer();
    void fireTimer();

  private:
    struct debugOptions_t m_debugOptions;
    int m_canSocket;
    bool m_canfd;
    Timer m_timer;

    std::string m_canInterfaceName;

    /* Performance Counters */
    uint64_t m_rxCount;
    uint64_t m_txCount;
    /*
     * Cumulative count of frames the kernel dropped at CAN ingress because this
     * socket's receive buffer was full while the inline per-frame fan-out ran
     * (epic caveat 8). Read from the SO_RXQ_OVFL ancillary counter so the loss
     * is observable instead of silent; reported in the shutdown summary.
     */
    uint64_t m_rxDropCount;
};

}
