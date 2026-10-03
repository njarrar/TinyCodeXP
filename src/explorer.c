/* explorer.c - the side bar: Explorer file tree, Search across files and
 * the Run view. */
#include "xpcode.h"

#define HDR_H 35
#define SEC_H 22
#define ROW_H 22
#define SB_W 10

typedef struct Node {
    char *name, *path;
    int isdir, open, loaded;
    struct Node **kids; int nk;
    struct Node *parent;
    int depth;
} Node;

static Node *s_root;
static Node **s_vis; static int s_nvis, s_vcap;
static Node *s_sel;
static int s_top, s_hover = -1, s_rootOpen = 1;
static VScroll s_vs;
static RECT s_secBtn[4];
static char s_opPath[MAX_PATH];

/* search view */
typedef struct { int file, line, col, len; char *text; } SRes;
typedef struct { char *path; int count, collapsed; } SFile;
static HWND s_q;
static WNDPROC s_oldQ;
static SRes *s_res; static int s_nres, s_rcap;
static SFile *s_sf; static int s_nsf, s_sfcap;
static int s_sCase, s_sWord, s_sTop, s_sSel = -1, s_truncated;
static char s_sQuery[256];
static RECT s_sBtn[2];
static VScroll s_svs;

/* run view */
static RECT s_runBtn[2];

/* ------------------------------------------------------------------ tree */

static void node_free(Node *n)
{
    int i;
    if (!n) return;
    for (i = 0; i < n->nk; i++) node_free(n->kids[i]);
    free(n->kids); free(n->name); free(n->path); free(n);
}

static Node *node_new(const char *path, const char *name, int isdir, Node *parent)
{
    Node *n = (Node *)xalloc(sizeof(Node));
    n->path = xstrdup(path); n->name = xstrdup(name); n->isdir = isdir;
    n->parent = parent; n->depth = parent ? parent->depth + 1 : 0;
    return n;
}

static int node_cmp(const void *a, const void *b)
{
    const Node *x = *(Node **)a, *y = *(Node **)b;
    if (x->isdir != y->isdir) return y->isdir - x->isdir;
    return lstrcmpiA(x->name, y->name);
}

static int skip_dir(const char *name)
{
    return !lstrcmpA(name, ".git") || !lstrcmpA(name, ".svn") || !lstrcmpA(name, ".hg");
}

static void node_load(Node *n)
{
    WIN32_FIND_DATAA fd; HANDLE h; char pat[MAX_PATH];
    Node **old = n->kids; int nold = n->nk, i, cap = 0;
    n->kids = 0; n->nk = 0;
    path_join(pat, n->path, "*");
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            Node *k = 0; int isdir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            char full[MAX_PATH];
            if (!lstrcmpA(fd.cFileName, ".") || !lstrcmpA(fd.cFileName, "..")) continue;
            if (isdir && skip_dir(fd.cFileName)) continue;
            for (i = 0; i < nold; i++) if (old[i] && old[i]->isdir == isdir && !lstrcmpiA(old[i]->name, fd.cFileName)) { k = old[i]; old[i] = 0; break; }
            if (!k) { path_join(full, n->path, fd.cFileName); k = node_new(full, fd.cFileName, isdir, n); }
            if (n->nk >= cap) { cap = cap * 2 + 16; n->kids = (Node **)realloc(n->kids, cap * sizeof(Node *)); }
            n->kids[n->nk++] = k;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    for (i = 0; i < nold; i++) if (old[i]) { if (s_sel == old[i]) s_sel = 0; node_free(old[i]); }
    free(old);
    if (n->nk) qsort(n->kids, n->nk, sizeof(Node *), node_cmp);
    n->loaded = 1;
    /* refresh open subfolders too */
    for (i = 0; i < n->nk; i++) {
        Node *k = n->kids[i];
        if (!k->isdir || !k->loaded) continue;
        if (k->open) node_load(k);
        else k->loaded = 0;
    }
}

static void add_vis(Node *n)
{
    int i;
    for (i = 0; i < n->nk; i++) {
        Node *k = n->kids[i];
        if (s_nvis >= s_vcap) { s_vcap = s_vcap * 2 + 64; s_vis = (Node **)realloc(s_vis, s_vcap * sizeof(Node *)); }
        s_vis[s_nvis++] = k;
        if (k->isdir && k->open) add_vis(k);
    }
}

static void rebuild(void)
{
    s_nvis = 0;
    if (s_root && s_rootOpen) add_vis(s_root);
    InvalidateRect(g_side, 0, 0);
}

void tree_set_root(const char *path)
{
    node_free(s_root);
    s_root = 0; s_sel = 0; s_top = 0;
    if (path && path[0]) {
        s_root = node_new(path, path_name(path), 1, 0);
        s_root->depth = -1;
        s_root->open = 1;
        node_load(s_root);
    }
    rebuild();
}

void tree_refresh(void)
{
    if (!s_root) return;
    node_load(s_root);
    rebuild();
}

static int tree_rows(void)
{
    RECT r; GetClientRect(g_side, &r);
    return (r.bottom - HDR_H - SEC_H) / ROW_H;
}

static void ensure_row(int i)
{
    int rows = tree_rows();
    if (i < s_top) s_top = i;
    if (i >= s_top + rows) s_top = i - rows + 1;
    if (s_top < 0) s_top = 0;
}

void tree_reveal(const char *path)
{
    int n = s_root ? lstrlenA(s_root->path) : 0, i;
    Node *cur = s_root;
    const char *p;
    if (!s_root || !path || !path[0] || _strnicmp(path, s_root->path, n) || (path[n] != '\\' && path[n] != '/')) return;
    p = path + n + 1;
    while (*p && cur) {
        char seg[MAX_PATH]; int k = 0; Node *next = 0;
        while (*p && *p != '\\' && *p != '/' && k < MAX_PATH - 1) seg[k++] = *p++;
        seg[k] = 0;
        if (*p) p++;
        if (!cur->loaded) node_load(cur);
        cur->open = 1;
        for (i = 0; i < cur->nk; i++) if (!lstrcmpiA(cur->kids[i]->name, seg)) { next = cur->kids[i]; break; }
        if (!next) { tree_refresh(); return; }
        cur = next;
    }
    s_sel = cur;
    rebuild();
    for (i = 0; i < s_nvis; i++) if (s_vis[i] == s_sel) { ensure_row(i); break; }
}

static int vis_index(Node *n) { int i; for (i = 0; i < s_nvis; i++) if (s_vis[i] == n) return i; return -1; }

static void toggle(Node *n)
{
    if (!n->isdir) return;
    n->open = !n->open;
    if (n->open && !n->loaded) node_load(n);
    rebuild();
}

/* ------------------------------------------------------------------ file operations */

static void target_dir(char *out)
{
    if (s_sel) { if (s_sel->isdir) lstrcpyA(out, s_sel->path); else path_dir(out, s_sel->path); }
    else if (s_root) lstrcpyA(out, s_root->path);
    else out[0] = 0;
}

static void new_file_cb(const char *name, int idx)
{
    char p[MAX_PATH], *q;
    HANDLE h;
    (void)idx;
    if (!name[0]) return;
    path_join(p, s_opPath, name);
    for (q = p + lstrlenA(s_opPath) + 1; *q; q++) if (*q == '/') *q = '\\';
    /* create missing parent folders: "src/util.c" works */
    for (q = p + lstrlenA(s_opPath) + 1; *q; q++) if (*q == '\\') { *q = 0; CreateDirectoryA(p, 0); *q = '\\'; }
    h = CreateFileA(p, GENERIC_WRITE, 0, 0, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) { if (!file_exists(p)) { MessageBoxA(g_main, "Could not create the file.", APPNAME, MB_ICONWARNING); return; } }
    else CloseHandle(h);
    tree_refresh(); files_invalidate();
    open_file(p, 0, 0);
}

static void new_dir_cb(const char *name, int idx)
{
    char p[MAX_PATH];
    (void)idx;
    if (!name[0]) return;
    path_join(p, s_opPath, name);
    if (!CreateDirectoryA(p, 0)) { MessageBoxA(g_main, "Could not create the folder.", APPNAME, MB_ICONWARNING); return; }
    tree_refresh();
    tree_reveal(p);
}

static void rename_cb(const char *name, int idx)
{
    char dir[MAX_PATH], np[MAX_PATH]; int i;
    (void)idx;
    if (!name[0] || !lstrcmpA(name, path_name(s_opPath))) return;
    path_dir(dir, s_opPath);
    path_join(np, dir, name);
    if (!MoveFileA(s_opPath, np)) { MessageBoxA(g_main, "Could not rename. The name may already exist.", APPNAME, MB_ICONWARNING); return; }
    for (i = 0; i < g_ndocs; i++) {
        Doc *d = g_docs[i]; int n = lstrlenA(s_opPath);
        if (!lstrcmpiA(d->path, s_opPath)) { lstrcpyA(d->path, np); lstrcpyA(d->name, name); doc_set_lang(d); }
        else if (!_strnicmp(d->path, s_opPath, n) && d->path[n] == '\\') {
            char t[MAX_PATH * 2]; wsprintfA(t, "%s%s", np, d->path + n); lstrcpynA(d->path, t, MAX_PATH);
        }
    }
    tree_refresh(); files_invalidate();
    tree_reveal(np);
    chrome_dirty();
}

typedef struct { HWND hwnd; UINT wFunc; LPCSTR pFrom; LPCSTR pTo; WORD fFlags; BOOL aborted; LPVOID map; LPCSTR title; } XSHFOP;
typedef int (WINAPI *PSHFileOp)(XSHFOP *);

static void delete_node(Node *n)
{
    char m[MAX_PATH + 100], from[MAX_PATH + 2];
    PSHFileOp op = (PSHFileOp)dyn("shell32.dll", "SHFileOperationA");
    XSHFOP f;
    wsprintfA(m, "Are you sure you want to delete '%s'%s?\n\nYou can restore it from the Recycle Bin.", n->name, n->isdir ? " and its contents" : "");
    if (MessageBoxA(g_main, m, APPNAME, MB_YESNO | MB_ICONWARNING) != IDYES) return;
    memset(from, 0, sizeof(from));
    lstrcpyA(from, n->path);
    memset(&f, 0, sizeof(f));
    f.hwnd = g_main; f.wFunc = 3; /* FO_DELETE */
    f.pFrom = from; f.fFlags = 0x40 | 0x10 | 0x400; /* ALLOWUNDO | NOCONFIRMATION | NOERRORUI */
    if (!op || op(&f)) {
        if (n->isdir ? !RemoveDirectoryA(n->path) : !DeleteFileA(n->path))
            MessageBoxA(g_main, "Could not delete it.", APPNAME, MB_ICONWARNING);
    }
    tree_refresh(); files_invalidate();
}

static void shell_open(const char *verb, const char *file, const char *args)
{
    typedef HINSTANCE (WINAPI *PSE)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
    PSE se = (PSE)dyn("shell32.dll", "ShellExecuteA");
    if (se) se(g_main, verb, file, args, 0, SW_SHOWNORMAL);
}

static void op(int cmd, Node *n)
{
    char a[MAX_PATH + 32];
    switch (cmd) {
    case 1: target_dir(s_opPath); if (s_opPath[0]) pal_input("New file name (folders with \\ are created too)", "", new_file_cb); break;
    case 2: target_dir(s_opPath); if (s_opPath[0]) pal_input("New folder name", "", new_dir_cb); break;
    case 3: if (n) { lstrcpyA(s_opPath, n->path); pal_input("Rename to", n->name, rename_cb); } break;
    case 4: if (n) delete_node(n); break;
    case 5: if (n) { wsprintfA(a, "/select,\"%s\"", n->path); shell_open("open", "explorer.exe", a); }
            else if (s_root) shell_open("open", s_root->path, 0); break;
    case 6: if (n) clip_set(n->path, lstrlenA(n->path)); break;
    case 7: if (n) { const char *r = rel_path(n->path); clip_set(r, lstrlenA(r)); } break;
    case 8: {
        char dir[MAX_PATH], c[MAX_PATH + 16];
        if (n && n->isdir) lstrcpyA(dir, n->path); else if (n) path_dir(dir, n->path); else if (s_root) lstrcpyA(dir, s_root->path); else break;
        set_panel(2);
        wsprintfA(c, "cd /d \"%s\"", dir);
        term_send(c, 1);
        break;
    }
    case 9: tree_refresh(); files_invalidate(); break;
    case 10: if (s_root) { int i; for (i = 0; i < s_root->nk; i++) s_root->kids[i]->open = 0; rebuild(); } break;
    case 11: if (n && !n->isdir) { open_file(n->path, 0, 0); run_cmd(CM_RUN); } break;
    case 12: run_cmd(CM_OPENFOLDER); break;
    }
}

static void context_menu(Node *n, int x, int y)
{
    HMENU m = CreatePopupMenu(); POINT p; int r;
    if (!s_root) { AppendMenuA(m, MF_STRING, 12, "Open Folder..."); }
    else {
        AppendMenuA(m, MF_STRING, 1, "New File...");
        AppendMenuA(m, MF_STRING, 2, "New Folder...");
        AppendMenuA(m, MF_STRING, 5, "Reveal in Windows Explorer");
        AppendMenuA(m, MF_STRING, 8, "Open in Integrated Terminal");
        if (n) {
            AppendMenuA(m, MF_SEPARATOR, 0, 0);
            if (!n->isdir) AppendMenuA(m, MF_STRING, 11, "Run File\tF5");
            AppendMenuA(m, MF_STRING, 6, "Copy Path");
            AppendMenuA(m, MF_STRING, 7, "Copy Relative Path");
            AppendMenuA(m, MF_SEPARATOR, 0, 0);
            AppendMenuA(m, MF_STRING, 3, "Rename...\tF2");
            AppendMenuA(m, MF_STRING, 4, "Delete\tDel");
        }
        AppendMenuA(m, MF_SEPARATOR, 0, 0);
        AppendMenuA(m, MF_STRING, 9, "Refresh");
        AppendMenuA(m, MF_STRING, 10, "Collapse Folders");
    }
    p.x = x; p.y = y; ClientToScreen(g_side, &p);
    r = TrackPopupMenu(m, TPM_RETURNCMD, p.x, p.y, 0, g_side, 0);
    DestroyMenu(m);
    if (r) op(r, n);
}

/* ------------------------------------------------------------------ search */

static void sres_clear(void)
{
    int i;
    for (i = 0; i < s_nres; i++) free(s_res[i].text);
    for (i = 0; i < s_nsf; i++) free(s_sf[i].path);
    s_nres = s_nsf = 0; s_sTop = 0; s_sSel = -1; s_truncated = 0;
}

#define ISWC(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || ((c) >= '0' && (c) <= '9') || (c) == '_')

static int find_in(const char *s, int n, const char *q, int qn, int from)
{
    int i, k;
    for (i = from; i + qn <= n; i++) {
        for (k = 0; k < qn; k++) {
            char a = s[i+k], b = q[k];
            if (!s_sCase) { if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32; }
            if (a != b) break;
        }
        if (k < qn) continue;
        if (s_sWord && ((i > 0 && ISWC(s[i-1])) || (i + qn < n && ISWC(s[i+qn])))) continue;
        return i;
    }
    return -1;
}

static void search_file(const char *path)
{
    char *buf; int len, i, start = 0, line = 0, qn = lstrlenA(s_sQuery), fi = -1;
    if (!read_file(path, &buf, &len)) return;
    if (len > 4 * 1024 * 1024 || memchr(buf, 0, len < 8000 ? len : 8000)) { free(buf); return; }
    for (i = 0; i <= len && s_nres < 5000; i++) {
        if (i == len || buf[i] == '\n') {
            int e = i, c;
            if (e > start && buf[e-1] == '\r') e--;
            c = find_in(buf + start, e - start, s_sQuery, qn, 0);
            if (c >= 0) {
                SRes *r; int ts = start, te = e;
                if (fi < 0) {
                    if (s_nsf >= s_sfcap) { s_sfcap = s_sfcap * 2 + 32; s_sf = (SFile *)realloc(s_sf, s_sfcap * sizeof(SFile)); }
                    fi = s_nsf++;
                    s_sf[fi].path = xstrdup(path); s_sf[fi].count = 0; s_sf[fi].collapsed = 0;
                }
                if (s_nres >= s_rcap) { s_rcap = s_rcap * 2 + 256; s_res = (SRes *)realloc(s_res, s_rcap * sizeof(SRes)); }
                r = &s_res[s_nres++];
                r->file = fi; r->line = line; r->col = c; r->len = qn;
                while (ts < start + c && (buf[ts] == ' ' || buf[ts] == '\t')) ts++;
                if (start + c - ts > 30) ts = start + c - 30;
                if (te - ts > 220) te = ts + 220;
                r->text = (char *)malloc(te - ts + 1);
                memcpy(r->text, buf + ts, te - ts); r->text[te - ts] = 0;
                { char *q; for (q = r->text; *q; q++) if (*q == '\t') *q = ' '; }
                r->col = start + c - ts;
                s_sf[fi].count++;
            }
            start = i + 1; line++;
        }
    }
    if (s_nres >= 5000) s_truncated = 1;
    free(buf);
}

static void search_dir(const char *dir, int depth)
{
    WIN32_FIND_DATAA fd; HANDLE h; char pat[MAX_PATH], full[MAX_PATH];
    if (depth > 20 || s_nres >= 5000) return;
    path_join(pat, dir, "*");
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.' && (!fd.cFileName[1] || fd.cFileName[1] == '.')) continue;
        path_join(full, dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (fd.cFileName[0] == '.' || skip_build_dir(fd.cFileName)) continue;
            search_dir(full, depth + 1);
        } else {
            const char *e = strrchr(fd.cFileName, '.');
            if (e && (!lstrcmpiA(e, ".exe") || !lstrcmpiA(e, ".dll") || !lstrcmpiA(e, ".obj") || !lstrcmpiA(e, ".o") ||
                      !lstrcmpiA(e, ".ico") || !lstrcmpiA(e, ".png") || !lstrcmpiA(e, ".zip") || !lstrcmpiA(e, ".pdb"))) continue;
            search_file(full);
        }
    } while (FindNextFileA(h, &fd) && s_nres < 5000);
    FindClose(h);
}

static void run_search(void)
{
    HCURSOR old;
    sres_clear();
    GetWindowTextA(s_q, s_sQuery, sizeof(s_sQuery));
    if (!s_sQuery[0] || !g_folder[0]) { InvalidateRect(g_side, 0, 0); return; }
    old = SetCursor(LoadCursor(0, IDC_WAIT));
    search_dir(g_folder, 0);
    SetCursor(old);
    InvalidateRect(g_side, 0, 0);
}

/* the flat list of rows shown: file headers and their matches */
static int srow_count(void)
{
    int i, n = s_nsf;
    for (i = 0; i < s_nsf; i++) if (!s_sf[i].collapsed) n += s_sf[i].count;
    return n;
}

static int srow_get(int row, int *isFile)
{
    int i, r = 0, k = 0;
    for (i = 0; i < s_nsf; i++) {
        if (r == row) { *isFile = 1; return i; }
        r++;
        if (!s_sf[i].collapsed) {
            if (row < r + s_sf[i].count) { *isFile = 0; while (s_res[k].file != i) k++; return k + (row - r); }
            r += s_sf[i].count;
        }
        while (k < s_nres && s_res[k].file == i) k++;
    }
    *isFile = -1;
    return -1;
}

static int s_list_top(void) { return HDR_H + 44; }

static LRESULT CALLBACK QProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    if (m == WM_KEYDOWN && wp == VK_RETURN) { run_search(); return 0; }
    if (m == WM_KEYDOWN && wp == VK_ESCAPE) { focus_editor(); return 0; }
    if (m == WM_KEYDOWN && wp == VK_DOWN && s_nres) { s_sSel = 0; SetFocus(g_side); InvalidateRect(g_side, 0, 0); return 0; }
    if (m == WM_CHAR && (wp == '\r' || wp == 27)) return 0;
    if (m == WM_CHAR && wp == 1) { SendMessageA(w, EM_SETSEL, 0, -1); return 0; }
    return CallWindowProcA(s_oldQ, w, m, wp, lp);
}

void search_focus(void)
{
    if (!s_q) return;
    {
        Doc *d = cur_doc();
        if (d && doc_has_sel(d) && d->cl == d->al) { int n; char *t = doc_sel_text(d, &n); if (n < 200) SetWindowTextA(s_q, t); free(t); }
    }
    SetFocus(s_q);
    SendMessageA(s_q, EM_SETSEL, 0, -1);
}

static void open_result(int row)
{
    int isFile, i = srow_get(row, &isFile);
    if (i < 0) return;
    if (isFile == 1) { s_sf[i].collapsed = !s_sf[i].collapsed; InvalidateRect(g_side, 0, 0); return; }
    if (open_file(s_sf[s_res[i].file].path, s_res[i].line + 1, 0)) {
        Doc *d = cur_doc();
        char *t = s_res[i].text; int l = s_res[i].line;
        /* select the match: find it on the line by text */
        int c = d->ln[l].len >= 0 ? -1 : -1, k, qn = lstrlenA(s_sQuery);
        for (k = 0; k + qn <= d->ln[l].len; k++) if (find_in(d->ln[l].s, d->ln[l].len, s_sQuery, qn, k) == k) { c = k; break; }
        (void)t;
        if (c >= 0) { d->al = l; d->ac = c; d->cl = l; d->cc = c + qn; ed_scroll_to_caret(); }
    }
}

/* ------------------------------------------------------------------ painting */

static void paint_tree(HDC dc, int W, int H)
{
    int i, y, rows;
    char title[MAX_PATH];
    int focused = GetFocus() == g_side;
    /* section header: folder name */
    fill(dc, 0, HDR_H, W, SEC_H, C_SIDE);
    SelectObject(dc, g_uiBold);
    if (s_root) {
        lstrcpynA(title, s_root->name, sizeof(title)); CharUpperA(title);
        draw_icon(dc, s_rootOpen ? IC_CHEVD : IC_CHEVR, 2, HDR_H + 3, C_TEXT);
        ui_text(dc, 20, HDR_H + (SEC_H - g_uiH) / 2, title, XRGB(0xCC,0xCC,0xCC));
        for (i = 0; i < 4; i++) {
            int x = W - SB_W - 4 - (4 - i) * 22;
            SetRect(&s_secBtn[i], x, HDR_H + 1, x + 20, HDR_H + SEC_H - 1);
            draw_icon(dc, i == 0 ? IC_NEWFILE : i == 1 ? IC_NEWDIR : i == 2 ? IC_REFRESH : IC_COLLAPSE, x + 2, HDR_H + 3, XRGB(0xC5,0xC5,0xC5));
        }
    } else {
        ui_text(dc, 20, HDR_H + (SEC_H - g_uiH) / 2, "NO FOLDER OPENED", XRGB(0xCC,0xCC,0xCC));
        SelectObject(dc, g_ui);
        ui_text(dc, 20, HDR_H + SEC_H + 14, "You have not yet opened a folder.", C_TEXT);
        fill(dc, 20, HDR_H + SEC_H + 44, W - 40, 26, XRGB(0x0E,0x63,0x9C));
        { SIZE z; GetTextExtentPoint32A(dc, "Open Folder", 11, &z); ui_text(dc, W / 2 - z.cx / 2, HDR_H + SEC_H + 44 + (26 - g_uiH) / 2, "Open Folder", C_WHITE); }
        SetRect(&s_secBtn[0], 20, HDR_H + SEC_H + 44, W - 20, HDR_H + SEC_H + 70);
        return;
    }
    SelectObject(dc, g_ui);
    rows = (H - HDR_H - SEC_H) / ROW_H + 1;
    if (s_top > s_nvis - rows + 1) s_top = s_nvis - rows + 1;
    if (s_top < 0) s_top = 0;
    for (i = 0; i < rows && s_top + i < s_nvis; i++) {
        Node *n = s_vis[s_top + i];
        int x = 8 + (n->depth) * 8 + 8 * (n->depth > 0 ? 1 : 0), g;
        Doc *d = cur_doc();
        COLORREF tc = C_TEXT;
        y = HDR_H + SEC_H + i * ROW_H;
        x = 8 + n->depth * 12;
        if (n == s_sel) {
            fill(dc, 0, y, W - SB_W, ROW_H, focused ? C_LISTFOC : C_LISTSEL);
            if (focused) { HPEN p = CreatePen(PS_SOLID, 1, C_FOCUS), op = (HPEN)SelectObject(dc, p); HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, 0, y, W - SB_W, y + ROW_H); SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(p); }
        } else if (s_top + i == s_hover) fill(dc, 0, y, W - SB_W, ROW_H, C_LISTHOV);
        for (g = 1; g <= n->depth; g++) vline(dc, 8 + (g - 1) * 12 + 7, y, ROW_H, XRGB(0x45,0x45,0x45));
        if (n->isdir) draw_icon(dc, n->open ? IC_CHEVD : IC_CHEVR, x, y + 3, C_TEXT);
        else file_glyph(dc, n->name, 0, x + 2, y + (ROW_H - g_uiH) / 2);
        if (d && !lstrcmpiA(d->path, n->path) && doc_dirty(d)) tc = XRGB(0xE2,0xC0,0x8D);
        ui_text(dc, x + 20, y + (ROW_H - g_uiH) / 2, n->name, tc);
    }
    s_vs.r.left = W - SB_W; s_vs.r.top = HDR_H + SEC_H; s_vs.r.right = W; s_vs.r.bottom = H;
    s_vs.page = rows - 1; s_vs.max = s_nvis; s_vs.pos = s_top;
    vs_draw(dc, &s_vs);
}

static void paint_search(HDC dc, int W, int H)
{
    int y, i, rows, total;
    char buf[300];
    fill(dc, 8, HDR_H, W - 16 - 52, g_uiH + 10, C_INPUT);
    for (i = 0; i < 2; i++) {
        int on = i ? s_sWord : s_sCase, x = W - 8 - 48 + i * 24;
        SetRect(&s_sBtn[i], x, HDR_H + 1, x + 22, HDR_H + g_uiH + 9);
        if (on) fill(dc, x, HDR_H, 22, g_uiH + 10, XRGB(0x2B,0x4E,0x6E));
        SelectObject(dc, g_ui);
        ui_text(dc, x + 4, HDR_H + 5, i ? "ab" : "Aa", on ? C_WHITE : C_DIM);
        if (i) hline(dc, x + 4, HDR_H + g_uiH + 5, 13, on ? C_WHITE : C_DIM);
    }
    SelectObject(dc, g_ui);
    if (!g_folder[0]) { ui_text(dc, 12, HDR_H + 32, "Open a folder to search in files.", C_DIM); return; }
    if (s_sQuery[0]) {
        wsprintfA(buf, s_nres ? "%d result%s in %d file%s%s" : "No results found.", s_nres, s_nres == 1 ? "" : "s", s_nsf, s_nsf == 1 ? "" : "s", s_truncated ? " (stopped at 5000)" : "");
        ui_text(dc, 12, HDR_H + 26, buf, C_DIM);
    } else ui_text(dc, 12, HDR_H + 26, "Type and press Enter to search the folder.", C_DIM);
    total = srow_count();
    rows = (H - s_list_top()) / ROW_H + 1;
    if (s_sTop > total - rows + 1) s_sTop = total - rows + 1;
    if (s_sTop < 0) s_sTop = 0;
    for (i = 0; i < rows; i++) {
        int isFile, k = srow_get(s_sTop + i, &isFile);
        if (k < 0) break;
        y = s_list_top() + i * ROW_H;
        if (s_sTop + i == s_sSel) fill(dc, 0, y, W - SB_W, ROW_H, GetFocus() == g_side ? C_LISTFOC : C_LISTSEL);
        if (isFile) {
            SFile *f = &s_sf[k]; char dir[MAX_PATH]; SIZE z;
            const char *nm = path_name(f->path);
            draw_icon(dc, f->collapsed ? IC_CHEVR : IC_CHEVD, 4, y + 3, C_TEXT);
            file_glyph(dc, nm, 0, 20, y + (ROW_H - g_uiH) / 2);
            ui_text(dc, 40, y + (ROW_H - g_uiH) / 2, nm, C_TEXT);
            GetTextExtentPoint32A(dc, nm, lstrlenA(nm), &z);
            path_dir(dir, f->path);
            ui_text(dc, 46 + z.cx, y + (ROW_H - g_uiH) / 2, rel_path(dir) == dir ? "" : rel_path(dir), C_DIM);
            wsprintfA(buf, "%d", f->count);
            GetTextExtentPoint32A(dc, buf, lstrlenA(buf), &z);
            fill(dc, W - SB_W - z.cx - 18, y + 3, z.cx + 10, ROW_H - 6, XRGB(0x4D,0x4D,0x4D));
            ui_text(dc, W - SB_W - z.cx - 13, y + (ROW_H - g_uiH) / 2, buf, C_WHITE);
        } else {
            SRes *r = &s_res[k]; SIZE a, b; int x = 40;
            GetTextExtentPoint32A(dc, r->text, r->col, &a);
            GetTextExtentPoint32A(dc, r->text + r->col, r->len, &b);
            fill(dc, x + a.cx, y + 3, b.cx, ROW_H - 6, XRGB(0x62,0x33,0x15));
            SetTextColor(dc, C_TEXT); SetBkMode(dc, TRANSPARENT);
            TextOutA(dc, x, y + (ROW_H - g_uiH) / 2, r->text, lstrlenA(r->text));
        }
    }
    s_svs.r.left = W - SB_W; s_svs.r.top = s_list_top(); s_svs.r.right = W; s_svs.r.bottom = H;
    s_svs.page = rows - 1; s_svs.max = total; s_svs.pos = s_sTop;
    vs_draw(dc, &s_svs);
}

static void paint_run(HDC dc, int W)
{
    static const char *labels[2] = { "Run Active File  (F5)", "Run build.bat  (Ctrl+Shift+B)" };
    static const char *help[] = {
        "F5 runs the file in the editor in the", "terminal:",
        "  .c    compiled with TCC, then run", "  .py   python -u", "  .bat  call", "  .js   cscript //nologo", "",
        "Ctrl+Shift+B runs build.bat in the", "open folder. Errors land in Problems;", "F8 jumps to the next one.", "",
        "Shift+F5 sends Ctrl+C to the terminal.", 0 };
    int i, y = HDR_H + 8;
    SelectObject(dc, g_ui);
    for (i = 0; i < 2; i++) {
        SIZE z;
        SetRect(&s_runBtn[i], 16, y, W - 16, y + 28);
        fill(dc, 16, y, W - 32, 28, i == 0 ? XRGB(0x0E,0x63,0x9C) : XRGB(0x3A,0x3D,0x41));
        GetTextExtentPoint32A(dc, labels[i], lstrlenA(labels[i]), &z);
        ui_text(dc, W / 2 - z.cx / 2, y + (28 - g_uiH) / 2, labels[i], C_WHITE);
        y += 38;
    }
    y += 8;
    for (i = 0; help[i]; i++) { ui_text(dc, 16, y, help[i], C_DIM); y += g_uiH + 4; }
}

static void side_paint(HWND w)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(w, &ps), dc;
    RECT r; HGDIOBJ of;
    static const char *titles[3] = { "EXPLORER", "SEARCH", "RUN AND DEBUG" };
    GetClientRect(w, &r);
    dc = bb_begin(w, wdc, r.right, r.bottom);
    of = SelectObject(dc, g_ui);
    SetBkMode(dc, TRANSPARENT);
    fill(dc, 0, 0, r.right, r.bottom, C_SIDE);
    ui_text(dc, 20, (HDR_H - g_uiH) / 2, titles[g_sideMode], XRGB(0xBB,0xBB,0xBB));
    if (g_sideMode == 0) paint_tree(dc, r.right, r.bottom);
    else if (g_sideMode == 1) paint_search(dc, r.right, r.bottom);
    else paint_run(dc, r.right);
    SelectObject(dc, of);
    BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    
    EndPaint(w, &ps);
}

void side_layout(void)
{
    RECT r;
    if (!g_side) return;
    GetClientRect(g_side, &r);
    if (s_q) {
        MoveWindow(s_q, 12, HDR_H + 5, r.right - 24 - 52, g_uiH + 2, TRUE);
        ShowWindow(s_q, g_sideMode == 1 ? SW_SHOW : SW_HIDE);
    }
    InvalidateRect(g_side, 0, 0);
}

/* ------------------------------------------------------------------ input */

static void tree_key(int vk)
{
    int i = vis_index(s_sel), rows = tree_rows();
    Node *n = s_sel;
    if (!s_nvis) return;
    switch (vk) {
    case VK_UP: i = i <= 0 ? 0 : i - 1; break;
    case VK_DOWN: i = i < 0 ? 0 : (i + 1 < s_nvis ? i + 1 : i); break;
    case VK_PRIOR: i -= rows; if (i < 0) i = 0; break;
    case VK_NEXT: i += rows; if (i >= s_nvis) i = s_nvis - 1; break;
    case VK_HOME: i = 0; break;
    case VK_END: i = s_nvis - 1; break;
    case VK_RIGHT: if (n && n->isdir) { if (!n->open) toggle(n); else if (n->nk) i++; } break;
    case VK_LEFT: if (n && n->isdir && n->open) toggle(n); else if (n && n->parent && n->parent != s_root) i = vis_index(n->parent); break;
    case VK_RETURN: case VK_SPACE: if (n) { if (n->isdir) toggle(n); else open_file(n->path, 0, 0); } return;
    case VK_F2: op(3, n); return;
    case VK_DELETE: if (n) op(4, n); return;
    case VK_ESCAPE: focus_editor(); return;
    case VK_APPS: context_menu(n, 40, (i - s_top + 1) * ROW_H + HDR_H + SEC_H); return;
    default: return;
    }
    if (i >= 0 && i < s_nvis) { s_sel = s_vis[i]; ensure_row(i); }
    InvalidateRect(g_side, 0, 0);
}

static void search_key(int vk)
{
    int total = srow_count(), rows = (100) ;
    RECT r; GetClientRect(g_side, &r);
    rows = (r.bottom - s_list_top()) / ROW_H;
    switch (vk) {
    case VK_UP: if (s_sSel > 0) s_sSel--; else { SetFocus(s_q); return; } break;
    case VK_DOWN: if (s_sSel < total - 1) s_sSel++; break;
    case VK_RETURN: open_result(s_sSel); SetFocus(g_side); return;
    case VK_ESCAPE: focus_editor(); return;
    default: return;
    }
    if (s_sSel < s_sTop) s_sTop = s_sSel;
    if (s_sSel >= s_sTop + rows) s_sTop = s_sSel - rows + 1;
    InvalidateRect(g_side, 0, 0);
}

static LRESULT CALLBACK SideProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE:
        s_q = CreateWindowExA(0, "EDIT", "", WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, w, 0, g_inst, 0);
        SendMessageA(s_q, WM_SETFONT, (WPARAM)g_ui, 0);
        s_oldQ = (WNDPROC)SetWindowLongA(s_q, GWL_WNDPROC, (LONG)(LONG_PTR)QProc);
        return 0;
    case WM_SIZE: side_layout(); return 0;
    case WM_PAINT: side_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS: case WM_KILLFOCUS: InvalidateRect(w, 0, 0); return 0;
    case WM_CTLCOLOREDIT: SetTextColor((HDC)wp, C_TEXT); SetBkColor((HDC)wp, C_INPUT); return (LRESULT)g_brInput;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (g_sideMode == 0) tree_key((int)wp);
        else if (g_sideMode == 1) search_key((int)wp);
        return 0;
    case WM_MOUSEWHEEL: {
        int d = -(short)HIWORD(wp) / 120 * 3;
        if (g_sideMode == 0) s_top += d; else if (g_sideMode == 1) s_sTop += d;
        if (s_top < 0) s_top = 0;
        if (s_sTop < 0) s_sTop = 0;
        InvalidateRect(w, 0, 0);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), h;
        if (s_vs.drag) { vs_mouse(&s_vs, w, m, x, y); s_top = s_vs.pos; InvalidateRect(w, 0, 0); return 0; }
        if (s_svs.drag) { vs_mouse(&s_svs, w, m, x, y); s_sTop = s_svs.pos; InvalidateRect(w, 0, 0); return 0; }
        h = (g_sideMode == 0 && y >= HDR_H + SEC_H) ? s_top + (y - HDR_H - SEC_H) / ROW_H : -1;
        if (h >= s_nvis) h = -1;
        if (h != s_hover) {
            TRACKMOUSEEVENT t; t.cbSize = sizeof(t); t.dwFlags = TME_LEAVE; t.hwndTrack = w; t.dwHoverTime = 0; TrackMouseEvent(&t);
            s_hover = h; InvalidateRect(w, 0, 0);
        }
        return 0;
    }
    case WM_MOUSELEAVE: s_hover = -1; InvalidateRect(w, 0, 0); return 0;
    case WM_LBUTTONUP:
        if (s_vs.drag) vs_mouse(&s_vs, w, m, 0, 0);
        if (s_svs.drag) vs_mouse(&s_svs, w, m, 0, 0);
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), i;
        POINT p; p.x = x; p.y = y;
        if (g_sideMode == 0) {
            if (m != WM_RBUTTONDOWN && vs_mouse(&s_vs, w, m, x, y)) { s_top = s_vs.pos; InvalidateRect(w, 0, 0); return 0; }
            SetFocus(w);
            if (!s_root) { if (PtInRect(&s_secBtn[0], p) || m == WM_RBUTTONDOWN) { if (m == WM_RBUTTONDOWN) context_menu(0, x, y); else run_cmd(CM_OPENFOLDER); } return 0; }
            if (y >= HDR_H && y < HDR_H + SEC_H) {
                for (i = 0; i < 4; i++) if (PtInRect(&s_secBtn[i], p)) { s_sel = s_sel; op(i == 0 ? 1 : i == 1 ? 2 : i == 2 ? 9 : 10, s_sel); return 0; }
                s_rootOpen = !s_rootOpen; rebuild(); return 0;
            }
            if (y < HDR_H) return 0;
            i = s_top + (y - HDR_H - SEC_H) / ROW_H;
            if (i >= 0 && i < s_nvis) {
                Node *n = s_vis[i];
                s_sel = n;
                if (m == WM_RBUTTONDOWN) { InvalidateRect(w, 0, 0); UpdateWindow(w); context_menu(n, x, y); return 0; }
                if (n->isdir) toggle(n);
                else { open_file(n->path, 0, 0); if (m == WM_LBUTTONDOWN) SetFocus(g_edit); }
            } else {
                s_sel = 0;
                if (m == WM_RBUTTONDOWN) context_menu(0, x, y);
            }
            InvalidateRect(w, 0, 0);
        } else if (g_sideMode == 1) {
            if (m != WM_RBUTTONDOWN && vs_mouse(&s_svs, w, m, x, y)) { s_sTop = s_svs.pos; InvalidateRect(w, 0, 0); return 0; }
            for (i = 0; i < 2; i++) if (PtInRect(&s_sBtn[i], p)) { if (i) s_sWord = !s_sWord; else s_sCase = !s_sCase; run_search(); return 0; }
            if (y >= s_list_top()) {
                SetFocus(w);
                s_sSel = s_sTop + (y - s_list_top()) / ROW_H;
                if (s_sSel >= srow_count()) s_sSel = -1;
                InvalidateRect(w, 0, 0);
                if (s_sSel >= 0) open_result(s_sSel);
            }
        } else {
            for (i = 0; i < 2; i++) if (PtInRect(&s_runBtn[i], p)) { run_cmd(i ? CM_BUILD : CM_RUN); return 0; }
        }
        return 0;
    }
    }
    return DefWindowProcA(w, m, wp, lp);
}

void side_register(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = SideProc; wc.hInstance = g_inst; wc.lpszClassName = "XPSide";
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    RegisterClassA(&wc);
}
