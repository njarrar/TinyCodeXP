/* palette.c - the quick input box: Command Palette (Ctrl+Shift+P), Quick Open
 * (Ctrl+P), Go to Line (Ctrl+G), and small prompts and pick lists, all with
 * fuzzy matching. */
#include "xpcode.h"

#define ROW_H 22
#define MAXROWS 13
#define INPUT_H 26

typedef struct { char *text, *detail, *path; const char *right; int id, score; unsigned char hits[64]; } PItem;

static PItem *s_items; static int s_nitems, s_icap;
static int *s_view; static int s_nview, s_vcap;
static int s_mode, s_sel, s_top, s_hasHits;
static PalCb s_cb;
static char s_title[256];
static HWND s_ed, s_prevFocus;
static WNDPROC s_oldEd;
static int s_closing;

/* cached file list for Quick Open */
static char **s_files; static int s_nfiles, s_fcap, s_filesValid;

void files_invalidate(void) { s_filesValid = 0; }

/* build output and other files nobody opens in a text editor */
static int binary_ext(const char *n)
{
    char e[16]; const char *d = strrchr(n, '.');
    if (!d || lstrlenA(d) > 12) return 0;
    wsprintfA(e, " %s ", d);
    CharLowerA(e);
    return strstr(" .exe .dll .obj .o .a .lib .pdb .ilk .exp .res .pyc .class .zip .7z ", e) != 0;
}

static void scan(const char *dir, int depth)
{
    WIN32_FIND_DATAA fd; HANDLE h; char pat[MAX_PATH], full[MAX_PATH];
    if (depth > 12 || s_nfiles >= 20000) return;
    path_join(pat, dir, "*");
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.' && (!fd.cFileName[1] || fd.cFileName[1] == '.')) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        path_join(full, dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.cFileName[0] == '.' || skip_build_dir(fd.cFileName)) continue;
            scan(full, depth + 1);
        } else {
            if (binary_ext(fd.cFileName)) continue;
            if (s_nfiles >= s_fcap) { s_fcap = s_fcap * 2 + 256; s_files = (char **)realloc(s_files, s_fcap * sizeof(char *)); }
            s_files[s_nfiles++] = xstrdup(full);
        }
    } while (FindNextFileA(h, &fd) && s_nfiles < 20000);
    FindClose(h);
}

static void ensure_files(void)
{
    int i;
    if (s_filesValid) return;
    for (i = 0; i < s_nfiles; i++) free(s_files[i]);
    s_nfiles = 0;
    if (g_folder[0]) scan(g_folder, 0);
    s_filesValid = 1;
}

/* ------------------------------------------------------------------ fuzzy */

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int boundary(const char *s, int i)
{
    char p;
    if (i == 0) return 1;
    p = s[i-1];
    if (p == '\\' || p == '/' || p == ' ' || p == '_' || p == '-' || p == '.' || p == ':') return 1;
    return (s[i] >= 'A' && s[i] <= 'Z') && (p >= 'a' && p <= 'z');
}

/* subsequence match; returns 0 for no match, else a score (higher is better) */
int fuzzy(const char *pat, const char *s, unsigned char *hits)
{
    int i = 0, j = 0, score = 1, last = -2, n = lstrlenA(s);
    if (hits) memset(hits, 0, 64);
    if (!*pat) return 1;
    for (; pat[j]; j++) {
        int c = lower((unsigned char)pat[j]), found = -1, k;
        if (c == ' ') continue;
        /* prefer a word-start occurrence */
        for (k = i; k < n; k++) if (lower((unsigned char)s[k]) == c && boundary(s, k) && (k == last + 1 || k - i < 40)) { found = k; break; }
        if (found < 0 || (found != last + 1 && i < n && lower((unsigned char)s[i]) == c && i == last + 1)) {
            for (k = i; k < n; k++) if (lower((unsigned char)s[k]) == c) { if (found < 0 || k < found) found = k; break; }
        }
        if (found < 0) return 0;
        score += 1;
        if (found == last + 1) score += 6;
        if (boundary(s, found)) score += 8;
        if (s[found] == pat[j]) score += 1;
        score -= (found - i) / 4;
        if (hits && found < 512) hits[found / 8] |= (unsigned char)(1 << (found % 8));
        last = found; i = found + 1;
    }
    if (score < 1) score = 1;
    return score + (n < 60 ? (60 - n) / 10 : 0);
}

/* ------------------------------------------------------------------ items */

static void clear_items(void)
{
    int i;
    for (i = 0; i < s_nitems; i++) { free(s_items[i].text); free(s_items[i].detail); free(s_items[i].path); }
    s_nitems = 0;
}

static PItem *add_item(const char *text, const char *detail, const char *path, const char *right, int id)
{
    PItem *p;
    if (s_nitems >= s_icap) { s_icap = s_icap * 2 + 64; s_items = (PItem *)realloc(s_items, s_icap * sizeof(PItem)); }
    p = &s_items[s_nitems++];
    memset(p, 0, sizeof(*p));
    p->text = xstrdup(text); p->detail = detail ? xstrdup(detail) : 0; p->path = path ? xstrdup(path) : 0;
    p->right = right; p->id = id;
    return p;
}

/* ------------------------------------------------------------------ symbols */

static int is_id(int c) { return c == '_' || (c >= '0' && c <= '9') || ((c | 32) >= 'a' && (c | 32) <= 'z'); }

/* copies the identifier at s into out; returns its length */
static int take_id(const char *s, char *out)
{
    int n = 0;
    while (is_id((unsigned char)s[n]) && n < 63) { out[n] = s[n]; n++; }
    out[n] = 0;
    return n;
}

/* one pass over the active file: functions, types, defines, headings, labels */
static void sym_items(void)
{
    Doc *d = cur_doc();
    int i;
    if (!d) return;
    for (i = 0; i < d->n; i++) {
        const char *s = d->ln[i].s, *p, *kind = 0;
        char name[64]; int len = d->ln[i].len;
        name[0] = 0;
        if (!len) continue;
        p = s; while (*p == ' ' || *p == '\t') p++;
        switch (d->lang) {
        case L_C: case L_JS:
            if (!strncmp(s, "#define ", 8)) { take_id(s + 8, name); kind = "define"; }
            else if (!strncmp(p, "function ", 9)) { take_id(p + 9, name); kind = "function"; }
            else if (!strncmp(p, "class ", 6)) { take_id(p + 6, name); kind = "class"; }
            else if ((!strncmp(s, "struct ", 7) || !strncmp(s, "enum ", 5) || !strncmp(s, "typedef struct ", 15)) && !strchr(s, ';')) {
                const char *q = strrchr(s, ' '); if (q && is_id((unsigned char)q[1])) { take_id(q + 1, name); kind = "struct"; }
                else if (!strncmp(s, "typedef struct ", 15)) { take_id(s + 15, name); kind = "struct"; }
            } else if (is_id((unsigned char)s[0]) && s[len - 1] != ';' && (p = strchr(s, '(')) != 0) {
                const char *q = p; while (q > s && q[-1] == ' ') q--;
                while (q > s && is_id((unsigned char)q[-1])) q--;
                if (q < p && !in_list(" if while for switch return sizeof ", name, take_id(q, name), 0)) kind = "function";
            }
            break;
        case L_PY:
            if (!strncmp(p, "def ", 4)) { take_id(p + 4, name); kind = "function"; }
            else if (!strncmp(p, "class ", 6)) { take_id(p + 6, name); kind = "class"; }
            break;
        case L_MD: if (s[0] == '#') { lstrcpynA(name, s, sizeof(name)); kind = "heading"; } break;
        case L_INI: if (s[0] == '[') { lstrcpynA(name, s, sizeof(name)); kind = "section"; } break;
        case L_BAT: if (p[0] == ':' && p[1] != ':') { take_id(p + 1, name); kind = "label"; } break;
        }
        if (kind && name[0]) add_item(name, kind, 0, 0, i);
    }
}

static void build_items(void)
{
    int i;
    clear_items();
    if (s_mode == PAL_SYM) sym_items();
    else if (s_mode == PAL_CMD) {
        for (i = 0; i < cmd_count(); i++) add_item(cmd_name(cmd_at(i)), 0, 0, cmd_key(cmd_at(i)), cmd_at(i));
    } else if (s_mode == PAL_FILE) {
        ensure_files();   /* the folder watcher calls files_invalidate() */
        for (i = 0; i < g_ndocs; i++) if (g_docs[i]->path[0]) {
            char dir[MAX_PATH]; path_dir(dir, g_docs[i]->path);
            add_item(g_docs[i]->name, rel_path(dir) == dir ? dir : rel_path(dir), g_docs[i]->path, "open", -1);
        }
        for (i = 0; i < s_nfiles; i++) {
            char dir[MAX_PATH]; int k, dup = 0;
            for (k = 0; k < g_ndocs; k++) if (!lstrcmpiA(g_docs[k]->path, s_files[i])) { dup = 1; break; }
            if (dup) continue;
            path_dir(dir, s_files[i]);
            add_item(path_name(s_files[i]), rel_path(dir) == dir ? "" : rel_path(dir), s_files[i], 0, i);
        }
    }
}

static int cmp_score(const void *a, const void *b)
{
    const PItem *x = &s_items[*(int *)a], *y = &s_items[*(int *)b];
    if (x->score != y->score) return y->score - x->score;
    return *(int *)a - *(int *)b;
}

static void filter(void)
{
    char q[300], *pq;
    int i;
    GetWindowTextA(s_ed, q, sizeof(q));
    /* VS Code style prefixes switch modes */
    if (s_mode == PAL_FILE && q[0] == '>') { s_mode = PAL_CMD; build_items(); }
    else if (s_mode == PAL_FILE && q[0] == ':') { s_mode = PAL_LINE; clear_items(); }
    else if (s_mode == PAL_FILE && q[0] == '@') { s_mode = PAL_SYM; build_items(); }
    else if ((s_mode == PAL_CMD && q[0] != '>') || (s_mode == PAL_LINE && q[0] != ':') || (s_mode == PAL_SYM && q[0] != '@')) { s_mode = PAL_FILE; build_items(); }
    pq = q;
    if ((s_mode == PAL_CMD || s_mode == PAL_LINE || s_mode == PAL_SYM) && *pq) pq++;
    while (*pq == ' ') pq++;
    s_nview = 0; s_sel = 0; s_top = 0;
    s_hasHits = *pq != 0;
    if (s_mode == PAL_LINE) {
        Doc *d = cur_doc();
        char t[200];
        clear_items();
        if (d) {
            int ln = atoi(pq);
            if (ln > 0) wsprintfA(t, "Go to line %d.", ln > d->n ? d->n : ln);
            else wsprintfA(t, "Current line: %d, Character: %d. Type a line number between 1 and %d to navigate to.", d->cl + 1, d->cc + 1, d->n);
            add_item(t, 0, 0, 0, ln);
        }
    }
    if (s_mode == PAL_INPUT) { clear_items(); add_item(s_title, 0, 0, 0, 0); s_hasHits = 0; }
    for (i = 0; i < s_nitems; i++) {
        PItem *p = &s_items[i];
        char pat[300]; char *colon;
        lstrcpynA(pat, pq, sizeof(pat));
        if (s_mode == PAL_FILE && (colon = strchr(pat, ':')) && colon > pat + 1) *colon = 0;
        if (s_mode == PAL_LINE || s_mode == PAL_INPUT) p->score = 1;
        else if (s_mode == PAL_FILE) {
            char rel[MAX_PATH * 2];
            int a = fuzzy(pat, p->text, p->hits), b;
            if (p->detail && p->detail[0]) wsprintfA(rel, "%s\\%s", p->detail, p->text); else lstrcpyA(rel, p->text);
            b = a ? 0 : fuzzy(pat, rel, 0);
            p->score = a ? a * 2 + 10 : b;
            if (!a) memset(p->hits, 0, sizeof(p->hits));
            if (p->right && p->score) p->score += pat[0] ? 3 : 1000;
        } else p->score = fuzzy(pat, p->text, p->hits);
        if (!p->score) continue;
        if (s_nview >= s_vcap) { s_vcap = s_vcap * 2 + 256; s_view = (int *)realloc(s_view, s_vcap * sizeof(int)); }
        s_view[s_nview++] = i;
    }
    if (pq[0]) qsort(s_view, s_nview, sizeof(int), cmp_score);
    if (s_nview > 2000) s_nview = 2000;
}

/* ------------------------------------------------------------------ geometry */

static int rows_shown(void) { return s_nview < MAXROWS ? s_nview : MAXROWS; }

static void place(void)
{
    RECT r; int w, h, rows = rows_shown();
    GetClientRect(g_main, &r);
    w = r.right - 120 > 600 ? 600 : r.right - 120;
    if (w < 300) w = r.right - 20;
    h = 6 + INPUT_H + 6 + rows * ROW_H + (rows ? 6 : 0);
    SetWindowPos(g_pal, HWND_TOP, (r.right - w) / 2, 4, w, h, SWP_SHOWWINDOW);
    MoveWindow(s_ed, 12, 6 + (INPUT_H - g_uiH) / 2, w - 24, g_uiH + 2, TRUE);
    InvalidateRect(g_pal, 0, 0);
}

/* ------------------------------------------------------------------ actions */

void pal_close(void)
{
    if (!g_pal || !IsWindowVisible(g_pal) || s_closing) return;
    s_closing = 1;
    ShowWindow(g_pal, SW_HIDE);
    if (s_prevFocus && IsWindow(s_prevFocus) && IsWindowVisible(s_prevFocus)) SetFocus(s_prevFocus);
    else focus_editor();
    s_closing = 0;
    InvalidateRect(g_main, 0, 0);
}

static void accept(int row)
{
    char q[300];
    PItem *p;
    int mode = s_mode;
    PalCb cb = s_cb;
    GetWindowTextA(s_ed, q, sizeof(q));
    if (mode == PAL_INPUT) { pal_close(); if (cb) cb(q, 0); return; }
    if (row < 0 || row >= s_nview) return;
    p = &s_items[s_view[row]];
    switch (mode) {
    case PAL_CMD: { int id = p->id; pal_close(); run_cmd(id); break; }
    case PAL_FILE: {
        char path[MAX_PATH], *c = strchr(q, ':'); int line = 0, col = 0;
        lstrcpynA(path, p->path, MAX_PATH);
        if (c && c > q + 1) { line = atoi(c + 1); c = strchr(c + 1, ':'); if (c) col = atoi(c + 1); }
        pal_close();
        open_file(path, line, col);
        break;
    }
    case PAL_LINE: {
        Doc *d = cur_doc(); char *s = q + 1, *c; int ln = atoi(s), col = 1;
        c = strchr(s, ':'); if (!c) c = strchr(s, ','); if (c) col = atoi(c + 1);
        pal_close();
        if (d && ln > 0) { ed_goto(d, ln - 1, col - 1, 1); SetFocus(g_edit); }
        break;
    }
    case PAL_SYM: { Doc *d = cur_doc(); int ln = p->id; pal_close(); if (d && ln < d->n) { ed_goto(d, ln, 0, 1); SetFocus(g_edit); } break; }
    case PAL_PICK: { int id = p->id; char t[256]; lstrcpynA(t, p->text, sizeof(t)); pal_close(); if (cb) cb(t, id); break; }
    }
}

static void move_sel(int d)
{
    int rows = rows_shown();
    if (!s_nview) return;
    s_sel += d;
    if (s_sel < 0) s_sel = d == -1 ? s_nview - 1 : 0;
    if (s_sel >= s_nview) s_sel = d == 1 ? 0 : s_nview - 1;
    if (s_sel < s_top) s_top = s_sel;
    if (s_sel >= s_top + rows) s_top = s_sel - rows + 1;
    InvalidateRect(g_pal, 0, 0);
}

static LRESULT CALLBACK EdProc2(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_KEYDOWN) {
        switch (wp) {
        case VK_UP: move_sel(-1); return 0;
        case VK_DOWN: move_sel(1); return 0;
        case VK_PRIOR: move_sel(-MAXROWS); return 0;
        case VK_NEXT: move_sel(MAXROWS); return 0;
        case VK_RETURN: accept(s_sel); return 0;
        case VK_ESCAPE: pal_close(); return 0;
        }
    }
    if (m == WM_CHAR && (wp == '\r' || wp == 27 || wp == '\n')) return 0;
    if (m == WM_CHAR && wp == 1) { SendMessageA(w, EM_SETSEL, 0, -1); return 0; }
    if (m == WM_KILLFOCUS && !s_closing) {
        HWND to = (HWND)wp;
        if (to != g_pal) PostMessageA(g_pal, WM_APP + 5, 0, 0);
    }
    return CallWindowProcA(s_oldEd, w, m, wp, lp);
}

/* ------------------------------------------------------------------ painting */

static void draw_hl(HDC dc, int x, int y, const char *s, const unsigned char *hits, COLORREF c)
{
    int i, n = lstrlenA(s);
    ui_text(dc, x, y, s, c);
    if (!hits || !s_hasHits) return;
    for (i = 0; i < n && i < 512; i++) if (hits[i / 8] & (1 << (i % 8))) {
        SIZE z; GetTextExtentPoint32A(dc, s, i, &z);
        SetTextColor(dc, XRGB(0x2A,0xAA,0xFF));
        TextOutA(dc, x + z.cx, y, s + i, 1);
    }
}

static void pal_paint(HWND w)
{
    PAINTSTRUCT ps; HDC wdc = BeginPaint(w, &ps), dc;
    RECT r; HGDIOBJ of; int i, rows = rows_shown();
    GetClientRect(w, &r);
    dc = bb_begin(w, wdc, r.right, r.bottom);
    fill(dc, 0, 0, r.right, r.bottom, C_WIDGET);
    { HPEN p = CreatePen(PS_SOLID, 1, XRGB(0x45,0x45,0x45)), op = (HPEN)SelectObject(dc, p); HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, 0, 0, r.right, r.bottom); SelectObject(dc, obr); SelectObject(dc, op); DeleteObject(p); }
    fill(dc, 6, 6, r.right - 12, INPUT_H, C_INPUT);
    { HPEN p = CreatePen(PS_SOLID, 1, C_FOCUS), op = (HPEN)SelectObject(dc, p); HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
      Rectangle(dc, 6, 6, r.right - 6, 6 + INPUT_H); SelectObject(dc, obr); SelectObject(dc, op); DeleteObject(p); }
    of = SelectObject(dc, g_ui);
    SetBkMode(dc, TRANSPARENT);
    for (i = 0; i < rows && s_top + i < s_nview; i++) {
        PItem *p = &s_items[s_view[s_top + i]];
        int y = 6 + INPUT_H + 6 + i * ROW_H, ty = y + (ROW_H - g_uiH) / 2, x = 12;
        int sel = s_top + i == s_sel && s_mode != PAL_INPUT;
        if (sel) fill(dc, 1, y, r.right - 2, ROW_H, XRGB(0x04,0x39,0x5E));
        if (s_mode == PAL_FILE) { file_glyph(dc, p->text, 0, x, ty); x += 22; }
        draw_hl(dc, x, ty, p->text, p->hits, s_mode == PAL_INPUT ? C_DIM : C_TEXT);
        if (p->detail && p->detail[0]) {
            SIZE z; GetTextExtentPoint32A(dc, p->text, lstrlenA(p->text), &z);
            ui_text(dc, x + z.cx + 8, ty, p->detail, C_DIM);
        }
        if (p->right && p->right[0]) {
            SIZE z; GetTextExtentPoint32A(dc, p->right, lstrlenA(p->right), &z);
            fill(dc, r.right - 18 - z.cx, y + 3, z.cx + 8, ROW_H - 6, sel ? XRGB(0x0E,0x50,0x7A) : XRGB(0x33,0x33,0x33));
            ui_text(dc, r.right - 14 - z.cx, ty, p->right, XRGB(0xCC,0xCC,0xCC));
        }
    }
    if (s_nview > rows) {
        int H = rows * ROW_H, th = H * rows / s_nview, ty = 6 + INPUT_H + 6 + H * s_top / s_nview;
        if (th < 12) th = 12;
        fill(dc, r.right - 8, ty, 6, th, C_SCROLL);
    }
    SelectObject(dc, of);
    BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    
    EndPaint(w, &ps);
}

static LRESULT CALLBACK PalProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: pal_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_CTLCOLOREDIT: SetTextColor((HDC)wp, C_TEXT); SetBkColor((HDC)wp, C_INPUT); return (LRESULT)g_brInput;
    case WM_COMMAND:
        if (HIWORD(wp) == EN_CHANGE && s_mode != PAL_INPUT) { filter(); place(); }
        return 0;
    case WM_APP + 5:
        if (GetFocus() != s_ed && GetFocus() != w) pal_close();
        return 0;
    case WM_SETFOCUS: SetFocus(s_ed); return 0;
    case WM_MOUSEWHEEL: s_top -= (short)HIWORD(wp) / 120 * 3; if (s_top > s_nview - rows_shown()) s_top = s_nview - rows_shown(); if (s_top < 0) s_top = 0; InvalidateRect(w, 0, 0); return 0;
    case WM_LBUTTONDOWN: {
        int y = GET_Y_LPARAM(lp) - (6 + INPUT_H + 6);
        if (y >= 0) { int row = s_top + y / ROW_H; if (row < s_nview) { s_sel = row; accept(row); } }
        else SetFocus(s_ed);
        return 0;
    }
    }
    return DefWindowProcA(w, m, wp, lp);
}

void pal_register(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = PalProc; wc.hInstance = g_inst; wc.lpszClassName = "XPPalette";
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    RegisterClassA(&wc);
}

static void open_pal(int mode, const char *init)
{
    HWND f = GetFocus();
    if (!g_pal) {
        g_pal = CreateWindowExA(0, "XPPalette", "", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 10, 10, g_main, 0, g_inst, 0);
        s_ed = CreateWindowExA(0, "EDIT", "", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, g_pal, 0, g_inst, 0);
        SendMessageA(s_ed, WM_SETFONT, (WPARAM)g_ui, 0);
        s_oldEd = (WNDPROC)SetWindowLongA(s_ed, GWL_WNDPROC, (LONG)(LONG_PTR)EdProc2);
    }
    if (f != s_ed && f != g_pal) s_prevFocus = f;
    s_mode = mode;
    if (mode == PAL_FILE || mode == PAL_CMD || mode == PAL_SYM) build_items();
    s_closing = 1;
    SetWindowTextA(s_ed, init);
    s_closing = 0;
    filter();
    place();
    BringWindowToTop(g_pal);
    SetFocus(s_ed);
    SendMessageA(s_ed, EM_SETSEL, mode == PAL_INPUT ? 0 : lstrlenA(init), -1);
}

void pal_show(int mode, const char *init)
{
    s_cb = 0;
    if (mode == PAL_CMD) open_pal(PAL_CMD, init && init[0] ? init : ">");
    else if (mode == PAL_LINE) open_pal(PAL_LINE, ":");
    else if (mode == PAL_SYM) open_pal(PAL_SYM, "@");
    else open_pal(PAL_FILE, init ? init : "");
}

void pal_input(const char *title, const char *init, PalCb cb)
{
    s_cb = cb;
    lstrcpynA(s_title, title, sizeof(s_title));
    lstrcatA(s_title, "  (Enter to confirm, Escape to cancel)");
    open_pal(PAL_INPUT, init);
    /* select the name without its extension, like VS Code's rename */
    { const char *dot = strrchr(init, '.'); if (dot && dot != init) SendMessageA(s_ed, EM_SETSEL, 0, dot - init); }
}

void pal_pick(const char *title, const char **items, int n, PalCb cb)
{
    int i;
    (void)title;
    clear_items();
    for (i = 0; i < n; i++) add_item(items[i], 0, 0, 0, i);
    s_cb = cb;
    open_pal(PAL_PICK, "");
}
