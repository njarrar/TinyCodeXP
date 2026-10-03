/* rsrc.c - adds a .rsrc section (icon + XP visual styles manifest) to a
 * 32-bit PE built by TCC, which cannot compile .rc files itself.
 * It writes the section directly instead of using UpdateResource, which
 * is unreliable on Windows XP.
 *
 *   rsrc <exe> <icon.ico> <app.manifest>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32;

static u8 *load(const char *p, u32 *n)
{
    FILE *f = fopen(p, "rb"); u8 *b;
    if (!f) { fprintf(stderr, "rsrc: cannot open %s\n", p); exit(1); }
    fseek(f, 0, SEEK_END); *n = (u32)ftell(f); fseek(f, 0, SEEK_SET);
    b = (u8 *)malloc(*n + 1);
    if (fread(b, 1, *n, f) != *n) { fprintf(stderr, "rsrc: read error %s\n", p); exit(1); }
    fclose(f);
    return b;
}

static u16 rd16(const u8 *p) { return (u16)(p[0] | p[1] << 8); }
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static void wr16(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static u32 align(u32 v, u32 a) { return (v + a - 1) / a * a; }

/* one leaf: type/id -> data */
typedef struct { u32 type, id; const u8 *data; u32 size; } Res;

static u8 sec[1 << 20];
static u32 slen;

static u32 put(const void *d, u32 n) { u32 at = slen; memcpy(sec + slen, d, n); slen += n; return at; }
static u32 zero(u32 n) { u32 at = slen; memset(sec + slen, 0, n); slen += n; return at; }

static u32 dir(u32 nentries)
{
    u32 at = zero(16 + 8 * nentries);
    wr16(sec + at + 14, nentries);   /* NumberOfIdEntries */
    return at;
}

int main(int argc, char **argv)
{
    u8 *exe, *ico, *man, *pe, *opt, *sh, *last, *out;
    u32 exeN, icoN, manN, i, j, k, nsec, optSize, fileAlign, secAlign, va, raw, nres = 0, ntypes = 0;
    Res res[64];
    u8 grp[6 + 14 * 16];
    u32 types[8], root, tdir[8], leafDataOff[64];
    FILE *f;

    if (argc != 4) { fprintf(stderr, "usage: rsrc <exe> <icon.ico> <app.manifest>\n"); return 1; }
    exe = load(argv[1], &exeN); ico = load(argv[2], &icoN); man = load(argv[3], &manN);

    pe = exe + rd32(exe + 0x3C);
    if (memcmp(pe, "PE\0\0", 4) || rd16(pe + 24) != 0x10B) { fprintf(stderr, "rsrc: not a 32-bit PE\n"); return 1; }
    nsec = rd16(pe + 6); optSize = rd16(pe + 20);
    opt = pe + 24;
    secAlign = rd32(opt + 32); fileAlign = rd32(opt + 36);
    if (rd32(opt + 96 + 2 * 8)) { fprintf(stderr, "rsrc: %s already has resources\n", argv[1]); return 1; }
    sh = opt + optSize;
    if ((u32)(sh + 40 * (nsec + 1) - exe) > rd32(opt + 60)) { fprintf(stderr, "rsrc: no room for a section header\n"); return 1; }
    last = sh + 40 * (nsec - 1);
    va = align(rd32(last + 12) + rd32(last + 8), secAlign);
    raw = align(exeN, fileAlign);

    /* icons: each image is RT_ICON n, plus one RT_GROUP_ICON listing them */
    k = rd16(ico + 4);
    if (k > 16) k = 16;
    memset(grp, 0, sizeof(grp));
    wr16(grp + 2, 1); wr16(grp + 4, k);
    for (i = 0; i < k; i++) {
        const u8 *e = ico + 6 + 16 * i;
        res[nres].type = 3; res[nres].id = i + 1;
        res[nres].size = rd32(e + 8); res[nres].data = ico + rd32(e + 12);
        nres++;
        memcpy(grp + 6 + 14 * i, e, 12);       /* width..bytes in res */
        wr16(grp + 6 + 14 * i + 12, i + 1);    /* id instead of file offset */
    }
    res[nres].type = 14; res[nres].id = 1; res[nres].data = grp; res[nres].size = 6 + 14 * k; nres++;
    res[nres].type = 24; res[nres].id = 1; res[nres].data = man; res[nres].size = manN; nres++;

    for (i = 0; i < nres; i++) {
        for (j = 0; j < ntypes; j++) if (types[j] == res[i].type) break;
        if (j == ntypes) types[ntypes++] = res[i].type;
    }

    /* tree: root -> type dirs -> id dirs -> language dir -> data entry */
    slen = 0;
    root = dir(ntypes);
    for (j = 0; j < ntypes; j++) {
        u32 n = 0;
        for (i = 0; i < nres; i++) if (res[i].type == types[j]) n++;
        tdir[j] = dir(n);
        wr32(sec + root + 16 + 8 * j, types[j]);
        wr32(sec + root + 16 + 8 * j + 4, 0x80000000 | tdir[j]);
    }
    for (j = 0; j < ntypes; j++) {
        u32 n = 0;
        for (i = 0; i < nres; i++) if (res[i].type == types[j]) {
            u32 ld = dir(1), de = zero(16);
            wr32(sec + tdir[j] + 16 + 8 * n, res[i].id);
            wr32(sec + tdir[j] + 16 + 8 * n + 4, 0x80000000 | ld);
            wr32(sec + ld + 16, 0x409);
            wr32(sec + ld + 20, de);
            leafDataOff[i] = de;
            n++;
        }
    }
    for (i = 0; i < nres; i++) {
        u32 at;
        slen = align(slen, 4);
        at = put(res[i].data, res[i].size);
        wr32(sec + leafDataOff[i], va + at);
        wr32(sec + leafDataOff[i] + 4, res[i].size);
    }

    /* new section header */
    sh += 40 * nsec;
    memset(sh, 0, 40);
    memcpy(sh, ".rsrc", 5);
    wr32(sh + 8, slen);                       /* VirtualSize */
    wr32(sh + 12, va);
    wr32(sh + 16, align(slen, fileAlign));    /* SizeOfRawData */
    wr32(sh + 20, raw);
    wr32(sh + 36, 0x40000040);                /* initialized data, readable */
    wr16(pe + 6, nsec + 1);
    wr32(opt + 56, align(va + slen, secAlign));                       /* SizeOfImage */
    wr32(opt + 8, rd32(opt + 8) + align(slen, fileAlign));            /* SizeOfInitializedData */
    wr32(opt + 96 + 2 * 8, va);
    wr32(opt + 96 + 2 * 8 + 4, slen);
    wr32(opt + 64, 0);                                                /* CheckSum */

    out = (u8 *)calloc(1, raw + align(slen, fileAlign));
    memcpy(out, exe, exeN);
    memcpy(out + raw, sec, slen);
    f = fopen(argv[1], "wb");
    if (!f) { fprintf(stderr, "rsrc: cannot write %s\n", argv[1]); return 1; }
    fwrite(out, 1, raw + align(slen, fileAlign), f);
    fclose(f);
    printf("embedded icon (%u images) and manifest into %s\n", k, argv[1]);
    return 0;
}
