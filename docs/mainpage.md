pairring {#mainpage}
========

A point-to-point message channel over memory each side can write remotely and
read locally. The protocol is described in `docs/protocol.md`; the README has
an overview.

- Lower layer contract: pairring::RemoteRegion, pairring::LocalRegion
- Channel: pairring::Channel, pairring::Config, pairring::State, pairring::Stats
- Ready lower layer for mapped memory: pairring::MappedRemoteRegion

@code
#include <pairring/pairring.hpp>

pairring::MappedRemoteRegion remote(theirs, size);
pairring::Channel ch({mine, size}, remote, pairring::Config{});
ch.send(msg, len);
ch.drain([](const void* payload, std::size_t len) { handle(payload, len); });
@endcode
