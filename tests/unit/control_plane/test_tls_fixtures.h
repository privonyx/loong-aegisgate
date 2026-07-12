#pragma once

// Self-signed RSA-2048 certificate for Phase 9.3 gRPC control-plane tests.
//
// ---- Generation command (kept here for reproducibility) ----
//   openssl req -x509 -newkey rsa:2048 -nodes
//       -keyout srv.key -out srv.crt -days 3650
//       -subj "/CN=test-ci-only.invalid/O=AegisGate-Test"
//
// This key pair is test-only: the private key is shipped in plaintext in
// the source tree, the certificate's CN is `test-ci-only.invalid`, and the
// material must never be used in any deployed environment. It exists so
// ServerBootstrap + mTLS tests can run hermetically without shelling out
// to `openssl` at test time and without polluting the filesystem.
//
// Validity: 2026-04-21 .. 2036-04-18 (10 years). Refresh before then by
// rerunning the generation command above and copy-pasting both blocks.

#include <string>

namespace aegisgate::control_plane::test_fixtures {

inline const std::string& serverCertPem() {
    static const std::string kPem =
        "-----BEGIN CERTIFICATE-----\n"
        "MIIDUTCCAjmgAwIBAgIUYMuSnoscI3pcIFLvbs5ggiz2Dd4wDQYJKoZIhvcNAQEL\n"
        "BQAwODEdMBsGA1UEAwwUdGVzdC1jaS1vbmx5LmludmFsaWQxFzAVBgNVBAoMDkFl\n"
        "Z2lzR2F0ZS1UZXN0MB4XDTI2MDQyMTE0NDg0MloXDTM2MDQxODE0NDg0MlowODEd\n"
        "MBsGA1UEAwwUdGVzdC1jaS1vbmx5LmludmFsaWQxFzAVBgNVBAoMDkFlZ2lzR2F0\n"
        "ZS1UZXN0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAmu/N1Ej8yjR0\n"
        "yjJha2r3r4nLsF2igyXMxaau/d5RGujncWyCo2BTYAP1oUJJjh6lhCy6UelRRYL9\n"
        "tKD/8PHdK5xCzijESxBlerOfh2sjk+PNvIZwin9cGqkqxwk3mAOrR20uM/E0xNIO\n"
        "NFf1pqVHbjOajMOGE8bQ4P9NuGCPPjrwmtLW5uXWAAxnPRjsXNHo+u1wovvjnu0P\n"
        "UfJ3niQe9uWaJUk0KJu3HpTyBSuBOmlYbdaQuc6zJ0MSgJtCGyf5znmG6tuw2XzB\n"
        "FtTc9lwWZwidJcI3j4MltdqWviyiILdxrD6US7Bv+dNWZafb1SUb1UfoFXXW2wrv\n"
        "8B7lZ3dnFwIDAQABo1MwUTAdBgNVHQ4EFgQUCrCqZ6UHlYz09kRdme48yeIhW7Mw\n"
        "HwYDVR0jBBgwFoAUCrCqZ6UHlYz09kRdme48yeIhW7MwDwYDVR0TAQH/BAUwAwEB\n"
        "/zANBgkqhkiG9w0BAQsFAAOCAQEAkE0CrZJjLYHkmEsgDBnlnRs6nqwMVcfN+brz\n"
        "HP2gxsGIxKdd07KP2el4zSswz/eMqqtV39HnNm4js9f+kAwnK9wr5V0BPv6yyTvE\n"
        "VHUfDga+LTgxxtsRE145J93zZBMModJLaMF4whux3NG0BKAYaDGJQMM/SC9Nx9++\n"
        "Lx2Kud1TzjKQaNrQJGrYu4bVM02uz7XIW8mFfaSQikFDT9fJ6Ix6A4TNsSSFpaOM\n"
        "wQCZtO8tfjR0sTP2kPhUR34VA5c/4e2SBp3K6spgJd4YNveZInujPvOfA5m2aLee\n"
        "9eGwZ6neKXZo4MOYKVr+F5XiNySJueRK1UwPvY/XUgB9zgoGkw==\n"
        "-----END CERTIFICATE-----\n";
    return kPem;
}

inline const std::string& serverKeyPem() {
    static const std::string kPem =
        "-----BEGIN PRIVATE KEY-----\n"
        "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQCa783USPzKNHTK\n"
        "MmFravevicuwXaKDJczFpq793lEa6OdxbIKjYFNgA/WhQkmOHqWELLpR6VFFgv20\n"
        "oP/w8d0rnELOKMRLEGV6s5+HayOT4828hnCKf1waqSrHCTeYA6tHbS4z8TTE0g40\n"
        "V/WmpUduM5qMw4YTxtDg/024YI8+OvCa0tbm5dYADGc9GOxc0ej67XCi++Oe7Q9R\n"
        "8neeJB725ZolSTQom7celPIFK4E6aVht1pC5zrMnQxKAm0IbJ/nOeYbq27DZfMEW\n"
        "1Nz2XBZnCJ0lwjePgyW12pa+LKIgt3GsPpRLsG/501Zlp9vVJRvVR+gVddbbCu/w\n"
        "HuVnd2cXAgMBAAECggEASp6e/5QtZ4dvDijIgY8RflH7r1PRXp94aWL3WZ5WaoLs\n"
        "YNS1cFEGprIRfVprCY8aATf3flbEBRnq7bEyww0Bi6zlAdxheZOKhFd5SCOiDWqE\n"
        "Hj2TeyiOp+p8h0KZq1VtCuf6/ho342kVXUFVtq5YYitpTk0myGSTGiHrgRbENmA1\n"
        "goC2psHw6s+UX7DAL28ZZqvAP86/4dxNmJSAHUF1wMrrJwqSLVh+ckMWxXGYEJYp\n"
        "mAUfn+J+8+/XV5xX/7pYq2hTU+HmxMLK3kMzLXosAjoi5MesyjcPLOXI5nQTHwvT\n"
        "FrJQFKyDfmPKCLMnhRVzHKOp/z5bYHS8SSdAIjoJhQKBgQC8/wrN3X1YgF/mXxQ+\n"
        "6U4k8STxfUbMyQ5IeEB6kphJ47a8bD3Rx+AWIEve0AtbygPsvC/emaeAzi3MBZnz\n"
        "8stGza6Fpjt/Zp7lK5Ex1BYZzpKWxRYCm92QlKORJW1ptvDDAiDWxrxE07X0Z+DD\n"
        "WXS5Xin72qQRRSDR9seGEIRhnQKBgQDR3ZTGXAW3rq/pk9EvI4Z78t6SBjnYuSpJ\n"
        "gGBJ2L+r3vDq4v+8U1ITIMzKnO9Gioflw//sfFAyfpixlwSJWCzwyrpAIs590jgE\n"
        "DMsLJ3xWD4tTlmWfnI1O7KiJL4tLpHxBAzuJWhN+mI693mFCwxAVn9q3ii7jl6hL\n"
        "FAArKgPXQwKBgBe7Gnsw9X04WVJO2/buSo6e7NmZtlseX7m/x7DcWVzlx9su6DBA\n"
        "HaYJlh07GEIFQqrmEkisGHk26k804NjdwqJ4TxKfBdeAZjE2YDvWepPw+T0PMC1R\n"
        "rudpkoQ2I9/jRaXmzYLKX3dw8ebnaDR/NgXUigcCtBkrCezzRKhTyJuNAoGAUeqo\n"
        "wMh4ntr75TOCimDWhJknUV5GxBZ6sBgA/bIyFrc92KFkazEmrzq1VTzulN1L8F6S\n"
        "Dc/0SdFqbp9g8O9PE2o+SvyLF0ev/7yyoJb4DGui2ayx3Bxyd+UeX/YD7DG3InVN\n"
        "ju5u/5Iy3V/pHPMg2x/7cnrolIPE+BFFg5OxzhsCgYEAjv73Q9RWe9UYuxYkp70c\n"
        "dCBo9w426ySsozmRQJ5gFoUyvwAqnzd/KKDXWL7gRc4cHXWhkQGzMKpnb0aDqt0F\n"
        "PAlTo6QtA3FD9CQoJGlxybnuts+USygFfJE7OLmER7rHex4kfSUkb+mTD8gILCAo\n"
        "uduNMVLtwjtorATmPZ5fimw=\n"
        "-----END PRIVATE KEY-----\n";
    return kPem;
}

} // namespace aegisgate::control_plane::test_fixtures
