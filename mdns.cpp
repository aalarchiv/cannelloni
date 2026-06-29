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

#include "mdns.h"

#ifdef AVAHI_SUPPORT

#include <cstring>
#include <ctime>
#include <string>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/strlst.h>
#include <avahi-common/thread-watch.h>

#include "logging.h"

namespace cannelloni {

/* The DNS-SD service type advertised + browsed by every cannellonis instance. */
static const char *MDNS_SERVICE_TYPE = "_cannelloni._udp";

/* Extract the value of a TXT key ("key=value") as a string, "" if absent. */
static std::string txtValue(AvahiStringList *txt, const char *key) {
  AvahiStringList *node = avahi_string_list_find(txt, key);
  if (node == nullptr)
    return std::string();
  char *k = nullptr;
  char *v = nullptr;
  size_t size = 0;
  if (avahi_string_list_get_pair(node, &k, &v, &size) != 0)
    return std::string();
  std::string out = (v != nullptr) ? std::string(v, size) : std::string();
  avahi_free(k);
  avahi_free(v);
  return out;
}

class MdnsDiscovery::Impl {
  public:
    Impl(int addressFamily, uint16_t port, std::string canInterface,
         bool canFd, uint8_t protocolVersion, PeerFoundCallback onPeerFound)
      : m_addressFamily(addressFamily)
      , m_avahiProto(addressFamily == AF_INET6 ? AVAHI_PROTO_INET6 : AVAHI_PROTO_INET)
      , m_port(port)
      , m_canInterface(std::move(canInterface))
      , m_canFd(canFd)
      , m_protocolVersion(protocolVersion)
      , m_onPeerFound(std::move(onPeerFound))
    {
      /* A stable, unique-per-instance service name lets us recognise (and skip)
       * our own advertisement; Avahi de-collides clashes by suffixing. */
      char host[256];
      if (gethostname(host, sizeof(host)) != 0)
        std::strcpy(host, "cannellonis");
      host[sizeof(host) - 1] = '\0';
      m_serviceName = std::string("cannellonis ") + host + " " + m_canInterface;
      if (m_serviceName.size() > 63) /* DNS-SD label limit */
        m_serviceName.resize(63);
    }

    ~Impl() { stop(); }

    bool start() {
      m_poll = avahi_threaded_poll_new();
      if (m_poll == nullptr) {
        lerror << "mDNS: failed to create Avahi poll" << std::endl;
        return false;
      }
      /*
       * NO_FAIL tolerates the avahi DAEMON being absent/restarting once the
       * client exists (it reconnects and our callbacks re-publish + re-browse).
       * It does NOT cover the D-Bus system bus or the daemon's bus name not yet
       * being ready at startup: avahi_client_new then fails outright with a
       * D-Bus error. That is a real startup race (cannelloni racing avahi-daemon
       * at boot), so retry briefly before giving up rather than silently running
       * without discovery for the rest of the process's life.
       */
      int error = 0;
      for (int attempt = 0; attempt < 30; ++attempt) {
        m_client = avahi_client_new(avahi_threaded_poll_get(m_poll), AVAHI_CLIENT_NO_FAIL,
                                    clientCallback, this, &error);
        if (m_client != nullptr)
          break;
        struct timespec delay = { 0, 500 * 1000 * 1000 }; /* 500 ms */
        nanosleep(&delay, nullptr);
      }
      if (m_client == nullptr) {
        lerror << "mDNS: failed to create Avahi client: " << avahi_strerror(error) << std::endl;
        avahi_threaded_poll_free(m_poll);
        m_poll = nullptr;
        return false;
      }
      if (avahi_threaded_poll_start(m_poll) < 0) {
        lerror << "mDNS: failed to start Avahi poll thread" << std::endl;
        avahi_client_free(m_client);
        m_client = nullptr;
        avahi_threaded_poll_free(m_poll);
        m_poll = nullptr;
        return false;
      }
      linfo << "mDNS: advertising/browsing " << MDNS_SERVICE_TYPE << " as \""
            << m_serviceName << "\" on port " << m_port << std::endl;
      return true;
    }

    void stop() {
      if (m_poll != nullptr)
        avahi_threaded_poll_stop(m_poll);
      /* Freeing the client tears down the entry group, browser and any
       * outstanding resolvers it owns. */
      if (m_client != nullptr) {
        avahi_client_free(m_client);
        m_client = nullptr;
      }
      m_group = nullptr;
      m_browser = nullptr;
      if (m_poll != nullptr) {
        avahi_threaded_poll_free(m_poll);
        m_poll = nullptr;
      }
    }

  private:
    /* (Re)publish our service entry. Runs on the Avahi thread only. */
    void createServices(AvahiClient *client) {
      if (m_group == nullptr) {
        m_group = avahi_entry_group_new(client, groupCallback, this);
        if (m_group == nullptr) {
          lerror << "mDNS: avahi_entry_group_new failed: "
                 << avahi_strerror(avahi_client_errno(client)) << std::endl;
          return;
        }
      }
      avahi_entry_group_reset(m_group);

      const std::string txtVersion = "version=" + std::to_string(m_protocolVersion);
      const std::string txtFd = std::string("canfd=") + (m_canFd ? "1" : "0");
      const std::string txtIface = "caniface=" + m_canInterface;

      for (;;) {
        int ret = avahi_entry_group_add_service(
            m_group, AVAHI_IF_UNSPEC, m_avahiProto, AvahiPublishFlags(0),
            m_serviceName.c_str(), MDNS_SERVICE_TYPE, nullptr, nullptr, m_port,
            txtVersion.c_str(), txtFd.c_str(), txtIface.c_str(), nullptr);
        if (ret == AVAHI_ERR_COLLISION) {
          /* A local name clash: pick an alternative and try again. */
          pickAlternativeName();
          continue;
        }
        if (ret < 0) {
          lerror << "mDNS: failed to add service: " << avahi_strerror(ret) << std::endl;
          return;
        }
        break;
      }
      int ret = avahi_entry_group_commit(m_group);
      if (ret < 0)
        lerror << "mDNS: failed to commit service group: " << avahi_strerror(ret) << std::endl;
    }

    void pickAlternativeName() {
      char *alt = avahi_alternative_service_name(m_serviceName.c_str());
      m_serviceName = alt;
      avahi_free(alt);
    }

    /* A resolved peer passed the compatibility + self filters: build its
     * address and hand it to the callback (which is responsible for thread-safe
     * delivery to the data path). */
    void deliverPeer(AvahiIfIndex interface, const AvahiAddress *address, uint16_t port) {
      struct sockaddr_storage ss;
      std::memset(&ss, 0, sizeof(ss));
      if (address->proto == AVAHI_PROTO_INET && m_addressFamily == AF_INET) {
        struct sockaddr_in *sin = reinterpret_cast<struct sockaddr_in *>(&ss);
        sin->sin_family = AF_INET;
        sin->sin_port = htons(port);
        sin->sin_addr.s_addr = address->data.ipv4.address; /* already network order */
      } else if (address->proto == AVAHI_PROTO_INET6 && m_addressFamily == AF_INET6) {
        struct sockaddr_in6 *sin6 = reinterpret_cast<struct sockaddr_in6 *>(&ss);
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(port);
        std::memcpy(&sin6->sin6_addr, address->data.ipv6.address, sizeof(sin6->sin6_addr));
        sin6->sin6_scope_id = static_cast<uint32_t>(interface); /* for link-local */
      } else {
        return; /* address family does not match our configured one */
      }
      if (m_onPeerFound)
        m_onPeerFound(ss);
    }

    /* ---- Avahi callbacks (all invoked on the threaded-poll thread) -------- */

    static void clientCallback(AvahiClient *client, AvahiClientState state, void *userdata) {
      Impl *self = static_cast<Impl *>(userdata);
      switch (state) {
        case AVAHI_CLIENT_S_RUNNING:
          /* The daemon is up: (re)publish our service and start browsing. */
          self->createServices(client);
          if (self->m_browser == nullptr) {
            self->m_browser = avahi_service_browser_new(
                client, AVAHI_IF_UNSPEC, self->m_avahiProto, MDNS_SERVICE_TYPE, nullptr,
                AvahiLookupFlags(0), browseCallback, self);
            if (self->m_browser == nullptr)
              lerror << "mDNS: failed to create service browser: "
                     << avahi_strerror(avahi_client_errno(client)) << std::endl;
          }
          break;
        case AVAHI_CLIENT_FAILURE:
          lerror << "mDNS: Avahi client failure: "
                 << avahi_strerror(avahi_client_errno(client)) << std::endl;
          break;
        case AVAHI_CLIENT_S_COLLISION:
        case AVAHI_CLIENT_S_REGISTERING:
          /* The server is reconfiguring; drop our entries until it runs again. */
          if (self->m_group != nullptr)
            avahi_entry_group_reset(self->m_group);
          break;
        case AVAHI_CLIENT_CONNECTING:
          break;
      }
    }

    static void groupCallback(AvahiEntryGroup *group, AvahiEntryGroupState state, void *userdata) {
      Impl *self = static_cast<Impl *>(userdata);
      switch (state) {
        case AVAHI_ENTRY_GROUP_ESTABLISHED:
          linfo << "mDNS: service \"" << self->m_serviceName << "\" established" << std::endl;
          break;
        case AVAHI_ENTRY_GROUP_COLLISION:
          /* Another host announced the same name: rename and re-publish. */
          self->pickAlternativeName();
          lwarn << "mDNS: service name collision, renaming to \"" << self->m_serviceName
                << "\"" << std::endl;
          self->createServices(avahi_entry_group_get_client(group));
          break;
        case AVAHI_ENTRY_GROUP_FAILURE:
          lerror << "mDNS: entry group failure: "
                 << avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(group)))
                 << std::endl;
          break;
        case AVAHI_ENTRY_GROUP_UNCOMMITED:
        case AVAHI_ENTRY_GROUP_REGISTERING:
          break;
      }
    }

    static void browseCallback(AvahiServiceBrowser *browser, AvahiIfIndex interface,
                               AvahiProtocol protocol, AvahiBrowserEvent event,
                               const char *name, const char *type, const char *domain,
                               AvahiLookupResultFlags /*flags*/, void *userdata) {
      Impl *self = static_cast<Impl *>(userdata);
      switch (event) {
        case AVAHI_BROWSER_NEW:
          /* Resolve to learn the address+port and read the capability TXT
           * records. The resolver frees itself in its callback. The self-filter
           * runs there, where we have the final name and flags. */
          if (avahi_service_resolver_new(
                  self->m_client, interface, protocol, name, type, domain,
                  self->m_avahiProto, AvahiLookupFlags(0), resolveCallback, self) == nullptr) {
            lerror << "mDNS: failed to resolve service \"" << name << "\": "
                   << avahi_strerror(avahi_client_errno(self->m_client)) << std::endl;
          }
          break;
        case AVAHI_BROWSER_REMOVE:
          /* The peer withdrew its advertisement. We do not evict here: a peer
           * still sending CAN traffic must stay, and one that has truly gone
           * falls silent and is reaped by the data-liveness sweep (Phase 3). */
          linfo << "mDNS: service \"" << name << "\" left the network" << std::endl;
          break;
        case AVAHI_BROWSER_FAILURE:
          lerror << "mDNS: browser failure: "
                 << avahi_strerror(avahi_client_errno(avahi_service_browser_get_client(browser)))
                 << std::endl;
          break;
        case AVAHI_BROWSER_CACHE_EXHAUSTED:
        case AVAHI_BROWSER_ALL_FOR_NOW:
          break;
      }
    }

    static void resolveCallback(AvahiServiceResolver *resolver, AvahiIfIndex interface,
                                AvahiProtocol /*protocol*/, AvahiResolverEvent event,
                                const char *name, const char * /*type*/, const char * /*domain*/,
                                const char *hostName, const AvahiAddress *address, uint16_t port,
                                AvahiStringList *txt, AvahiLookupResultFlags /*flags*/,
                                void *userdata) {
      Impl *self = static_cast<Impl *>(userdata);
      if (event == AVAHI_RESOLVER_FOUND) {
        do {
          /* Self-filter: never peer with our own advertised instance. The name
           * is unique per instance (Avahi de-collides), so this is exact even
           * with several cannellonis on one host. */
          if (self->m_serviceName == name)
            break;

          /* Capability pre-filter from the TXT records. A differing protocol
           * version is a hard incompatibility; a differing CAN-FD capability is
           * the mixed-FD/classic caveat (epic caveat 4) -- do not auto-peer
           * across it (a static --peer still can, opting in explicitly). */
          const std::string peerVersion = txtValue(txt, "version");
          if (peerVersion != std::to_string(self->m_protocolVersion)) {
            lwarn << "mDNS: skipping \"" << name << "\" (" << hostName
                  << "): protocol version \"" << peerVersion << "\" != "
                  << static_cast<int>(self->m_protocolVersion) << std::endl;
            break;
          }
          const std::string peerFd = txtValue(txt, "canfd");
          const std::string ownFd = self->m_canFd ? "1" : "0";
          if (peerFd != ownFd) {
            lwarn << "mDNS: skipping incompatible \"" << name << "\" (" << hostName
                  << "): CAN-FD capability " << peerFd << " != " << ownFd << std::endl;
            break;
          }

          linfo << "mDNS: resolved peer \"" << name << "\" (" << hostName << ") port "
                << port << std::endl;
          self->deliverPeer(interface, address, port);
        } while (false);
      } else { /* AVAHI_RESOLVER_FAILURE */
        lwarn << "mDNS: failed to resolve \"" << name << "\": "
              << avahi_strerror(avahi_client_errno(avahi_service_resolver_get_client(resolver)))
              << std::endl;
      }
      avahi_service_resolver_free(resolver);
    }

    /* Immutable configuration. */
    int m_addressFamily;
    AvahiProtocol m_avahiProto;
    uint16_t m_port;
    std::string m_canInterface;
    bool m_canFd;
    uint8_t m_protocolVersion;
    PeerFoundCallback m_onPeerFound;

    /* Avahi state -- only touched on the threaded-poll thread (or with the poll
     * stopped, during start()/stop()). */
    AvahiThreadedPoll *m_poll = nullptr;
    AvahiClient *m_client = nullptr;
    AvahiEntryGroup *m_group = nullptr;
    AvahiServiceBrowser *m_browser = nullptr;
    std::string m_serviceName;
};

MdnsDiscovery::MdnsDiscovery(int addressFamily, uint16_t port, std::string canInterface,
                             bool canFd, uint8_t protocolVersion, PeerFoundCallback onPeerFound)
  : m_impl(std::make_unique<Impl>(addressFamily, port, std::move(canInterface), canFd,
                                  protocolVersion, std::move(onPeerFound)))
{}

MdnsDiscovery::~MdnsDiscovery() = default;

bool MdnsDiscovery::start() { return m_impl->start(); }
void MdnsDiscovery::stop() { m_impl->stop(); }

}

#endif /* AVAHI_SUPPORT */
