# mod-acsdk

An [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk) module that lets programs log characters in
without a game client ("headless") and drive them over gRPC: move, fight, cast, chat, talk to NPCs, quest,
trade with vendors, loot, and receive game events. Every action goes through AzerothCore's own packet handlers,
so the normal game rules apply.

It is a plain drop-in module for an **unmodified** AzerothCore: no core patches, only the standard module hooks.
Programs talk to it with the Python SDK, [AzerothCore-SDK](https://github.com/SimianJake/AzerothCore-SDK)
(`pip install` a wheel), or any gRPC client built from the protos in [proto/](proto/).

## Requirements

- AzerothCore 3.3.5a (`azerothcore-wotlk`) at or after commit `92fed92ea` (adds the headless-session APIs the
  module uses). Developed against master `0540ae80`.
- gRPC and protobuf for C++, installed so CMake can find their config packages (`find_package(gRPC CONFIG)`).
- Everything AzerothCore itself needs to build.

## Install

1. **Get the module.** Clone it into AzerothCore's `modules` folder. The folder must be named `mod-acsdk`:
   AzerothCore derives the module's loader function and build hook from that name.

   ```bash
   cd azerothcore-wotlk/modules
   git clone https://github.com/SimianJake/mod-acsdk.git mod-acsdk
   ```

2. **Install gRPC and protobuf.**
   - **Windows:** use vcpkg with [deps/vcpkg.json](deps/vcpkg.json) (triplet `x64-windows-static-md`) and pass
     vcpkg's toolchain file to CMake. [docs/windows-build.md](docs/windows-build.md) has the full, tested recipe,
     and `scripts/build.ps1` automates it.
   - **Linux:** vcpkg works the same way (triplet `x64-linux`), or use distribution packages that ship CMake
     config files (for example `libgrpc++-dev`, `protobuf-compiler-grpc` and `libprotobuf-dev` on recent
     Debian/Ubuntu). The Linux build has not been tested yet; reports are welcome.

3. **Re-run CMake and rebuild** AzerothCore as you normally do (`MODULES=static` is the default). The module adds
   itself to `worldserver`. The packet tests are off by default; add `-DACSDK_BUILD_TESTS=ON` to build
   `acsdk-packet-tests`.

4. **Configure.** The install step copies `mod_acsdk.conf.dist` into `configs/modules/`. Copy it to
   `mod_acsdk.conf` and set at least:

   | Setting | What to set |
   |---|---|
   | `ACSDK.AuthToken` | A long random secret. The listener refuses to start while it is empty. Everyone with it can drive eligible characters. |
   | `ACSDK.BindAddress` / `ACSDK.Port` | Default `127.0.0.1:50051`. Keep it on localhost unless you have a reason not to (see Security). |
   | `ACSDK.HeadlessAccountPrefix` | Only characters on accounts whose names start with this (default `acsdkhc`) can be listed or driven. |
   | `ACSDK.Debug.Enable` | Leave at `0`. It serves a development-only service that creates accounts and characters. |

   Every setting is documented in [conf/mod_acsdk.conf.dist](conf/mod_acsdk.conf.dist).

5. **Create accounts for SDK use** with the normal AzerothCore tools (for example `.account create acsdkhcbot1 <password>`
   in the worldserver console), then create characters on them. Keep these accounts player-level, not GM: chat
   commands from a GM account would run as GM commands. One character per account can be online at a time.

6. **Start worldserver.** Its log shows `mod-acsdk: gRPC listener started on 127.0.0.1:50051.`

## Giving people access

SDK users need three things from you: the address (`127.0.0.1:50051` when they run on the same machine), the
`ACSDK.AuthToken`, and the names of the characters they may drive. They then follow
[installation.md](https://github.com/SimianJake/AzerothCore-SDK/blob/main/docs/installation.md) in the SDK
repository.

## Security

The gRPC connection is not encrypted and every controller shares one token. Keep the listener on localhost. For
users on other machines, prefer an SSH tunnel or a private network (such as Tailscale) over opening the port to the
internet. Controllers can only ever drive characters on prefix-matched accounts, never real players.

## Versions

The protos in [proto/](proto/) are the API. The SDK repository keeps a copy matched to a module release, and the
server reports its version through `ServerService.GetInfo` (`sdk_version`, currently `1.0.0`).

## More

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how the module works, file by file.
- [docs/events.md](docs/events.md): where each event comes from.
- [docs/research/](docs/research/): the AzerothCore internals behind the design.
