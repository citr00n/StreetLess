# Streetless

**StreetPass without touching grass.**

Streetless is an open-source Nintendo 3DS homebrew client and self-hosted relay for exchanging native StreetPass game data over the Internet. It is inspired by [NetPass](https://gitlab.com/3ds-netpass/netpass) and is independent of NetPass and Nintendo.

> **Status:** the background module is experimental and has not been verified on physical 3DS hardware. Successful builds and automated tests do not establish compatibility with a particular game, console, or network.

## Features

- A public lobby with up to **five incoming random passes per rolling 24 hours**, plus a private mode that disables random exchange.
- Confirmed friends, invite-only rooms, and directed sends. A website request is fulfilled when the sender's 3DS next syncs.
- Per-game outgoing selection on the console, using local title names where available.
- A player website and administrator dashboard. Registration is open by default; administrators can require approval.
- Console pairing via a one-time QR code or manually entered code.
- Manual sync and an optional, experimental background module.
- An isolated debug mode for exporting native slots and creating targeted test deliveries.

The relay is independent of Pretendo; Pretendo integration is not implemented. In production, the client uses HTTPS over IPv4/TCP and verifies the server certificate. The configured IPv4 avoids a DNS dependency on the 3DS, but cannot bypass blocks on the server IP, TCP, TLS, or hostname.

## Getting started

### Requirements

- A 3DS with Luma3DS and Homebrew Launcher, or a CIA installer on a suitably configured console.
- A Streetless relay and an account on it. To run your own relay, see [DEPLOY.md](DEPLOY.md).
- StreetPass already enabled in a supported game, such as Mii Plaza.

### Install and pair

1. Download `streetless.3dsx` or `streetless.cia` from a project release or CI artifact. Copy the `.3dsx` to `SD:/3ds/streetless/streetless.3dsx`, or install the CIA with your CIA installer. The CIA requires CFW with signature patches.
2. Register on your relay's website. If the relay requires approval, wait for an administrator to approve your account.
3. In your account, generate a pairing code. In Streetless, choose **Pair console**, then scan the QR code or enter the relay hostname, IPv4 address, port and code.
4. Select **Sync** to exchange passes. Pairing codes expire after ten minutes; do not share them. Pairing a new console replaces the previous console credential.

Releases include `streetless-cia-qr.png`, which points to the release's CIA download for an installer such as FBI. The release must be publicly accessible to the console.

### Choose outgoing games

Open **Outgoing games** in the client. Select a title with the D-pad, toggle it with **A**, save with **X**, or cancel with **B**. The client shows the game name when it can read local title metadata; otherwise it shows the exact CEC ID.

Before the first save, all eligible games are enabled. After saving, newly discovered games are disabled until selected. This setting filters outgoing slots, not incoming metadata; however, games requiring a two-way exchange may not receive without an outgoing slot. Already staged or queued deliveries cannot be recalled by a local selection change. Sync again to replace the relay's prior outbox snapshot.

Preferences are stored independently of pairing in `SD:/3ds/streetless/outgoing.bin`. If saved preferences are missing or corrupt, sync fails closed. Restore the file from a backup, or deliberately delete both `outgoing.bin` and `outgoing.protected` on the SD card to reset **all** titles to enabled.

## Background sync

The optional `0004013000F5A302.cxi` module is **experimental and unverified on physical 3DS hardware**. It requires Luma3DS 13 or newer and a production HTTPS relay. The CIA QR installs only the foreground app, not this module.

1. Copy the module to `SD:/luma/sysmodules/0004013000F5A302.cxi`. Do not overwrite an existing file with that name without checking it first.
2. Hold **SELECT** while booting and enable **Enable loading external FIRMs and modules** in Luma's configuration.
3. Pair the console, choose outgoing games, then select **Background sync** in Streetless and confirm the warning. Once the service reports it connected, Streetless can be closed.
4. Start the service manually again after each power-on. Reopen **Background sync** to check its status or stop it.

The module attempts a sync immediately and every 15 minutes while the console is awake and networking is available. It also sends selected outgoing slots through allowed routes. Sleep-mode networking and delivery are not guaranteed. Manual Sync, pairing, slot export and outgoing-game selection require a confirmed stop; the client prompts for it.

If the service cannot be stopped or its status is uncertain, power the console off completely. Remove the module from the SD card if needed. A stale or damaged `SD:/3ds/streetless/background.pending` marker may be removed **only while the console is completely powered off**, after confirming the module is not running.

## Relay deployment

See [DEPLOY.md](DEPLOY.md) for Docker and Compose setup, direct HTTPS, same-host reverse proxy configuration, administrator credentials, backups and operational settings.

## Development

### Prerequisites

- Go 1.26+, a host C compiler, libcurl development headers, Python 3, and OpenSSL.
- [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `3ds-dev`, `3ds-curl` and `3ds-jansson`.
- `makerom` and `bannertool` on `PATH` to build the CIA; `makerom` is also needed for the background CXI. Docker is optional.

`server/` contains the Go relay, SQLite store, templates and assets; `client/` contains the 3DS application and host tests; `background/` contains the optional module.

### Build

From the repository root:

```sh
# Relay
(cd server && go build ./cmd/streetlessd)

# 3DSX only; no CIA packaging tools required
make -C client streetless.3dsx

# 3DSX and CIA; requires makerom and bannertool
make -C client

# Optional background CXI; requires makerom
make -C background
```

The outputs are `client/streetless.3dsx`, `client/streetless.cia` and `background/0004013000F5A302.cxi`. The client and module embed ISRG Root X1 by default. For a test CA, rebuild **both** with `CA_FILE=/absolute/path/to/root.pem`.

To build the same pinned CIA packaging tools used in CI on an x86-64 Linux host:

```sh
D=$(mktemp -d)
git clone https://github.com/3DSGuy/Project_CTR.git "$D/project-ctr"
git -C "$D/project-ctr" checkout --detach d52c2b0e788c2d085763d7c5dcc4b2edd9ffb7d8
make -C "$D/project-ctr/makerom" deps
make -C "$D/project-ctr/makerom"
git clone https://github.com/Epicpkmn11/bannertool.git "$D/bannertool"
git -C "$D/bannertool" checkout --detach 51dac927a4c43de769900d91fd3cb6142295263f
git -C "$D/bannertool" -c url.https://github.com/.insteadOf=git://github.com/ submodule update --init --recursive
make -C "$D/bannertool"
mkdir -p "$D/tools"
cp "$D/project-ctr/makerom/bin/makerom" "$D/bannertool/output/linux-x86_64/bannertool" "$D/tools/"
export STREETLESS_CIA_TOOLS="$D/tools"
```

These Linux binaries can be mounted into the matching devkitPro container:

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" \
  -v "$STREETLESS_CIA_TOOLS:/streetless-tools:ro" -w /work \
  devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5 \
  sh -lc 'dkp-pacman --noconfirm --needed -S 3ds-dev 3ds-curl 3ds-jansson python && export PATH="/streetless-tools:$PATH" && make -C client && make -C background'
```

### Tests

```sh
(cd server && go test ./... && go vet ./...)

cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/wire.c client/tests/wire_test.c -o /tmp/streetless-wire-test
/tmp/streetless-wire-test
cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/pair.c client/source/config.c client/tests/pair_test.c -o /tmp/streetless-pair-test
/tmp/streetless-pair-test
cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/http.c client/tests/http_debug_test.c -lcurl -o /tmp/streetless-http-test
/tmp/streetless-http-test
cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/http.c client/tests/http_ca_blob_test.c -lcurl -o /tmp/streetless-ca-test
python3 client/tests/http_ca_blob_test.py /tmp/streetless-ca-test
cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/title_names.c client/tests/title_names_test.c -o /tmp/streetless-title-test
/tmp/streetless-title-test
cc -std=c11 -Wall -Wextra -Werror -DSTREETLESS_SELECTION_HOST_TEST -Iclient/include client/source/selection.c client/tests/selection_test.c -o /tmp/streetless-selection-test
/tmp/streetless-selection-test
cc -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -DSTREETLESS_SELECTION_HOST_TEST -DSTREETLESS_SELECTION_STORAGE_TEST -Iclient/include client/source/selection.c client/tests/selection_storage_test.c -o /tmp/streetless-selection-storage-test
/tmp/streetless-selection-storage-test
cc -std=c11 -Wall -Wextra -Werror -Ibackground/include background/source/lifecycle.c background/tests/lifecycle_test.c -o /tmp/streetless-bg-test
/tmp/streetless-bg-test
```

GitHub Actions runs the Go and host tests, a Docker Compose HTTPS smoke test, and builds the client formats and background module. Passing CI is not a substitute for testing StreetPass, QR scanning, sleep/wake, and recovery on real hardware.

### Debugging

**Use debug mode only on an isolated, trusted network with a separate test database and disposable accounts.** HTTP exposes passwords, pairing codes, console tokens, game data and the debug PIN. The four-digit PIN is not encryption and cannot authenticate a server against an active network attacker.

For a loopback-only relay, from the repository root:

```sh
umask 077
D=$(mktemp -d)
openssl rand -hex 32 > "$D/admin-password.txt"
tr -d '\n' < "$D/admin-password.txt" | openssl dgst -sha256 -r | cut -d ' ' -f1 > "$D/admin.sha256"
cd server
env -u STREETLESS_TRANSPORT \
  STREETLESS_DEBUG=1 STREETLESS_ORIGIN=http://localhost:8080 \
  STREETLESS_LISTEN=127.0.0.1:8080 STREETLESS_PUBLIC_IPV4=127.0.0.1 \
  STREETLESS_ADMIN_HASH_FILE="$D/admin.sha256" STREETLESS_DB="$D/test.sqlite" \
  go run ./cmd/streetlessd
```

Open `http://localhost:8080/` on that machine; the admin username is `admin`, with the password in `$D/admin-password.txt`. `--debug` is an alternative to `STREETLESS_DEBUG=1`. The server prints a fresh four-digit PIN on each start. To use a real 3DS, configure a hostname and reachable LAN IPv4 for the isolated test relay.

On the 3DS, select **Debug mode**, accept the warning, enter the PIN and pair with the debug relay. Select **Debug mode** again and press **A** to enter a new PIN, or **X** to return to production HTTPS. The PIN is memory-only and debug mode resets on exit. A debug client probes the server before sending credentials or game data; production clients do not fall back to HTTP.

In debug mode, **Export local slot** saves an original native slot to `SD:/3ds/streetless/test-XXXXXXXX.slot`. The administrator can upload it for any active participant without a separate opt-in. Test deliveries expire after 24 hours, do not consume public-lobby quota, and use normal ACK/reject handling. Export and share native data only with its owner's consent. The background module does not use debug HTTP.

## Privacy and security

Relay operators can access stored native slot data, which can contain game identifiers and names. Participants must trust their relay operator. Game HMAC keys remain on the console; malformed deliveries and confirmed HMAC mismatches are rejected separately from successful ACKs. Temporary CECD failures and full inboxes leave deliveries queued.

Browser sessions expire after 24 hours. Passwords must be 8–128 UTF-8 bytes; common passwords and trivial repeated or sequential patterns are rejected, but this filter cannot guarantee strength. Use a unique password or passphrase. Never commit relay databases, administrator credentials, TLS keys, pairing codes or exported slots.

## Contributing

Issues and pull requests are welcome. Include reproduction steps and relevant logs, but remove credentials, pairing codes and native slot contents. Format changed Go code with `gofmt` and run the relevant tests. For device-specific changes, describe the model, firmware, Luma3DS version and hardware verification performed.

Version tags such as `v1.2.3` or `v1.2.3-rc.1` publish both client formats, the background CXI and the CIA QR to a GitHub Release, plus the relay image to `ghcr.io/<owner>/<repository>:<tag>`. Prerelease tags create prereleases; no `latest` image tag is published.

## License and credits

Streetless code is distributed under **GPL-3.0-or-later**; see [LICENSE](LICENSE). CECD wrappers derive from [NetPass](https://gitlab.com/3ds-netpass/netpass) and retain their original copyright notices. The bundled QR decoder has its own [ISC license](client/quirc/LICENSE). Do not copy NetPass artwork or sound assets without satisfying their separate license.
