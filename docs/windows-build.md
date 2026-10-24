# Building AzerothCore + mod-acsdk on Windows

Reproducible steps for a native Windows x64 build, using this repo's `scripts\` (the maintainer's setup).
For any other server, the short version is in [README.md](../README.md). Verified on Windows 11 Pro with
Visual Studio Community 2026 (MSVC 14.51), CMake 4.4.4, MySQL 8.4.9, Python 3.12.10, and
AzerothCore master `0540ae8013bc92d93f1f0d3346ccbc1a1a4d6f85`.

## 1. Tools

Visual Studio 2022 or 2026 with the **Desktop development with C++** workload. Then:

```powershell
winget install --id Git.Git -e
winget install --id Kitware.CMake -e
winget install --id Python.Python.3.12 -e
winget install --id Oracle.MySQL -e      # server + include/ + lib/libmysql.{lib,dll}
```

Boost and OpenSSL are **not** installed separately; vcpkg builds them (step 3).

## 2. Sources

```powershell
cd "<your projects folder>"
git clone --branch master https://github.com/azerothcore/azerothcore-wotlk.git azerothcore-wotlk
git clone https://github.com/SimianJake/mod-acsdk.git mod-acsdk
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg
C:\vcpkg\bootstrap-vcpkg.bat -disableMetrics
```

AzerothCore must be at or after `92fed92ea` (headless-session APIs; see `docs/research/core-gaps.md`).
`scripts\env.ps1` holds all paths; override them with `ACSDK_AC_SOURCE`, `ACSDK_BUILD_ROOT`,
`VCPKG_ROOT`, `ACSDK_MYSQL_ROOT`.

## 3. Native dependencies (vcpkg)

`deps\vcpkg.json` lists Boost (only the libraries AC uses), OpenSSL, gRPC and protobuf. They are
built with the **`x64-windows-static-md`** triplet:

- **Static libraries, dynamic CRT (`/MD`)** — matches worldserver (CMake's MSVC default, and AC's
  `Boost_USE_STATIC_LIBS ON` / `Boost_USE_STATIC_RUNTIME OFF` in `deps/boost/CMakeLists.txt`).
- **One OpenSSL.** AzerothCore and gRPC both resolve vcpkg's OpenSSL 3.x, so worldserver links a
  single copy. (AC's `FindOpenSSL.cmake` picks it up through the vcpkg toolchain's prefix path.)
- **No extra DLLs** next to `worldserver.exe` (Abseil/protobuf/gRPC are linked statically).

The first build takes ~30 minutes (gRPC dominates). `scripts\build.ps1` runs it; the install goes
to `C:\acsdk-build\vcpkg_installed`.

## 4. Configure and build

```powershell
.\scripts\build.ps1              # RelWithDebInfo; -Config Debug for crash hunting; -Reconfigure to redo CMake
```

What it does:

1. `scripts\link_module.ps1` junctions this repo into `<ac>\modules\mod-acsdk` (or clone it there directly and skip the junction).
2. `vcpkg install` from the manifest (no-op when up to date).
3. CMake configure (only if there is no cache yet) with:
   `-G "Visual Studio 18 2026" -A x64`, `CMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake`,
   `VCPKG_TARGET_TRIPLET=x64-windows-static-md`, `VCPKG_MANIFEST_MODE=OFF`,
   `VCPKG_INSTALLED_DIR=C:/acsdk-build/vcpkg_installed`, `MYSQL_ROOT_DIR`, `ACSDK_PROTO_DIR=<repo>/proto`,
   `ACSDK_BUILD_TESTS=ON`, `TOOLS_BUILD=all` (map extractors), `SCRIPTS=static`, `MODULES=static`.
4. Build, then install to `C:\acsdk-build\server`.

Build tree: `C:\acsdk-build\ac`; binaries in `C:\acsdk-build\ac\bin\<Config>`.

### How mod-acsdk links gRPC

AzerothCore includes `modules/<name>/<name>.cmake` inline after creating the `modules` target
(`modules/CMakeLists.txt`). `mod-acsdk.cmake`:

- `find_package(protobuf CONFIG)` / `find_package(gRPC CONFIG)` from vcpkg,
- generates C++ code from `proto/acsdk/v1/*.proto` with `protobuf_generate` (cpp + grpc plugin)
  into a separate static library `acsdk-proto`, so AC's warning flags don't apply to generated code,
- links `modules` → `acsdk-proto` → `gRPC::grpc++`, `protobuf::libprotobuf`.

The module source dir for gRPC code is `src/rpc/` (not `src/grpc/`): AC adds every module
subdirectory to the include path, and a `grpc/` folder could shadow `<grpc/...>` headers.

## 5. Database

MySQL runs as a plain process (no Windows service, no admin rights) with data in `C:\acsdk-build\mysql`:

```powershell
$my = "C:\Program Files\MySQL\MySQL Server 8.4\bin"
# one-time init (root with empty password, bound to 127.0.0.1 only — local dev)
& "$my\mysqld.exe" --defaults-file="C:\acsdk-build\mysql\my.ini" --initialize-insecure --console
Start-Process "$my\mysqld.exe" '--defaults-file="C:\acsdk-build\mysql\my.ini"' -WindowStyle Hidden
Get-Content <ac>\data\sql\create\create_mysql.sql -Raw | & "$my\mysql.exe" -u root --host=127.0.0.1
```

`my.ini`:

```ini
[mysqld]
basedir="C:/Program Files/MySQL/MySQL Server 8.4"
datadir="C:/acsdk-build/mysql/data"
bind-address=127.0.0.1
port=3306
max_allowed_packet=64M
```

This creates user `acore`/`acore` and the `acore_auth`, `acore_characters`, `acore_world` databases;
worldserver populates them on first start (`Updates.EnableDatabases`).

## 6. Run

```powershell
.\scripts\run_realm.ps1 -DataDir <folder with dbc, maps, vmaps, mmaps>
```

Starts MySQL if needed, copies `libmysql.dll` next to the binaries, creates `worldserver.conf`,
`authserver.conf` and `modules\mod_acsdk.conf` from their `.dist` files on first run (generating a
random `ACSDK.AuthToken`), and launches authserver and worldserver.

**worldserver exits at startup without client data**. We use AzerothCore's prebuilt client-data (`https://github.com/wowgaming/client-data/releases/download/v20.0/data.zip`, the version `apps/installer` pins) unzipped into `C:\acsdk-build\data`; alternatively extract from a 3.3.5a client. Without it, worldserver stops ("Failed to find map files for starting
areas", `World.cpp:334`), so maps/vmaps/mmaps must be extracted first (PLAN.md Phase 0 step 3;
extractors are built by `TOOLS_BUILD=all`).

## 7. Python SDK

The Python SDK, its tests and examples are in [AzerothCore-SDK](https://github.com/SimianJake/AzerothCore-SDK); see its `docs/installation.md`.

## Gotchas

- Tools installed by winget aren't on `PATH` in shells opened before the install; `env.ps1` reloads `PATH`.
- CMake 4.x prints a CMP0167 (FindBoost removed) policy warning from vcpkg's Boost wrapper; harmless.

