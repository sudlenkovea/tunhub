#!/usr/bin/env bash
# Creates a persistent self-signed code-signing certificate "TunHub Dev".
# Run ONCE. It may ask for your login-keychain password — that's expected.
set -euo pipefail
NAME="TunHub Dev"

if security find-identity -v -p codesigning 2>/dev/null | grep -q "$NAME"; then
    echo "\"$NAME\" already exists — nothing to do."
    exit 0
fi

TMP="$(mktemp -d)"
cat > "$TMP/cfg" <<EOF
[req]
distinguished_name = dn
x509_extensions = v3
prompt = no
[dn]
CN = $NAME
[v3]
basicConstraints = critical,CA:false
keyUsage = critical,digitalSignature
extendedKeyUsage = critical,codeSigning
EOF

openssl req -x509 -newkey rsa:2048 -nodes -keyout "$TMP/key.pem" -out "$TMP/cert.pem" \
    -days 3650 -config "$TMP/cfg"
openssl pkcs12 -export -inkey "$TMP/key.pem" -in "$TMP/cert.pem" -out "$TMP/id.p12" \
    -passout pass:tunhub -name "$NAME"

# Import into the login keychain, allowing codesign to use the key.
security import "$TMP/id.p12" -k "$HOME/Library/Keychains/login.keychain-db" \
    -P tunhub -T /usr/bin/codesign

rm -rf "$TMP"
echo ""
echo "Done: \"$NAME\" created."
security find-identity -v -p codesigning | grep "$NAME" || true
echo "Now run:  ./build.sh"
