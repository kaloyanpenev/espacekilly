// udp_wire.h — the on-wire codec shared by udp_broadcaster and udp_client.
//
// The message/order structs are the ground truth from <matcher/matcher.h>.
// This header defines how those structs map to bytes on the wire, so both
// processes encode/decode identically and cannot drift.
//
// PROTOCOL RULE: every multi-byte field is BIG-ENDIAN on the wire ("network
// byte order", the same convention IP/UDP headers use), with a fixed width,
// serialized field-by-field. Never memcpy a whole struct: that would smuggle
// the host's endianness AND its struct padding into the format.
//
// On a little-endian host (x86/ARM) both sender and receiver actually byte-swap
// every field, so the receiver sees the stream exactly as it would from any
// foreign big-endian network peer. This is the same swap htons/htonl perform,
// generalized to 64-bit via std::byteswap.

#pragma once

#include <matcher/matcher.h>

#include <bit>       // std::endian, std::byteswap
#include <chrono>
#include <concepts>  // std::integral
#include <cstddef>   // std::byte
#include <cstdint>
#include <cstring>   // std::memcpy
#include <variant>

namespace wire
{

// Write one fixed-width integer in big-endian (network order), advancing p.
template <std::integral T>
void put(std::byte*& p, T v)
{
	if constexpr (std::endian::native != std::endian::big)
		v = std::byteswap(v); // host -> network
	std::memcpy(p, &v, sizeof(v));
	p += sizeof(v);
}

// Read one fixed-width integer in big-endian (network order), advancing p.
template <std::integral T>
T get(const std::byte*& p)
{
	T v;
	std::memcpy(&v, p, sizeof(v));
	p += sizeof(v);
	if constexpr (std::endian::native != std::endian::big)
		v = std::byteswap(v); // network -> host
	return v;
}

// std::visit helper.
template <class... Ts>
struct overloaded : Ts...
{
	using Ts::operator()...;
};

// Fixed wire widths of the framing, so the encoder can reserve/skip precisely.
constexpr size_t kMessageHeaderBytes = sizeof(uint16_t) + sizeof(uint32_t) + sizeof(int64_t); // 14
constexpr size_t kRequestHeaderBytes = sizeof(uint16_t) + sizeof(int32_t);                    // 6

inline int64_t nowNs()
{
	return std::chrono::steady_clock::now().time_since_epoch().count();
}

// --- request body: each field pinned to an explicit wire width ---
// size_t-backed types (MessageID, OrderID, Instrument) are pinned to uint64_t
// so a 32-bit peer can never disagree on their width.

inline matcher::RequestType encodeBody(std::byte*& p, const matcher::NewRequest& req)
{
	matcher::RequestType type{};
	std::visit(overloaded{[&](const matcher::NewOrderRequest& r)
					{
						type = matcher::RequestType::NewOrder;
						put<uint64_t>(p, r.msgId);
						put<uint64_t>(p, r.order.id);
						put<int64_t>(p, r.order.quantityLots);
						put<uint8_t>(p, static_cast<uint8_t>(r.orderType));
						put<uint64_t>(p, static_cast<uint64_t>(r.instrumentId));
						put<uint64_t>(p, r.priceTicksLimit);
						put<uint64_t>(p, r.priceTicksStop);
					},
				   [&](const matcher::CancelOrderRequest& r)
				   {
					   type = matcher::RequestType::CancelOrder;
					   put<uint64_t>(p, r.msgId);
					   put<uint64_t>(p, r.toCancel);
					   put<uint64_t>(p, static_cast<uint64_t>(r.instrumentId));
				   }},
		req);
	return type;
}

inline matcher::NewRequest decodeBody(const std::byte*& p, matcher::RequestType type)
{
	using namespace matcher;
	if (type == RequestType::CancelOrder)
	{
		CancelOrderRequest r{};
		r.msgId = get<uint64_t>(p);
		r.toCancel = get<uint64_t>(p);
		r.instrumentId = static_cast<Instrument>(get<uint64_t>(p));
		return r;
	}

	NewOrderRequest r{};
	r.msgId = get<uint64_t>(p);
	r.order.id = get<uint64_t>(p);
	r.order.quantityLots = get<int64_t>(p);
	r.orderType = static_cast<OrderType>(get<uint8_t>(p));
	r.instrumentId = static_cast<Instrument>(get<uint64_t>(p));
	r.priceTicksLimit = get<uint64_t>(p);
	r.priceTicksStop = get<uint64_t>(p);
	return r;
}

// Encode one framed message (MessageHeader + OrderRequestHeader + body) into
// buf. Returns the datagram length. Two-pass: reserve the request header, write
// the body, then backfill the body size.
inline size_t encodeDatagram(std::byte* buf, uint16_t seqnum, const matcher::NewRequest& req)
{
	std::byte* p = buf;

	put<uint16_t>(p, seqnum);         // MessageHeader.seqnum
	put<uint32_t>(p, 1u);             // MessageHeader.numOfMessages
	put<int64_t>(p, nowNs());         // MessageHeader.timestamp_ns

	std::byte* reqHeaderAt = p;       // reserve OrderRequestHeader, backfill later
	p += kRequestHeaderBytes;

	std::byte* bodyStart = p;
	matcher::RequestType type = encodeBody(p, req);
	auto bodySize = static_cast<uint16_t>(p - bodyStart);

	std::byte* hp = reqHeaderAt;
	put<uint16_t>(hp, bodySize);                          // OrderRequestHeader.size
	put<int32_t>(hp, static_cast<int32_t>(type));         // OrderRequestHeader.type

	return static_cast<size_t>(p - buf);
}

// End-of-stream sentinel: a MessageHeader advertising zero messages.
inline size_t encodeSentinel(std::byte* buf, uint16_t seqnum)
{
	std::byte* p = buf;
	put<uint16_t>(p, seqnum);
	put<uint32_t>(p, 0u);
	put<int64_t>(p, 0);
	return static_cast<size_t>(p - buf);
}

// Decode every message in one datagram, calling onRequest for each. Returns
// false when the sentinel (numOfMessages == 0) is seen.
template <class F>
bool decodeDatagram(const std::byte* buf, size_t len, F&& onRequest)
{
	const std::byte* p = buf;

	(void)get<uint16_t>(p);                   // seqnum
	uint32_t numOfMessages = get<uint32_t>(p);
	(void)get<int64_t>(p);                    // timestamp_ns

	if (numOfMessages == 0)
		return false;

	for (uint32_t m = 0; m < numOfMessages; ++m)
	{
		uint16_t bodySize = get<uint16_t>(p);
		auto type = static_cast<matcher::RequestType>(get<int32_t>(p));

		const std::byte* bodyStart = p;
		onRequest(decodeBody(p, type));
		p = bodyStart + bodySize;             // advance by declared size (robust to unknown fields)
	}

	return true;
}

} // namespace wire
