/* ============================================================================
 *  persist_storage.c -- ATA LBA28 PIO + AES-128-CTR encrypted Titan volume
 * ==========================================================================*/
#include "persist_storage.h"
#include "ossystemsafety.h"

/* ---- IO ---- */
static inline void outb(p_u16 port, p_u8 v) {
    __asm__ volatile ("outb %0, %1" :: "a"(v), "Nd"(port));
}
static inline p_u8 inb(p_u16 port) {
    p_u8 r; __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void outw(p_u16 port, p_u16 v) {
    __asm__ volatile ("outw %0, %1" :: "a"(v), "Nd"(port));
}
static inline p_u16 inw(p_u16 port) {
    p_u16 r; __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port)); return r;
}

#define ATA_DATA   0x1F0
#define ATA_FEAT   0x1F1
#define ATA_SCNT   0x1F2
#define ATA_LBA0   0x1F3
#define ATA_LBA1   0x1F4
#define ATA_LBA2   0x1F5
#define ATA_DRIVE  0x1F6
#define ATA_CMD    0x1F7
#define ATA_STATUS 0x1F7

#define ATA_SR_BSY  0x80
#define ATA_SR_DRQ  0x08
#define ATA_SR_ERR  0x01

#define VOL_LBA_START  64u          /* skip low LBAs */
#define VOL_MAX_BYTES  (256u * 512u) /* 128 KiB volume window */
#define VOL_MAGIC      0x54495632u  /* TIV2 */
#define VOL_VERSION    2u

static persist_export_fn g_export = 0;
static persist_import_fn g_import = 0;
static p_bool g_disk = P_FALSE;
static p_bool g_mounted = P_FALSE;
static char g_err[48];

static void set_err(const char *s) {
    p_u32 i = 0;
    if (!s) { g_err[0] = 0; return; }
    while (s[i] && i < sizeof(g_err) - 1) { g_err[i] = s[i]; i++; }
    g_err[i] = 0;
}

const char *persist_last_error(void) { return g_err; }
p_bool persist_is_mounted(void) { return g_mounted; }

static int ata_wait(p_u8 mask, p_u8 val) {
    for (p_u32 i = 0; i < 1000000u; i++) {
        p_u8 st = inb(ATA_STATUS);
        if ((st & mask) == val) return 0;
        if (st & ATA_SR_ERR) return -1;
    }
    return -1;
}

static p_bool ata_present(void) {
    outb(ATA_DRIVE, 0xE0); /* master, LBA */
    for (volatile int i = 0; i < 1000; i++) {}
    outb(ATA_SCNT, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_CMD, 0xEC); /* IDENTIFY */
    p_u8 st = inb(ATA_STATUS);
    if (st == 0 || st == 0xFF) return P_FALSE;
    if (ata_wait(ATA_SR_BSY, 0) != 0) return P_FALSE;
    /* drain identify data */
    for (int i = 0; i < 256; i++) (void)inw(ATA_DATA);
    return P_TRUE;
}

static p_bool ata_read_lba(p_u32 lba, p_u8 *buf /*512*/) {
    if (ata_wait(ATA_SR_BSY, 0) != 0) return P_FALSE;
    outb(ATA_DRIVE, (p_u8)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_SCNT, 1);
    outb(ATA_LBA0, (p_u8)(lba));
    outb(ATA_LBA1, (p_u8)(lba >> 8));
    outb(ATA_LBA2, (p_u8)(lba >> 16));
    outb(ATA_CMD, 0x20); /* READ SECTORS */
    if (ata_wait(ATA_SR_BSY, 0) != 0) return P_FALSE;
    if (ata_wait(ATA_SR_DRQ, ATA_SR_DRQ) != 0) return P_FALSE;
    for (int i = 0; i < 256; i++) {
        p_u16 w = inw(ATA_DATA);
        buf[i * 2] = (p_u8)w;
        buf[i * 2 + 1] = (p_u8)(w >> 8);
    }
    return P_TRUE;
}

static p_bool ata_write_lba(p_u32 lba, const p_u8 *buf /*512*/) {
    if (ata_wait(ATA_SR_BSY, 0) != 0) return P_FALSE;
    outb(ATA_DRIVE, (p_u8)(0xE0 | ((lba >> 24) & 0x0F)));
    outb(ATA_SCNT, 1);
    outb(ATA_LBA0, (p_u8)(lba));
    outb(ATA_LBA1, (p_u8)(lba >> 8));
    outb(ATA_LBA2, (p_u8)(lba >> 16));
    outb(ATA_CMD, 0x30); /* WRITE SECTORS */
    if (ata_wait(ATA_SR_BSY, 0) != 0) return P_FALSE;
    if (ata_wait(ATA_SR_DRQ, ATA_SR_DRQ) != 0) return P_FALSE;
    for (int i = 0; i < 256; i++) {
        p_u16 w = (p_u16)buf[i * 2] | ((p_u16)buf[i * 2 + 1] << 8);
        outw(ATA_DATA, w);
    }
    if (ata_wait(ATA_SR_BSY, 0) != 0) return P_FALSE;
    outb(ATA_CMD, 0xE7); /* FLUSH CACHE */
    ata_wait(ATA_SR_BSY, 0);
    return P_TRUE;
}

/* ---- CRC32 (Ethernet poly) ---- */
static p_u32 crc32(const p_u8 *data, p_u32 len) {
    p_u32 c = 0xFFFFFFFFu;
    for (p_u32 i = 0; i < len; i++) {
        c ^= data[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (-(p_u32)(c & 1)));
    }
    return ~c;
}

/* ---- Tiny AES-128 (encrypt block) ---- */
static const p_u8 sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};
static const p_u8 rcon[11] = {0,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36};

static p_u8 xtime(p_u8 x) {
    return (p_u8)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static void aes_key_expand(const p_u8 key[16], p_u8 rk[176]) {
    for (int i = 0; i < 16; i++) rk[i] = key[i];
    for (int i = 4; i < 44; i++) {
        p_u8 t[4];
        t[0]=rk[4*(i-1)]; t[1]=rk[4*(i-1)+1]; t[2]=rk[4*(i-1)+2]; t[3]=rk[4*(i-1)+3];
        if (i % 4 == 0) {
            p_u8 tmp=t[0]; t[0]=sbox[t[1]]^rcon[i/4]; t[1]=sbox[t[2]]; t[2]=sbox[t[3]]; t[3]=sbox[tmp];
        }
        rk[4*i]=rk[4*(i-4)]^t[0]; rk[4*i+1]=rk[4*(i-4)+1]^t[1];
        rk[4*i+2]=rk[4*(i-4)+2]^t[2]; rk[4*i+3]=rk[4*(i-4)+3]^t[3];
    }
}

static void aes_encrypt_block(const p_u8 rk[176], const p_u8 in[16], p_u8 out[16]) {
    p_u8 s[16];
    for (int i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
    for (int round = 1; round <= 10; round++) {
        /* sub + shift */
        p_u8 t[16];
        t[0]=sbox[s[0]]; t[1]=sbox[s[5]]; t[2]=sbox[s[10]]; t[3]=sbox[s[15]];
        t[4]=sbox[s[4]]; t[5]=sbox[s[9]]; t[6]=sbox[s[14]]; t[7]=sbox[s[3]];
        t[8]=sbox[s[8]]; t[9]=sbox[s[13]]; t[10]=sbox[s[2]]; t[11]=sbox[s[7]];
        t[12]=sbox[s[12]]; t[13]=sbox[s[1]]; t[14]=sbox[s[6]]; t[15]=sbox[s[11]];
        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                p_u8 a=t[4*c], b=t[4*c+1], c1=t[4*c+2], d=t[4*c+3];
                t[4*c]   = (p_u8)(xtime(a)^xtime(b)^b^c1^d);
                t[4*c+1] = (p_u8)(a^xtime(b)^xtime(c1)^c1^d);
                t[4*c+2] = (p_u8)(a^b^xtime(c1)^xtime(d)^d);
                t[4*c+3] = (p_u8)(xtime(a)^a^b^c1^xtime(d));
            }
        }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[round * 16 + i];
    }
    for (int i = 0; i < 16; i++) out[i] = s[i];
}

static void aes_ctr_xor(const p_u8 key[16], const p_u8 iv[16], p_u8 *data, p_u32 len) {
    p_u8 rk[176];
    aes_key_expand(key, rk);
    p_u8 ctr[16], ks[16];
    for (int i = 0; i < 16; i++) ctr[i] = iv[i];
    p_u32 off = 0;
    while (off < len) {
        aes_encrypt_block(rk, ctr, ks);
        p_u32 n = len - off;
        if (n > 16) n = 16;
        for (p_u32 i = 0; i < n; i++) data[off + i] ^= ks[i];
        /* increment CTR (big-endian counter in last 8 bytes) */
        for (int i = 15; i >= 8; i--) { ctr[i]++; if (ctr[i]) break; }
        off += n;
    }
    safety_wipe(rk, sizeof(rk));
    safety_wipe(ks, sizeof(ks));
    safety_wipe(ctr, sizeof(ctr));
}

/* password -> key (iterated mix; not full Argon2, but offline + AES still helps) */
/* Strong KDF: many rounds of AES-based mixing (PBKDF-style).
 * Not Argon2, but far harder than a single hash for offline brute force. */
#define KDF_ROUNDS  65536u

static void derive_key(const char *password, const p_u8 salt[16], p_u8 out_key[16]) {
    p_u8 block[16];
    p_u8 rk[176];
    p_u8 pwblock[16];
    p_u32 plen = 0;
    while (password[plen]) plen++;
    for (int i = 0; i < 16; i++) {
        pwblock[i] = salt[i];
        if (plen) pwblock[i] ^= (p_u8)password[i % plen];
        block[i] = (p_u8)(salt[i] ^ (p_u8)(0x5A + i));
    }
    /* Expand password into initial AES key material */
    p_u8 seed[16];
    for (int i = 0; i < 16; i++) seed[i] = pwblock[i];
    aes_key_expand(seed, rk);
    for (p_u32 r = 0; r < KDF_ROUNDS; r++) {
        /* mix counter into block */
        block[0] ^= (p_u8)r;
        block[1] ^= (p_u8)(r >> 8);
        block[2] ^= (p_u8)(r >> 16);
        block[3] ^= (p_u8)(r >> 24);
        p_u8 out[16];
        aes_encrypt_block(rk, block, out);
        for (int i = 0; i < 16; i++) block[i] = out[i] ^ pwblock[i];
        /* periodically re-key from block */
        if ((r & 0xFFFu) == 0xFFFu) {
            aes_key_expand(block, rk);
        }
    }
    for (int i = 0; i < 16; i++) out_key[i] = block[i];
    safety_wipe(block, sizeof(block));
    safety_wipe(rk, sizeof(rk));
    safety_wipe(pwblock, sizeof(pwblock));
    safety_wipe(seed, sizeof(seed));
}

/* Auth tag: AES encrypt of (crc||len||magic) under data key — detects tampering */
static void compute_auth_tag(const p_u8 key[16], p_u32 crc, p_u32 len, p_u8 tag[16]) {
    p_u8 rk[176], plain[16], out[16];
    aes_key_expand(key, rk);
    for (int i = 0; i < 16; i++) plain[i] = 0;
    plain[0] = (p_u8)crc; plain[1] = (p_u8)(crc >> 8);
    plain[2] = (p_u8)(crc >> 16); plain[3] = (p_u8)(crc >> 24);
    plain[4] = (p_u8)len; plain[5] = (p_u8)(len >> 8);
    plain[6] = (p_u8)(len >> 16); plain[7] = (p_u8)(len >> 24);
    plain[8] = 0x54; plain[9] = 0x49; plain[10] = 0x56; plain[11] = 0x32; /* TIV2 */
    aes_encrypt_block(rk, plain, out);
    for (int i = 0; i < 16; i++) tag[i] = out[i];
    safety_wipe(rk, sizeof(rk));
    safety_wipe(plain, sizeof(plain));
    safety_wipe(out, sizeof(out));
}

typedef struct {
    p_u32 magic;
    p_u32 version;
    p_u32 payload_len;
    p_u32 crc_plain;
    p_u8  salt[16];
    p_u8  iv[16];
    p_u8  auth_tag[16]; /* AES auth over crc||len — anti-tamper */
} vol_hdr_t;

static p_u8 vol_buf[VOL_MAX_BYTES];

void persist_init(persist_export_fn exp, persist_import_fn imp) {
    g_export = exp;
    g_import = imp;
    g_mounted = P_FALSE;
    set_err("");
    g_disk = ata_present();
    if (!g_disk) set_err("no ATA disk");
}

p_bool persist_disk_present(void) { return g_disk; }

static p_bool write_volume_raw(const p_u8 *data, p_u32 len) {
    if (len > VOL_MAX_BYTES) return P_FALSE;
    p_u32 sectors = (len + 511u) / 512u;
    p_u8 sec[512];
    for (p_u32 s = 0; s < sectors; s++) {
        safety_memset(sec, 0, 512, 512);
        p_u32 off = s * 512u;
        p_u32 n = len - off;
        if (n > 512) n = 512;
        if (n) safety_memcpy(sec, 512, data + off, n);
        if (!ata_write_lba(VOL_LBA_START + s, sec)) return P_FALSE;
    }
    return P_TRUE;
}

static p_bool read_volume_raw(p_u8 *data, p_u32 len) {
    if (len > VOL_MAX_BYTES) return P_FALSE;
    p_u32 sectors = (len + 511u) / 512u;
    p_u8 sec[512];
    for (p_u32 s = 0; s < sectors; s++) {
        if (!ata_read_lba(VOL_LBA_START + s, sec)) return P_FALSE;
        p_u32 off = s * 512u;
        p_u32 n = len - off;
        if (n > 512) n = 512;
        if (n) safety_memcpy(data + off, VOL_MAX_BYTES - off, sec, n);
    }
    return P_TRUE;
}

p_bool persist_format(const char *password) {
    if (!g_disk || !password || !g_export) { set_err("format precheck"); return P_FALSE; }
    p_u32 plen = g_export(vol_buf + sizeof(vol_hdr_t), VOL_MAX_BYTES - sizeof(vol_hdr_t));
    if (plen == 0) { set_err("export empty"); return P_FALSE; }

    vol_hdr_t hdr;
    hdr.magic = VOL_MAGIC;
    hdr.version = VOL_VERSION;
    hdr.payload_len = plen;
    hdr.crc_plain = crc32(vol_buf + sizeof(vol_hdr_t), plen);
    /* salt/iv from CRC mix + size (not CSPRNG; offline device) */
    p_u32 seed = hdr.crc_plain ^ plen ^ 0xC0FFEEu;
    for (int i = 0; i < 16; i++) {
        seed = seed * 1664525u + 1013904223u;
        hdr.salt[i] = (p_u8)(seed >> 16);
        seed = seed * 1664525u + 1013904223u;
        hdr.iv[i] = (p_u8)(seed >> 8);
    }

    p_u8 key[16];
    derive_key(password, hdr.salt, key);
    compute_auth_tag(key, hdr.crc_plain, plen, hdr.auth_tag);
    aes_ctr_xor(key, hdr.iv, vol_buf + sizeof(vol_hdr_t), plen);
    safety_wipe(key, sizeof(key));

    safety_memcpy(vol_buf, VOL_MAX_BYTES, &hdr, sizeof(hdr));
    if (!write_volume_raw(vol_buf, sizeof(hdr) + plen)) {
        set_err("disk write failed");
        return P_FALSE;
    }
    g_mounted = P_TRUE;
    set_err("ok");
    return P_TRUE;
}

p_bool persist_save(const char *password) {
    return persist_format(password); /* same path: export+encrypt+write */
}

p_bool persist_load(const char *password) {
    if (!g_disk || !password || !g_import) { set_err("load precheck"); return P_FALSE; }
    if (!read_volume_raw(vol_buf, sizeof(vol_hdr_t))) { set_err("disk read failed"); return P_FALSE; }

    vol_hdr_t hdr;
    safety_memcpy(&hdr, sizeof(hdr), vol_buf, sizeof(hdr));
    if (hdr.magic != VOL_MAGIC || hdr.version != VOL_VERSION) {
        set_err("no titan volume");
        return P_FALSE;
    }
    if (hdr.payload_len == 0 || hdr.payload_len > VOL_MAX_BYTES - sizeof(vol_hdr_t)) {
        set_err("bad size");
        return P_FALSE;
    }
    if (!read_volume_raw(vol_buf, sizeof(hdr) + hdr.payload_len)) {
        set_err("payload read fail");
        return P_FALSE;
    }
    safety_memcpy(&hdr, sizeof(hdr), vol_buf, sizeof(hdr));

    p_u8 key[16];
    derive_key(password, hdr.salt, key);
    {
        p_u8 expect[16];
        compute_auth_tag(key, hdr.crc_plain, hdr.payload_len, expect);
        p_bool ok = P_TRUE;
        for (int i = 0; i < 16; i++) if (expect[i] != hdr.auth_tag[i]) ok = P_FALSE;
        safety_wipe(expect, sizeof(expect));
        if (!ok) {
            safety_wipe(key, sizeof(key));
            safety_wipe(vol_buf, sizeof(vol_buf));
            set_err("auth tag fail (bad key/tamper)");
            return P_FALSE;
        }
    }
    aes_ctr_xor(key, hdr.iv, vol_buf + sizeof(vol_hdr_t), hdr.payload_len);
    safety_wipe(key, sizeof(key));

    p_u32 got = crc32(vol_buf + sizeof(vol_hdr_t), hdr.payload_len);
    if (got != hdr.crc_plain) {
        safety_wipe(vol_buf, sizeof(vol_buf));
        set_err("crc fail (bad key/corrupt)");
        return P_FALSE;
    }
    if (!g_import(vol_buf + sizeof(vol_hdr_t), hdr.payload_len)) {
        set_err("import failed");
        return P_FALSE;
    }
    safety_wipe(vol_buf, sizeof(vol_buf));
    g_mounted = P_TRUE;
    set_err("ok");
    return P_TRUE;
}


/* ========== Persistent login credentials + settings (LBA 32) ========== */
#define CRED_LBA    32u
#define CRED_MAGIC  0x43524544u /* CRED */

typedef struct {
    p_u32 magic;
    p_u32 version;
    p_u8  salt[16];
    p_u8  pass_hash[16];
    p_u32 wallpaper;
    p_u32 theme;
    p_u32 security;
    p_u32 idle_sec;
    p_u32 crc;
} cred_t;

static p_u32 cred_crc(const cred_t *c) {
    return crc32((const p_u8*)c, (p_u32)((p_u8*)&c->crc - (p_u8*)c));
}

p_bool persist_cred_exists(void) {
    if (!g_disk) return P_FALSE;
    p_u8 sec[512];
    if (!ata_read_lba(CRED_LBA, sec)) return P_FALSE;
    cred_t c;
    safety_memcpy(&c, sizeof(c), sec, sizeof(c) < 512 ? sizeof(c) : 512);
    return c.magic == CRED_MAGIC && c.version == 1 && c.crc == cred_crc(&c);
}

p_bool persist_cred_save(const char *password, p_u32 wallpaper, p_u32 theme,
                         p_u32 security, p_u32 idle_sec) {
    if (!g_disk || !password) return P_FALSE;
    cred_t c;
    safety_wipe(&c, sizeof(c));
    c.magic = CRED_MAGIC;
    c.version = 1;
    {
        volatile p_u32 t = 0xC0FFEEu;
        for (int i = 0; i < 16; i++) {
            t = t * 1103515245u + 12345u + (p_u32)(p_u8)password[i % (password[0] ? 8 : 1)];
            c.salt[i] = (p_u8)(t >> 8);
        }
    }
    derive_key(password, c.salt, c.pass_hash);
    c.wallpaper = wallpaper;
    c.theme = theme;
    c.security = security;
    c.idle_sec = idle_sec ? idle_sec : 120;
    c.crc = cred_crc(&c);
    p_u8 sec[512];
    safety_wipe(sec, 512);
    safety_memcpy(sec, 512, &c, sizeof(c) < 512 ? sizeof(c) : 512);
    return ata_write_lba(CRED_LBA, sec);
}

p_bool persist_cred_load(p_u32 *wallpaper, p_u32 *theme, p_u32 *security, p_u32 *idle_sec) {
    if (!g_disk) return P_FALSE;
    p_u8 sec[512];
    if (!ata_read_lba(CRED_LBA, sec)) return P_FALSE;
    cred_t c;
    safety_memcpy(&c, sizeof(c), sec, sizeof(c) < 512 ? sizeof(c) : 512);
    if (c.magic != CRED_MAGIC || c.version != 1 || c.crc != cred_crc(&c))
        return P_FALSE;
    if (wallpaper) *wallpaper = c.wallpaper;
    if (theme) *theme = c.theme;
    if (security) *security = c.security;
    if (idle_sec) *idle_sec = c.idle_sec ? c.idle_sec : 120;
    return P_TRUE;
}

p_bool persist_cred_verify(const char *password) {
    if (!g_disk || !password) return P_FALSE;
    p_u8 sec[512];
    if (!ata_read_lba(CRED_LBA, sec)) return P_FALSE;
    cred_t c;
    safety_memcpy(&c, sizeof(c), sec, sizeof(c) < 512 ? sizeof(c) : 512);
    if (c.magic != CRED_MAGIC || c.version != 1 || c.crc != cred_crc(&c))
        return P_FALSE;
    p_u8 got[16];
    derive_key(password, c.salt, got);
    p_bool ok = P_TRUE;
    for (int i = 0; i < 16; i++) if (got[i] != c.pass_hash[i]) ok = P_FALSE;
    safety_wipe(got, sizeof(got));
    return ok;
}
