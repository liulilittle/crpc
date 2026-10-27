# Reverse-proxy headers

[简体中文](proxy-headers.zh-CN.md) | **English**

Behind nginx, HAProxy, a cloud load balancer or an API gateway, the socket peer is
the proxy, not the client. The client's address arrives in a header, and that
header is attacker-controlled unless the proxy that wrote it is trusted.

`crpc::http::resolve_forwarded()` answers exactly one question: **given the peer I
am actually connected to, which hop can I still trust?**

```cpp
#include <crpc/http/forwarded.hpp>

crpc::http::trusted_proxies trusted;
trusted.networks.push_back(*crpc::net::ip_network::parse("10.0.0.0/8"));
trusted.networks.push_back(*crpc::net::ip_network::parse("2001:db8::/32"));

// `peer` must be the address of the socket the request arrived on.
const auto client = crpc::http::resolve_forwarded(request, peer, trusted);
if (!client) {
    // A malformed Forwarded header: answer 400, or ignore the headers.
}
if (client.value().has_address()) {
    client.value().address;      // the client, as far as the chain can prove
    client.value().port;         // 0 when the header carried none
    client.value().is_https();   // scheme, only when a trusted hop reported it
    client.value().host;         // the Host the client addressed
    client.value().trusted_hops; // how many proxies were skipped
}
```

## The trust model

1. **The peer is not a trusted proxy** → every forwarding header is ignored and
   the peer is the client. This is the only safe default: otherwise any client
   can name itself `127.0.0.1`.
2. **The peer is trusted** → the chain is walked right to left, skipping hops that
   are themselves trusted. The first untrusted hop is the client.
3. **A hop that cannot be parsed** (`for=_hidden`, `for=unknown`, or garbage)
   stops the walk. `has_address()` is then false: the resolver refuses to name a
   client it cannot prove. It does not fall back to the peer, because that would
   be a different — and wrong — answer.
4. **Every hop trusted** → the leftmost hop is reported, with `trusted_hops`
   counting how many proxies vouched for it.

`trusted_proxies::trust_any` exists for a deployment whose proxy addresses are not
known in advance. It means any client can forge its own address, so it is an
explicit decision and never a default.

## Header precedence

RFC 7239 `Forwarded` wins over `X-Forwarded-*` when both are present, because it is
the standardised form and it is what modern proxies emit. Within a header:

| Field | Used for |
| --- | --- |
| `Forwarded: for=` | client address and port |
| `Forwarded: proto=` | scheme, from the leftmost element only |
| `Forwarded: host=` | the `Host` the client addressed, from the leftmost element |
| `X-Forwarded-For` | client address and port |
| `X-Forwarded-Proto` | scheme |
| `X-Forwarded-Host` | host |
| `X-Forwarded-Port` | port |

Repeated fields are joined in wire order before parsing, because HTTP allows a
list field to be split across fields and the chain is only correct when the
pieces are concatenated in order.

`Forwarded: by=` is parsed for validity but not reported: it identifies the proxy,
which the resolver already knows from the peer.

## Syntax accepted

* IPv4 dotted quad, no leading zeros (`010.0.0.1` is rejected: octal to some
  resolvers, decimal to others).
* IPv6 per RFC 4291, with `::` compression and an embedded IPv4 tail. A zone id
  (`%eth0`) is transport metadata, not an address, and is rejected.
* Ports: `for=198.51.100.7:6321`, `for="[2001:db8::1]:4711"`, quoted and escaped
  values as RFC 7239 allows.
* Obfuscated tokens `_name` and `unknown`, which deliberately yield no address.

A malformed `Forwarded` header is an `rpc_code::invalid_argument`, not something to
skip. A malformed entry in `X-Forwarded-For` is treated as an unparseable hop,
which stops the walk instead of failing the request — that header is not
standardised, and real deployments do put junk in it.

## Why this is not just "read the first X-Forwarded-For entry"

The leftmost entry is what the *client* claimed, and a client can claim anything.
Only the entries written by proxies you trust are evidence. Walking from the
trusted peer is the only direction where each step is vouched for by a hop you
already trust.

## Limits

* The resolver does not verify that a hop is a proxy, only that its address is in
  a trusted range. A shared NAT that also runs a proxy needs its range listed.
* A `Forwarded` element with an unknown parameter is accepted and ignored; only
  structural errors (no `=`, an unparseable `for`) fail the request.
* Scheme and host are reported whenever the peer is trusted, even if the chain
  stopped early. A caller that makes a security decision on them should also
  require `trusted_hops > 0`.
