#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "Please run this installer as root (sudo)." >&2
    exit 1
fi
if [ "$(uname -s)" != "Linux" ] || [ "$(uname -m)" != "x86_64" ]; then
    echo "This installer requires Linux x86_64." >&2
    exit 1
fi
for cmd in curl sha256sum systemctl mktemp chmod mv install; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "Missing required command: $cmd" >&2
        exit 1
    fi
done

UT_ORIGIN="${UT_ORIGIN:-}"
UT_DEVICE="${UT_DEVICE:-}"
UT_CODE="${UT_CODE:-}"

printf '%s' "$UT_ORIGIN" | grep -Eq '^https://[a-zA-Z0-9.-]+(:[0-9]{1,5})?$' || { echo "Invalid origin" >&2; exit 1; }
printf '%s' "$UT_DEVICE" | grep -Eq '^d-[0-9a-f]{64}$' || { echo "Invalid device id" >&2; exit 1; }
printf '%s' "$UT_CODE" | grep -Eq '^[0-9a-f]{64}$' || { echo "Invalid enrollment code" >&2; exit 1; }

extract_json_string() {
    printf '%s' "$1" | tr -d '\n\r' | sed -nE "s/.*\"$2\":\"([^\"]*)\".*/\1/p"
}
extract_json_number() {
    printf '%s' "$1" | tr -d '\n\r' | sed -nE "s/.*\"$2\":([0-9]+).*/\1/p"
}

MANIFEST_BODY=$(printf '{"device_id":"%s","code":"%s","platform":"linux-x64"}' "$UT_DEVICE" "$UT_CODE")
MANIFEST=$(curl -fsSL -X POST -H 'Content-Type: application/json' -d "$MANIFEST_BODY" "${UT_ORIGIN}/api/install/manifest")

FILE=$(extract_json_string "$MANIFEST" "file")
SHA256=$(extract_json_string "$MANIFEST" "sha256" | tr 'A-F' 'a-f')
NATIVE_HOST=$(extract_json_string "$MANIFEST" "native_host")
NATIVE_ADDR=$(extract_json_string "$MANIFEST" "native_address")
NATIVE_PORT=$(extract_json_number "$MANIFEST" "native_port")

printf '%s' "$FILE" | grep -Eq '^ultraterimal-agent-linux-x64-[0-9]+\.[0-9]+\.[0-9]+$' || { echo "Invalid agent file" >&2; exit 1; }
printf '%s' "$SHA256" | grep -Eq '^[0-9a-f]{64}$' || { echo "Invalid sha256" >&2; exit 1; }
printf '%s' "$NATIVE_HOST" | grep -Eq '^[a-zA-Z0-9.-]+$' || { echo "Invalid native_host" >&2; exit 1; }
printf '%s' "$NATIVE_ADDR" | grep -Eq '^[a-zA-Z0-9.-]+$' || { echo "Invalid native_address" >&2; exit 1; }
[ -n "$NATIVE_PORT" ] && [ "$NATIVE_PORT" -ge 1 ] && [ "$NATIVE_PORT" -le 65535 ] || { echo "Invalid native_port" >&2; exit 1; }

if [ -f /etc/ultraterimal/agent.json ]; then
    EXISTING_DEVICE=$(extract_json_string "$(cat /etc/ultraterimal/agent.json)" "device_id")
    if [ -n "$EXISTING_DEVICE" ] && [ "$EXISTING_DEVICE" != "$UT_DEVICE" ]; then
        echo "This host is already bound to another device ($EXISTING_DEVICE)." >&2
        exit 1
    fi
fi

CA_FILE="system"
for candidate in /etc/ssl/certs/ca-certificates.crt /etc/pki/tls/certs/ca-bundle.crt /etc/ssl/cert.pem; do
    if [ -r "$candidate" ]; then
        CA_FILE="$candidate"
        break
    fi
done

TMPDIR=$(mktemp -d /tmp/ultraterimal-install.XXXXXX)
cleanup() {
    UT_CODE=""
    rm -rf "$TMPDIR"
}
trap cleanup EXIT INT TERM

PKG_BODY=$(printf '{"device_id":"%s","code":"%s","sha256":"%s","platform":"linux-x64"}' "$UT_DEVICE" "$UT_CODE" "$SHA256")
echo "Downloading Ultraterimal Linux Agent..."
curl -fsSL -X POST -H 'Content-Type: application/json' -d "$PKG_BODY" "${UT_ORIGIN}/api/install/package" -o "${TMPDIR}/agent"
ACTUAL_SHA=$(sha256sum "${TMPDIR}/agent" | awk '{print $1}')
if [ "$ACTUAL_SHA" != "$SHA256" ]; then
    echo "Agent binary checksum mismatch." >&2
    exit 1
fi

install -d -m 0755 /opt/ultraterimal
install -d -m 0700 /var/lib/ultraterimal
install -d -m 0755 /etc/ultraterimal
if [ -f /opt/ultraterimal/agent ] && systemctl is-active --quiet ultraterimal-agent 2>/dev/null; then
    systemctl stop ultraterimal-agent
fi
install -m 0755 "${TMPDIR}/agent" /opt/ultraterimal/agent.new
mv -f /opt/ultraterimal/agent.new /opt/ultraterimal/agent

BOOTSTRAP="${TMPDIR}/bootstrap.json"
printf '{"host":"%s","address":"%s","port":"%s","ca_file":"%s","credential_file":"/var/lib/ultraterimal/credential"}' \
    "$NATIVE_HOST" "$NATIVE_ADDR" "$NATIVE_PORT" "$CA_FILE" > "$BOOTSTRAP"
chmod 0600 "$BOOTSTRAP"

HOST_NAME=$(hostname 2>/dev/null || uname -n)
HOST_NAME=$(printf '%s' "$HOST_NAME" | tr -cd 'a-zA-Z0-9._-' | cut -c1-120)
[ -n "$HOST_NAME" ] || HOST_NAME="linux-host"

if [ ! -f /var/lib/ultraterimal/credential ]; then
    printf '{"code":"%s","name":"%s"}' "$UT_CODE" "$HOST_NAME" | /opt/ultraterimal/agent --enroll "$BOOTSTRAP" > "${TMPDIR}/agent.json"
    chmod 0600 "${TMPDIR}/agent.json"
    mv -f "${TMPDIR}/agent.json" /etc/ultraterimal/agent.json
else
    printf '{"host":"%s","address":"%s","port":"%s","device_id":"%s","ca_file":"%s","credential_file":"/var/lib/ultraterimal/credential"}' \
        "$NATIVE_HOST" "$NATIVE_ADDR" "$NATIVE_PORT" "$UT_DEVICE" "$CA_FILE" > "${TMPDIR}/agent.json"
    chmod 0600 "${TMPDIR}/agent.json"
    mv -f "${TMPDIR}/agent.json" /etc/ultraterimal/agent.json
fi

/opt/ultraterimal/agent --install
/opt/ultraterimal/agent --start
echo "Ultraterimal Linux Agent is installed and running."
