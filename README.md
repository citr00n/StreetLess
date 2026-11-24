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

Build the Docker image from the repository root:

```sh
docker build -t streetless:local .
```

The image contains only the server binary and runs as an unprivileged user by default. Its `/data` directory must be mounted from persistent, writable storage. The examples below run Docker as a non-root host user and explicitly use that user's UID and GID so the bind-mounted data and secrets are accessible.

**The public website and 3DS API require HTTPS.** The 3DS client verifies the TLS certificate and does not support plain HTTP. Use either direct TLS in Streetless or TLS termination at a reverse proxy. Proxy mode uses plain HTTP only on the same host's loopback interface; never expose that backend port.

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
export STREETLESS_REGISTRATION=open  # use approval to require admin approval
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
  -e STREETLESS_ORIGIN -e STREETLESS_PUBLIC_IPV4 -e STREETLESS_REGISTRATION \
  -e STREETLESS_TRANSPORT=direct -e STREETLESS_LISTEN=0.0.0.0:8443 \
  -e STREETLESS_ADMIN_HASH_FILE=/data/admin.sha256 \
  -e STREETLESS_TLS_CERT=/tls/fullchain.pem -e STREETLESS_TLS_KEY=/tls/privkey.pem \
  streetless:local
```

For external port 443, omit `:8443` from `STREETLESS_ORIGIN` and replace `-p 8443:8443` with `-p 443:8443`. The process still listens on unprivileged port 8443 inside the container. The database persists in `STREETLESS_DATA`; back it up along with its SQLite WAL files while the server is stopped.

### Option 2: Same-host reverse proxy

The proxy terminates HTTPS and forwards requests over loopback. On a **native Linux Docker host**, host networking lets Streetless bind to the same loopback interface as the proxy; do not publish or expose port 8080. Local processes can reach the backend, so use a trusted host:

```sh
export STREETLESS_ORIGIN=https://streetless.example.org
docker run -d --name streetless --restart unless-stopped --network host \
  --user "$(id -u):$(id -g)" --cap-drop ALL --security-opt no-new-privileges \
  --mount "type=bind,src=$STREETLESS_DATA,dst=/data" \
  -e STREETLESS_ORIGIN -e STREETLESS_PUBLIC_IPV4 -e STREETLESS_REGISTRATION \
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

Open `https://streetless.example.org/healthz` (include the port if not 443); a healthy server returns `ok`. The website is at `/`, the account dashboard at `/me`, and the admin dashboard at `/admin` (HTTP Basic user `admin`, password in `admin-password.txt`). In approval mode, approve registrations in the admin dashboard. Revocation cannot recall a pass already committed on a console.

Use `docker logs streetless` to inspect startup errors and `docker stop streetless` to stop the server. To change configuration, remove the stopped container and run the appropriate command again; the data directory remains intact. In direct mode, restart the container after renewing the mounted certificate files. In proxy mode, reload the proxy after renewing its certificate.

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

CECD wrappers are derived from [NetPass](https://gitlab.com/3ds-netpass/netpass) and retain their copyright notices. The code is distributed under GPL-3.0-or-later; see [LICENSE](LICENSE). Do not copy NetPass artwork or sound assets without satisfying their separate license.
