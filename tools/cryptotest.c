/*
 * NimbusOS - host test for net/crypto.c against the published test vectors:
 * FIPS 180-2 (SHA-256), RFC 4231 (HMAC), RFC 5869 (HKDF), RFC 8448 (TLS 1.3
 * key schedule), RFC 7748 (X25519), RFC 8439 (ChaCha20-Poly1305), FIPS 197
 * (AES-128) and the GCM specification (AES-128-GCM).
 */
#include <stdio.h>
#include <string.h>
#include "net/crypto.h"

static int fails, checks;

static void hex(const char *h, u8 *out, size_t *n)
{
    size_t i = 0;
    for (; h[0] && h[1]; h += 2) {
        unsigned v;
        sscanf(h, "%2x", &v);
        out[i++] = (u8)v;
    }
    if (n) *n = i;
}

static void check(const char *name, const u8 *got, const char *want_hex)
{
    u8 want[512];
    size_t n;
    hex(want_hex, want, &n);
    checks++;
    if (memcmp(got, want, n)) {
        fails++;
        printf("FAIL %s\n  got  ", name);
        for (size_t i = 0; i < n; i++) printf("%02x", got[i]);
        printf("\n  want %s\n", want_hex);
    } else {
        printf("ok   %s\n", name);
    }
}

int main(void)
{
    u8 out[256], a[256], b[256], c[256];
    size_t al, bl, cl;

    /* SHA-256 */
    sha256("abc", 3, out);
    check("sha256 abc", out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    sha256("", 0, out);
    check("sha256 empty", out, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    {
        const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        sha256(m, strlen(m), out);
        check("sha256 448-bit", out, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
        struct sha256 s;
        sha256_init(&s);
        static u8 thousand_a[1000];
        memset(thousand_a, 'a', sizeof thousand_a);
        for (int i = 0; i < 1000; i++) sha256_update(&s, thousand_a, sizeof thousand_a);
        sha256_final(&s, out);
        check("sha256 one million 'a'", out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    /* HMAC-SHA256, RFC 4231 test case 1 and 2 */
    memset(a, 0x0b, 20);
    hmac_sha256(a, 20, "Hi There", 8, out);
    check("hmac rfc4231 #1", out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
    hmac_sha256((const u8 *)"Jefe", 4, "what do ya want for nothing?", 28, out);
    check("hmac rfc4231 #2", out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

    /* HKDF, RFC 5869 test case 1 */
    memset(a, 0x0b, 22);
    hex("000102030405060708090a0b0c", b, &bl);
    hex("f0f1f2f3f4f5f6f7f8f9", c, &cl);
    u8 prk[32];
    hkdf_extract(b, bl, a, 22, prk);
    check("hkdf extract", prk, "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5");
    hkdf_expand(prk, c, cl, out, 42);
    check("hkdf expand", out, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865");

    /* TLS 1.3 key schedule start, RFC 8448 "Simple 1-RTT Handshake" */
    u8 zeros[32] = { 0 }, early[32], empty_hash[32];
    hkdf_extract(NULL, 0, zeros, 32, early);
    check("tls13 early secret", early, "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a");
    sha256("", 0, empty_hash);
    hkdf_expand_label(early, "derived", empty_hash, 32, out, 32);
    check("tls13 derived secret", out, "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba");

    /* X25519, RFC 7748 section 6.1 */
    hex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, &al);
    hex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b, &bl);
    u8 apub[32], bpub[32], s1[32], s2[32];
    x25519_base(apub, a);
    check("x25519 alice public", apub, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    x25519_base(bpub, b);
    check("x25519 bob public", bpub, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f");
    x25519(s1, a, bpub);
    x25519(s2, b, apub);
    check("x25519 shared (alice)", s1, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    check("x25519 shared (bob)", s2, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
    /* RFC 7748 section 5.2, first test vector */
    hex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", a, &al);
    hex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", b, &bl);
    x25519(out, a, b);
    check("x25519 rfc7748 5.2", out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");

    /* ChaCha20-Poly1305, RFC 8439 section 2.8.2 */
    {
        const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
        size_t n = strlen(pt);
        u8 key[32], nonce[12], ad[12], ct[200], back[200];
        size_t kl, nl, adl;
        hex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key, &kl);
        hex("070000004041424344454647", nonce, &nl);
        hex("50515253c0c1c2c3c4c5c6c7", ad, &adl);
        chacha20_poly1305_seal(key, nonce, ad, adl, (const u8 *)pt, n, ct);
        check("chacha20-poly1305 ciphertext", ct, "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116");
        check("chacha20-poly1305 tag", ct + n, "1ae10b594f09e26a7e902ecbd0600691");
        checks++;
        if (chacha20_poly1305_open(key, nonce, ad, adl, ct, n + 16, back) || memcmp(back, pt, n)) { fails++; printf("FAIL chacha20-poly1305 open\n"); }
        else printf("ok   chacha20-poly1305 open\n");
        ct[5] ^= 1;
        checks++;
        if (!chacha20_poly1305_open(key, nonce, ad, adl, ct, n + 16, back)) { fails++; printf("FAIL chacha20-poly1305 tamper detection\n"); }
        else printf("ok   chacha20-poly1305 rejects tampered data\n");
    }

    /* AES-128, FIPS 197 appendix C.1 */
    {
        u8 key[16], pt[16];
        hex("000102030405060708090a0b0c0d0e0f", key, NULL);
        hex("00112233445566778899aabbccddeeff", pt, NULL);
        struct aes128 ctx;
        aes128_init(&ctx, key);
        aes128_encrypt(&ctx, pt, out);
        check("aes128 fips197", out, "69c4e0d86a7b0430d8cdb78070b4c55a");
    }

    /* AES-128-GCM, GCM spec test cases 2 and 4 */
    {
        u8 key[16] = { 0 }, nonce[12] = { 0 }, pt[64] = { 0 }, ct[128], back[128];
        aes128_gcm_seal(key, nonce, NULL, 0, pt, 16, ct);
        check("aes128-gcm #2 ciphertext", ct, "0388dace60b6a392f328c2b971b2fe78");
        check("aes128-gcm #2 tag", ct + 16, "ab6e47d42cec13bdf53a67b21257bddf");
        size_t pl, adl;
        u8 ad[32];
        hex("feffe9928665731c6d6a8f9467308308", key, NULL);
        hex("cafebabefacedbaddecaf888", nonce, NULL);
        hex("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b39", pt, &pl);
        hex("feedfacedeadbeeffeedfacedeadbeefabaddad2", ad, &adl);
        aes128_gcm_seal(key, nonce, ad, adl, pt, pl, ct);
        check("aes128-gcm #4 ciphertext", ct, "42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091");
        check("aes128-gcm #4 tag", ct + pl, "5bc94fbc3221a5db94fae95ae7121a47");
        checks++;
        if (aes128_gcm_open(key, nonce, ad, adl, ct, pl + 16, back) || memcmp(back, pt, pl)) { fails++; printf("FAIL aes128-gcm open\n"); }
        else printf("ok   aes128-gcm open\n");
    }

    /* SHA-512, FIPS 180-2 examples */
    sha512("abc", 3, out);
    check("sha512 abc", out, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    sha512("", 0, out);
    check("sha512 empty", out, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e");
    {
        const char *m = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
        sha512(m, strlen(m), out);
        check("sha512 896-bit", out, "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909");
    }

    /* Ed25519, RFC 8032 section 7.1 tests 1, 2 and 3 */
    {
        static const struct { const char *sk, *pk, *msg, *sig; } t[] = {
            { "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
              "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
              "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b" },
            { "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
              "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
              "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00" },
            { "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
              "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
              "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a" },
        };
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
            u8 sk[32], pk[32], msg[16], sig[64], want_sig[64];
            size_t ml;
            hex(t[i].sk, sk, NULL);
            hex(t[i].msg, msg, &ml);
            hex(t[i].sig, want_sig, NULL);
            char name[64];
            ed25519_public_key(pk, sk);
            snprintf(name, sizeof name, "ed25519 #%u public key", i + 1);
            check(name, pk, t[i].pk);
            ed25519_sign(sig, msg, ml, sk, pk);
            snprintf(name, sizeof name, "ed25519 #%u signature", i + 1);
            check(name, sig, t[i].sig);
            checks++;
            if (ed25519_verify(want_sig, msg, ml, pk)) { fails++; printf("FAIL ed25519 #%u verify\n", i + 1); }
            else printf("ok   ed25519 #%u verify\n", i + 1);
            want_sig[10] ^= 4;
            checks++;
            if (!ed25519_verify(want_sig, msg, ml, pk)) { fails++; printf("FAIL ed25519 #%u accepted a bad signature\n", i + 1); }
            else printf("ok   ed25519 #%u rejects a bad signature\n", i + 1);
        }
    }

    printf("cryptotest: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
