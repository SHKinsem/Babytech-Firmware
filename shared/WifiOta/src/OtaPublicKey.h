#pragma once

// Development signing key. The private half stays in ignored out/ota/signing-key.pem.
// Replace this public key and store the release private key offline before production.
static constexpr char kBabytechOtaPublicKey[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEOYNXaTX9ibzbv6L1komI6zHNyKVC\n"
    "SD0/6oOoVHlI4QSX5fK/YxMk10g0S+YSYcpjMPkVs3Z1JcnSDxTzcudPtg==\n"
    "-----END PUBLIC KEY-----\n";
