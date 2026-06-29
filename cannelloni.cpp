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

#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <getopt.h>
#include <iostream>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include <iomanip>

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>

#include <sys/signalfd.h>

#include "cannelloni.h"
#include "config.h"
#include "connection.h"
#include "inet_address.h"
#include "tcpthread.h"
#include "udpthread.h"

#ifdef SCTP_SUPPORT
#include "sctpthread.h"
#endif

#ifdef AVAHI_SUPPORT
#include "mdns.h"
#endif

#include "canthread.h"
#include "csvmapparser.h"
#include "framebuffer.h"
#include "logging.h"
#include "make_unique.h"
#include "peer.h"
#include "peerregistry.h"
#include "router.h"
#include <memory>

#define MIN_LINK_MTU_SIZE 100

#define CANNELLONI_VERSION "2.0.1"

using namespace cannelloni;

void printUsage() {
  std::cout << "cannellonis Release: " << CANNELLONI_VERSION << std::endl;
  std::cout << "Usage: cannellonis OPTIONS" << std::endl;
  std::cout << "Available options:" << std::endl;
#ifdef SCTP_SUPPORT
  std::cout << "\t -S [cs] \t\t enable SCTP transport." << std::endl;
  std::cout << "\t\t\t c : act as client" << std::endl;
  std::cout << "\t\t\t s : act as server" << std::endl;
#endif
  std::cout << "\t -C [cs] \t\t enable TCP transport." << std::endl;
  std::cout << "\t\t\t c : act as client" << std::endl;
  std::cout << "\t\t\t s : act as server" << std::endl;
  std::cout << "\t -l PORT \t\t listening port, default: 20000" << std::endl;
  std::cout << "\t -L ADDRESS \t\t listening ADDRESS, default: 0.0.0.0" << std::endl;
  std::cout << "\t -r PORT \t\t remote port, default: 20000" << std::endl;
  std::cout << "\t -R ADDRESS \t\t remote ADDRESS (mandatory for UDP unless --peer is used), default: 127.0.0.1" << std::endl;
  std::cout << "\t --peer HOST[:PORT] \t add a UDP hub peer (repeatable); PORT defaults to -r" << std::endl;
  std::cout << "\t --peers-file FILE \t read UDP hub peers from FILE (one HOST[:PORT] per line)" << std::endl;
  std::cout << "\t --discover \t\t learn UDP peers at runtime from valid traffic (hub, off by default)" << std::endl;
  std::cout << "\t --max-peers N \t\t cap on discovered UDP peers / accepted TCP server clients, default: 16" << std::endl;
  std::cout << "\t --peer-timeout SEC \t evict a discovered peer after SEC seconds of silence (0=never), default: 30" << std::endl;
#ifdef AVAHI_SUPPORT
  std::cout << "\t --mdns \t\t discover UDP peers via mDNS/Avahi zeroconf (hub, off by default)" << std::endl;
#endif
  std::cout << "\t --buffer-frames N \t per-participant egress pool: frames preallocated, default: 1000" << std::endl;
  std::cout << "\t --buffer-max N \t\t per-participant egress pool: hard cap on frames (0=unlimited), default: 16000" << std::endl;
  std::cout << "\t -I INTERFACE \t\t can interface, default: vcan0" << std::endl;
  std::cout << "\t -t timeout \t\t buffer timeout for can messages (us), default: 100000" << std::endl;
  std::cout << "\t -T table.csv \t\t path to csv with individual timeouts" << std::endl;
  std::cout << "\t -s           \t\t enable frame sorting" << std::endl;
  std::cout << "\t -p           \t\t no peer checking" << std::endl;
  std::cout << "\t -d [cubt]\t\t enable debug, can be any of these: " << std::endl;
  std::cout << "\t\t\t c : enable debugging of can frames" << std::endl;
#ifdef SCTP_SUPPORT
  std::cout << "\t\t\t u : enable debugging of udp/tcp/sctp frames" << std::endl;
#else
  std::cout << "\t\t\t u : enable debugging of udp/tcp frames" << std::endl;
#endif
  std::cout << "\t\t\t b : enable debugging of internal buffer structures" << std::endl;
  std::cout << "\t\t\t t : enable debugging of internal timers" << std::endl;
  std::cout << "\t -4 \t\t\t use IPv4 (default)" << std::endl;
  std::cout << "\t -6 \t\t\t use IPv6" << std::endl;
  std::cout << "\t -m \t\t\t set MTU, default: 1500 bytes" << std::endl;
  std::cout << "\t -f \t\t\t fork into background / daemon mode" << std::endl;
  std::cout << "\t -P \t\t\t pid file path (only in daemon mode), default: /var/run/cannellonis.pid" << std::endl;
  std::cout << "\t -h \t\t\t display this help text" << std::endl;
}

void daemonize(std::string pidFilePath) {
  pid_t pid = fork();
  if (pid < 0) {
    exit(EXIT_FAILURE);
  }
  if (pid > 0) {
    exit(EXIT_SUCCESS); // exit parent
  }

  if (setsid() < 0) {
    exit(EXIT_FAILURE);
  }

  // fork again
  pid = fork();
  if (pid < 0) {
    exit(EXIT_FAILURE);
  }
  if (pid > 0) {
    exit(EXIT_SUCCESS); // exit parent
  }

  std::cout << "pid: " << getpid() << std::endl;

  std::ofstream pidFile;
  pidFile.open(pidFilePath,  std::ofstream::out);
  if (pidFile.fail()) {
    std::cerr << "could not write pid file" << std::endl;
    exit(EXIT_FAILURE);
  }
  pidFile << getpid() << std::endl;
  pidFile.flush();
  pidFile.close();

  // change to root, cannelloni only may read the
  // timeoutTableFile which has already happend
  // by the time this function is called
  if (chdir("/") < 0) {
    exit(EXIT_FAILURE);
  }

  close(STDIN_FILENO);
  close(STDOUT_FILENO);
  close(STDERR_FILENO);

  int null_fd = open("/dev/null", O_RDWR);
  if (null_fd == -1) {
    exit(EXIT_FAILURE);
  }

  dup2(null_fd, STDIN_FILENO);
  dup2(null_fd, STDOUT_FILENO);
  dup2(null_fd, STDERR_FILENO);

  if (null_fd > STDERR_FILENO) {
    close(null_fd);
  }
}

/* Long-only options (no short equivalent) for the multi-peer hub. */
enum {
  OPT_PEER = 1000,
  OPT_PEERS_FILE,
  OPT_DISCOVER,
  OPT_MAX_PEERS,
  OPT_PEER_TIMEOUT,
  OPT_BUFFER_FRAMES,
  OPT_BUFFER_MAX,
  OPT_MDNS,
};

/*
 * Parse a "host:port" peer specification into a sockaddr_storage. IPv6
 * literals with a port must be bracketed ("[::1]:20000"); a bare address uses
 * defaultPort. host may be an IP literal or a DNS name (resolved by
 * parseAddress). Returns false on a malformed spec or resolution failure.
 */
static bool parsePeerSpec(const std::string &spec, int addressFamily,
                          uint16_t defaultPort, struct sockaddr_storage *out) {
  std::string host;
  std::string portStr;

  if (!spec.empty() && spec.front() == '[') {
    /* Bracketed IPv6 literal: [addr] or [addr]:port */
    std::string::size_type close = spec.find(']');
    if (close == std::string::npos)
      return false;
    host = spec.substr(1, close - 1);
    if (close + 1 < spec.size() && spec[close + 1] == ':')
      portStr = spec.substr(close + 2);
  } else {
    std::string::size_type colon = spec.rfind(':');
    /* Only treat as host:port when there is exactly one colon (IPv4 / name);
     * an unbracketed IPv6 literal has several colons and uses defaultPort. */
    if (colon != std::string::npos && spec.find(':') == colon) {
      host = spec.substr(0, colon);
      portStr = spec.substr(colon + 1);
    } else {
      host = spec;
    }
  }

  if (host.empty())
    return false;

  uint16_t port = defaultPort;
  if (!portStr.empty())
    port = static_cast<uint16_t>(strtoul(portStr.c_str(), NULL, 10));

  memset(out, 0, sizeof(*out));
  if (!parseAddress(host.c_str(), (struct sockaddr *) out, addressFamily))
    return false;

  if (addressFamily == AF_INET)
    ((struct sockaddr_in *) out)->sin_port = htons(port);
  else
    ((struct sockaddr_in6 *) out)->sin6_port = htons(port);
  return true;
}

int main(int argc, char **argv) {
  int opt;
  bool remoteIPSupplied = false;
  bool sortUDP = false;
  bool checkPeer = true;
  bool useTCP = false;
  bool useSCTP = false;
  bool useIPv4 = true;
  bool useIPv6 = false;
  bool forkIntoBackground = false;
  uint16_t linkMtuSize = 1500;
  TCPThreadRole tcpRole = TCP_CLIENT;
#ifdef SCTP_SUPPORT
  SCTPThreadRole sctpRole = SCTP_CLIENT;
#endif
  char remoteIP[INET6_ADDRSTRLEN] = "";
  uint16_t remotePort = 20000;
  char localIP[INET6_ADDRSTRLEN] = "";
  uint16_t localPort = 20000;
  std::string canInterfaceName = "vcan0";
  uint32_t bufferTimeout = 100000;
  std::string timeoutTableFile;
  std::string pidFilePath = "/var/run/cannellonis.pid";
  /* Key is CAN ID, Value is timeout in us */
  std::map<uint32_t, uint32_t> timeoutTable;
  /* Static UDP hub peer list (--peer host:port, repeatable; --peers-file) */
  std::vector<std::string> peerSpecs;
  std::string peersFile;
  /* Dynamic UDP peer discovery (Phase 3, --discover). */
  bool discover = false;
  size_t maxPeers = 16;
  uint32_t peerTimeoutSec = 30;
  /* mDNS/Avahi zeroconf peer discovery (cannelloni-84a.7, --mdns). */
  bool useMdns = false;
  /* Per-participant egress FrameBuffer pool sizing (see framebuffer.h). */
  size_t bufferFrames = DEFAULT_BUFFER_FRAMES;
  size_t bufferMax = DEFAULT_BUFFER_MAX;

  struct debugOptions_t debugOptions = { /* can */ 0, /* udp */ 0, /* buffer */ 0, /* timer */ 0 };

  const std::string argument_options = "C:l:L:r:R:I:t:T:d:m:P:hsp46f"
#ifdef SCTP_SUPPORT
  "S:";
#else
  ;
#endif

  static struct option long_options[] = {
    {"peer",          required_argument, 0, OPT_PEER},
    {"peers-file",    required_argument, 0, OPT_PEERS_FILE},
    {"discover",      no_argument,       0, OPT_DISCOVER},
    {"max-peers",     required_argument, 0, OPT_MAX_PEERS},
    {"peer-timeout",  required_argument, 0, OPT_PEER_TIMEOUT},
    {"buffer-frames", required_argument, 0, OPT_BUFFER_FRAMES},
    {"buffer-max",    required_argument, 0, OPT_BUFFER_MAX},
    {"mdns",          no_argument,       0, OPT_MDNS},
    {0, 0, 0, 0}
  };

  while ((opt = getopt_long(argc, argv, argument_options.c_str(), long_options, NULL)) != -1) {
    switch(opt) {
      case OPT_PEER:
        peerSpecs.push_back(optarg);
        break;
      case OPT_PEERS_FILE:
        peersFile = std::string(optarg);
        break;
      case OPT_DISCOVER:
        discover = true;
        break;
      case OPT_MAX_PEERS:
        maxPeers = static_cast<size_t>(strtoul(optarg, NULL, 10));
        break;
      case OPT_PEER_TIMEOUT:
        peerTimeoutSec = static_cast<uint32_t>(strtoul(optarg, NULL, 10));
        break;
      case OPT_BUFFER_FRAMES:
        bufferFrames = static_cast<size_t>(strtoul(optarg, NULL, 10));
        break;
      case OPT_BUFFER_MAX:
        bufferMax = static_cast<size_t>(strtoul(optarg, NULL, 10));
        break;
      case OPT_MDNS:
#ifdef AVAHI_SUPPORT
        useMdns = true;
#else
        std::cout << "Usage Error: " << std::endl
                  << "mDNS discovery is not supported in this build." << std::endl
                  << std::endl;
        printUsage();
        return -1;
#endif
        break;
      case 'C':
        switch (optarg[0]) {
          case 's':
          case 'S':
            tcpRole = TCP_SERVER;
            useTCP = true;
            break;
          case 'c':
          case 'C':
            tcpRole = TCP_CLIENT;
            useTCP = true;
            break;
          default:
            std::cout << "Usage Error: " << std::endl
                      << "-C only accepts [s]erver or [c]lient" << std::endl;
            printUsage();
            return -1;
        }
        break;
#ifdef SCTP_SUPPORT
      case 'S':
        switch (optarg[0]) {
          case 's':
          case 'S':
            sctpRole = SCTP_SERVER;
            useSCTP = true;
            break;
          case 'c':
          case 'C':
            sctpRole = SCTP_CLIENT;
            useSCTP = true;
            break;
          default:
            std::cout << "Usage Error: " << std::endl
                      << "-S only accepts [s]erver or [c]lient" << std::endl;
            printUsage();
            return -1;
        }
        break;
#else
      case'S':
            std::cout << "Usage Error: " << std::endl
                      << "SCTP Transport is not supported in this build." << std::endl
                                                                          << std::endl;
            printUsage();
            return -1;
#endif
      case 'l':
        localPort = strtoul(optarg, NULL, 10);
        break;
      case 'L':
        strncpy(localIP, optarg, INET6_ADDRSTRLEN-1);
        localIP[INET6_ADDRSTRLEN-1] = '\0';
        break;
      case 'r':
        remotePort = strtoul(optarg, NULL, 10);
        break;
      case 'R':
        strncpy(remoteIP, optarg, INET6_ADDRSTRLEN-1);
        remoteIP[INET6_ADDRSTRLEN-1] = '\0';
        remoteIPSupplied = true;
        break;
      case 'I':
        canInterfaceName = std::string(optarg);
        break;
      case 't':
        bufferTimeout = static_cast<uint32_t>(strtoul(optarg, NULL, 10));
        break;
      case 'T':
        timeoutTableFile = std::string(optarg);
        break;
      case 'd':
        if (strchr(optarg, 'c'))
          debugOptions.can = 1;
        if (strchr(optarg, 'u'))
          debugOptions.udp = 1;
        if (strchr(optarg, 'b'))
          debugOptions.buffer = 1;
        if (strchr(optarg, 't'))
          debugOptions.timer = 1;
        break;
      case 'h':
        printUsage();
        return 0;
      case 's':
        sortUDP = true;
        break;
      case 'p':
        checkPeer = false;
        break;
      case '4':
        useIPv4 = true;
        break;
      case '6':
        useIPv6 = true;
        useIPv4 = false;
        break;
      case 'm':
        linkMtuSize = static_cast<uint16_t>(strtol(optarg, NULL, 10));
        break;
      case 'f':
        forkIntoBackground = true;
        break;
      case 'P':
        pidFilePath = std::string(optarg);
        break;
      default:
        printUsage();
        return -1;
    }
  }
  if (useIPv4 && useIPv6) {
    std::cout << "Usage Error: " << std::endl
              << "Can't use IPv4 and IPv6 simultaneously" << std::endl
              << std::endl;
    printUsage();
    return -1;
  }

  if (useTCP && useSCTP) {
    std::cout << "Usage Error: " << std::endl
              << "Can't use TCP and SCTP simultaneously" << std::endl
                                          << std::endl;
    printUsage();
    return -1;
  }

  /* Pull additional peer specs from a peers file (one host:port per line;
   * blank lines and lines starting with '#' are ignored). */
  if (!peersFile.empty()) {
    std::ifstream peersStream(peersFile);
    if (!peersStream.is_open()) {
      lerror << "Unable to open peers file " << peersFile << "." << std::endl;
      return -1;
    }
    std::string line;
    while (std::getline(peersStream, line)) {
      std::string::size_type start = line.find_first_not_of(" \t\r\n");
      if (start == std::string::npos)
        continue;
      std::string::size_type end = line.find_last_not_of(" \t\r\n");
      std::string trimmed = line.substr(start, end - start + 1);
      if (trimmed.empty() || trimmed[0] == '#')
        continue;
      peerSpecs.push_back(trimmed);
    }
  }

  /* The hub (static peers or dynamic discovery, traffic- or mDNS-learnt) is
   * UDP-only in this phase. */
  if ((!peerSpecs.empty() || discover || useMdns) && (useTCP || useSCTP)) {
    std::cout << "Usage Error: " << std::endl
              << "--peer/--peers-file/--discover/--mdns is only supported for UDP" << std::endl
              << std::endl;
    printUsage();
    return -1;
  }

  /* A discovery hub may start with no peers at all and learn them at runtime,
   * so -R/--peer is not required when --discover or --mdns is set. */
  if (!remoteIPSupplied && peerSpecs.empty() && !discover && !useMdns && !useSCTP && !useTCP) {
    std::cout << "Usage Error: " << std::endl
              << "Remote IP not supplied (use -R, --peer, --discover or --mdns for UDP)" << std::endl
              << std::endl;
    printUsage();
    return -1;
  }
  if (bufferTimeout == 0) {
    std::cout << "Usage Error: " << std::endl
              << "Only non-zero timeouts are allowed" << std::endl
              << std::endl;
    printUsage();
    return -1;
  }
  /* A hard cap below the preallocation would over-allocate at startup and then
   * refuse to grow (resizePool ignores the cap on the initial reservation), so
   * reject the inconsistent combination up front. A cap of 0 means unlimited. */
  if (bufferMax != 0 && bufferFrames > bufferMax) {
    std::cout << "Usage Error: " << std::endl
              << "--buffer-frames must not exceed --buffer-max (0 = unlimited)" << std::endl
              << std::endl;
    printUsage();
    return -1;
  }
  if (linkMtuSize < MIN_LINK_MTU_SIZE) {
    std::cout << "Usage Error: " << std::endl
              << "Specify a link mtu size greater than " << MIN_LINK_MTU_SIZE << std::endl
              << std::endl;
    printUsage();
    return -1;
  }

  // set default values if no IPs have been provided
  if (strlen(localIP) == 0) {
    if (useIPv4) {
      strcpy(localIP, "0.0.0.0");
    } else {
      strcpy(localIP, "::");
    }
  }

  if (!timeoutTableFile.empty()) {
    CSVMapParser<uint32_t,uint32_t> mapParser;
    if(!mapParser.open(timeoutTableFile)) {
      lerror << "Unable to open " << timeoutTableFile << "." << std::endl;
      return -1;
    }
    if(!mapParser.parse()) {
      lerror << "Error while parsing " << timeoutTableFile << "." << std::endl;
      return -1;
    }
    if(!mapParser.close()) {
      lerror << "Error while closing" << timeoutTableFile << "." << std::endl;
      return -1;
    }
    timeoutTable = mapParser.read();
  }

  if (debugOptions.timer) {
    if (timeoutTable.empty()) {
      linfo << "No custom timeout table specified, using "
            << bufferTimeout << " us for all frames." << std::endl;
    } else {
      linfo << "Custom timeout table loaded: " << std::endl;
      linfo << "*---------------------*" << std::endl;
      linfo << "|  ID  | Timeout (us) |" << std::endl;
      std::map<uint32_t,uint32_t>::iterator it;
      for (it=timeoutTable.begin(); it!=timeoutTable.end(); ++it)
        linfo << "|" << std::setw(6) << it->first << "|" << std::setw(14) << it->second << "| " << std::endl;
      linfo << "*---------------------*" << std::endl;
      linfo << "Other Frames:" << bufferTimeout << " us." << std::endl;
    }
  }

  /* We use the signalfd() system call to create a
   * file descriptor to receive signals */
  sigset_t signalMask;
  struct signalfd_siginfo signalFdInfo;
  int signalFD;

  /* Prepare the signalMask */
  sigemptyset(&signalMask);
  sigaddset(&signalMask, SIGTERM);
  sigaddset(&signalMask, SIGINT);
  /* Block these signals... */
  if (sigprocmask(SIG_BLOCK, &signalMask, NULL) == -1) {
    lerror << "sigprocmask error" << std::endl;
    return -1;
  }
  /* ...since we want to receive them through signalFD */
  signalFD = signalfd(-1, &signalMask, 0);
  if (signalFD == -1) {
    lerror << "signalfd error" << std::endl;
    return -1;
  }

  struct sockaddr_storage remoteAddr;
  struct sockaddr_storage localAddr;
  memset(&remoteAddr, 0, sizeof(sockaddr_storage));
  memset(&localAddr, 0, sizeof(sockaddr_storage));

  int addressFamily = AF_INET;
  if (useIPv6) {
    addressFamily = AF_INET6;
  }

  if (remoteIPSupplied && !parseAddress(remoteIP, (struct sockaddr *) &remoteAddr, addressFamily)) {
    lerror << "Invalid remote address";
    return -1;
  }

  if (!parseAddress(localIP, (struct sockaddr *) &localAddr, addressFamily)) {
    lerror << "Invalid listen address";
    return -1;
  }

  if (addressFamily == AF_INET) {
    ((struct sockaddr_in *) &remoteAddr)->sin_port = htons(remotePort);
    ((struct sockaddr_in *) &localAddr)->sin_port = htons(localPort);
  } else if (addressFamily == AF_INET6) {
    ((struct sockaddr_in6 *) &remoteAddr)->sin6_port = htons(remotePort);
    ((struct sockaddr_in6 *) &localAddr)->sin6_port = htons(localPort);
  }
  
  if (forkIntoBackground) {
    std::cout << "cannellonis is forking into background." << std::endl;
    daemonize(pidFilePath);
  }

  /*
   * Build the hub. The local CAN bus is participant 0; each network peer gets
   * an id >= 1. Every participant owns an egress FrameBuffer and the Router
   * fans a frame out to every participant except its origin (origin-exclusion).
   * The registry, router and per-peer buffers outlive the threads, which are
   * joined below before main returns.
   */
  auto canThread = std::make_unique<CANThread>(debugOptions, canInterfaceName);
  auto canFrameBuffer = std::make_unique<FrameBuffer>(bufferFrames, bufferMax);
  canThread->setFrameBuffer(canFrameBuffer.get());

  PeerRegistry registry;
  registry.add(Peer{ CAN_PEER_ID, canThread.get(), canFrameBuffer.get() });

  /* Per-network-peer egress buffers, owned here (outlive the threads). */
  std::vector<std::unique_ptr<FrameBuffer>> netFrameBuffers;

#ifdef AVAHI_SUPPORT
  /* The UDP hub thread, captured for the mDNS backend's resolve callback (which
   * hands discovered addresses to it). Null unless this is a UDP mDNS hub. */
  UDPThread *mdnsTarget = nullptr;
#endif

  std::unique_ptr<ConnectionThread> netThread;
  if (useTCP && tcpRole == TCP_SERVER) {
    /*
     * The TCP server is a hub: it accepts and tracks many clients, each a
     * participant on the virtual shared bus, learnt on accept and evicted on
     * disconnect. It therefore starts with no static net peer (unlike the TCP
     * client / SCTP fallback below) and mutates the same registry the Router
     * reads. --max-peers caps the number of simultaneous clients.
     */
    auto serverThread = std::make_unique<TCPServerThread>(debugOptions, TCPServerThreadParams {
        .remoteAddr = remoteAddr,
        .localAddr = localAddr,
        .addressFamily = addressFamily,
        .checkPeer = checkPeer
      });
    serverThread->setRegistry(&registry, maxPeers);
    serverThread->setEgressPoolSize(bufferFrames, bufferMax);
    netThread = std::move(serverThread);
  } else if (useTCP && tcpRole == TCP_CLIENT) {
    netThread = std::make_unique<TCPClientThread>(debugOptions, TCPThreadParams {
        .remoteAddr = remoteAddr,
        .localAddr = localAddr,
        .addressFamily = addressFamily,
    });
  } else if (useSCTP) {
#ifdef SCTP_SUPPORT
    auto sctpThread = std::make_unique<SCTPThread>(debugOptions, SCTPThreadParams {
      .remoteAddr = remoteAddr,
      .localAddr = localAddr,
      .addressFamily = addressFamily,
      .sortFrames = sortUDP,
      .checkPeer = checkPeer,
      .linkMtuSize = linkMtuSize,
      .role = sctpRole,
    });
    sctpThread.get()->setTimeout(bufferTimeout);
    sctpThread.get()->setTimeoutTable(timeoutTable);
    netThread = std::move(sctpThread);
#endif
  } else {
    /*
     * UDP hub: one multiplexed thread serving one or more peers over a single
     * bound socket. Each peer gets its own egress buffer (per-peer seq_no +
     * stream), registered with both the thread (for sendto/flush scheduling)
     * and the registry (for routing).
     */
    auto udpThread = std::make_unique<UDPThread>(debugOptions, UDPThreadParams{
      .remoteAddr = remoteAddr,
      .localAddr = localAddr,
      .addressFamily = addressFamily,
      .sortFrames = sortUDP,
      .checkPeer = checkPeer,
      .linkMtuSize = linkMtuSize,
    });

    udpThread.get()->setTimeout(bufferTimeout);
    udpThread.get()->setTimeoutTable(timeoutTable);

    /*
     * Resolve the static peer address list: explicit --peer/--peers-file specs,
     * else the legacy single -R/-r remote (which keeps 1-peer behaviour
     * identical). With --discover and no static peers the hub starts empty and
     * learns every peer at runtime, so the list is left empty here.
     */
    std::vector<struct sockaddr_storage> udpPeerAddrs;
    if (!peerSpecs.empty()) {
      for (const std::string &spec : peerSpecs) {
        struct sockaddr_storage addr;
        if (!parsePeerSpec(spec, addressFamily, remotePort, &addr)) {
          lerror << "Invalid peer specification: " << spec << std::endl;
          return -1;
        }
        udpPeerAddrs.push_back(addr);
      }
    } else if (remoteIPSupplied || !discover) {
      udpPeerAddrs.push_back(remoteAddr);
    }

    udpThread->setEgressPoolSize(bufferFrames, bufferMax);

    PeerId nextId = FIRST_NET_PEER_ID;
    for (const struct sockaddr_storage &addr : udpPeerAddrs) {
      auto egress = std::make_unique<FrameBuffer>(bufferFrames, bufferMax);
      udpThread->addPeer(nextId, addr, egress.get());
      registry.add(Peer{ nextId, udpThread.get(), egress.get() });
      netFrameBuffers.push_back(std::move(egress));
      ++nextId;
    }

    /*
     * Dynamic discovery: the thread learns/evicts peers at runtime, mutating
     * the same registry the Router reads (serialised by the registry mutex).
     * mDNS feeds the same machinery (proactively adding peers it resolves), so
     * a discovered peer ages out / re-learns by traffic exactly like Phase 3.
     */
    if (discover || useMdns)
      /* learnFromData only for --discover: mDNS adds peers via resolved,
       * self-filtered services, never by adopting arbitrary senders. */
      udpThread->enableDiscovery(&registry, maxPeers, peerTimeoutSec, /*learnFromData=*/discover);
#ifdef AVAHI_SUPPORT
    if (useMdns)
      mdnsTarget = udpThread.get();
#endif

    netThread = std::move(udpThread);
  }

  /*
   * The TCP client and SCTP serve a single peer: one static egress buffer. (A
   * UDP discovery hub and the TCP server hub legitimately start with no peers
   * and learn them at runtime, so this fallback is gated on the single-peer
   * transports, not merely on an empty buffer list.)
   */
  bool tcpServerHub = useTCP && tcpRole == TCP_SERVER;
  if (netFrameBuffers.empty() && (useTCP || useSCTP) && !tcpServerHub) {
    auto egress = std::make_unique<FrameBuffer>(bufferFrames, bufferMax);
    netThread->setFrameBuffer(egress.get());
    registry.add(Peer{ FIRST_NET_PEER_ID, netThread.get(), egress.get() });
    netFrameBuffers.push_back(std::move(egress));
  }

  Router router(registry, debugOptions.buffer);
  canThread->setRouter(&router, CAN_PEER_ID);
  netThread->setRouter(&router, FIRST_NET_PEER_ID);

  int netStartReturn = netThread->start();
  int canStartReturn = canThread->start();

#ifdef AVAHI_SUPPORT
  /*
   * Start mDNS discovery only once both data threads are up: the CAN thread has
   * by now negotiated CAN-FD (advertised in the TXT records) and the UDP socket
   * is bound to receive the peers we are about to announce ourselves to. The
   * resolve callback runs on Avahi's own thread and just hands the address to
   * the UDP net thread (queueDiscoveredPeer), so no peer state is touched here.
   */
  std::unique_ptr<MdnsDiscovery> mdns;
  if (mdnsTarget != nullptr && netStartReturn == 0 && canStartReturn == 0) {
    mdns = std::make_unique<MdnsDiscovery>(
        addressFamily, localPort, canInterfaceName, canThread->isCanFd(),
        static_cast<uint8_t>(CANNELLONI_FRAME_VERSION),
        [mdnsTarget](const struct sockaddr_storage &addr) {
          mdnsTarget->queueDiscoveredPeer(addr);
        });
    if (!mdns->start()) {
      lwarn << "mDNS discovery could not start; continuing without it." << std::endl;
      mdns.reset();
    }
  }
#endif

  while (netStartReturn == 0 && canStartReturn == 0) {
    struct timeval timeout;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(signalFD, &set);

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    int ret = select(signalFD + 1, &set, NULL, NULL, &timeout);
    if (ret == -1) {
      lerror << "select error" << std::endl;
      break;
    } else if (ret == 0) {
      if (!(netThread->isRunning() && canThread->isRunning())) {
        break;
      }
    } else {
      ssize_t receivedBytes = read(signalFD, &signalFdInfo, sizeof(struct signalfd_siginfo));
      if (receivedBytes != sizeof(struct signalfd_siginfo)) {
        lerror << "signalfd read error" << std::endl;
        break;
      }
      /* Currently we only receive SIGTERM and SIGINT but we check nonetheless */
      if (signalFdInfo.ssi_signo == SIGTERM || signalFdInfo.ssi_signo == SIGINT) {
        linfo << "Received signal " << signalFdInfo.ssi_signo << ": Exiting" << std::endl;
        break;
      }
    }
  }

#ifdef AVAHI_SUPPORT
  /* Stop mDNS first: this joins the Avahi control thread, so no resolve
   * callback can queue a peer into the UDP thread once it begins shutting down. */
  if (mdns)
    mdns->stop();
#endif

  netThread->stop();
  netThread->join();
  canThread->stop();
  canThread->join();

  /* Clear/free pools once all threads are joined */
  for (auto &buffer : netFrameBuffers)
    buffer->clearPool();
  canFrameBuffer->clearPool();

  close(signalFD);
  return 0;
}
