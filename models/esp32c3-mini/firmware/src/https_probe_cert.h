/* A self-signed ECDSA P-256 certificate for the HTTPS memory probe only
 * (docs/plan-firmware-https.md). Not a secret: a test key, never a station's. */
static const char k_probe_cert[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIBiTCCAS+gAwIBAgIUL2Orq73WwsS9EuEJmSCTY6zdNCcwCgYIKoZIzj0EAwIw\n"
"GjEYMBYGA1UEAwwPeDN2Z2F6LnhwcnMuZGV2MB4XDTI2MTExMDE1MjQzNFoXDTM2\n"
"MTEwNzE1MjQzNFowGjEYMBYGA1UEAwwPeDN2Z2F6LnhwcnMuZGV2MFkwEwYHKoZI\n"
"zj0CAQYIKoZIzj0DAQcDQgAELQo1h6CRip6LEvgF8Xhyj2yJmIzumxcOQnmHDTI5\n"
"idGhG/IeRlkeSSfYzmCv1ocvxL4lKDjp1xP9qDnvdQhEZqNTMFEwHQYDVR0OBBYE\n"
"FFdEac9H+xt8RNVzh21sTgyG7rwoMB8GA1UdIwQYMBaAFFdEac9H+xt8RNVzh21s\n"
"TgyG7rwoMA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIgLhYwqH1Q\n"
"nANoPwDvgw5OEsiDVoMuhqIvceGI/75dNEgCIQDe7TGeI5JZnnfNCrdr6R/GMJ+r\n"
"4dixSozVvBQvBMUwiw==\n"
"-----END CERTIFICATE-----\n";
static const char k_probe_key[] =
"-----BEGIN EC PRIVATE KEY-----\n"
"MHcCAQEEIE4iu5/D66qOZCXyvaAgdfMmwUScRbe+r/4gwMViXvA+oAoGCCqGSM49\n"
"AwEHoUQDQgAELQo1h6CRip6LEvgF8Xhyj2yJmIzumxcOQnmHDTI5idGhG/IeRlke\n"
"SSfYzmCv1ocvxL4lKDjp1xP9qDnvdQhEZg==\n"
"-----END EC PRIVATE KEY-----\n";
