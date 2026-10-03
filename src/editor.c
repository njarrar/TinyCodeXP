/* editor.c - the text editor: line buffer, undo/redo, painting with
 * highlighting, indent guides, bracket matching, minimap, find/replace,
 * and reload when the file changes on disk. */
#include "xpcode.h"

#define MINIMAP_W 90
#define SB_W 14
#define TABW 4

static unsigned char *s_cls; static int s_ccap;
static int s_drag, s_mdrag, s_lastX, s_lastY, s_clicks; static DWORD s_lastClick;
static VScroll s_vs;
static HWND s_fFind, s_fRepl;
static int s_fRepMode, s_fCase, s_fWord, s_fCount, s_fIndex;
static WNDPROC s_oldEdit;
static int s_hasCaret;
static int s_lastCtl;

/* ------------------------------------------------------------------ buffer */

static void line_reserve(Line *L, int n)
{
    if (n + 1 > L->cap) {
        L->cap = n + 16 + n / 2;
        L->s = (char *)realloc(L->s, L->cap);
    }
}

static void lines_reserve(Doc *d, int n)
{
    if (n > d->cap) {
        d->cap = n + 64 + n / 2;
        d->ln = (Line *)realloc(d->ln, d->cap * sizeof(Line));
    }
}

static void insert_lines(Doc *d, int at, int count)
{
    int i;
    lines_reserve(d, d->n + count);
    memmove(d->ln + at + count, d->ln + at, (d->n - at) * sizeof(Line));
    for (i = 0; i < count; i++) { memset(&d->ln[at + i], 0, sizeof(Line)); line_reserve(&d->ln[at + i], 0); }
    d->n += count;
}

Doc *doc_new(void)
{
    Doc *d = (Doc *)xalloc(sizeof(Doc));
    insert_lines(d, 0, 1);
    d->crlf = 1; d->savedCu = 0; d->valid = 1;
    return d;
}

static void free_undo(Doc *d, int from)
{
    int i;
    for (i = from; i < d->nu; i++) free(d->u[i].t);
    d->nu = from;
}

void doc_free(Doc *d)
{
    int i;
    for (i = 0; i < d->n; i++) free(d->ln[i].s);
    free(d->ln);
    free_undo(d, 0); free(d->u);
    free(d);
}

int doc_dirty(Doc *d) { return d->cu != d->savedCu; }

static void raw_insert(Doc *d, int l, int c, const char *t, int n, int *el, int *ec)
{
    Line *L = &d->ln[l];
    int i, segs = 0, tail = L->len - c;
    char *tailbuf = 0;
    for (i = 0; i < n; i++) if (t[i] == '\n') segs++;
    if (!segs) {
        line_reserve(L, L->len + n);
        memmove(L->s + c + n, L->s + c, tail);
        memcpy(L->s + c, t, n);
        L->len += n;
        *el = l; *ec = c + n;
    } else {
        int start = 0, li = l;
        if (tail) { tailbuf = (char *)malloc(tail); memcpy(tailbuf, L->s + c, tail); }
        L->len = c;
        insert_lines(d, l + 1, segs);
        for (i = 0; i <= n; i++) {
            if (i == n || t[i] == '\n') {
                Line *M = &d->ln[li];
                int k = i - start;
                line_reserve(M, M->len + k);
                memcpy(M->s + M->len, t + start, k);
                M->len += k;
                if (i < n) li++;
                start = i + 1;
            }
        }
        *el = li; *ec = d->ln[li].len;
        if (tail) {
            Line *M = &d->ln[li];
            line_reserve(M, M->len + tail);
            memcpy(M->s + M->len, tailbuf, tail);
            M->len += tail;
            free(tailbuf);
        }
    }
    if (d->valid > l + 1) d->valid = l + 1;
}

static void raw_delete(Doc *d, int l1, int c1, int l2, int c2)
{
    Line *A = &d->ln[l1], *B = &d->ln[l2];
    int i, rest = B->len - c2;
    if (l1 == l2) {
        memmove(A->s + c1, A->s + c2, A->len - c2);
        A->len -= c2 - c1;
    } else {
        line_reserve(A, c1 + rest);
        memcpy(A->s + c1, B->s + c2, rest);
        A->len = c1 + rest;
        for (i = l1 + 1; i <= l2; i++) free(d->ln[i].s);
        memmove(d->ln + l1 + 1, d->ln + l2 + 1, (d->n - l2 - 1) * sizeof(Line));
        d->n -= l2 - l1;
    }
    if (d->valid > l1 + 1) d->valid = l1 + 1;
}

static char *get_text(Doc *d, int l1, int c1, int l2, int c2, int *len)
{
    int i, n = 0;
    char *b, *p;
    if (l1 == l2) n = c2 - c1;
    else { n = d->ln[l1].len - c1 + 1; for (i = l1 + 1; i < l2; i++) n += d->ln[i].len + 1; n += c2; }
    p = b = (char *)malloc(n + 1);
    if (l1 == l2) { memcpy(p, d->ln[l1].s + c1, n); p += n; }
    else {
        memcpy(p, d->ln[l1].s + c1, d->ln[l1].len - c1); p += d->ln[l1].len - c1; *p++ = '\n';
        for (i = l1 + 1; i < l2; i++) { memcpy(p, d->ln[i].s, d->ln[i].len); p += d->ln[i].len; *p++ = '\n'; }
        memcpy(p, d->ln[l2].s, c2); p += c2;
    }
    *p = 0;
    if (len) *len = n;
    return b;
}

static void end_of(int l, int c, const char *t, int n, int *el, int *ec)
{
    int i;
    *el = l; *ec = c;
    for (i = 0; i < n; i++) { if (t[i] == '\n') { (*el)++; *ec = 0; } else (*ec)++; }
}

/* ------------------------------------------------------------------ undo */

static void begin(Doc *d, int typing)
{
    if (!(typing && d->typing)) d->grp++;
    d->typing = typing;
}

static void push_undo(Doc *d, int ins, int l, int c, const char *t, int n)
{
    Undo *e;
    free_undo(d, d->cu);
    if (d->savedCu > d->cu) d->savedCu = -1;
    if (ins && d->cu > 0) {
        Undo *p = &d->u[d->cu - 1];
        if (p->ins && p->grp == d->grp && p->l == l && p->c + p->n == c && !memchr(t, '\n', n) && !memchr(p->t, '\n', p->n)) {
            p->t = (char *)realloc(p->t, p->n + n + 1);
            memcpy(p->t + p->n, t, n); p->n += n; p->t[p->n] = 0;
            return;
        }
    }
    if (d->nu >= d->capu) { d->capu = d->capu * 2 + 64; d->u = (Undo *)realloc(d->u, d->capu * sizeof(Undo)); }
    e = &d->u[d->nu++];
    d->cu = d->nu;
    e->ins = (char)ins; e->grp = d->grp; e->l = l; e->c = c; e->n = n;
    e->t = (char *)malloc(n + 1); memcpy(e->t, t, n); e->t[n] = 0;
    e->bl = d->cl; e->bc = d->cc; e->ba = d->al; e->bb = d->ac;
}

static void ed_ins(Doc *d, int l, int c, const char *t, int n)
{
    int was = doc_dirty(d), el, ec;
    if (n <= 0) return;
    push_undo(d, 1, l, c, t, n);
    raw_insert(d, l, c, t, n, &el, &ec);
    d->cl = d->al = el; d->cc = d->ac = ec;
    if (was != doc_dirty(d)) chrome_dirty();
}

static void ed_del(Doc *d, int l1, int c1, int l2, int c2)
{
    int was = doc_dirty(d), n;
    char *t;
    if (l1 == l2 && c1 == c2) return;
    t = get_text(d, l1, c1, l2, c2, &n);
    push_undo(d, 0, l1, c1, t, n);
    free(t);
    raw_delete(d, l1, c1, l2, c2);
    d->cl = d->al = l1; d->cc = d->ac = c1;
    if (was != doc_dirty(d)) chrome_dirty();
}

static void do_undo(Doc *d, int redo)
{
    int g, el, ec, was = doc_dirty(d);
    if (!redo) {
        if (d->cu == 0) return;
        g = d->u[d->cu - 1].grp;
        while (d->cu > 0 && d->u[d->cu - 1].grp == g) {
            Undo *e = &d->u[--d->cu];
            if (e->ins) { end_of(e->l, e->c, e->t, e->n, &el, &ec); raw_delete(d, e->l, e->c, el, ec); }
            else raw_insert(d, e->l, e->c, e->t, e->n, &el, &ec);
            d->cl = e->bl; d->cc = e->bc; d->al = e->ba; d->ac = e->bb;
        }
    } else {
        if (d->cu >= d->nu) return;
        g = d->u[d->cu].grp;
        while (d->cu < d->nu && d->u[d->cu].grp == g) {
            Undo *e = &d->u[d->cu++];
            if (e->ins) { raw_insert(d, e->l, e->c, e->t, e->n, &el, &ec); d->cl = el; d->cc = ec; }
            else { end_of(e->l, e->c, e->t, e->n, &el, &ec); raw_delete(d, e->l, e->c, el, ec); d->cl = e->l; d->cc = e->c; }
            d->al = d->cl; d->ac = d->cc;
        }
    }
    if (d->cl >= d->n) d->cl = d->n - 1;
    if (d->al >= d->n) d->al = d->n - 1;
    if (d->cc > d->ln[d->cl].len) d->cc = d->ln[d->cl].len;
    if (d->ac > d->ln[d->al].len) d->ac = d->ln[d->al].len;
    d->typing = 0; d->grp++;
    if (was != doc_dirty(d)) chrome_dirty();
}

/* ------------------------------------------------------------------ load/save */

static int is_utf8(const unsigned char *s, int n, int *high)
{
    int i = 0;
    *high = 0;
    while (i < n) {
        unsigned char c = s[i];
        int k = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0) return 0;
        if (k) *high = 1;
        if (i + k >= n && k) return 0;
        i++;
        while (k-- > 0) { if ((s[i] & 0xC0) != 0x80) return 0; i++; }
    }
    return 1;
}

static char *convert_cp(const char *s, int n, UINT from, UINT to, int *outn)
{
    int wn = MultiByteToWideChar(from, 0, s, n, 0, 0), m;
    WCHAR *w = (WCHAR *)malloc((wn + 1) * sizeof(WCHAR));
    char *o;
    MultiByteToWideChar(from, 0, s, n, w, wn);
    m = WideCharToMultiByte(to, 0, w, wn, 0, 0, 0, 0);
    o = (char *)malloc(m + 1);
    WideCharToMultiByte(to, 0, w, wn, o, m, 0, 0);
    o[m] = 0; free(w);
    *outn = m;
    return o;
}

void doc_set_text(Doc *d, const char *t)
{
    int el, ec;
    raw_insert(d, 0, 0, t, lstrlenA(t), &el, &ec);
    d->valid = 1;
}

void doc_set_lang(Doc *d)
{
    d->lang = lang_from_path(d->path[0] ? d->path : d->name);
    d->valid = 1;
}

static void get_ft(const char *path, FILETIME *ft)
{
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &a)) *ft = a.ftLastWriteTime;
    else memset(ft, 0, sizeof(*ft));
}

int doc_load(Doc *d, const char *path)
{
    char *buf, *t; int len, i, start, high, tabs = 0, spaces = 0;
    if (!read_file(path, &buf, &len)) return 0;
    for (i = 0; i < d->n; i++) free(d->ln[i].s);
    d->n = 0;
    d->enc = 0; t = buf;
    if (len >= 3 && (unsigned char)buf[0] == 0xEF && (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        d->enc = 1; t = convert_cp(buf + 3, len - 3, CP_UTF8, CP_ACP, &len); free(buf); buf = t;
    } else if (is_utf8((unsigned char *)buf, len, &high) && high) {
        d->enc = 2; t = convert_cp(buf, len, CP_UTF8, CP_ACP, &len); free(buf); buf = t;
    }
    d->crlf = 0;
    for (i = 0; i < len; i++) if (buf[i] == '\n') { d->crlf = i > 0 && buf[i-1] == '\r'; break; }
    if (i == len) d->crlf = 1;
    start = 0;
    for (i = 0; i <= len; i++) {
        if (i == len || buf[i] == '\n') {
            int e = i;
            Line *L;
            if (e > start && buf[e-1] == '\r') e--;
            lines_reserve(d, d->n + 1);
            L = &d->ln[d->n++];
            memset(L, 0, sizeof(Line));
            line_reserve(L, e - start);
            memcpy(L->s, buf + start, e - start);
            L->len = e - start;
            if (L->len && L->s[0] == '\t') tabs++;
            else if (L->len > 1 && L->s[0] == ' ' && L->s[1] == ' ') spaces++;
            start = i + 1;
        }
    }
    free(buf);
    d->tabs = tabs > spaces;
    lstrcpynA(d->path, path, MAX_PATH);
    lstrcpynA(d->name, path_name(path), MAX_PATH);
    free_undo(d, 0); d->cu = d->savedCu = 0;
    if (d->cl >= d->n) d->cl = d->n - 1;
    if (d->cc > d->ln[d->cl].len) d->cc = d->ln[d->cl].len;
    d->al = d->cl; d->ac = d->cc;
    if (d->top >= d->n) d->top = d->n - 1;
    get_ft(path, &d->ft); d->ftAsked = 0;
    doc_set_lang(d);
    return 1;
}

int doc_write(Doc *d, const char *path)
{
    int i, n = 0, eol = d->crlf ? 2 : 1, outn;
    char *b, *p, *o;
    HANDLE h; DWORD w;
    for (i = 0; i < d->n; i++) n += d->ln[i].len + eol;
    p = b = (char *)malloc(n + 1);
    for (i = 0; i < d->n; i++) {
        memcpy(p, d->ln[i].s, d->ln[i].len); p += d->ln[i].len;
        if (i < d->n - 1) { if (d->crlf) *p++ = '\r'; *p++ = '\n'; }
    }
    n = (int)(p - b);
    o = b; outn = n;
    if (d->enc) o = convert_cp(b, n, CP_ACP, CP_UTF8, &outn);
    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { if (o != b) free(o); free(b); return 0; }
    if (d->enc == 1) WriteFile(h, "\xEF\xBB\xBF", 3, &w, 0);
    WriteFile(h, o, outn, &w, 0);
    CloseHandle(h);
    if (o != b) free(o);
    free(b);
    lstrcpynA(d->path, path, MAX_PATH);
    lstrcpynA(d->name, path_name(path), MAX_PATH);
    d->savedCu = d->cu;
    get_ft(path, &d->ft); d->ftAsked = 0;
    doc_set_lang(d);
    return 1;
}

void ed_check_disk(void)
{
    int i;
    for (i = 0; i < g_ndocs; i++) {
        Doc *d = g_docs[i];
        FILETIME ft;
        if (!d->path[0]) continue;
        get_ft(d->path, &ft);
        if (!ft.dwLowDateTime && !ft.dwHighDateTime) continue;
        if (CompareFileTime(&ft, &d->ft) == 0) continue;
        if (!doc_dirty(d)) {
            int top = d->top;
            if (doc_load(d, d->path)) { d->top = top < d->n ? top : 0; out_log("Reloaded %s (changed on disk)", d->name); }
        } else if (!d->ftAsked) {
            char m[MAX_PATH + 200];
            d->ftAsked = 1;
            d->ft = ft;
            wsprintfA(m, "%s has changed on disk, and you have unsaved changes.\n\nReload it and lose your changes?", d->name);
            if (MessageBoxA(g_main, m, APPNAME, MB_YESNO | MB_ICONWARNING) == IDYES) doc_load(d, d->path);
        }
        chrome_dirty();
        if (i == g_cur) InvalidateRect(g_edit, 0, 0);
    }
}

/* ------------------------------------------------------------------ geometry */

static void ensure_states(Doc *d, int upto)
{
    if (upto >= d->n) upto = d->n - 1;
    d->ln[0].st = 0;
    while (d->valid <= upto) {
        int i = d->valid - 1;
        Line *L = &d->ln[i];
        if (L->len > s_ccap) { s_ccap = L->len + 256; s_cls = (unsigned char *)realloc(s_cls, s_ccap); }
        d->ln[i + 1 <= d->n - 1 ? i + 1 : i].st = (unsigned char)lex_line(d->lang, L->s, L->len, L->st, s_cls);
        d->valid++;
        if (i + 1 >= d->n - 1) { d->valid = d->n; break; }
    }
}

static unsigned char *line_cls(Doc *d, int l)
{
    Line *L = &d->ln[l];
    ensure_states(d, l);
    if (L->len + 1 > s_ccap) { s_ccap = L->len + 256; s_cls = (unsigned char *)realloc(s_cls, s_ccap); }
    lex_line(d->lang, L->s, L->len, L->st, s_cls);
    return s_cls;
}

static int vis_col(Line *L, int c)
{
    int i, v = 0;
    for (i = 0; i < c && i < L->len; i++) v = L->s[i] == '\t' ? (v / TABW + 1) * TABW : v + 1;
    return v;
}

static int col_from_vis(Line *L, int vx)
{
    int i, v = 0;
    for (i = 0; i < L->len; i++) {
        int nv = L->s[i] == '\t' ? (v / TABW + 1) * TABW : v + 1;
        if (vx < nv) return (vx - v) * 2 < (nv - v) ? i : i + 1;
        v = nv;
    }
    return L->len;
}

static int gutter_w(Doc *d)
{
    int digits = 1, n = d->n;
    while (n >= 10) { n /= 10; digits++; }
    if (digits < 3) digits = 3;
    return (digits + 4) * g_cw;
}

static void client_dims(int *w, int *h, int *textW)
{
    RECT r; GetClientRect(g_edit, &r);
    *w = r.right; *h = r.bottom;
    *textW = r.right - SB_W - (g_minimap ? MINIMAP_W : 0);
}

static int vis_lines(void)
{
    RECT r; GetClientRect(g_edit, &r);
    return r.bottom / (g_lh ? g_lh : 16);
}

static int caret_x(Doc *d) { return gutter_w(d) + vis_col(&d->ln[d->cl], d->cc) * g_cw - d->left; }

void ed_caret_sync(void)
{
    Doc *d = cur_doc();
    if (!s_hasCaret) return;
    if (!d) { SetCaretPos(-100, -100); return; }
    {
        int y = (d->cl - d->top) * g_lh, x = caret_x(d), w, h, tw;
        client_dims(&w, &h, &tw);
        if (x < gutter_w(d) || x >= tw) x = -100;
        SetCaretPos(x, y);
    }
    status_dirty();
}

void ed_scroll_to_caret(void)
{
    Doc *d = cur_doc();
    int vis, w, h, tw, x, gw;
    if (!d) return;
    vis = vis_lines(); if (vis < 1) vis = 1;
    if (d->cl < d->top) d->top = d->cl;
    if (d->cl >= d->top + vis) d->top = d->cl - vis + 1;
    client_dims(&w, &h, &tw);
    gw = gutter_w(d);
    x = vis_col(&d->ln[d->cl], d->cc) * g_cw;
    if (x < d->left) d->left = x - g_cw * 4 < 0 ? 0 : x - g_cw * 4;
    if (x > d->left + (tw - gw) - g_cw * 2) d->left = x - (tw - gw) + g_cw * 6;
    InvalidateRect(g_edit, 0, 0);
    ed_caret_sync();
}

void ed_goto(Doc *d, int line, int col, int center)
{
    int vis = vis_lines();
    if (line < 0) line = 0;
    if (line >= d->n) line = d->n - 1;
    if (col < 0) col = 0;
    if (col > d->ln[line].len) col = d->ln[line].len;
    d->cl = d->al = line; d->cc = d->ac = col;
    d->wantX = -1;
    if (center && (line < d->top || line >= d->top + vis)) { d->top = line - vis / 2; if (d->top < 0) d->top = 0; }
    ed_scroll_to_caret();
}

/* ------------------------------------------------------------------ selection helpers */

int doc_has_sel(Doc *d) { return d->cl != d->al || d->cc != d->ac; }

static void sel_range(Doc *d, int *l1, int *c1, int *l2, int *c2)
{
    if (d->al < d->cl || (d->al == d->cl && d->ac < d->cc)) { *l1 = d->al; *c1 = d->ac; *l2 = d->cl; *c2 = d->cc; }
    else { *l1 = d->cl; *c1 = d->cc; *l2 = d->al; *c2 = d->ac; }
}

char *doc_sel_text(Doc *d, int *len)
{
    int l1, c1, l2, c2;
    sel_range(d, &l1, &c1, &l2, &c2);
    return get_text(d, l1, c1, l2, c2, len);
}

static void del_sel(Doc *d)
{
    int l1, c1, l2, c2;
    if (!doc_has_sel(d)) return;
    sel_range(d, &l1, &c1, &l2, &c2);
    ed_del(d, l1, c1, l2, c2);
}

static void insert_text(Doc *d, const char *t, int n)
{
    del_sel(d);
    ed_ins(d, d->cl, d->cc, t, n);
}

static void sel_lines(Doc *d, int *l1, int *l2)
{
    int a, b, c1, c2;
    sel_range(d, &a, &c1, &b, &c2);
    if (b > a && c2 == 0) b--;
    *l1 = a; *l2 = b;
}

static void move_to(Doc *d, int l, int c, int shift)
{
    d->cl = l; d->cc = c;
    if (!shift) { d->al = l; d->ac = c; }
    d->typing = 0;
}

#define ISW(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || ((c) >= '0' && (c) <= '9') || (c) == '_' || (unsigned char)(c) >= 0x80)

static void word_left(Doc *d, int *l, int *c)
{
    Line *L;
    if (*c == 0) { if (*l > 0) { (*l)--; *c = d->ln[*l].len; } return; }
    L = &d->ln[*l];
    while (*c > 0 && (L->s[*c - 1] == ' ' || L->s[*c - 1] == '\t')) (*c)--;
    if (*c > 0 && ISW(L->s[*c - 1])) while (*c > 0 && ISW(L->s[*c - 1])) (*c)--;
    else if (*c > 0) while (*c > 0 && !ISW(L->s[*c - 1]) && L->s[*c - 1] != ' ' && L->s[*c - 1] != '\t') (*c)--;
}

static void word_right(Doc *d, int *l, int *c)
{
    Line *L = &d->ln[*l];
    if (*c >= L->len) { if (*l < d->n - 1) { (*l)++; *c = 0; } return; }
    if (ISW(L->s[*c])) while (*c < L->len && ISW(L->s[*c])) (*c)++;
    else if (L->s[*c] != ' ' && L->s[*c] != '\t') while (*c < L->len && !ISW(L->s[*c]) && L->s[*c] != ' ' && L->s[*c] != '\t') (*c)++;
    while (*c < L->len && (L->s[*c] == ' ' || L->s[*c] == '\t')) (*c)++;
}

static int indent_of(Line *L) { int i = 0; while (i < L->len && (L->s[i] == ' ' || L->s[i] == '\t')) i++; return i; }

static void unit(Doc *d, char *buf)
{
    if (d->tabs) strcpy(buf, "\t"); else strcpy(buf, "    ");
}

/* ------------------------------------------------------------------ bracket matching */

static int find_match(Doc *d, int l, int c, int *ml, int *mc)
{
    static const char *br = "()[]{}";
    Line *L = &d->ln[l];
    const char *p;
    char open, close;
    int dir, depth = 0, steps = 0, i;
    unsigned char *cls;
    if (c >= L->len) return 0;
    p = strchr(br, L->s[c]);
    if (!p || !L->s[c]) return 0;
    cls = line_cls(d, l);
    if (cls[c] == T_STRING || cls[c] == T_COMMENT) return 0;
    i = (int)(p - br);
    dir = (i & 1) ? -1 : 1;
    open = br[i & ~1]; close = br[i | 1];
    for (;;) {
        L = &d->ln[l];
        for (; c >= 0 && c < L->len; c += dir) {
            char ch = L->s[c];
            if ((ch == open || ch == close) && cls[c] != T_STRING && cls[c] != T_COMMENT) {
                if (ch == (dir > 0 ? open : close)) depth++;
                else if (--depth == 0) { *ml = l; *mc = c; return 1; }
            }
        }
        l += dir;
        if (l < 0 || l >= d->n || ++steps > 3000) return 0;
        cls = line_cls(d, l);
        c = dir > 0 ? 0 : d->ln[l].len - 1;
    }
}

static int bracket_near(Doc *d, int *l, int *c)
{
    Line *L = &d->ln[d->cl];
    if (d->cc < L->len && strchr("()[]{}", L->s[d->cc]) && L->s[d->cc]) { *l = d->cl; *c = d->cc; return 1; }
    if (d->cc > 0 && strchr("()[]{}", L->s[d->cc - 1])) { *l = d->cl; *c = d->cc - 1; return 1; }
    return 0;
}

/* ------------------------------------------------------------------ painting */

static COLORREF blend(COLORREF a, COLORREF b, int pct)
{
    return XRGB((GetRValue(a) * pct + GetRValue(b) * (100 - pct)) / 100,
                (GetGValue(a) * pct + GetGValue(b) * (100 - pct)) / 100,
                (GetBValue(a) * pct + GetBValue(b) * (100 - pct)) / 100);
}

static int mini_top(Doc *d, int h)
{
    int rows = h / 2, vis = vis_lines(), mt;
    if (d->n <= rows) return 0;
    if (d->n - vis <= 0) return 0;
    mt = (int)((double)d->top * (d->n - rows) / (d->n - vis > 0 ? d->n - vis : 1));
    if (mt > d->n - rows) mt = d->n - rows;
    if (mt < 0) mt = 0;
    return mt;
}

static void paint_minimap(HDC dc, Doc *d, int x0, int h)
{
    BITMAPINFO bi; DWORD *px; HBITMAP bm; HDC mdc; HGDIOBJ old;
    int mt = mini_top(d, h), rows = h / 2, i, x, vis = vis_lines(), sy, sh;
    DWORD bg = 0x1E1E1E;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = MINIMAP_W; bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, 0, 0);
    if (!bm) return;
    for (i = 0; i < MINIMAP_W * h; i++) px[i] = bg;
    sy = (d->top - mt) * 2; sh = vis * 2;
    for (i = sy < 0 ? 0 : sy; i < sy + sh && i < h; i++)
        for (x = 0; x < MINIMAP_W; x++) px[i * MINIMAP_W + x] = 0x2E2E2E;
    ensure_states(d, mt + rows);
    for (i = 0; i < rows && mt + i < d->n; i++) {
        Line *L = &d->ln[mt + i];
        unsigned char *cls;
        int c, v = 0, y = i * 2;
        if (!L->len) continue;
        cls = line_cls(d, mt + i);
        for (c = 0; c < L->len && v < MINIMAP_W - 4; c++) {
            char ch = L->s[c];
            if (ch == '\t') { v = (v / TABW + 1) * TABW; continue; }
            if (ch != ' ') {
                COLORREF col = blend(g_tokColor[cls[c]], C_BG, 75);
                px[y * MINIMAP_W + 2 + v] = (GetRValue(col) << 16) | (GetGValue(col) << 8) | GetBValue(col);
            }
            v++;
        }
    }
    mdc = CreateCompatibleDC(dc);
    old = SelectObject(mdc, bm);
    BitBlt(dc, x0, 0, MINIMAP_W, h, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, old);
    DeleteDC(mdc);
    DeleteObject(bm);
}

static void draw_welcome(HDC dc, int w, int h)
{
    static const char *rows[] = {
        "Show All Commands", "Ctrl+Shift+P",  "Go to File", "Ctrl+P",
        "Open Folder", "Ctrl+K Ctrl+O", "Open File", "Ctrl+O",
        "New File", "Ctrl+N", "Toggle Terminal", "Ctrl+`",
        "Run Active File", "F5", "Run build.bat", "Ctrl+Shift+B", 0 };
    int i, y = h / 2 - 150, cx = w / 2;
    SIZE sz;
    HGDIOBJ of = SelectObject(dc, g_uiBig);
    if (y < 20) y = 20;
    SetTextColor(dc, XRGB(0x3C,0x3C,0x3C));
    GetTextExtentPoint32A(dc, APPNAME, 7, &sz);
    TextOutA(dc, cx - sz.cx / 2, y, APPNAME, 7);
    y += sz.cy + 30;
    SelectObject(dc, g_ui);
    for (i = 0; rows[i]; i += 2) {
        SetTextColor(dc, XRGB(0x9D,0x9D,0x9D));
        GetTextExtentPoint32A(dc, rows[i], lstrlenA(rows[i]), &sz);
        TextOutA(dc, cx - 10 - sz.cx, y, rows[i], lstrlenA(rows[i]));
        SetTextColor(dc, XRGB(0xCC,0xCC,0xCC));
        TextOutA(dc, cx + 10, y, rows[i+1], lstrlenA(rows[i+1]));
        y += g_uiH + 12;
    }
    SelectObject(dc, of);
}

static void find_text(char *buf, int n)
{
    buf[0] = 0;
    if (s_fFind && IsWindowVisible(g_find)) GetWindowTextA(s_fFind, buf, n);
}

static int match_at(const char *s, int len, int i, const char *f, int fn)
{
    int k;
    if (i + fn > len) return 0;
    if (s_fCase) { if (memcmp(s + i, f, fn)) return 0; }
    else {
        static unsigned char lc[256]; static int init;
        if (!init) { for (k = 0; k < 256; k++) lc[k] = (unsigned char)k; CharLowerBuffA((char *)lc + 1, 255); init = 1; }
        for (k = 0; k < fn; k++) if (lc[(unsigned char)s[i+k]] != lc[(unsigned char)f[k]]) return 0;
    }
    if (s_fWord) {
        if (i > 0 && ISW(s[i-1])) return 0;
        if (i + fn < len && ISW(s[i+fn])) return 0;
    }
    return 1;
}

static void paint(HWND w)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(w, &ps), dc;
    HGDIOBJ of;
    Doc *d = cur_doc();
    int W, H, tw, gw, row, vis, l1, c1, l2, c2, hasSel, bl = -1, bc = 0, ml = -1, mc = 0, fn;
    char ftxt[256];
    RECT r;
    client_dims(&W, &H, &tw);
    if (W <= 0 || H <= 0) { EndPaint(w, &ps); return; }
    dc = bb_begin(w, wdc, W, H);
    fill(dc, 0, 0, W, H, C_BG);
    SetBkMode(dc, TRANSPARENT);
    if (!d) {
        draw_welcome(dc, W, H);
        BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
        EndPaint(w, &ps);
        return;
    }
    of = SelectObject(dc, g_mono);
    gw = gutter_w(d);
    vis = H / g_lh + 1;
    hasSel = doc_has_sel(d);
    sel_range(d, &l1, &c1, &l2, &c2);
    if (bracket_near(d, &bl, &bc)) { if (!find_match(d, bl, bc, &ml, &mc)) bl = -1; } else bl = -1;
    find_text(ftxt, sizeof(ftxt));
    fn = lstrlenA(ftxt);
    ensure_states(d, d->top + vis);
    r.left = gw; r.top = 0; r.right = tw; r.bottom = H;
    IntersectClipRect(dc, gw - 4, 0, tw, H);
    for (row = 0; row < vis; row++) {
        int l = d->top + row, y = row * g_lh, i, x, v, ind, g;
        Line *L;
        unsigned char *cls;
        if (l >= d->n) break;
        L = &d->ln[l];
        if (l == d->cl && !hasSel) {
            HPEN p = CreatePen(PS_SOLID, 1, C_CURLINE), op = (HPEN)SelectObject(dc, p);
            HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, gw - 4, y, tw + 2, y + g_lh);
            SelectObject(dc, obr); SelectObject(dc, op); DeleteObject(p);
        }
        if (fn) {
            for (i = 0; i + fn <= L->len; i++)
                if (match_at(L->s, L->len, i, ftxt, fn)) {
                    int a = vis_col(L, i), b = vis_col(L, i + fn);
                    fill(dc, gw + a * g_cw - d->left, y, (b - a) * g_cw, g_lh, C_FINDHL);
                    i += fn - 1;
                }
        }
        if (hasSel && l >= l1 && l <= l2) {
            int a = l == l1 ? vis_col(L, c1) : 0, b = l == l2 ? vis_col(L, c2) : vis_col(L, L->len) + 1;
            fill(dc, gw + a * g_cw - d->left, y, (b - a) * g_cw, g_lh, GetFocus() == w ? C_SEL : C_SELOFF);
        }
        /* indent guides: blank lines borrow the indent of the next non-blank line */
        ind = vis_col(L, indent_of(L));
        if (indent_of(L) == L->len) {
            int k, a = 0, b = 0;
            for (k = l - 1; k >= 0 && k > l - 200; k--) if (indent_of(&d->ln[k]) < d->ln[k].len) { a = vis_col(&d->ln[k], indent_of(&d->ln[k])); break; }
            for (k = l + 1; k < d->n && k < l + 200; k++) if (indent_of(&d->ln[k]) < d->ln[k].len) { b = vis_col(&d->ln[k], indent_of(&d->ln[k])); break; }
            ind = a < b ? a : b;
        }
        for (g = 0; g < ind; g += TABW) vline(dc, gw + g * g_cw - d->left, y, g_lh, C_GUIDE);
        cls = line_cls(d, l);
        i = 0; v = 0;
        while (i < L->len) {
            int j = i, t = cls[i];
            if (L->s[i] == '\t') { v = (v / TABW + 1) * TABW; i++; continue; }
            while (j < L->len && cls[j] == t && L->s[j] != '\t') j++;
            x = gw + v * g_cw - d->left;
            if (x < tw && x + (j - i) * g_cw > gw - g_cw) {
                SetTextColor(dc, g_tokColor[t]);
                TextOutA(dc, x, y, L->s + i, j - i);
            }
            v += j - i;
            i = j;
        }
        if (bl >= 0) {
            HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
            HPEN p = CreatePen(PS_SOLID, 1, C_BRACKET), op = (HPEN)SelectObject(dc, p);
            if (l == bl) { x = gw + vis_col(L, bc) * g_cw - d->left; Rectangle(dc, x, y, x + g_cw + 1, y + g_lh); }
            if (l == ml) { x = gw + vis_col(L, mc) * g_cw - d->left; Rectangle(dc, x, y, x + g_cw + 1, y + g_lh); }
            SelectObject(dc, op); DeleteObject(p); SelectObject(dc, obr);
        }
    }
    SelectClipRgn(dc, 0);
    /* gutter */
    fill(dc, 0, 0, gw - 4, H, C_BG);
    for (row = 0; row < vis; row++) {
        int l = d->top + row;
        char num[16]; SIZE sz;
        if (l >= d->n) break;
        wsprintfA(num, "%d", l + 1);
        GetTextExtentPoint32A(dc, num, lstrlenA(num), &sz);
        SetTextColor(dc, l == d->cl ? C_LINENOA : C_LINENO);
        TextOutA(dc, gw - g_cw * 2 - sz.cx, row * g_lh, num, lstrlenA(num));
    }
    if (d->left > 0) { int i; for (i = 0; i < 4; i++) vline(dc, gw - 4 + i, 0, H, blend(XRGB(0,0,0), C_BG, 40 - i * 10)); }
    SelectObject(dc, of);
    if (g_minimap) paint_minimap(dc, d, tw, H);
    s_vs.r.left = W - SB_W; s_vs.r.top = 0; s_vs.r.right = W; s_vs.r.bottom = H;
    s_vs.page = H / g_lh; s_vs.max = d->n - 1 + s_vs.page; s_vs.pos = d->top;
    fill(dc, W - SB_W, 0, SB_W, H, C_BG);
    vline(dc, W - SB_W, 0, H, XRGB(0x2B,0x2B,0x2B));
    if (d->n > 1) vs_draw(dc, &s_vs);
    {   /* caret marker in the scroll bar */
        int y = s_vs.max > 0 ? (int)((double)d->cl * H / s_vs.max) : 0;
        fill(dc, W - SB_W + 1, y, SB_W - 1, 2, XRGB(0xA0,0xA0,0xA0));
    }
    BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    EndPaint(w, &ps);
}

/* ------------------------------------------------------------------ hit testing */

static void hit(Doc *d, int x, int y, int *l, int *c)
{
    int gw = gutter_w(d), row = y < 0 ? (y - g_lh + 1) / g_lh : y / g_lh, vx;
    *l = d->top + row;
    if (*l < 0) *l = 0;
    if (*l >= d->n) { *l = d->n - 1; *c = d->ln[*l].len; return; }
    vx = (x - gw + d->left + g_cw / 2) / g_cw;
    if (vx < 0) vx = 0;
    *c = col_from_vis(&d->ln[*l], vx);
}

static void set_top(Doc *d, int top)
{
    if (top > d->n - 1) top = d->n - 1;
    if (top < 0) top = 0;
    d->top = top;
    InvalidateRect(g_edit, 0, 0);
    ed_caret_sync();
}

/* ------------------------------------------------------------------ editing commands */

static void cls_before(Doc *d, int *cl)
{
    unsigned char *cls = line_cls(d, d->cl);
    *cl = d->cc > 0 ? cls[d->cc - 1] : (d->ln[d->cl].len ? cls[0] : T_TEXT);
    if (d->cc > 0 && d->cc < d->ln[d->cl].len && cls[d->cc] != *cl) *cl = T_TEXT;
}

static void type_char(Doc *d, char ch)
{
    static const char *opens = "([{\"'`", *closes = ")]}\"'`";
    Line *L = &d->ln[d->cl];
    char next = d->cc < L->len ? L->s[d->cc] : 0, prev = d->cc > 0 ? L->s[d->cc - 1] : 0;
    const char *po = strchr(opens, ch), *pc = strchr(closes, ch);
    char buf[3];
    begin(d, ch != ' ' || (prev == ' '));
    if (ch == ' ' && prev != ' ') { d->grp++; }
    if (doc_has_sel(d) && po && ch) {
        int l1, c1, l2, c2, n; char *t, *b;
        sel_range(d, &l1, &c1, &l2, &c2);
        t = get_text(d, l1, c1, l2, c2, &n);
        b = (char *)malloc(n + 3);
        b[0] = ch; memcpy(b + 1, t, n); b[n + 1] = closes[po - opens]; b[n + 2] = 0;
        begin(d, 0);
        ed_del(d, l1, c1, l2, c2);
        ed_ins(d, l1, c1, b, n + 2);
        d->al = l1; d->ac = c1 + 1; d->cl = l1 + (l2 - l1); d->cc = l1 == l2 ? c2 + 1 : c2;
        free(t); free(b);
        return;
    }
    del_sel(d);
    L = &d->ln[d->cl];
    next = d->cc < L->len ? L->s[d->cc] : 0;
    if (pc && ch && next == ch && (ch == ')' || ch == ']' || ch == '}' || ch == '"' || ch == '\'' || ch == '`')) {
        d->cc++; d->ac = d->cc; return;
    }
    if (po && ch && d->lang != L_TEXT) {
        int ok = !next || next == ' ' || next == '\t' || strchr(")]};,:", next);
        if (ch == '"' || ch == '\'' || ch == '`') {
            int t; cls_before(d, &t);
            if (ISW(prev) || prev == ch || t == T_STRING || t == T_COMMENT) ok = 0;
            if (ch == '\'' && (d->lang == L_MD || d->lang == L_BAT || d->lang == L_INI)) ok = 0;
            if (ch == '`' && d->lang != L_JS && d->lang != L_MD) ok = 0;
        }
        if (ok) {
            buf[0] = ch; buf[1] = closes[po - opens]; buf[2] = 0;
            ed_ins(d, d->cl, d->cc, buf, 2);
            d->cc--; d->ac = d->cc;
            return;
        }
    }
    if (ch == '}' && (d->lang == L_C || d->lang == L_JS) && indent_of(L) == d->cc && d->cc > 0) {
        int rm = d->tabs ? 1 : (d->cc % 4 ? d->cc % 4 : 4);
        if (rm > d->cc) rm = d->cc;
        ed_del(d, d->cl, d->cc - rm, d->cl, d->cc);
    }
    buf[0] = ch;
    ed_ins(d, d->cl, d->cc, buf, 1);
}

static void do_enter(Doc *d)
{
    Line *L;
    char buf[512], u[8];
    int n, k, i, indent;
    char prev = 0, next;
    begin(d, 0);
    del_sel(d);
    L = &d->ln[d->cl];
    indent = indent_of(L);
    if (indent > d->cc) indent = d->cc;
    if (indent > 400) indent = 400;
    for (i = d->cc - 1; i >= 0; i--) if (L->s[i] != ' ' && L->s[i] != '\t') { prev = L->s[i]; break; }
    next = d->cc < L->len ? L->s[d->cc] : 0;
    unit(d, u);
    buf[0] = '\n'; memcpy(buf + 1, L->s, indent); n = indent + 1;
    k = (prev == '{' || prev == '[' || prev == '(' || (d->lang == L_PY && prev == ':'));
    if (d->lang == L_HTML && prev == '>' && next == '<' && d->cc + 1 < L->len && L->s[d->cc + 1] == '/') k = 2;
    if (k) { strcpy(buf + n, u); n += lstrlenA(u); }
    if ((k == 1 && ((prev == '{' && next == '}') || (prev == '[' && next == ']') || (prev == '(' && next == ')'))) || k == 2) {
        int caretN = n, ll, cc;
        buf[n++] = '\n'; memcpy(buf + n, L->s, indent); n += indent;
        ed_ins(d, d->cl, d->cc, buf, n);
        ll = d->cl - 1; cc = caretN - 1 - 0;
        d->cl = d->al = ll; d->cc = d->ac = indent + lstrlenA(u);
        (void)cc;
        return;
    }
    /* drop trailing whitespace left on the line we split */
    ed_ins(d, d->cl, d->cc, buf, n);
    {
        Line *P = &d->ln[d->cl - 1];
        int e = P->len;
        while (e > 0 && (P->s[e-1] == ' ' || P->s[e-1] == '\t')) e--;
        if (e < P->len && e > 0) {
            int cl = d->cl, cc = d->cc;
            ed_del(d, d->cl - 1, e, d->cl - 1, P->len);
            d->cl = d->al = cl; d->cc = d->ac = cc;
        }
    }
}

static void indent_lines(Doc *d, int out)
{
    int l1, l2, l, i;
    char u[8];
    int sa = d->ac, sc = d->cc, al = d->al, cl = d->cl;
    unit(d, u);
    sel_lines(d, &l1, &l2);
    begin(d, 0);
    for (l = l1; l <= l2; l++) {
        Line *L = &d->ln[l];
        if (!out) {
            if (!L->len) continue;
            ed_ins(d, l, 0, u, lstrlenA(u));
            if (l == al) sa += lstrlenA(u);
            if (l == cl) sc += lstrlenA(u);
        } else {
            int rm = 0;
            if (L->len && L->s[0] == '\t') rm = 1;
            else for (i = 0; i < 4 && i < L->len && L->s[i] == ' '; i++) rm++;
            if (!rm) continue;
            ed_del(d, l, 0, l, rm);
            if (l == al) sa = sa > rm ? sa - rm : 0;
            if (l == cl) sc = sc > rm ? sc - rm : 0;
        }
    }
    d->al = al; d->ac = sa; d->cl = cl; d->cc = sc;
}

static void toggle_comment(Doc *d)
{
    const char *end, *pre = lang_comment(d->lang, &end);
    int l1, l2, l, all = 1, minInd = 9999, pl, el;
    int cl = d->cl, cc = d->cc, al = d->al, ac = d->ac;
    char p2[16];
    if (!pre) return;
    pl = lstrlenA(pre); el = end ? lstrlenA(end) : 0;
    lstrcpynA(p2, pre, sizeof(p2));
    if (pl && p2[pl-1] == ' ') p2[--pl] = 0;    /* match without the trailing space */
    sel_lines(d, &l1, &l2);
    for (l = l1; l <= l2; l++) {
        Line *L = &d->ln[l]; int i = indent_of(L);
        if (i == L->len) continue;
        if (i < minInd) minInd = i;
        if (L->len - i < pl || _strnicmp(L->s + i, p2, pl)) all = 0;
    }
    if (minInd == 9999) return;
    begin(d, 0);
    for (l = l1; l <= l2; l++) {
        Line *L = &d->ln[l]; int i = indent_of(L), rm;
        if (i == L->len) continue;
        if (all) {
            if (end) {
                int e = L->len; const char *e2 = end; int e2n = el;
                if (e2[0] == ' ' && !(L->len >= e2n && !memcmp(L->s + L->len - e2n, e2, e2n))) { e2++; e2n--; }
                if (L->len >= e2n && !memcmp(L->s + L->len - e2n, e2, e2n)) ed_del(d, l, e - e2n, l, e);
            }
            rm = pl;
            if (i + rm < L->len && L->s[i + rm] == ' ') rm++;
            ed_del(d, l, i, l, i + rm);
            if (l == cl) cc = cc > i + rm ? cc - rm : (cc > i ? i : cc);
            if (l == al) ac = ac > i + rm ? ac - rm : (ac > i ? i : ac);
        } else {
            char b[20]; int bn;
            wsprintfA(b, "%s ", p2); bn = lstrlenA(b);
            if (end) ed_ins(d, l, L->len, end, el);
            ed_ins(d, l, minInd, b, bn);
            if (l == cl && cc >= minInd) cc += bn;
            if (l == al && ac >= minInd) ac += bn;
        }
    }
    d->cl = cl; d->cc = cc; d->al = al; d->ac = ac;
    if (d->cc > d->ln[d->cl].len) d->cc = d->ln[d->cl].len;
    if (d->ac > d->ln[d->al].len) d->ac = d->ln[d->al].len;
}

static void move_lines(Doc *d, int down)
{
    int l1, l2, n, cl = d->cl, cc = d->cc, al = d->al, ac = d->ac;
    char *t;
    sel_lines(d, &l1, &l2);
    begin(d, 0);
    if (!down) {
        if (l1 == 0) return;
        t = get_text(d, l1 - 1, 0, l1 - 1, d->ln[l1 - 1].len, &n);
        ed_del(d, l1 - 1, 0, l1, 0);
        if (l2 < d->n) {
            char *b = (char *)malloc(n + 2); memcpy(b, t, n); b[n] = '\n';
            if (l2 >= d->n) { ed_ins(d, l2 - 1, d->ln[l2 - 1].len, "\n", 1); ed_ins(d, l2, 0, t, n); }
            else ed_ins(d, l2, 0, b, n + 1);
            free(b);
        } else {
            char *b = (char *)malloc(n + 2); b[0] = '\n'; memcpy(b + 1, t, n);
            ed_ins(d, l2 - 1, d->ln[l2 - 1].len, b, n + 1);
            free(b);
        }
        free(t);
        cl--; al--;
    } else {
        char *b;
        if (l2 >= d->n - 1) return;
        t = get_text(d, l2 + 1, 0, l2 + 1, d->ln[l2 + 1].len, &n);
        ed_del(d, l2, d->ln[l2].len, l2 + 1, d->ln[l2 + 1].len);
        b = (char *)malloc(n + 2); memcpy(b, t, n); b[n] = '\n';
        ed_ins(d, l1, 0, b, n + 1);
        free(b); free(t);
        cl++; al++;
    }
    d->cl = cl; d->cc = cc; d->al = al; d->ac = ac;
}

static void copy_lines(Doc *d, int down)
{
    int l1, l2, n, cl = d->cl, cc = d->cc, al = d->al, ac = d->ac;
    char *t, *b;
    sel_lines(d, &l1, &l2);
    begin(d, 0);
    t = get_text(d, l1, 0, l2, d->ln[l2].len, &n);
    b = (char *)malloc(n + 2);
    if (down) {
        b[0] = '\n'; memcpy(b + 1, t, n);
        ed_ins(d, l2, d->ln[l2].len, b, n + 1);
        cl += l2 - l1 + 1; al += l2 - l1 + 1;
    } else {
        memcpy(b, t, n); b[n] = '\n';
        ed_ins(d, l1, 0, b, n + 1);
    }
    free(b); free(t);
    d->cl = cl; d->cc = cc; d->al = al; d->ac = ac;
}

static void delete_lines(Doc *d)
{
    int l1, l2, cc = d->cc;
    sel_lines(d, &l1, &l2);
    begin(d, 0);
    if (l2 < d->n - 1) ed_del(d, l1, 0, l2 + 1, 0);
    else if (l1 > 0) ed_del(d, l1 - 1, d->ln[l1 - 1].len, l2, d->ln[l2].len);
    else ed_del(d, 0, 0, l2, d->ln[l2].len);
    if (d->cl >= d->n) d->cl = d->n - 1;
    d->cc = cc > d->ln[d->cl].len ? d->ln[d->cl].len : cc;
    d->al = d->cl; d->ac = d->cc;
}

static void do_copy(Doc *d, int cut)
{
    int n; char *t, *b, *p; int i, extra = 0;
    if (doc_has_sel(d)) t = doc_sel_text(d, &n);
    else {   /* no selection: whole line */
        int l = d->cl;
        t = get_text(d, l, 0, l, d->ln[l].len, &n);
        t = (char *)realloc(t, n + 2); t[n++] = '\n'; t[n] = 0;
    }
    for (i = 0; i < n; i++) if (t[i] == '\n') extra++;
    p = b = (char *)malloc(n + extra + 1);
    for (i = 0; i < n; i++) { if (t[i] == '\n') *p++ = '\r'; *p++ = t[i]; }
    clip_set(b, (int)(p - b));
    free(b); free(t);
    if (cut) {
        begin(d, 0);
        if (doc_has_sel(d)) del_sel(d); else delete_lines(d);
    }
}

static void do_paste(Doc *d)
{
    char *t = clip_get(), *p, *q;
    if (!t) return;
    for (p = q = t; *p; p++) if (*p != '\r') *q++ = *p;
    *q = 0;
    begin(d, 0);
    insert_text(d, t, (int)(q - t));
    free(t);
}

static void trim_ws(Doc *d)
{
    int l;
    begin(d, 0);
    for (l = 0; l < d->n; l++) {
        Line *L = &d->ln[l]; int e = L->len;
        while (e > 0 && (L->s[e-1] == ' ' || L->s[e-1] == '\t')) e--;
        if (e < L->len && !(l == d->cl && d->cc > e)) ed_del(d, l, e, l, L->len);
    }
    if (d->cc > d->ln[d->cl].len) d->cc = d->ln[d->cl].len;
    d->al = d->cl; d->ac = d->cc;
}

/* ------------------------------------------------------------------ find / replace */

static int find_from(Doc *d, int l, int c, int back, int *fl, int *fc)
{
    char f[256]; int fn, i, k;
    find_text(f, sizeof(f));
    if (!f[0]) GetWindowTextA(s_fFind, f, sizeof(f));
    fn = lstrlenA(f);
    if (!fn) return 0;
    for (k = 0; k <= d->n; k++) {
        int li = back ? (l - k + d->n * 2) % d->n : (l + k) % d->n;
        Line *L = &d->ln[li];
        if (!back) {
            for (i = k == 0 ? c : 0; i + fn <= L->len; i++) if (match_at(L->s, L->len, i, f, fn)) { *fl = li; *fc = i; return fn; }
        } else {
            int s = k == 0 ? c - 1 : L->len - fn;
            if (k == d->n) s = L->len - fn;
            for (i = s; i >= 0; i--) if (i + fn <= L->len && match_at(L->s, L->len, i, f, fn)) { *fl = li; *fc = i; return fn; }
        }
    }
    return 0;
}

static void count_matches(Doc *d)
{
    char f[256]; int fn, i, l, l1, c1, l2, c2;
    find_text(f, sizeof(f));
    fn = lstrlenA(f);
    s_fCount = s_fIndex = 0;
    if (!fn || !d) return;
    sel_range(d, &l1, &c1, &l2, &c2);
    for (l = 0; l < d->n && s_fCount < 99999; l++) {
        Line *L = &d->ln[l];
        for (i = 0; i + fn <= L->len; i++)
            if (match_at(L->s, L->len, i, f, fn)) {
                s_fCount++;
                if (l == l1 && i == c1) s_fIndex = s_fCount;
                i += fn - 1;
            }
    }
}

static void find_next(int back, int fromSelStart)
{
    Doc *d = cur_doc();
    int l1, c1, l2, c2, fl, fc, fn;
    if (!d) return;
    sel_range(d, &l1, &c1, &l2, &c2);
    if (back) fn = find_from(d, l1, c1, 1, &fl, &fc);
    else if (fromSelStart) fn = find_from(d, l1, c1, 0, &fl, &fc);
    else fn = find_from(d, l2, c2, 0, &fl, &fc);
    if (fn) {
        int vis = vis_lines();
        d->al = fl; d->ac = fc; d->cl = fl; d->cc = fc + fn;
        if (fl < d->top || fl >= d->top + vis) { d->top = fl - vis / 3; if (d->top < 0) d->top = 0; }
        ed_scroll_to_caret();
    }
    count_matches(d);
    if (g_find) InvalidateRect(g_find, 0, 0);
    InvalidateRect(g_edit, 0, 0);
}

static void replace_one(int all)
{
    Doc *d = cur_doc();
    char f[256], r[256];
    int fn, rn;
    if (!d) return;
    GetWindowTextA(s_fFind, f, sizeof(f)); GetWindowTextA(s_fRepl, r, sizeof(r));
    fn = lstrlenA(f); rn = lstrlenA(r);
    if (!fn) return;
    if (!all) {
        int l1, c1, l2, c2;
        sel_range(d, &l1, &c1, &l2, &c2);
        if (l1 == l2 && c2 - c1 == fn && match_at(d->ln[l1].s, d->ln[l1].len, c1, f, fn)) {
            begin(d, 0);
            ed_del(d, l1, c1, l2, c2);
            ed_ins(d, l1, c1, r, rn);
        }
        find_next(0, 0);
    } else {
        int l, i, count = 0;
        begin(d, 0);
        for (l = d->n - 1; l >= 0; l--) {
            Line *L = &d->ln[l];
            int pos[512], np = 0;
            for (i = 0; i + fn <= L->len && np < 512; i++) if (match_at(L->s, L->len, i, f, fn)) { pos[np++] = i; i += fn - 1; }
            while (np-- > 0) { ed_del(d, l, pos[np], l, pos[np] + fn); ed_ins(d, l, pos[np], r, rn); count++; }
        }
        out_log("Replaced %d occurrence(s) in %s", count, d->name);
        count_matches(d);
        InvalidateRect(g_find, 0, 0);
    }
    ed_scroll_to_caret();
}

/* find widget layout: [>][ find edit ][Aa][ab] n of m [^][v][x]
                          [ replace edit ][R][A] */
#define FW_W 420
static RECT fbtn[8];

void find_layout(void)
{
    RECT r; int h, x;
    if (!g_find) return;
    GetClientRect(g_edit, &r);
    h = s_fRepMode ? 62 : 34;
    x = r.right - FW_W - SB_W - (g_minimap ? MINIMAP_W : 0) - 4;
    if (x < 0) x = 0;
    MoveWindow(g_find, x, 0, FW_W, h, TRUE);
    MoveWindow(s_fFind, 26, 8, 190, g_uiH + 4, TRUE);
    MoveWindow(s_fRepl, 26, 36, 190, g_uiH + 4, TRUE);
    ShowWindow(s_fRepl, s_fRepMode ? SW_SHOW : SW_HIDE);
    SetRect(&fbtn[0], 2, 4, 20, h - 4);                 /* toggle replace */
    SetRect(&fbtn[1], 222, 5, 244, 27);                /* Aa */
    SetRect(&fbtn[2], 246, 5, 268, 27);                /* whole word */
    SetRect(&fbtn[3], 340, 5, 362, 27);                /* prev */
    SetRect(&fbtn[4], 364, 5, 386, 27);                /* next */
    SetRect(&fbtn[5], 390, 5, 412, 27);                /* close */
    SetRect(&fbtn[6], 222, 33, 244, 55);               /* replace */
    SetRect(&fbtn[7], 246, 33, 268, 55);               /* replace all */
}

int find_visible(void) { return g_find && IsWindowVisible(g_find); }

static void find_paint(HWND w)
{
    PAINTSTRUCT ps; HDC dc = BeginPaint(w, &ps);
    RECT r; HGDIOBJ of; char buf[64]; int i;
    GetClientRect(w, &r);
    fill(dc, 0, 0, r.right, r.bottom, C_WIDGET);
    vline(dc, 0, 0, r.bottom, C_FOCUS);
    hline(dc, 0, r.bottom - 1, r.right, XRGB(0x45,0x45,0x45));
    vline(dc, r.right - 1, 0, r.bottom, XRGB(0x45,0x45,0x45));
    fill(dc, 24, 6, 250, g_uiH + 8, C_INPUT);
    if (s_fRepMode) fill(dc, 24, 34, 250, g_uiH + 8, C_INPUT);
    of = SelectObject(dc, g_ui);
    SetBkMode(dc, TRANSPARENT);
    draw_icon(dc, s_fRepMode ? IC_CHEVD : IC_CHEVR, 6, r.bottom / 2 - 8, C_TEXT);
    for (i = 1; i <= 2; i++) {
        int on = i == 1 ? s_fCase : s_fWord;
        if (on) { fill(dc, fbtn[i].left, fbtn[i].top + 2, 20, 18, XRGB(0x2B,0x4E,0x6E)); }
        SetTextColor(dc, on ? C_WHITE : C_DIM);
        TextOutA(dc, fbtn[i].left + 3, fbtn[i].top + 4, i == 1 ? "Aa" : "ab", 2);
        if (i == 2) hline(dc, fbtn[i].left + 3, fbtn[i].top + 17, 14, on ? C_WHITE : C_DIM);
    }
    {
        char f[256]; find_text(f, sizeof(f));
        if (!f[0]) strcpy(buf, "No results");
        else if (!s_fCount) strcpy(buf, "No results");
        else if (s_fIndex) wsprintfA(buf, "%d of %d", s_fIndex, s_fCount);
        else wsprintfA(buf, "? of %d", s_fCount);
        SetTextColor(dc, (f[0] && !s_fCount) ? C_ERR : C_TEXT);
        TextOutA(dc, 274, 9, buf, lstrlenA(buf));
    }
    draw_icon(dc, IC_UP, fbtn[3].left + 3, fbtn[3].top + 3, C_TEXT);
    draw_icon(dc, IC_DOWN, fbtn[4].left + 3, fbtn[4].top + 3, C_TEXT);
    draw_icon(dc, IC_CLOSE, fbtn[5].left + 3, fbtn[5].top + 3, C_TEXT);
    if (s_fRepMode) {
        SetTextColor(dc, C_TEXT);
        TextOutA(dc, fbtn[6].left + 2, fbtn[6].top + 4, "R1", 2);
        TextOutA(dc, fbtn[7].left + 1, fbtn[7].top + 4, "All", 3);
    }
    SelectObject(dc, of);
    EndPaint(w, &ps);
}

static void find_close(void)
{
    ShowWindow(g_find, SW_HIDE);
    InvalidateRect(g_edit, 0, 0);
    focus_editor();
}

static LRESULT CALLBACK FindEditProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            if (w == s_fRepl) replace_one(key_mods() & M_CTRL);
            else find_next((key_mods() & M_SHIFT) != 0, 0);
            return 0;
        }
        if (wp == VK_ESCAPE) { find_close(); return 0; }
        if (wp == VK_TAB) { if (s_fRepMode) SetFocus(w == s_fFind ? s_fRepl : s_fFind); return 0; }
        if (wp == VK_F3) { find_next((key_mods() & M_SHIFT) != 0, 0); return 0; }
    }
    if (m == WM_CHAR && (wp == '\r' || wp == 27 || wp == '\t')) return 0;
    if (m == WM_CHAR && wp == 1 && (key_mods() & M_CTRL)) { SendMessageA(w, EM_SETSEL, 0, -1); return 0; }
    return CallWindowProcA(s_oldEdit, w, m, wp, lp);
}

static LRESULT CALLBACK FindProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: find_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wp, C_TEXT); SetBkColor((HDC)wp, C_INPUT);
        return (LRESULT)g_brInput;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && (HWND)lp == s_fFind) { find_next(0, 1); }
        return 0;
    case WM_LBUTTONDOWN: {
        POINT p; int i;
        p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
        for (i = 0; i < 8; i++) if (PtInRect(&fbtn[i], p)) break;
        switch (i) {
        case 0: s_fRepMode = !s_fRepMode; find_layout(); if (s_fRepMode) SetFocus(s_fRepl); break;
        case 1: s_fCase = !s_fCase; find_next(0, 1); break;
        case 2: s_fWord = !s_fWord; find_next(0, 1); break;
        case 3: find_next(1, 0); break;
        case 4: find_next(0, 0); break;
        case 5: find_close(); return 0;
        case 6: if (s_fRepMode) replace_one(0); break;
        case 7: if (s_fRepMode) replace_one(1); break;
        }
        InvalidateRect(w, 0, 0);
        return 0;
    }
    }
    return DefWindowProcA(w, m, wp, lp);
}

void find_show(int replace)
{
    Doc *d = cur_doc();
    if (!d) return;
    if (!g_find) {
        WNDCLASSA wc; memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc = FindProc; wc.hInstance = g_inst; wc.lpszClassName = "XPFind";
        wc.hCursor = LoadCursor(0, IDC_ARROW);
        RegisterClassA(&wc);
        g_find = CreateWindowExA(0, "XPFind", "", WS_CHILD | WS_CLIPCHILDREN, 0, 0, FW_W, 34, g_edit, 0, g_inst, 0);
        s_fFind = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, g_find, 0, g_inst, 0);
        s_fRepl = CreateWindowExA(0, "EDIT", "", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, g_find, 0, g_inst, 0);
        SendMessageA(s_fFind, WM_SETFONT, (WPARAM)g_ui, 0);
        SendMessageA(s_fRepl, WM_SETFONT, (WPARAM)g_ui, 0);
        s_oldEdit = (WNDPROC)SetWindowLongA(s_fFind, GWL_WNDPROC, (LONG)(LONG_PTR)FindEditProc);
        SetWindowLongA(s_fRepl, GWL_WNDPROC, (LONG)(LONG_PTR)FindEditProc);
    }
    s_fRepMode = replace;
    if (doc_has_sel(d) && d->cl == d->al) {
        int n; char *t = doc_sel_text(d, &n);
        if (n < 200) SetWindowTextA(s_fFind, t);
        free(t);
    }
    find_layout();
    ShowWindow(g_find, SW_SHOW);
    SetFocus(replace && GetWindowTextLengthA(s_fFind) ? s_fRepl : s_fFind);
    SendMessageA(s_fFind, EM_SETSEL, 0, -1);
    count_matches(d);
    InvalidateRect(g_find, 0, 0);
    InvalidateRect(g_edit, 0, 0);
}

/* ------------------------------------------------------------------ command dispatch */

int ed_cmd(int id)
{
    Doc *d = cur_doc();
    int l, c;
    if (!d) return 0;
    switch (id) {
    case CM_UNDO: do_undo(d, 0); break;
    case CM_REDO: do_undo(d, 1); break;
    case CM_CUT: do_copy(d, 1); break;
    case CM_COPY: do_copy(d, 0); return 1;
    case CM_PASTE: do_paste(d); break;
    case CM_SELALL: d->al = 0; d->ac = 0; d->cl = d->n - 1; d->cc = d->ln[d->cl].len; break;
    case CM_SELLINE: {
        int l1, l2; sel_lines(d, &l1, &l2);
        d->al = l1; d->ac = 0;
        if (l2 + 1 < d->n) { d->cl = l2 + 1; d->cc = 0; } else { d->cl = l2; d->cc = d->ln[l2].len; }
        break;
    }
    case CM_EXPANDSEL: {
        Line *L = &d->ln[d->cl]; int a = d->cc, b = d->cc;
        if (doc_has_sel(d) && d->al == d->cl) {
            /* with a selection: jump to its next occurrence (wraps) */
            int n, k, i, fn, c0 = d->ac > d->cc ? d->ac : d->cc;
            char *t = doc_sel_text(d, &fn);
            for (k = 0; k <= d->n && fn; k++) {
                int li = (d->cl + k) % d->n; Line *M = &d->ln[li];
                for (i = k == 0 ? c0 : 0; i + fn <= M->len; i++)
                    if (!memcmp(M->s + i, t, fn)) { d->al = d->cl = li; d->ac = i; d->cc = i + fn; k = d->n + 1; break; }
            }
            free(t); (void)n;
            break;
        }
        while (a > 0 && ISW(L->s[a-1])) a--;
        while (b < L->len && ISW(L->s[b])) b++;
        d->al = d->cl; d->ac = a; d->cc = b;
        break;
    }
    case CM_MOVEUP: move_lines(d, 0); break;
    case CM_MOVEDOWN: move_lines(d, 1); break;
    case CM_COPYUP: copy_lines(d, 0); break;
    case CM_COPYDOWN: copy_lines(d, 1); break;
    case CM_DELLINE: delete_lines(d); break;
    case CM_INDENT: indent_lines(d, 0); break;
    case CM_OUTDENT: indent_lines(d, 1); break;
    case CM_COMMENT: toggle_comment(d); break;
    case CM_TRIMWS: trim_ws(d); break;
    case CM_INSLINEBELOW: {
        char b[512]; Line *L = &d->ln[d->cl]; int k = indent_of(L);
        if (k > 500) k = 500;
        b[0] = '\n'; memcpy(b + 1, L->s, k);
        begin(d, 0); ed_ins(d, d->cl, L->len, b, k + 1);
        break;
    }
    case CM_INSLINEABOVE: {
        char b[512]; Line *L = &d->ln[d->cl]; int k = indent_of(L), l0 = d->cl;
        if (k > 500) k = 500;
        memcpy(b, L->s, k); b[k] = '\n';
        begin(d, 0); ed_ins(d, l0, 0, b, k + 1);
        d->cl = d->al = l0; d->cc = d->ac = k;
        break;
    }
    case CM_GOTOBRACKET:
        if (bracket_near(d, &l, &c) && find_match(d, l, c, &l, &c)) { d->cl = d->al = l; d->cc = d->ac = c; }
        break;
    case CM_FIND: find_show(0); return 1;
    case CM_REPLACE: find_show(1); return 1;
    case CM_FINDNEXT: if (!g_find) { find_show(0); return 1; } find_next(0, 0); return 1;
    case CM_FINDPREV: if (!g_find) { find_show(0); return 1; } find_next(1, 0); return 1;
    default: return 0;
    }
    d->wantX = -1;
    ed_scroll_to_caret();
    if (find_visible()) { count_matches(d); InvalidateRect(g_find, 0, 0); }
    return 1;
}

/* ------------------------------------------------------------------ keyboard */

static void key_down(HWND w, int vk)
{
    Doc *d = cur_doc();
    int mods = key_mods(), shift = mods & M_SHIFT, ctrl = mods & M_CTRL;
    int l, c, vis;
    if (!d) return;
    l = d->cl; c = d->cc;
    vis = vis_lines(); if (vis < 1) vis = 1;
    switch (vk) {
    case VK_LEFT:
        if (!shift && doc_has_sel(d) && !ctrl) { int a, b, e, f; sel_range(d, &a, &b, &e, &f); move_to(d, a, b, 0); break; }
        if (ctrl) word_left(d, &l, &c);
        else if (c > 0) c--; else if (l > 0) { l--; c = d->ln[l].len; }
        move_to(d, l, c, shift); d->wantX = -1; break;
    case VK_RIGHT:
        if (!shift && doc_has_sel(d) && !ctrl) { int a, b, e, f; sel_range(d, &a, &b, &e, &f); move_to(d, e, f, 0); break; }
        if (ctrl) word_right(d, &l, &c);
        else if (c < d->ln[l].len) c++; else if (l < d->n - 1) { l++; c = 0; }
        move_to(d, l, c, shift); d->wantX = -1; break;
    case VK_UP: case VK_DOWN: case VK_PRIOR: case VK_NEXT: {
        int delta = vk == VK_UP ? -1 : vk == VK_DOWN ? 1 : vk == VK_PRIOR ? -vis : vis;
        if (ctrl && (vk == VK_UP || vk == VK_DOWN)) { set_top(d, d->top + delta); return; }
        if (d->wantX < 0) d->wantX = vis_col(&d->ln[l], c);
        if (l + delta < 0) { l = 0; c = 0; }
        else if (l + delta >= d->n) { l = d->n - 1; c = d->ln[l].len; }
        else { l += delta; c = col_from_vis(&d->ln[l], d->wantX); }
        if (vk == VK_PRIOR || vk == VK_NEXT) { d->top += delta; if (d->top < 0) d->top = 0; if (d->top > d->n - 1) d->top = d->n - 1; }
        { int wx = d->wantX; move_to(d, l, c, shift); d->wantX = wx; }
        break;
    }
    case VK_HOME: {
        int ind = indent_of(&d->ln[l]);
        if (ctrl) { l = 0; c = 0; } else c = (c == ind) ? 0 : ind;
        move_to(d, l, c, shift); d->wantX = -1; break;
    }
    case VK_END:
        if (ctrl) l = d->n - 1;
        move_to(d, l, d->ln[l].len, shift); d->wantX = -1; break;
    case VK_BACK:
        if (doc_has_sel(d)) { begin(d, 0); del_sel(d); }
        else if (ctrl) { word_left(d, &l, &c); begin(d, 0); ed_del(d, l, c, d->cl, d->cc); }
        else if (c > 0) {
            Line *L = &d->ln[l];
            int ind = indent_of(L), rm = 1;
            begin(d, 1);
            d->typing = 0;
            if (c > 0 && c < L->len && strchr("([{\"'`", L->s[c-1]) && L->s[c] == ")]}\"'`"[strchr("([{\"'`", L->s[c-1]) - "([{\"'`"])
                { ed_del(d, l, c - 1, l, c + 1); break; }
            if (!d->tabs && c <= ind && c >= 4 && L->s[c-1] == ' ') { rm = c % 4 ? c % 4 : 4; }
            ed_del(d, l, c - rm, l, c);
        } else if (l > 0) { begin(d, 0); ed_del(d, l - 1, d->ln[l - 1].len, l, 0); }
        d->wantX = -1;
        break;
    case VK_DELETE:
        if (shift && !doc_has_sel(d)) { delete_lines(d); break; }
        if (shift) { do_copy(d, 1); break; }
        begin(d, 0);
        if (doc_has_sel(d)) del_sel(d);
        else if (ctrl) { word_right(d, &l, &c); ed_del(d, d->cl, d->cc, l, c); }
        else if (c < d->ln[l].len) ed_del(d, l, c, l, c + 1);
        else if (l < d->n - 1) ed_del(d, l, c, l + 1, 0);
        d->wantX = -1;
        break;
    case VK_RETURN:
        if (ctrl) { ed_cmd(shift ? CM_INSLINEABOVE : CM_INSLINEBELOW); return; }
        do_enter(d); d->wantX = -1; break;
    case VK_TAB:
        if (ctrl) return;
        if (shift || (doc_has_sel(d) && d->cl != d->al)) indent_lines(d, shift);
        else {
            char u[8];
            begin(d, 0);
            if (d->tabs) strcpy(u, "\t");
            else { int v = vis_col(&d->ln[l], c), k = 4 - v % 4; memset(u, ' ', k); u[k] = 0; }
            insert_text(d, u, lstrlenA(u));
        }
        d->wantX = -1;
        break;
    case VK_INSERT:
        if (ctrl) do_copy(d, 0); else if (shift) do_paste(d);
        break;
    case VK_ESCAPE:
        if (find_visible()) { find_close(); return; }
        move_to(d, d->cl, d->cc, 0);
        break;
    case VK_APPS: {
        POINT p; p.x = caret_x(d); p.y = (d->cl - d->top + 1) * g_lh;
        ClientToScreen(w, &p);
        SendMessageA(w, WM_CONTEXTMENU, (WPARAM)w, MAKELPARAM(p.x, p.y));
        return;
    }
    default: return;
    }
    ed_scroll_to_caret();
    if (find_visible()) { count_matches(d); InvalidateRect(g_find, 0, 0); }
}

/* ------------------------------------------------------------------ window proc */

static void mouse_sel(HWND w, int x, int y, int shift)
{
    Doc *d = cur_doc(); int l, c;
    hit(d, x, y, &l, &c);
    if (s_clicks >= 2 && s_drag) {
        /* word-wise extend on double-click drag */
        int a = c, b = c; Line *L = &d->ln[l];
        while (a > 0 && ISW(L->s[a-1])) a--;
        while (b < L->len && ISW(L->s[b])) b++;
        if (l > d->al || (l == d->al && c >= d->ac)) c = b; else c = a;
    }
    move_to(d, l, c, shift);
    d->wantX = -1;
    ed_scroll_to_caret();
}

static LRESULT CALLBACK EdProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    Doc *d = cur_doc();
    switch (m) {
    case WM_PAINT: paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: find_layout(); InvalidateRect(w, 0, 0); return 0;
    case WM_SETFOCUS:
        CreateCaret(w, 0, 2, g_lh); s_hasCaret = 1;
        ed_caret_sync(); ShowCaret(w);
        InvalidateRect(w, 0, 0);
        return 0;
    case WM_KILLFOCUS:
        s_hasCaret = 0; DestroyCaret();
        InvalidateRect(w, 0, 0);
        return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_KEYDOWN: key_down(w, (int)wp); return 0;
    case WM_CHAR:
        if (!d) return 0;
        if (wp < 32 || wp == 127) return 0;
        if (key_mods() & M_CTRL && !(GetKeyState(VK_MENU) & 0x8000)) return 0;
        type_char(d, (char)wp);
        d->wantX = -1;
        ed_scroll_to_caret();
        if (find_visible()) { count_matches(d); InvalidateRect(g_find, 0, 0); }
        return 0;
    case WM_MOUSEWHEEL: {
        int delta = (short)HIWORD(wp);
        if (LOWORD(wp) & MK_CONTROL) { run_cmd(delta > 0 ? CM_ZOOMIN : CM_ZOOMOUT); return 0; }
        if (!d) return 0;
        if (LOWORD(wp) & MK_SHIFT) {
            d->left -= delta / 120 * g_cw * 8;
            if (d->left < 0) d->left = 0;
            InvalidateRect(w, 0, 0); ed_caret_sync();
            return 0;
        }
        set_top(d, d->top - delta / 120 * 3);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), W, H, tw;
        DWORD now = GetTickCount();
        SetFocus(w);
        if (!d) return 0;
        client_dims(&W, &H, &tw);
        if (vs_mouse(&s_vs, w, m, x, y)) { set_top(d, s_vs.pos); return 0; }
        if (g_minimap && x >= tw && x < W - SB_W) {
            int line = mini_top(d, H) + y / 2;
            set_top(d, line - vis_lines() / 2);
            s_mdrag = 1; SetCapture(w);
            return 0;
        }
        if (now - s_lastClick < GetDoubleClickTime() && abs(x - s_lastX) < 4 && abs(y - s_lastY) < 4) s_clicks++;
        else s_clicks = 1;
        s_lastClick = now; s_lastX = x; s_lastY = y;
        if (x < gutter_w(d) - 4) {   /* gutter: select the whole line */
            int l, c; hit(d, x, y, &l, &c);
            if (!(key_mods() & M_SHIFT)) { d->al = l; d->ac = 0; }
            if (l + 1 < d->n) { d->cl = l + 1; d->cc = 0; } else { d->cl = l; d->cc = d->ln[l].len; }
            ed_scroll_to_caret();
            return 0;
        }
        if (s_clicks == 2) {
            int l, c, a, b; Line *L;
            hit(d, x, y, &l, &c);
            L = &d->ln[l]; a = b = c;
            while (a > 0 && ISW(L->s[a-1])) a--;
            while (b < L->len && ISW(L->s[b])) b++;
            if (a == b && b < L->len) b++;
            d->al = l; d->ac = a; d->cl = l; d->cc = b;
            s_drag = 1; SetCapture(w);
            ed_scroll_to_caret();
            return 0;
        }
        if (s_clicks >= 3) {
            int l, c; hit(d, x, y, &l, &c);
            d->al = l; d->ac = 0;
            if (l + 1 < d->n) { d->cl = l + 1; d->cc = 0; } else d->cc = d->ln[l].len;
            ed_scroll_to_caret();
            return 0;
        }
        mouse_sel(w, x, y, key_mods() & M_SHIFT);
        s_drag = 1; SetCapture(w);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (!d) return 0;
        if (s_vs.drag) { vs_mouse(&s_vs, w, m, x, y); set_top(d, s_vs.pos); return 0; }
        if (s_mdrag) { RECT r; GetClientRect(w, &r); set_top(d, mini_top(d, r.bottom) + y / 2 - vis_lines() / 2); return 0; }
        if (s_drag) {
            RECT r; GetClientRect(w, &r);
            s_lastY = y; s_lastX = x;
            mouse_sel(w, x, y, 1);
            if (y < 0 || y > r.bottom) SetTimer(w, 1, 50, 0);
        } else {
            int W, H, tw; client_dims(&W, &H, &tw);
            SetCursor(LoadCursor(0, x < tw && x > gutter_w(d) - 4 ? IDC_IBEAM : IDC_ARROW));
        }
        return 0;
    }
    case WM_TIMER:
        if (wp == 1) {
            RECT r; GetClientRect(w, &r);
            if (!s_drag || !d || (s_lastY >= 0 && s_lastY <= r.bottom)) { KillTimer(w, 1); return 0; }
            set_top(d, d->top + (s_lastY < 0 ? -1 : 1));
            mouse_sel(w, s_lastX, s_lastY, 1);
        }
        return 0;
    case WM_LBUTTONUP:
        if (s_vs.drag) vs_mouse(&s_vs, w, m, 0, 0);
        s_drag = 0; s_mdrag = 0;
        KillTimer(w, 1);
        ReleaseCapture();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p; int W, H, tw;
            GetCursorPos(&p); ScreenToClient(w, &p);
            client_dims(&W, &H, &tw);
            SetCursor(LoadCursor(0, d && p.x < tw && p.x > gutter_w(d) - 4 ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_RBUTTONDOWN:
        SetFocus(w);
        if (d && !doc_has_sel(d)) {
            int l, c; hit(d, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), &l, &c);
            move_to(d, l, c, 0); ed_scroll_to_caret();
        }
        return 0;
    case WM_CONTEXTMENU: {
        HMENU mn;
        POINT p;
        if (!d) return 0;
        mn = CreatePopupMenu();
        AppendMenuA(mn, MF_STRING, CM_GOTOBRACKET, "Go to Bracket\tCtrl+Shift+\\");
        AppendMenuA(mn, MF_STRING, CM_PALETTE, "Command Palette...\tCtrl+Shift+P");
        AppendMenuA(mn, MF_SEPARATOR, 0, 0);
        AppendMenuA(mn, MF_STRING, CM_CUT, "Cut\tCtrl+X");
        AppendMenuA(mn, MF_STRING, CM_COPY, "Copy\tCtrl+C");
        AppendMenuA(mn, MF_STRING, CM_PASTE, "Paste\tCtrl+V");
        AppendMenuA(mn, MF_SEPARATOR, 0, 0);
        AppendMenuA(mn, MF_STRING, CM_COMMENT, "Toggle Line Comment\tCtrl+/");
        AppendMenuA(mn, MF_STRING, CM_FIND, "Find\tCtrl+F");
        AppendMenuA(mn, MF_STRING, CM_RUN, "Run Active File\tF5");
        p.x = GET_X_LPARAM(lp); p.y = GET_Y_LPARAM(lp);
        if (p.x == -1) { p.x = caret_x(d); p.y = (d->cl - d->top + 1) * g_lh; ClientToScreen(w, &p); }
        TrackPopupMenu(mn, TPM_LEFTALIGN | TPM_TOPALIGN, p.x, p.y, 0, g_main, 0);
        DestroyMenu(mn);
        return 0;
    }
    }
    return DefWindowProcA(w, m, wp, lp);
}

void ed_font_changed(void)
{
    HDC dc = GetDC(g_edit);
    HGDIOBJ of = SelectObject(dc, g_mono);
    TEXTMETRICA tm;
    GetTextMetricsA(dc, &tm);
    g_cw = tm.tmAveCharWidth; g_lh = tm.tmHeight + 4;
    SelectObject(dc, of);
    ReleaseDC(g_edit, dc);
    if (s_hasCaret) { DestroyCaret(); CreateCaret(g_edit, 0, 2, g_lh); ShowCaret(g_edit); ed_caret_sync(); }
    InvalidateRect(g_edit, 0, 0);
    (void)s_lastCtl;
}

void ed_register(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = EdProc; wc.hInstance = g_inst; wc.lpszClassName = "XPEdit";
    wc.hCursor = LoadCursor(0, IDC_IBEAM);
    RegisterClassA(&wc);
}
