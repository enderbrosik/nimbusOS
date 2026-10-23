# Networking and HTTPS

## Devices and stack

The default QEMU machine uses an Intel E1000 adapter with user-mode networking.
`drivers/e1000.c` connects to the network task in `net/net.c`. The in-tree
stack implements Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS, and client-side
TCP. TCP runs in the network task; blocking client work is performed by
network worker tasks so the desktop remains responsive.

The default QEMU user network commonly uses `10.0.2.2` as its host-side
gateway. Actual addresses and DNS servers are obtained from DHCP and are
shown by `ifconfig`.

## Terminal and browser features

The Terminal provides `ifconfig`, `dhcp`, `ping`, `nslookup`, `wget`, and
`http`. The Web Browser uses the same network jobs for page requests. The
browser supports a subset of HTML and CSS; it is not a general-purpose modern
browser and does not run JavaScript.

The HTTP client supports GET, form POST, redirects, and chunked responses.
TLS supports TLS 1.3 with X25519 and AES-128-GCM or ChaCha20-Poly1305. The
cryptographic primitives and test vectors are in `net/crypto.c`.

## Security limitation

TLS server certificates are **not verified**. Connections are encrypted and
authenticated against in-session tampering, but the client cannot establish
the server's identity. Do not use NimbusOS HTTPS for passwords, financial
activity, or other sensitive data. The browser surfaces this limitation.

The stack is client-only; it does not provide a general inbound server API.
Network, HTTP, and TLS checks are included in `make test-all` and the focused
scripts in `tools/`.