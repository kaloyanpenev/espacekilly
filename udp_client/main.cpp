// udp_client
//
// Joins the multicast group the broadcaster sends to and parses the whole
// stream back into matcher requests. Structs are the ground truth from
// <matcher/matcher.h>; the wire decode (explicit little-endian, field-by-field)
// is the shared udp_wire.h, so it works locally and across machines of any
// endianness or ABI. A datagram with numOfMessages == 0 ends the stream.

#include <udp_wire.h>

#include <arpa/inet.h>   // inet_pton
#include <netinet/in.h>  // sockaddr_in, ip_mreq
#include <sys/socket.h>  // socket, setsockopt, bind, recvfrom
#include <unistd.h>      // close

#include <cstdio>
#include <limits>
#include <print>
#include <variant>

using namespace matcher;

namespace
{
constexpr char kGroup[] = "239.255.0.1";
constexpr uint16_t kPort = 30001;

struct Stats
{
	size_t news = 0;
	size_t cancels = 0;
};

// Called for each decoded request. Counts by type and prints the first few.
void OnRequest(const NewRequest& req, Stats& stats)
{
	std::visit(wire::overloaded{[&](const NewOrderRequest& r)
					{
						++stats.news;
						if (stats.news <= 5)
							std::println("  NEW    id={} lots={} type={} ticks={}",
								r.order.id, r.order.quantityLots,
								static_cast<int>(r.orderType), r.priceTicksLimit);
					},
				   [&](const CancelOrderRequest& r)
				   {
					   ++stats.cancels;
					   if (stats.cancels <= 5)
						   std::println("  CANCEL id={} toCancel={}", r.msgId, r.toCancel);
				   }},
		req);
}
} // namespace

int main()
{
	int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0)
	{
		std::perror("socket");
		return 1;
	}

	// SO_REUSEADDR: allow multiple receivers on this port and clean restarts.
	int reuse = 1;
	::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	// Bind the PORT on any interface (INADDR_ANY); group membership does the
	// filtering, not the bound address.
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(kPort);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
	{
		std::perror("bind");
		return 1;
	}

	// Join the multicast group so the kernel delivers kGroup traffic here.
	ip_mreq mreq{};
	::inet_pton(AF_INET, kGroup, &mreq.imr_multiaddr);
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);
	if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
	{
		std::perror("IP_ADD_MEMBERSHIP");
		return 1;
	}

	std::println("listening on {}:{}", kGroup, kPort);

	Stats stats{};
	std::byte buf[std::numeric_limits<uint16_t>::max()];
	for (;;)
	{
		ssize_t len = ::recvfrom(fd, buf, sizeof(buf), 0, nullptr, nullptr);
		if (len < 0)
		{
			std::perror("recvfrom");
			break;
		}
		if (static_cast<size_t>(len) < wire::kMessageHeaderBytes)
			continue; // runt, ignore

		bool more = wire::decodeDatagram(buf, static_cast<size_t>(len),
			[&](const NewRequest& req) { OnRequest(req, stats); });
		if (!more)
			break; // sentinel seen
	}

	std::println("done: {} new orders, {} cancels ({} total)",
		stats.news, stats.cancels, stats.news + stats.cancels);
	::close(fd);
	return 0;
}
