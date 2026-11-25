# Streetless

**StreetPass without touching grass.**

Streetless brings Nintendo 3DS StreetPass encounters online through a self-hosted relay. The project is inspired by [NetPass](https://gitlab.com/3ds-netpass/netpass); it is an independent service, not affiliated with NetPass or Nintendo

In normal mode the client exchanges native game data over HTTPS/TCP, with an explicit relay IPv4 address so operation does not depend on DNS or UDP. An explicitly enabled debug mode also supports unencrypted HTTP for isolated development. Connectivity may still be affected by restrictions on the relay IP, TCP, TLS, or hostname. Only actual hardware testing can establish compatibility with a particular game or provider.

## Getting started

1. Open your relay's website and register an account. On a relay configured to require approval, wait for an administrator to approve it.
2. Install `streetless.3dsx` in `/3ds/streetless/` on the SD card and launch it with Homebrew Launcher, or install `streetless.cia` with a CIA installer on a suitably configured homebrew console.
3. In your account, generate a one-time pairing code. On your 3DS, scan its QR code or enter the relay address, IPv4 address and pairing code. Codes expire after ten minutes. Never share a code with anyone you do not want to connect to your account. StreetPass must already be enabled in a supported game, such as Mii Plaza; select **Sync** to exchange game data.
4. The public lobby is enabled by default and accepts at most five random incoming slots per rolling 24 hours. Switch it off for friends and invited rooms only. An explicit send requested on the site waits for the sender's next 3DS Sync; receiving also requires a Sync on the recipient's console.

A pairing code is temporary and can be claimed only by one console. Pairing again replaces the previous 3DS credential. In normal mode the client verifies HTTPS certificates and does not fall back to plaintext. A QR image or code is sensitive until it expires: do not post it publicly or send it to an untrusted device.

On the console, **Outgoing games** lists native StreetPass titles by name where available, or by CEC ID if the name cannot be read. Use the D-pad and **A** to toggle titles; **X** saves and **B** cancels. Before the first save, all eligible titles are enabled as in earlier versions. After saving, newly discovered titles are off until enabled. The setting filters outgoing slots without deliberately disabling incoming slots, although games requiring a two-way exchange may not receive if their outgoing slot is off. Already staged or queued server deliveries cannot be recalled by this setting; sync again to replace the server's prior outbox snapshot.

Preferences are stored separately from pairing in `/3ds/streetless/outgoing.bin`. If this file is missing after a selection was saved or is corrupt, sync fails closed. Restore it from a backup, or deliberately delete both `outgoing.bin` and `outgoing.protected` on the SD card to reset **all** titles to enabled.

Account passwords must be 8–128 UTF-8 bytes. Common passwords and trivial repeated or sequential patterns are rejected; this filter cannot guarantee a strong password, so use a unique password or passphrase. Browser sessions expire after 24 hours.

The former web-key login has been removed. An administrator can set a username and password for an existing participant from the dashboard; this invalidates that participant's existing browser sessions without changing the 3DS credential.

## Running a relay

Build the Docker image from the repository root:

```sh
docker build -t streetless:local .
```

The image contains only the server binary and runs as an unprivileged user by default. Its `/data` directory must be mounted from persistent, writable storage. The examples below run Docker as a non-root host user and explicitly use that user's UID and GID so the bind-mounted data and secrets are accessible.

**The production website and 3DS API require HTTPS.** The normal client verifies the TLS certificate and does not support plain HTTP. Use either direct TLS in Streetless or TLS termination at a reverse proxy. Proxy mode uses plain HTTP only on the same host's loopback interface; never expose that backend port. The separate debug mode described below is for isolated development only.

Point a domain at the relay's public IPv4 address and obtain a certificate for that domain, including its full certificate chain. The default 3DS client trusts ISRG Root X1; use a compatible certificate chain or build the client with a different trusted root as described below. Open the public TCP port in the host firewall and provider's firewall.

Create an administrator password and hash once. Keep the password in a password manager; never commit the hash file, TLS key or SQLite database:

```sh
umask 077
mkdir -p "$HOME/.local/share/streetless"
ADMIN_PASSWORD=$(openssl rand -hex 32)
printf '%s\n' "$ADMIN_PASSWORD" > "$HOME/.local/share/streetless/admin-password.txt"
printf '%s' "$ADMIN_PASSWORD" | openssl dgst -sha256 -r | cut -d ' ' -f1 \
  > "$HOME/.local/share/streetless/admin.sha256"
unset ADMIN_PASSWORD
```

Set the public options on the Docker host. Replace the domain and example IPv4 with the **public** address of the direct listener or proxy:

```sh
export STREETLESS_ORIGIN=https://streetless.example.org
export STREETLESS_PUBLIC_IPV4=203.0.113.10
export STREETLESS_DATA="$HOME/.local/share/streetless"
```

`STREETLESS_ORIGIN` is the public HTTPS origin, without a path or trailing slash. Include a non-default public port, for example `https://streetless.example.org:8443`; omit `:443`. The public IPv4 and origin are used for 3DS pairing and must be reachable by the console.

### Option 1: Direct HTTPS

Streetless terminates TLS itself. Set paths to the full certificate chain and private key on the Docker host. Both files must be readable by the chosen host user; do not make the private key publicly readable. Open TCP 8443 for this example:

```sh
export STREETLESS_ORIGIN=https://streetless.example.org:8443
export TLS_CERT=/absolute/path/to/fullchain.pem
export TLS_KEY=/absolute/path/to/privkey.pem
docker run -d --name streetless --restart unless-stopped \
  --user "$(id -u):$(id -g)" --cap-drop ALL --security-opt no-new-privileges \
  -p 8443:8443 \
  --mount "type=bind,src=$STREETLESS_DATA,dst=/data" \
  --mount "type=bind,src=$TLS_CERT,dst=/tls/fullchain.pem,readonly" \
  --mount "type=bind,src=$TLS_KEY,dst=/tls/privkey.pem,readonly" \
  -e STREETLESS_ORIGIN -e STREETLESS_PUBLIC_IPV4 \
  -e STREETLESS_TRANSPORT=direct -e STREETLESS_LISTEN=0.0.0.0:8443 \
  -e STREETLESS_ADMIN_HASH_FILE=/data/admin.sha256 \
  -e STREETLESS_TLS_CERT=/tls/fullchain.pem -e STREETLESS_TLS_KEY=/tls/privkey.pem \
  streetless:local
```

For external port 443, omit `:8443` from `STREETLESS_ORIGIN` and replace `-p 8443:8443` with `-p 443:8443`. The process still listens on unprivileged port 8443 inside the container. The database persists in `STREETLESS_DATA`; back it up along with its SQLite WAL files while the server is stopped.

#### Docker Compose

The [Compose example](compose.yaml) runs the same direct-HTTPS configuration. After creating the administrator hash and setting the common public options above, export the host user's numeric UID and GID and the absolute certificate paths:

```sh
export STREETLESS_UID="$(id -u)"
export STREETLESS_GID="$(id -g)"
export STREETLESS_ORIGIN=https://streetless.example.org:8443
export STREETLESS_PORT=8443
export TLS_CERT=/absolute/path/to/fullchain.pem
export TLS_KEY=/absolute/path/to/privkey.pem
docker compose up -d --build
```

The data directory and both certificate files must already exist and be accessible to that UID/GID. Compose mounts the certificates read-only and persists the database and administrator hash in `STREETLESS_DATA`. For public port 443 set `STREETLESS_PORT=443` and remove `:8443` from `STREETLESS_ORIGIN`. Use `docker compose logs -f` to view logs, `docker compose restart` after certificate renewal, and `docker compose down` to stop the container; the bind-mounted data is retained. This example is for direct HTTPS; for a same-host reverse proxy use the proxy instructions below.

### Option 2: Same-host reverse proxy

The proxy terminates HTTPS and forwards requests over loopback. On a **native Linux Docker host**, host networking lets Streetless bind to the same loopback interface as the proxy; do not publish or expose port 8080. Local processes can reach the backend, so use a trusted host:

```sh
export STREETLESS_ORIGIN=https://streetless.example.org
docker run -d --name streetless --restart unless-stopped --network host \
  --user "$(id -u):$(id -g)" --cap-drop ALL --security-opt no-new-privileges \
  --mount "type=bind,src=$STREETLESS_DATA,dst=/data" \
  -e STREETLESS_ORIGIN -e STREETLESS_PUBLIC_IPV4 \
  -e STREETLESS_TRANSPORT=proxy -e STREETLESS_LISTEN=127.0.0.1:8080 \
  -e STREETLESS_ADMIN_HASH_FILE=/data/admin.sha256 \
  streetless:local
```

`STREETLESS_ORIGIN` must be the **external HTTPS origin**, not the loopback address. Proxy mode accepts only a literal `127.0.0.1` listener and does not use TLS certificate paths. Docker Desktop host networking has different availability and semantics; alternatively use direct HTTPS and a reverse proxy that verifies the upstream certificate.

For example, add this nginx `server` block to the host's HTTP configuration. Replace the hostname and certificate paths, then run `nginx -t` and reload nginx:

```nginx
server {
    listen 443 ssl;
    server_name streetless.example.org;
    ssl_certificate /etc/nginx/tls/streetless.fullchain.pem;
    ssl_certificate_key /etc/nginx/tls/streetless.key;
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_early_data off;
    access_log off;
    client_max_body_size 2m;
    client_body_timeout 20s;
    location / {
        proxy_pass http://127.0.0.1:8080;
        proxy_http_version 1.1;
        proxy_set_header Host $http_host;
        proxy_set_header Connection "";
        proxy_set_header Forwarded "";
        proxy_set_header X-Forwarded-Host "";
        proxy_set_header X-Forwarded-Proto "";
        proxy_set_header X-Forwarded-Port "";
        proxy_set_header X-Forwarded-For "";
        proxy_set_header X-Real-IP "";
        proxy_connect_timeout 5s;
        proxy_send_timeout 20s;
        proxy_read_timeout 25s;
        proxy_request_buffering off;
        proxy_buffering off;
        proxy_cache off;
        proxy_next_upstream off;
        proxy_redirect off;
        proxy_intercept_errors off;
    }
}
```

Preserve the public `Host` header and mount Streetless at the root of its own origin, not under a URL prefix. For a proxy on another host, use direct TLS to the backend, restrict the upstream port with a firewall, and configure the proxy to verify the upstream certificate and hostname.

### Verify and operate

Open `https://streetless.example.org/healthz` (include the port if not 443); a healthy server returns `ok`. The website is at `/` and the account dashboard at `/me`. Sign in at `/login` as `admin` using the password in `admin-password.txt` to access the admin dashboard. Registration is open by default; an administrator can require approval in dashboard settings and approve pending registrations there. Revocation cannot recall a pass already committed on a console.

Use `docker logs streetless` to inspect startup errors and `docker stop streetless` to stop the server. To change configuration, remove the stopped container and run the appropriate command again; the data directory remains intact. In direct mode, restart the container after renewing the mounted certificate files. In proxy mode, reload the proxy after renewing its certificate.

## Building the 3DS client

Install devkitPro with `3ds-dev`, `3ds-curl` and `3ds-jansson`, Python 3, and compatible `makerom` and `bannertool` executables on `PATH`. Run `make -C client` to build both formats. To build only the Homebrew Launcher version without the CIA packaging tools, run `make -C client streetless.3dsx`.

For example, on an x86-64 Linux build host with Git, make and a C compiler, build the same pinned packaging tools used in CI:

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

With these Linux tools, use the matching x86-64 devkitPro Docker image from the repository root:

```sh
docker run --rm --platform linux/amd64 -v "$PWD:/work" \
  -v "$STREETLESS_CIA_TOOLS:/streetless-tools:ro" -w /work \
  devkitpro/devkitarm@sha256:116afba8df8453961de2936ffab20dd441edf4d682856c1ec8b0e53d7ed0bbf5 \
  sh -lc 'dkp-pacman --noconfirm --needed -S 3ds-dev 3ds-curl 3ds-jansson python && PATH="/streetless-tools:$PATH" make -C client'
```

The outputs are `client/streetless.3dsx` and `client/streetless.cia`. The CIA is unsigned homebrew and requires Luma3DS/CFW with signature patches. On systems without compatible packaging tools, use the CI artifacts or build only `.3dsx`. For a test CA, rebuild with `make -C client CA_FILE=/absolute/path/to/root.pem`. A real console needs current system software and Luma3DS; neither emulator fixtures nor a successful build establish hardware compatibility.

## Debug mode and test deliveries

**Debug mode is unsafe on an untrusted network.** HTTP exposes account passwords, console tokens, pairing codes, native slots and the debug PIN. A four-digit PIN is not encryption and cannot authenticate a server against an active network attacker. Use a separate test database and disposable accounts on an isolated LAN; never expose the debug listener to the Internet or use production credentials or game data without consent.

Start the server with `STREETLESS_DEBUG=1` or the `--debug` argument. Debug mode accepts an `http://` origin and prints a new four-digit PIN to the server console at every start. The console's debug mode requires this PIN and verifies that it is connecting to a debug server before sending credentials or game data; a normal production server cannot be used as a debug server. Re-enter the PIN after restarting the server. Normal mode continues to require HTTPS with certificate verification.

On the 3DS, choose **Debug mode** in the main menu, accept the plaintext warning and enter the server PIN. Pair the console using the account's QR code or manual details. Choose **Debug mode** again and press **A** to enter a new PIN, or **X** to return to production HTTPS. The PIN is held only in memory and debug mode resets when the application exits. **Export local slot** appears only while debug mode is enabled.

Admin test deliveries are available only on a debug server. The administrator may select any active participant; no recipient opt-in is required. Export an original native slot using the debug option in the client, then upload `/3ds/streetless/test-XXXXXXXX.slot` on the admin dashboard. Only export and share slots with their owner's consent. A test delivery expires after 24 hours, does not generate an automatic return pass and does not consume the public-lobby quota. A valid pass is committed before ACK; confirmed malformed slots and invalid HMACs are rejected, while full inboxes and temporary errors leave the pass queued.

For a loopback-only development server on the same machine, from the repository root:

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

Open `http://localhost:8080/` on that machine and sign in as `admin` with the password in `$D/admin-password.txt`. To connect a real 3DS, use a trusted isolated LAN, set `STREETLESS_LISTEN` and `STREETLESS_PUBLIC_IPV4` to the host's reachable LAN IPv4, and set `STREETLESS_ORIGIN` to a hostname and port the browser uses for the relay. The 3DS connects to the configured IPv4 directly. Preserve `D` if you need to reuse this test database; delete it only when its test data is no longer needed.

## Development

Prerequisites: Go 1.26+, a C compiler, OpenSSL, and devkitPro with `3ds-dev`, `3ds-curl`, and `3ds-jansson`. Docker is optional for reproducing the container and client builds. `server/` contains the Go relay and SQLite store; `client/` contains the 3DS application, native CECD code, and host-side codec tests. The server embeds the HTML templates and CSS from `server/internal/web/`.

Run the server checks from the repository root:

```sh
cd server
go test ./...
go vet ./...
cd ..
```

Run the portable client tests and build the console application:

```sh
cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/wire.c client/tests/wire_test.c -o /tmp/streetless-wire-test
/tmp/streetless-wire-test
cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/pair.c client/source/config.c client/tests/pair_test.c -o /tmp/streetless-pair-test
/tmp/streetless-pair-test
cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/http.c client/tests/http_debug_test.c -lcurl -o /tmp/streetless-http-test
/tmp/streetless-http-test
cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/title_names.c client/tests/title_names_test.c -o /tmp/streetless-title-test
/tmp/streetless-title-test
cc -std=c11 -Wall -Wextra -Werror -DSTREETLESS_SELECTION_HOST_TEST -Iclient/include client/source/selection.c client/tests/selection_test.c -o /tmp/streetless-selection-test
/tmp/streetless-selection-test
cc -std=c11 -Wall -Wextra -Werror -D_POSIX_C_SOURCE=200809L -DSTREETLESS_SELECTION_HOST_TEST -DSTREETLESS_SELECTION_STORAGE_TEST -Iclient/include client/source/selection.c client/tests/selection_storage_test.c -o /tmp/streetless-selection-storage-test
/tmp/streetless-selection-storage-test
make -C client
```

For a local debug relay, see the debug instructions above. Keep its database, credentials, and native slot exports separate from production and out of version control. Format changed Go code with `gofmt`; run the relevant tests before opening a pull request. Cross-compilation and synthetic fixtures do not substitute for a two-console hardware test.

## Verification

- `cd server && go test ./...` exercises the server, SQLite routing and proxy behavior.
- From the repository root: `cc -std=c11 -Wall -Wextra -Werror -Iclient/include client/source/wire.c client/tests/wire_test.c -o /tmp/streetless-wire-test && /tmp/streetless-wire-test`.
- GitHub Actions runs the Go tests and vet, the native wire and pairing tests, a Docker Compose HTTPS smoke test, and builds both 3DS client formats on pushes and pull requests. After the tests and build pass, download both files from the workflow run's `streetless-3ds` artifact.
- Verify QR scanning, pairing and StreetPass delivery on real 3DS hardware. A successful server test or cross-compilation is not hardware proof.

Version tags such as `v1.2.3` or `v1.2.3-rc.1` publish `streetless.3dsx`, `streetless.cia` and `streetless-cia-qr.png` to a GitHub Release, and the relay image to `ghcr.io/<owner>/<repository>:<tag>` (owner and repository are lowercase). The QR encodes the release's direct CIA download URL for a CIA installer such as FBI; the release must be publicly accessible to the console. Prerelease tags create prereleases; no `latest` tag is published.

CECD wrappers are derived from [NetPass](https://gitlab.com/3ds-netpass/netpass) and retain their copyright notices. The code is distributed under GPL-3.0-or-later; see [LICENSE](LICENSE). Do not copy NetPass artwork or sound assets without satisfying their separate license.
