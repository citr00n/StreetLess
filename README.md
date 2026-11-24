# Streetless

**StreetPass without touching grass.**

Streetless brings Nintendo 3DS StreetPass encounters online through a self-hosted relay. The project is inspired by [NetPass](https://gitlab.com/3ds-netpass/netpass); it is an independent service, not affiliated with NetPass or Nintendo

The client exchanges native game data over HTTPS/TCP, with an explicit relay IPv4 address so normal operation does not depend on DNS or UDP. Connectivity may still be affected by restrictions on the relay IP, TCP, TLS, or hostname. Only actual hardware testing can establish compatibility with a particular game or provider.

## Getting started

1. Open your relay's website and register an account. On a relay configured to require approval, wait for an administrator to approve it.
2. Install `streetless.3dsx` in `/3ds/streetless/` on the SD card and launch it with Homebrew Launcher.
3. In your account, generate a one-time pairing code. On your 3DS, scan its QR code or enter the relay address, IPv4 address and pairing code. Codes expire after ten minutes. Never share a code with anyone you do not want to connect to your account. StreetPass must already be enabled in a supported game, such as Mii Plaza; select **Sync** to exchange game data.
4. The public lobby is enabled by default and accepts at most three random incoming slots per rolling 24 hours. Switch it off for friends and invited rooms only. An explicit send requested on the site waits for the sender's next 3DS Sync; receiving also requires a Sync on the recipient's console.

A pairing code is temporary and can be claimed only by one console. Pairing again replaces the previous 3DS credential. The client verifies HTTPS certificates and does not fall back to plaintext. A QR image or code is sensitive until it expires: do not post it publicly or send it to an untrusted device.

## Running a relay

Build with Go 1.26 or later:

```sh
cd server
go build ./cmd/streetlessd
```

Create a separate administrator password and store its hash. Keep the password in a password manager; never commit the hash file, TLS key or SQLite database.

```sh
umask 077
ADMIN_PASSWORD=$(openssl rand -hex 32)
printf '%s\n' "$ADMIN_PASSWORD" > admin-password.txt
printf '%s' "$ADMIN_PASSWORD" | openssl dgst -sha256 -r | cut -d ' ' -f1 > admin.sha256
unset ADMIN_PASSWORD
```

Example direct HTTPS configuration (replace the example hostname and IPv4):

```sh
export STREETLESS_ORIGIN=https://streetless.example.org
export STREETLESS_PUBLIC_IPV4=203.0.113.10
export STREETLESS_ADMIN_HASH_FILE=/etc/streetless/admin.sha256
export STREETLESS_TLS_CERT=/etc/streetless/tls.crt
export STREETLESS_TLS_KEY=/etc/streetless/tls.key
export STREETLESS_DB=/var/lib/streetless/streetless.sqlite
export STREETLESS_REGISTRATION=open  # or approval
./streetlessd
```

Direct mode listens on `0.0.0.0:443` by default. Supply the certificate's complete chain and a certificate trusted by the 3DS client (its default trust root is ISRG Root X1). `/healthz` reports database readiness. The public website is at `/`, the account dashboard at `/me`, and the admin dashboard at `/admin` (HTTP Basic user `admin`). Administrators can approve registrations in approval mode and revoke access. Revocation cannot recall a pass already committed on a console.

For Linux, use [deploy/streetless.service](deploy/streetless.service) with a dedicated `streetless` account and the credentials described there. For a same-host reverse proxy, use the [nginx example](deploy/nginx/streetless.conf.example) and [systemd override](deploy/systemd/streetless.service.d/proxy.conf): set `STREETLESS_TRANSPORT=proxy` and `STREETLESS_LISTEN=127.0.0.1:8080`, keeping `STREETLESS_ORIGIN` and `STREETLESS_PUBLIC_IPV4` set to the public endpoint. The reverse proxy terminates TLS. Do not expose the backend port or mount Streetless beneath a URL prefix. For a proxy on another machine, use direct TLS to the backend and verify its certificate while restricting access with a firewall.

## Building the 3DS client

Install devkitPro with `3ds-dev`, `3ds-curl` and `3ds-jansson`, then run `make -C client`. Alternatively:

```sh
docker run --rm -v "$PWD:/work" -w /work \
  devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5 \
  sh -lc 'dkp-pacman --noconfirm --needed -S 3ds-dev 3ds-curl 3ds-jansson && make -C client'
```

The output is `client/streetless.3dsx`. For a test CA, rebuild with `make -C client CA_FILE=/absolute/path/to/root.pem`. A real console needs current system software and Luma3DS; neither emulator fixtures nor a successful build establish hardware compatibility.

## Admin test deliveries

The recipient must first opt in to admin test passes in their account or in the 3DS Privacy menu. Build the separate exporter with `make -C client TEST_SLOT_EXPORT=1`; run `streetless-slot-export.3dsx` on a test console and export an enabled game's native slot. Upload the resulting `/3ds/streetless/test-XXXXXXXX.slot` in the admin dashboard for the selected recipient. Use only data that the console owner agreed to share.

A test pass expires after 24 hours, does not generate an automatic return pass and does not consume the public-lobby quota. Syncing the recipient commits a valid pass before ACK; a confirmed malformed slot or invalid HMAC is rejected. Full inboxes and temporary errors leave the pass queued.

## Verification

- `cd server && go test ./...` exercises the server, SQLite routing and proxy behavior.
- From the repository root: `cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/wire.c client/tests/wire_test.c -o /tmp/streetless-wire-test && /tmp/streetless-wire-test`.
- Verify QR scanning, pairing and StreetPass delivery on real 3DS hardware. A successful server test or cross-compilation is not hardware proof.

CECD wrappers are derived from [NetPass](https://gitlab.com/3ds-netpass/netpass) and retain their copyright notices. The code is distributed under GPL-3.0-or-later; see [COPYING](COPYING). Do not copy NetPass artwork or sound assets without satisfying their separate license.
