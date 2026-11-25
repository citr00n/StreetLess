# Deploying Streetless

This guide covers a self-hosted relay using Docker. The production website and 3DS API require **HTTPS**: either Streetless terminates TLS itself, or a reverse proxy terminates TLS on the same host. The 3DS uses IPv4 over TCP; normal exchange does not depend on UDP or DNS on the console.

For client installation, development, and the isolated HTTP debug mode, see the [README](README.md).

## Prerequisites

- Docker; Docker Compose for the Compose example.
- A hostname pointing to the relay's public IPv4 address and an open public TCP port.
- A TLS certificate for that hostname, its full chain, and its private key. The default 3DS client trusts **ISRG Root X1**; use a compatible chain or build the client and background module with your own trusted CA.
- OpenSSL for the administrator credential below.

Keep the SQLite database, administrator password and hash, and TLS private key outside the repository. The relay operator can access native slot data; run a relay only for participants who trust the operator.

## Administrator credentials

Create the password and its SHA-256 hash once. Store the password securely; it is used at `/login` with username `admin`.

```sh
umask 077
mkdir -p "$HOME/.local/share/streetless"
ADMIN_PASSWORD=$(openssl rand -hex 32)
printf '%s\n' "$ADMIN_PASSWORD" > "$HOME/.local/share/streetless/admin-password.txt"
printf '%s' "$ADMIN_PASSWORD" | openssl dgst -sha256 -r | cut -d ' ' -f1 \
  > "$HOME/.local/share/streetless/admin.sha256"
unset ADMIN_PASSWORD
```

Registration is open by default. An administrator can switch to approval-required registration in the dashboard; the setting is stored in the database.

## Common settings

From the repository root, build the image and set the public endpoint. Replace the hostname and documentation-only IPv4 with your own:

```sh
docker build -t streetless:local .
export STREETLESS_ORIGIN=https://streetless.example.org
export STREETLESS_PUBLIC_IPV4=203.0.113.10
export STREETLESS_DATA="$HOME/.local/share/streetless"
```

`STREETLESS_ORIGIN` is the public origin, not the backend listener: use `https://<hostname>[:port]` with no path or trailing slash. Include a non-default port, but omit an explicit `:443`. `STREETLESS_PUBLIC_IPV4` must be reachable from the 3DS; it is included in pairing details. Open the corresponding TCP port in both the host and provider firewalls.

The examples run the container with the host user's numeric UID/GID, so the bind-mounted data directory and credentials must be accessible to that user. Do not make private keys publicly readable. The image runs as a non-root user by default and needs a persistent, writable `/data` mount.

| Setting | Purpose |
| --- | --- |
| `STREETLESS_ORIGIN` | Public HTTPS origin; required. |
| `STREETLESS_PUBLIC_IPV4` | Public IPv4 used for console pairing; required. |
| `STREETLESS_ADMIN_HASH_FILE` | Path to the administrator password hash; required. |
| `STREETLESS_DB` | SQLite path; defaults to `streetless.sqlite` outside the image and `/data/streetless.sqlite` in the image. |
| `STREETLESS_TRANSPORT` | `direct` by default, or `proxy` for a same-host reverse proxy. |
| `STREETLESS_LISTEN` | Listener; defaults to `0.0.0.0:443` in direct mode and `127.0.0.1:8080` in proxy mode. |
| `STREETLESS_TLS_CERT`, `STREETLESS_TLS_KEY` | Full certificate chain and private key; required in direct mode. |
| `STREETLESS_DEBUG` | `1` enables isolated HTTP debug mode; leave unset or `0` in production. |

## Option 1: direct HTTPS

Streetless terminates TLS itself. Set absolute paths to certificate files readable by the selected UID/GID. This example publishes TCP port 8443:

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

To publish external port 443 instead, omit `:8443` from `STREETLESS_ORIGIN` and replace `-p 8443:8443` with `-p 443:8443`. The container continues to listen on port 8443.

### Docker Compose

The supplied [compose.yaml](compose.yaml) implements direct HTTPS. After setting the common variables and creating the administrator hash, export the host UID, GID and absolute certificate paths:

```sh
export STREETLESS_UID="$(id -u)"
export STREETLESS_GID="$(id -g)"
export STREETLESS_ORIGIN=https://streetless.example.org:8443
export STREETLESS_PORT=8443
export TLS_CERT=/absolute/path/to/fullchain.pem
export TLS_KEY=/absolute/path/to/privkey.pem
docker compose up -d --build
```

The data directory and both certificate files must already exist and be accessible to that UID/GID. Set `STREETLESS_PORT=443` and remove `:8443` from the origin to publish on port 443. `docker compose down` removes the container but retains the bind-mounted data.

## Option 2: same-host reverse proxy

The reverse proxy terminates public HTTPS. In proxy mode, Streetless accepts **only** a literal `127.0.0.1:<port>` listener. The following host-network example is for a native Linux Docker host; local processes on that host can reach the backend, so use a trusted host. Do not publish port 8080.

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

`STREETLESS_ORIGIN` and `STREETLESS_PUBLIC_IPV4` still refer to the public endpoint, not loopback. Docker Desktop host networking has different availability and semantics. For a proxy on another host, use direct TLS to the backend, restrict the upstream port with a firewall, and verify its certificate and hostname.

Example nginx `server` block for a same-host proxy; replace the domain and certificate paths, enable the block, run `nginx -t`, then reload nginx:

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

Preserve the public `Host` header, forward the entire site and API without rewriting paths, and dedicate the origin's root to Streetless. The backend does not trust forwarding headers to determine its origin or authorize requests.

## Verify and operate

- Open `https://streetless.example.org/healthz`, including the port if not 443. A healthy relay returns `ok`.
- The website is at `/`, participant accounts are at `/me`, and the administrator dashboard is at `/admin` after signing in through `/login`.
- Inspect startup errors with `docker logs streetless` or `docker compose logs`.
- Persist and back up the data directory, including SQLite's WAL files, while the server is stopped. Back up TLS and administrator credentials separately and securely.
- After renewing certificates, restart the container in direct mode; reload the proxy in proxy mode. To change container settings, recreate the container without deleting its data directory.
- In production, do not enable debug mode. Debug HTTP exposes passwords, pairing codes, console tokens and game data; its four-digit PIN is not transport security. Use a separate database and an isolated test network as described in the [debugging guide](README.md#debugging).
