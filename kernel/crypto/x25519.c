#include <kernel/crypto.h>
#include <kernel/mem.h>

/* TweetNaCl Curve25519 Implementation (Public Domain) */
typedef s64 gf[16];

static void car25519(gf o) {
    for (int i = 0; i < 16; i++) {
        o[i] += (1LL << 16);
        s64 c = o[i] >> 16;
        o[(i + 1) * (i < 15)] += c - 1 + 37 * (c - 1) * (i == 15);
        o[i] -= c << 16;
    }
}

static void sel25519(gf p, gf q, int b) {
    s64 c = ~(s64)(b - 1);
    for (int i = 0; i < 16; i++) {
        s64 t = c & (p[i] ^ q[i]);
        p[i] ^= t;
        q[i] ^= t;
    }
}

static void pack25519(u8 *o, const gf n) {
    gf m, t;
    for (int i = 0; i < 16; i++) t[i] = n[i];
    car25519(t);
    car25519(t);
    car25519(t);
    for (int j = 0; j < 2; j++) {
        m[0] = t[0] - 0xffed;
        for (int i = 1; i < 15; i++) {
            m[i] = t[i] - 0xffff - ((m[i - 1] >> 16) & 1);
            m[i - 1] &= 0xffff;
        }
        m[15] = t[15] - 0x7fff - ((m[14] >> 16) & 1);
        s64 b = ((m[15] >> 16) & 1);
        m[14] &= 0xffff;
        sel25519(t, m, 1 - (int)b);
    }
    for (int i = 0; i < 16; i++) {
        o[2 * i] = (u8)(t[i] & 0xff);
        o[2 * i + 1] = (u8)((t[i] >> 8) & 0xff);
    }
}

static void unpack25519(gf o, const u8 *n) {
    for (int i = 0; i < 16; i++) {
        o[i] = (s64)n[2 * i] + ((s64)n[2 * i + 1] << 8);
    }
    o[15] &= 0x7fff;
}

static void fe_add(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] + b[i];
}

static void fe_sub(gf o, const gf a, const gf b) {
    for (int i = 0; i < 16; i++) o[i] = a[i] - b[i];
}

static void fe_mul(gf o, const gf a, const gf b) {
    s64 t[31] = {0};
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < 16; j++) {
            t[i + j] += a[i] * b[j];
        }
    }
    for (int i = 0; i < 15; i++) {
        t[i] += 38 * t[i + 16];
    }
    for (int i = 0; i < 16; i++) o[i] = t[i];
    car25519(o);
    car25519(o);
}

static void fe_sq(gf o, const gf a) {
    fe_mul(o, a, a);
}

static void fe_inv(gf o, const gf i) {
    gf c;
    for (int a = 0; a < 16; a++) c[a] = i[a];
    for (int a = 253; a >= 0; a--) {
        fe_sq(c, c);
        if (a != 2 && a != 4) fe_mul(c, c, i);
    }
    for (int a = 0; a < 16; a++) o[a] = c[a];
}

static const gf _121665 = {0xDB41, 1};

bool x25519_scalarmult(u8 q[X25519_KEY_SIZE],
                       const u8 n[X25519_KEY_SIZE],
                       const u8 p[X25519_KEY_SIZE]) {
    u8 z[32];
    for (int i = 0; i < 31; i++) z[i] = n[i];
    z[31] = (n[31] & 127) | 64;
    z[0] &= 248;

    s64 x_buf[80];
    gf a, b, c, d, e, f;
    unpack25519(x_buf, p);
    for (int i = 0; i < 16; i++) {
        b[i] = x_buf[i];
        d[i] = 0;
        a[i] = 0;
        c[i] = 0;
    }
    a[0] = 1;
    d[0] = 1;

    for (int i = 254; i >= 0; i--) {
        int r = (z[i >> 3] >> (i & 7)) & 1;
        sel25519(a, b, r);
        sel25519(c, d, r);
        fe_add(e, a, c);
        fe_sub(a, a, c);
        fe_add(c, b, d);
        fe_sub(b, b, d);
        fe_sq(d, e);
        fe_sq(f, a);
        fe_mul(a, c, a);
        fe_mul(c, b, e);
        fe_add(e, a, c);
        fe_sub(a, a, c);
        fe_sq(b, a);
        fe_sub(c, d, f);
        fe_mul(a, c, _121665);
        fe_add(a, a, d);
        fe_mul(c, c, a);
        fe_mul(a, d, f);
        fe_mul(d, b, x_buf);
        fe_sq(b, e);
        sel25519(a, b, r);
        sel25519(c, d, r);
    }

    for (int i = 0; i < 16; i++) {
        x_buf[i + 16] = a[i];
        x_buf[i + 32] = c[i];
        x_buf[i + 48] = b[i];
        x_buf[i + 64] = d[i];
    }
    fe_inv(x_buf + 32, x_buf + 32);
    fe_mul(x_buf + 16, x_buf + 16, x_buf + 32);
    pack25519(q, x_buf + 16);

    u8 zero_check = 0;
    for (int i = 0; i < 32; i++) zero_check |= q[i];
    return (zero_check != 0);
}

void x25519_keypair(u8 public_key[X25519_KEY_SIZE], u8 private_key[X25519_KEY_SIZE]) {
    static const u8 basepoint[32] = {9};
    crypto_get_random(private_key, 32);
    x25519_scalarmult(public_key, private_key, basepoint);
}
