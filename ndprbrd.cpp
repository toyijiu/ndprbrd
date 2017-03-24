// Copyright 2016 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <QCoreApplication>
#include <QHostAddress>
#include <QTimer>
#include <QHash>
#include <QNetworkInterface>
#include <QSocketNotifier>

#include <sstream>
#include <iostream>
#include <chrono>
#include <vector>
#include <list>
#include <memory>
#include <cstdlib>
#include <cstring>

#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/if_packet.h>
#include <net/ethernet.h>

#include "third_party/cxxopts/include/cxxopts.hpp"
#include "third_party/cpp-subprocess/include/subprocess.hpp"

class PrefixList {
 public:
  explicit PrefixList(const std::vector<std::string>& list) {
    if (list.empty()) {
      std::cerr << "Error: --prefix is required." << std::endl;
      std::exit(1);
    }
    for (const std::string& prefix : list) {
      prefixes_.push_back(
          QHostAddress::parseSubnet(QString::fromStdString(prefix)));
      if (prefixes_.back().second != 64) {
        std::cerr << "Error: prefix should be /64" << std::endl;
        std::exit(1);
      }
    }
  }
  bool contains(const QHostAddress& addr) const {
    for (const auto& prefix : prefixes_) {
      if (addr.isInSubnet(prefix)) return true;
    }
    return false;
  }

 private:
  std::vector<QPair<QHostAddress, int>> prefixes_;
};

class RouteTable {
 public:
  explicit RouteTable(const std::string& proto, std::chrono::duration<int> ttl)
      : proto_(proto), ttl_(ttl) {}
  void learn(const QHostAddress& addr, const QNetworkInterface& iface) {
    routes_.insert(
        addr, std::make_pair(iface, std::chrono::steady_clock::now() + ttl_));
  }
  void cleanup() {
    // TODO use min heap
    auto now = std::chrono::steady_clock::now();
    for (auto it = routes_.begin(); it != routes_.end();) {
      if (it.value().second < now) {
        qDebug() << "deleting" << it.key().toString() << "from"
                 << it.value().first.name();
        subprocess::popen pr("ip", {"-6", "route", "del",
                                    it.key().toString().toStdString(), "dev",
                                    it.value().first.name().toStdString(),
                                    "protocol", proto_});
        pr.wait();
        it = routes_.erase(it);
      } else {
        ++it;
      }
    }
  }
  void replaceSystem(const std::string& addr, QNetworkInterface& iface) {
    subprocess::popen pr("ip",
                         {"-6", "route", "replace", addr, "dev",
                          iface.name().toStdString(), "protocol", proto_});
    pr.wait();
  }

 private:
  QHash<QHostAddress,
        std::pair<QNetworkInterface,
                  std::chrono::steady_clock::time_point /* expiration time */>>
      routes_;
  std::string proto_;
  std::chrono::duration<int> ttl_;
};

class SocketWatcher {
 public:
  SocketWatcher(const QNetworkInterface& iface, const PrefixList* prefixes,
                RouteTable* routes)
      : iface_(iface), prefixes_(prefixes), routes_(routes) {
    int sock = ::socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
    if (sock == -1) {
      int e = errno;
      std::cerr << "Error: can't create ICMPv6 socket: " << std::strerror(e)
                << std::endl;
      std::exit(1);
    }
    QByteArray ifaceName = iface.name().toUtf8();
    if (::setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, ifaceName.constData(),
                     ifaceName.size()) == -1) {
      int e = errno;
      std::cerr << "Error: can't bind socket to " << iface.name().toStdString()
                << ": " << std::strerror(e) << std::endl;
      std::exit(1);
    }
    if (::fcntl(sock, F_SETFL, ::fcntl(sock, F_GETFL) | O_NONBLOCK) == -1) {
      int e = errno;
      std::cerr << "Error: can't make socket non-blocking: " << std::strerror(e)
                << std::endl;
      std::exit(1);
    }
    notifier_.reset(new QSocketNotifier(sock, QSocketNotifier::Read));
    QObject::connect(notifier_.get(), &QSocketNotifier::activated,
                     [this](int sock) { readFrom(sock); });
  }

 private:
  void readFrom(int sock) {
    char buf[24];
    sockaddr_storage addr{};
    socklen_t addrLen = sizeof addr;
    ssize_t len = ::recvfrom(sock, buf, sizeof buf, /* flags = */ 0,
                             reinterpret_cast<sockaddr*>(&addr), &addrLen);
    if (len < 24) return;
    if (buf[0] != '\x88') return;  // Neighbor Advertisement, RFC 4861
    QHostAddress remoteAddr(reinterpret_cast<quint8*>(buf + 8));
    if (!prefixes_->contains(remoteAddr)) return;
    routes_->learn(remoteAddr, iface_);
    routes_->replaceSystem(remoteAddr.toString().toStdString(), iface_);
  }
  QNetworkInterface iface_;
  std::unique_ptr<QSocketNotifier> notifier_;
  const PrefixList* prefixes_;
  RouteTable* routes_;
};

class Tunnel {
 public:
  Tunnel(const std::string& name, std::vector<QNetworkInterface> interfaces,
         const PrefixList* prefixes)
      : interfaces_(std::move(interfaces)), prefixes_(prefixes) {
    int tap = ::open("/dev/net/tun", O_RDWR);
    if (tap == -1) {
      std::cerr << "Error: can't create TAP interface" << std::endl;
      std::exit(1);
    }
    struct ifreq ifr {};
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    std::strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ);
    if (::ioctl(tap, TUNSETIFF, &ifr) == -1) {
      int e = errno;
      std::cerr << "Error: can't configure TAP interface: " << std::strerror(e)
                << std::endl;
      std::exit(1);
    }
    name_ = QString(ifr.ifr_name);
    if (::fcntl(tap, F_SETFL, ::fcntl(tap, F_GETFL) | O_NONBLOCK) == -1) {
      int e = errno;
      std::cerr << "Error: can't make TAP interface non-blocking: "
                << std::strerror(e) << std::endl;
      std::exit(1);
    }
    sock_ = ::socket(AF_PACKET, SOCK_RAW, ETH_P_IPV6);
    if (sock_ == -1) {
      int e = errno;
      std::cerr << "Error: can't create raw socket: " << std::strerror(e)
                << std::endl;
      std::exit(1);
    }
    // SIOCGIFFLAGS wants a socket. It doesn't matter which exact one.
    if (::ioctl(sock_, SIOCGIFFLAGS, &ifr) == -1) {
      int e = errno;
      std::cerr << "Error: can't get TAP flags: " << std::strerror(e)
                << std::endl;
      std::exit(1);
    }
    ifr.ifr_flags |= IFF_UP;
    if (::ioctl(sock_, SIOCSIFFLAGS, &ifr) == -1) {
      int e = errno;
      std::cerr << "Error: can't bring TAP interface up: " << std::strerror(e)
                << std::endl;
      std::exit(1);
    }
    tapReader_.reset(new QSocketNotifier(tap, QSocketNotifier::Read));
    QObject::connect(tapReader_.get(), &QSocketNotifier::activated,
                     [this](int tap) { readFrom(tap); });
  }

  QString name() const { return name_; }

 private:
  void readFrom(int tap) {
    char buf[86];
    ssize_t len = ::read(tap, buf, sizeof buf);
    if (len < 78) return;
    // Ethertype: IPv6
    if (buf[12] != '\x86' || buf[13] != '\xDD') return;
    // IPv6 Next Header: ICMPv6
    if (buf[20] != '\x3A') return;
    // ICMPv6 Type: Neighbor Solicitation
    if (buf[54] != '\x87') return;
    QHostAddress remoteAddr(reinterpret_cast<quint8*>(buf + 62));
    if (!prefixes_->contains(remoteAddr)) return;
    sockaddr_ll ll{};
    ll.sll_family = AF_PACKET;
    ll.sll_protocol = ETH_P_IPV6;
    ll.sll_halen = 6;
    std::memcpy(ll.sll_addr, buf, ETH_ALEN);
    for (const QNetworkInterface& iface : interfaces_) {
      sendTo(iface, &ll, buf, len);
    }
  }

  void sendTo(const QNetworkInterface& iface, sockaddr_ll* ll, char* buf,
              ssize_t len) {
    ll->sll_ifindex = iface.index();
    ifreq ifr{};
    std::strncpy(ifr.ifr_name, iface.name().toUtf8().constData(), IFNAMSIZ);
    if (::ioctl(sock_, SIOCGIFHWADDR, &ifr) == -1) return;
    // Source in Ethernet header
    std::memcpy(buf + 6, ifr.ifr_hwaddr.sa_data, 6);
    if (len == 86) {
      // Source link-layer address in Options of ICMPv6
      std::memcpy(buf + 80, ifr.ifr_hwaddr.sa_data, 6);
    }

    QHostAddress localIp;
    bool found = false;
    for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
      // TODO optimize
      if (entry.ip().isInSubnet(QHostAddress::parseSubnet("fe80::/10"))) {
        found = true;
        localIp = entry.ip();
        break;
      }
    }
    if (!found) return;
    addrinfo hints{};
    hints.ai_family = AF_INET6;
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo* result;
    if (::getaddrinfo(localIp.toString().toUtf8().constData(), nullptr, &hints,
                      &result) != 0)
      return;
    if (result->ai_addr->sa_family != AF_INET6) {
      ::freeaddrinfo(result);
      return;
    }
    std::memcpy(
        buf + 22,
        reinterpret_cast<sockaddr_in6*>(result->ai_addr)->sin6_addr.s6_addr,
        16);
    uint32_t sum = htons(0x3A);
    *reinterpret_cast<uint16_t*>(buf + 56) = 0;
    sum += *reinterpret_cast<uint16_t*>(buf + 18); // length
    for (int i = 22; i < len; i += 2) {
      sum += *reinterpret_cast<uint16_t*>(buf + i);
    }
    while (sum >> 16) sum = (sum >> 16) + (sum & 0xffff);
    *reinterpret_cast<uint16_t*>(buf + 56) = ~static_cast<uint16_t>(sum);
    ::freeaddrinfo(result);
    ::sendto(sock_, buf, len, /* flags = */ 0, reinterpret_cast<sockaddr*>(ll),
             sizeof *ll);
  }

  int sock_;
  std::unique_ptr<QSocketNotifier> tapReader_;
  const std::vector<QNetworkInterface> interfaces_;
  const PrefixList* prefixes_;
  QString name_;
};

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  cxxopts::Options options(argv[0], "NDP Routing Bridge Daemon");
  std::vector<std::string> interfacesStr;
  std::vector<std::string> prefixesStr;
  int expire{};
  int proto{};
  bool pendulumOpt{};
  std::string tun;
  options.add_options()
      // clang-format off
      ("help", "Print this help")
      ("interface", "LAN interfaces where hosts are discovered (multiple allowed)", cxxopts::value(interfacesStr), "IFACE")
      ("prefix", "Subnets /64 which should be routed by ndprbrd (multiple allowed)", cxxopts::value(prefixesStr), "PREFIX")
      ("expire", "How long should discovered routes stay when not used, in seconds", cxxopts::value(expire)->default_value("600"), "N")
      ("protocol", "Protocol num for routing table, as known by kernel", cxxopts::value(proto)->default_value("100"), "N")
      ("pendulum", "(not recommended) Rapidly switch default route between LAN interfaces instead of creating TAP interface", cxxopts::value(pendulumOpt))
      ("tun", "Name of TAP interface to create", cxxopts::value(tun)->default_value("ndprbrd%d"), "IFACE");
  // clang-format on
  auto printHelp = [&]() {
    std::cout << options.help({""}) << "\nExample:\n  " << argv[0]
              << " --interface=eth1 --interface=eth2 --prefix=2001:db8:1:2::/64"
              << std::endl;
  };
  try {
    options.parse(argc, argv);
  } catch (const cxxopts::OptionException& e) {
    std::cerr << "Error: " << e.what() << std::endl;
    printHelp();
    std::exit(1);
  }
  if (options.count("help")) {
    printHelp();
    std::exit(0);
  }
  if (interfacesStr.empty()) {
    std::cerr << "Error: --interface is required." << std::endl;
    printHelp();
    std::exit(1);
  }
  PrefixList prefixes(prefixesStr);

  RouteTable routes(std::to_string(proto), std::chrono::seconds(expire));
  std::list<SocketWatcher> watchers;
  std::vector<QNetworkInterface> interfaces;

  for (const std::string& iface : interfacesStr) {
    QNetworkInterface interface =
        QNetworkInterface::interfaceFromName(QString::fromStdString(iface));
    if (!interface.isValid()) {
      std::cerr << "Error: interface " << iface << " is not valid" << std::endl;
      return 1;
    }
    // Fill the table with existing routes
    subprocess::popen pr("ip", {"-6", "route", "show", "dev",
                                interface.name().toStdString()/*, "protocol",
                                std::to_string(proto)*/});
    pr.wait();
    std::string line;
    while (std::getline(pr.stdout(), line)) {
      std::istringstream inbuf(line);
      std::string prefix;
      std::getline(inbuf, prefix, ' ');
      if (prefix.empty()) {
        continue;
      }
      QHostAddress addr(QString::fromStdString(prefix));
      if (prefixes.contains(addr)) {
        routes.learn(addr, interface);
      }
    }
    // Start watching the interface
    watchers.emplace_back(interface, &prefixes, &routes);
    interfaces.push_back(interface);
  }

  QTimer expirator;
  QObject::connect(&expirator, &QTimer::timeout, [&]() { routes.cleanup(); });
  expirator.start(5000 /* ms */);

  int pendulumCurrent = 0;
  QTimer pendulum;
  std::unique_ptr<Tunnel> tunnel;

  if (pendulumOpt) {
    QObject::connect(&pendulum, &QTimer::timeout, [&]() {
      int& current = pendulumCurrent;
      for (const std::string& prefix : prefixesStr) {
        routes.replaceSystem(prefix, interfaces[current]);
      }
      current = (current + 1) % interfaces.size();
    });
    pendulum.start(1000 /* ms */);
  } else {
    tunnel.reset(new Tunnel(tun, std::move(interfaces), &prefixes));
    QNetworkInterface tun =
        QNetworkInterface::interfaceFromName(tunnel->name());
    for (const std::string& prefix : prefixesStr) {
      routes.replaceSystem(prefix, tun);
    }
  }

  return app.exec();
}
