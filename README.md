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
3. In your account, generate a pairing code. On the 3DS, open **Account** and choose **Scan QR code** or **Enter pairing code**. For manual pairing, enter the relay hostname, IPv4 address, port and code.
4. Select **Sync** to exchange passes. Pairing codes expire after ten minutes; do not share them. Pairing a new console replaces the previous console credential.

If the relay accepts pairing but the SD configuration cannot be saved, keep `pair.pending` and retry from **Account** in the same mode. The client can finish a previously flushed matching configuration without contacting the relay, even after the code expires. If it still fails, report the displayed SD operation and error code; never share `pair.pending`, `config.txt` or `config.txt.tmp`, as they contain credentials.

Releases include `streetless-cia-qr.png`, which points to the release's CIA download for an installer such as FBI. The release must be publicly accessible to the console.

The graphical home screen groups account pairing under **Account** and local preferences under **Settings**. Use the D-pad to navigate, **A** to open, **B** to go back and **START** to exit. Social pages and sync progress use the text interface.

### Choose outgoing games

Open **Settings → Outgoing games** in the client. Select a title with the D-pad, toggle it with **A**, save with **X**, or cancel with **B**. The client shows the game name when it can read local title metadata; otherwise it shows the exact CEC ID.

Before the first save, all eligible games are enabled. After saving, newly discovered games are disabled until selected. This setting filters outgoing slots, not incoming metadata; however, games requiring a two-way exchange may not receive without an outgoing slot. Already staged or queued deliveries cannot be recalled by a local selection change. Sync again to replace the relay's prior outbox snapshot.

Preferences are stored independently of pairing in `SD:/3ds/streetless/outgoing.bin`. If saved preferences are missing or corrupt, sync fails closed. Restore the file from a backup, or deliberately delete both `outgoing.bin` and `outgoing.protected` on the SD card to reset **all** titles to enabled.

### Check outgoing passes

After a successful manual Sync, the client lists the selected game slots uploaded to the relay. Uploading a slot does not mean that a recipient has been found or that a pass has been received.

The public-lobby status lists game slots currently staged for random matching. The console resolves installed game names where possible; the website displays CEC IDs. Staging does not mean another player has received a pass. A match requires another eligible player with a compatible game and available capacity. If the lobby is disabled, the snapshot has expired, or a game has no eligible outgoing slot, that game is not listed. A friends-only native slot is skipped rather than sent.

During manual Sync, the console shows its progress while looking up local game names. Uploaded and public-lobby lists share one lookup. If installed metadata is unavailable or conflicting, the client shows the CEC ID instead of guessing a title.

The client and account page also show the number of outgoing deliveries queued for other players since this counter was introduced. One slot queued to multiple recipients counts once per recipient; administrator test deliveries do not count. The total persists after deliveries are removed, so it is not the current queue size and does not confirm receipt. Existing deliveries from before the upgrade are not included.

## Background sync

The optional `0004013000F5A302.cxi` module is **experimental and unverified on physical 3DS hardware**. It requires Luma3DS 13 or newer and a production HTTPS relay. The CIA QR installs only the foreground app, not this module.

The `v0.1.0-alpha.1` background module can crash at startup when allocating linear memory. Use a build containing the sysmodule heap fix instead. If the old module causes a crash, power off the console and remove its CXI before restarting.

1. Copy the module to `SD:/luma/sysmodules/0004013000F5A302.cxi`. Do not overwrite an existing file with that name without checking it first.
2. Hold **SELECT** while booting and enable **Enable loading external FIRMs and modules** in Luma's configuration.
3. Pair the console, choose outgoing games, then select **Background sync** in Streetless and confirm the warning. Once the service reports it connected, Streetless can be closed.
4. Start the service manually again after each power-on. Reopen **Background sync** to check its status or stop it.

The module attempts a sync immediately and every 15 minutes while the console is awake and networking is available. It also sends selected outgoing slots through allowed routes. Sleep-mode networking and delivery are not guaranteed. Manual Sync, pairing, slot export and outgoing-game selection require a confirmed stop; the client prompts for it.

If the service cannot be stopped or its status is uncertain, power the console off completely. Remove the module from the SD card if needed. A stale or damaged `SD:/3ds/streetless/background.pending` marker may be removed **only while the console is completely powered off**, after confirming the module is not running.

## Relay deployment

See [DEPLOY.md](DEPLOY.md) for Docker and Compose setup, direct HTTPS, same-host reverse proxy configuration, administrator setup, backups and operational settings. On a new, empty database, the first successfully registered account becomes an immediately approved administrator. Keep access restricted until a trusted operator has registered: any first visitor can claim administration. Existing databases with registered accounts require offline administrator bootstrap as described in the deployment guide.

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

To reproduce the CI client and background build with Docker, run the following
from the repository root. The pinned packaging tools are built and run inside
the same pinned devkitPro container, avoiding host/container libc incompatibilities.
The `linux/amd64` platform may require emulation on other architectures.

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" -w /work \
  devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5 \
  sh -eu -c '
    apt-get update
    apt-get install -y --no-install-recommends build-essential ca-certificates git python3 zip
    dkp-pacman --noconfirm --needed -S 3ds-dev 3ds-curl 3ds-jansson
    git clone https://github.com/3DSGuy/Project_CTR.git /tmp/project-ctr
    git -C /tmp/project-ctr checkout --detach d52c2b0e788c2d085763d7c5dcc4b2edd9ffb7d8
    for lib in libmbedtls libblz libyaml; do
      make -C "/tmp/project-ctr/makerom/deps/$lib" static_lib
    done
    make -C /tmp/project-ctr/makerom
    git clone https://github.com/Epicpkmn11/bannertool.git /tmp/bannertool
    git -C /tmp/bannertool checkout --detach 51dac927a4c43de769900d91fd3cb6142295263f
    git -C /tmp/bannertool -c url.https://github.com/.insteadOf=git://github.com/ submodule update --init --recursive
    make -C /tmp/bannertool
    export PATH="/tmp/project-ctr/makerom/bin:/tmp/bannertool/output/linux-x86_64:$PATH"
    make -C client
    make -C background
    test -s client/streetless.3dsx
    test -s client/streetless.cia
    test -s background/0004013000F5A302.cxi
  '
```

### Tests

```sh
(cd server && go test ./... && go vet ./...)

cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/wire.c client/tests/wire_test.c -o /tmp/streetless-wire-test
/tmp/streetless-wire-test
cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/pair.c client/source/config.c client/tests/pair_test.c -o /tmp/streetless-pair-test
/tmp/streetless-pair-test
cc -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -Iclient/include client/source/pair_store.c client/source/sd_replace.c client/source/config.c client/tests/pair_store_test.c -o /tmp/streetless-pair-store-test
/tmp/streetless-pair-store-test
cc -std=c11 -Wall -Wextra -Werror -D__3DS__ -Iclient/tests/stubs -Iclient/include client/source/sd_replace.c client/tests/sd_replace_test.c -o /tmp/streetless-sd-replace-test
/tmp/streetless-sd-replace-test
cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/http.c client/tests/http_debug_test.c -lcurl -o /tmp/streetless-http-test
/tmp/streetless-http-test
cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/http.c client/tests/http_ca_blob_test.c -lcurl -o /tmp/streetless-ca-test
python3 client/tests/http_ca_blob_test.py /tmp/streetless-ca-test
cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/title_names.c client/tests/title_names_test.c -o /tmp/streetless-title-test
/tmp/streetless-title-test
cc -std=c11 -Wall -Wextra -Werror -D__3DS__ -Iclient/tests/title_stubs -Iclient/include client/source/title_names.c client/tests/title_names_3ds_test.c -o /tmp/streetless-title-3ds-test
/tmp/streetless-title-3ds-test
cc -std=c11 -Wall -Wextra -Werror -DSTREETLESS_SELECTION_HOST_TEST -Iclient/include client/source/selection.c client/tests/selection_test.c -o /tmp/streetless-selection-test
/tmp/streetless-selection-test
cc -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -DSTREETLESS_SELECTION_HOST_TEST -DSTREETLESS_SELECTION_STORAGE_TEST -Iclient/include client/source/selection.c client/tests/selection_storage_test.c -o /tmp/streetless-selection-storage-test
/tmp/streetless-selection-storage-test
cc -std=c11 -Wall -Wextra -Werror -Ibackground/include background/source/lifecycle.c background/tests/lifecycle_test.c -o /tmp/streetless-bg-test
/tmp/streetless-bg-test
cc -std=c11 -Wall -Wextra -Werror -Ibackground/tests/stubs background/source/heap.c background/tests/heap_test.c -o /tmp/streetless-bg-heap-test
/tmp/streetless-bg-heap-test
```

GitHub Actions runs the Go and host tests, a Docker Compose HTTPS smoke test, and builds the client formats and background module for pull requests ready for review, pushes to `main`, and merge queues. Draft pull requests skip the jobs; marking one ready starts CI. Successful CI runs provide the client and background artifacts. Passing CI is not a substitute for testing StreetPass, QR scanning, sleep/wake, and recovery on real hardware.

### Debugging

**Use debug mode only on an isolated, trusted network with a separate test database and disposable accounts.** HTTP exposes passwords, pairing codes, console tokens, game data and the debug PIN. The four-digit PIN is not encryption and cannot authenticate a server against an active network attacker.

For a loopback-only relay, from the repository root:

```sh
umask 077
D=$(mktemp -d)
cd server
env -u STREETLESS_TRANSPORT \
  STREETLESS_DEBUG=1 STREETLESS_ORIGIN=http://localhost:8080 \
  STREETLESS_LISTEN=127.0.0.1:8080 STREETLESS_PUBLIC_IPV4=127.0.0.1 \
  STREETLESS_DB="$D/test.sqlite" \
  go run ./cmd/streetlessd
```

Open `http://localhost:8080/` on that machine and register a disposable account; the first registered account becomes administrator. Sign in normally to use `/admin`. `--debug` is an alternative to `STREETLESS_DEBUG=1`. The server prints a fresh four-digit PIN on each start. To use a real 3DS, configure a hostname and reachable LAN IPv4 for the isolated test relay.

On the 3DS, select **Settings → Debug mode**, accept the warning, enter the PIN and pair with the debug relay from **Account**. Select **Settings → Debug mode** again and press **A** to enter a new PIN, or **X** to return to production HTTPS. The PIN is memory-only and debug mode resets on exit. A debug client probes the server before sending credentials or game data; production clients do not fall back to HTTP.

In debug mode, **Settings → Export local slot** saves an original native slot to `SD:/3ds/streetless/test-XXXXXXXX.slot`. The administrator can upload it for any active participant without a separate opt-in. Test deliveries expire after 24 hours, do not consume public-lobby quota, and use normal ACK/reject handling. Export and share native data only with its owner's consent. The background module does not use debug HTTP.

## Privacy and security

Relay operators can access stored native slot data, which can contain game identifiers and names. Participants must trust their relay operator. Game HMAC keys remain on the console; malformed deliveries and confirmed HMAC mismatches are rejected separately from successful ACKs. Temporary CECD failures and full inboxes leave deliveries queued.

Browser sessions expire after 24 hours. Passwords must be 8–128 UTF-8 bytes; common passwords and trivial repeated or sequential patterns are rejected, but this filter cannot guarantee strength. Use a unique password or passphrase. Never commit relay databases, administrator credentials, TLS keys, pairing codes or exported slots.

## Contributing

Issues and pull requests are welcome. Include reproduction steps and relevant logs, but remove credentials, pairing codes and native slot contents. Format changed Go code with `gofmt` and run the relevant tests. For device-specific changes, describe the model, firmware, Luma3DS version and hardware verification performed.

### Releases

Merge changes through a pull request to `main` and wait for CI. Repository maintainers should configure branch protection or a ruleset to require review and the CI checks; workflow files alone do not enforce these policies.

To release a commit that is already on `main`, create and push a version tag:

```sh
git switch main
git pull --ff-only
git tag -a v1.2.3-rc.1 -m "v1.2.3-rc.1"
git push origin v1.2.3-rc.1
```

The release workflow validates the tag and its target, reruns CI on that commit, and publishes only after the checks pass. Stable tags such as `v1.2.3` create releases; prerelease tags such as `v1.2.3-rc.1` create prereleases. Use canonical `vMAJOR.MINOR.PATCH` versions, optionally with a prerelease suffix; build metadata is not supported by Docker tags. Do not move or reuse a published tag.

Publication uses a draft release. If it fails after the draft is created, inspect the workflow logs and package state, delete the incomplete draft (not the tag), then rerun the failed workflow. Existing releases are never overwritten automatically.

Each release contains both client formats, the background CXI and the CIA QR. The relay image is published as `ghcr.io/<owner>/<repository>:<tag>`; no `latest` tag is published.

## License and credits

Streetless code is distributed under **GPL-3.0-or-later**; see [LICENSE](LICENSE). CECD wrappers derive from [NetPass](https://gitlab.com/3ds-netpass/netpass) and retain their original copyright notices. The bundled QR decoder has its own [ISC license](client/quirc/LICENSE). Do not copy NetPass artwork or sound assets without satisfying their separate license.
