#!/usr/bin/env python3
#
# This file is part of cannelloni, a SocketCAN over Ethernet tunnel.
#
# Copyright (C) 2014-2017 Maximilian Güntner <code@sourcediver.org>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#

"""Byte-level CAN frame comparison for cannelloni characterization tests.

Reads two ``candump -L`` log files and compares the frames they contain. The
``candump -L`` line format is::

    (1234567890.123456) vcan0 123#1122334455667788

The third whitespace-separated field is the *frame token*. candump renders the
complete frame in it, byte for byte, including:

  * classic data frames   ``123#1122334455667788``
  * 29-bit extended (EFF)  ``18FFAA01#DEADBEEF``
  * remote (RTR) frames    ``200#R`` / ``201#R8``
  * CAN-FD frames          ``300##411223344``      (## then flags nibble)
  * CAN-FD + BRS           ``301##5...``            (flags nibble has BRS bit)

Because both the sender bus and the receiver bus are captured with the same
``candump -L``, comparing the frame tokens *verbatim* is a true byte-level
comparison without having to re-implement candump's rendering. This is the
"or equivalent" comparator referenced by issue cannelloni-84a.8 (Phase 0).

Why not candump_compare.py? That script (a) requires globally unique CAN IDs
(it pairs the two arrivals of each ID), (b) raises ValueError on CAN-FD frames
in candump -L format (``frame.split('#')`` yields an empty flags field), and
(c) cannot characterize ordering. This comparator handles all frame types,
allows repeated IDs, and adds an ordered mode for the -s sort path.

Usage:

    frame_compare.py MODE LEFT.log RIGHT.log

    MODE = multiset   LEFT and RIGHT contain the same frames (order-independent)
           identical  LEFT and RIGHT contain the same frames in the same order
                      (characterizes FIFO pass-through of a batch without -s)
           subset     every frame in LEFT also appears in RIGHT (>= multiplicity)
           sorted     multiset(LEFT) == multiset(RIGHT) AND RIGHT is ordered by
                      ascending CAN id (characterizes cannelloni -s)

Exit status is 0 on success, 1 on mismatch (with a diagnostic on stderr).
"""

import sys
from collections import Counter


def frame_tokens(path):
    """Return the list of frame tokens (verbatim) from a candump -L log."""
    tokens = []
    with open(path) as f:
        for line in f:
            parts = line.split()
            # A valid line is "(timestamp) iface ID#DATA"; skip anything else
            # (blank lines, partially-written final line, etc.).
            if len(parts) < 3:
                continue
            if not (parts[0].startswith('(') and parts[0].endswith(')')):
                continue
            tokens.append(parts[2])
    return tokens


def can_id(token):
    """Numeric CAN id of a frame token, for ordering checks.

    The id is the hex string before the first '#'. EFF (8 hex digits) and SFF
    (3 hex digits) are both parsed as plain integers; the sort test uses a
    single id width so this stays unambiguous.
    """
    return int(token.split('#', 1)[0], 16)


def fail(msg):
    sys.stderr.write("frame_compare: FAIL: %s\n" % msg)
    return 1


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(__doc__)
        return 2
    mode, left_path, right_path = argv[1], argv[2], argv[3]
    left = frame_tokens(left_path)
    right = frame_tokens(right_path)
    # A characterization comparison always expects frames; an empty LEFT means
    # the capture (or the send) silently failed, so never pass vacuously.
    if not left:
        return fail("LEFT/expected frame set is empty (capture or send failure?)")
    lc, rc = Counter(left), Counter(right)

    if mode in ("multiset", "sorted"):
        if lc != rc:
            missing = list((lc - rc).elements())
            extra = list((rc - lc).elements())
            return fail("multiset mismatch (%d expected, %d actual)\n"
                        "  missing from actual: %s\n"
                        "  unexpected in actual: %s"
                        % (len(left), len(right), missing, extra))
        if mode == "sorted":
            ids = [can_id(t) for t in right]
            for i in range(1, len(ids)):
                if ids[i] < ids[i - 1]:
                    return fail("not sorted by CAN id at position %d: "
                                "%s then %s" % (i, right[i - 1], right[i]))
        print("frame_compare: OK (%s, %d frames)" % (mode, len(left)))
        return 0

    if mode == "identical":
        if left != right:
            # Find the first divergence to make failures legible.
            n = min(len(left), len(right))
            pos = next((i for i in range(n) if left[i] != right[i]), n)
            return fail("frame sequences differ at position %d "
                        "(expected %d frames, got %d)\n"
                        "  expected: %s\n"
                        "  actual:   %s"
                        % (pos, len(left), len(right),
                           left[pos] if pos < len(left) else "<end>",
                           right[pos] if pos < len(right) else "<end>"))
        print("frame_compare: OK (identical, %d frames)" % len(left))
        return 0

    if mode == "subset":
        deficit = lc - rc
        if deficit:
            return fail("not a subset; missing from actual: %s"
                        % list(deficit.elements()))
        print("frame_compare: OK (subset, %d expected frames present)"
              % len(left))
        return 0

    return fail("unknown mode %r" % mode)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
