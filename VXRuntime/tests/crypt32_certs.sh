#!/bin/bash
# Makes the made-up certificates of tests/crypt32.c (a test root, an RSA leaf it signed for TLS, valid under 825 days as
# macOS requires of a TLS server certificate, and a self-signed P-256 one) with
# macOS's own openssl and writes them as C arrays to tests/crypt32_certs.h. Run by hand when they need to change; the
# keys are thrown away (nothing signs with them again). usage: tests/crypt32_certs.sh  (from VXRuntime/)
set -e
out="$(pwd)/tests/crypt32_certs.h"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
ssl=/usr/bin/openssl
mkdir newcerts && touch index.txt && echo 01A2B3C4D5 > serial
cat > ca.cnf <<'EOF'
[ ca ]
default_ca = test
[ test ]
dir = .
database = ./index.txt
new_certs_dir = ./newcerts
serial = ./serial
default_md = sha256
policy = anything
unique_subject = no
email_in_dn = yes
[ anything ]
countryName = optional
stateOrProvinceName = optional
localityName = optional
organizationName = optional
organizationalUnitName = optional
commonName = supplied
emailAddress = optional
[ root ]
basicConstraints = critical,CA:TRUE
keyUsage = critical,keyCertSign,cRLSign
subjectKeyIdentifier = hash
[ leaf ]
basicConstraints = CA:FALSE
keyUsage = critical,digitalSignature,keyEncipherment
extendedKeyUsage = serverAuth,clientAuth
subjectAltName = DNS:leaf.vxrt.test,DNS:alt.vxrt.test,email:alt@vxrt.test,URI:https://vxrt.test/leaf,otherName:1.3.6.1.4.1.311.20.2.3;UTF8:user@vxrt.test
[ ec ]
basicConstraints = CA:FALSE
keyUsage = digitalSignature
extendedKeyUsage = codeSigning
EOF
$ssl genrsa -out root.key 2048 2>/dev/null
$ssl req -new -key root.key -subj "/C=JP/O=VeRantyx Test/CN=VXRT Test Root CA" -out root.csr
$ssl ca -batch -config ca.cnf -selfsign -keyfile root.key -in root.csr -extensions root -startdate 20200101000000Z -enddate 20600101000000Z -out root.pem 2>/dev/null
$ssl genrsa -out leaf.key 2048 2>/dev/null
$ssl req -new -key leaf.key -subj "/C=JP/ST=Tokyo/L=Shibuya/O=VeRantyx Test/OU=Runtime/CN=leaf.vxrt.test/emailAddress=leaf@vxrt.test" -out leaf.csr
$ssl ca -batch -config ca.cnf -cert root.pem -keyfile root.key -in leaf.csr -extensions leaf -startdate 210101000000Z -enddate 221231000000Z -out leaf.pem 2>/dev/null
$ssl ecparam -name prime256v1 -genkey -noout -out ec.key
$ssl req -new -key ec.key -subj "/O=VeRantyx Test/OU=Signing \"Lab\", Tokyo/CN=VXRT EC Test" -out ec.csr
$ssl ca -batch -config ca.cnf -selfsign -keyfile ec.key -in ec.csr -extensions ec -startdate 220601123000Z -enddate 320601123000Z -out ec.pem 2>/dev/null
bytes() { sed 's/../0x&, /g; s/, $//'; }                       # hex digits -> C bytes
{
    echo "// Made by tests/crypt32_certs.sh (made-up test certificates; their keys were discarded). Do not edit."
    for n in root leaf ec; do
        $ssl x509 -in $n.pem -outform DER -out $n.der
        echo "static const unsigned char cert_$n[] = {"
        xxd -i < $n.der
        echo "};"
        echo "static const unsigned char sha1_$n[20] = { $(shasum -a 1 < $n.der | cut -c1-40 | bytes) };"
    done
    # CERT_KEY_IDENTIFIER_PROP_ID of a certificate without a subject key identifier: SHA-1 of its SubjectPublicKeyInfo
    $ssl x509 -in leaf.pem -noout -pubkey | $ssl pkey -pubin -outform DER -out leaf_spki.der
    echo "static const unsigned char keyid_leaf[20] = { $(shasum -a 1 < leaf_spki.der | cut -c1-40 | bytes) };"
} > "$out"
echo "wrote $out"
