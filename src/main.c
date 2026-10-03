/* main.c - the workbench: main window, activity bar, tabs, breadcrumbs,
 * status bar, splitters, menus, key bindings, commands and session state. */
#include "xpcode.h"

#define ACT_W 48
#define TAB_H 35
#define CRUMB_H 22
#define PHDR_H 35
#define STATUS_H 22

HINSTANCE g_inst;
HWND g_main, g_edit, g_side, g_term, g_prob, g_find, g_pal;
HFONT g_ui, g_uiBold, g_uiBig, g_mono, g_monoMini;
int g_cw = 8, g_lh = 16, g_uiH = 13;
int g_zoom, g_minimap = 1;
char g_folder[MAX_PATH], g_exeDir[MAX_PATH], g_ini[MAX_PATH];
int g_sideVis = 1, g_sideMode, g_panelVis = 1, g_panelMode = 2, g_sideW = 250, g_panelH = 230;
HBRUSH g_brInput, g_brSide, g_brBg;
Doc *g_docs[MAXDOCS];
int g_ndocs, g_cur = -1;
int g_ninit;

static char s_monoFace[64];
static RECT s_tabsR, s_crumbR, s_phdrR, s_statusR, s_edR;
static int s_tabX[MAXDOCS], s_tabW[MAXDOCS], s_tabScroll, s_hoverTab = -1, s_hoverClose = -1;
static int s_split, s_chord, s_untitled;
static RECT s_stItem[8]; static int s_stCmd[8], s_nst;
static RECT s_phTab[3], s_phBtn[5]; static int s_phCmd[5], s_nph;
static HANDLE s_watch = INVALID_HANDLE_VALUE;

/* ------------------------------------------------------------------ helpers */

void *xalloc(int n) { void *p = calloc(1, n); if (!p) { MessageBoxA(0, "Out of memory", APPNAME, MB_ICONERROR); ExitProcess(1); } return p; }
char *xstrdup(const char *s) { int n = lstrlenA(s); char *p = (char *)malloc(n + 1); memcpy(p, s, n + 1); return p; }
int file_exists(const char *p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
int dir_exists(const char *p) { DWORD a = GetFileAttributesA(p); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }

void path_join(char *out, const char *a, const char *b)
{
    char t[MAX_PATH * 2];
    int n = lstrlenA(a);
    if (n && a[n-1] != '\\' && a[n-1] != '/') wsprintfA(t, "%s\\%s", a, b);
    else wsprintfA(t, "%s%s", a, b);
    lstrcpynA(out, t, MAX_PATH);
}

const char *path_name(const char *p)
{
    const char *a = strrchr(p, '\\'), *b = strrchr(p, '/');
    if (b > a) a = b;
    return a ? a + 1 : p;
}

void path_dir(char *out, const char *p)
{
    const char *n = path_name(p);
    int k = (int)(n - p);
    if (k > 0) k--;
    if (k == 2 && p[1] == ':') k = 3;
    memcpy(out, p, k); out[k] = 0;
}

int read_file(const char *path, char **buf, int *len)
{
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0, OPEN_EXISTING, 0, 0);
    DWORD sz, rd;
    if (h == INVALID_HANDLE_VALUE) return 0;
    sz = GetFileSize(h, 0);
    if (sz == INVALID_FILE_SIZE || sz > 64 * 1024 * 1024) { CloseHandle(h); return 0; }
    *buf = (char *)malloc(sz + 1);
    if (!ReadFile(h, *buf, sz, &rd, 0)) rd = 0;
    CloseHandle(h);
    (*buf)[rd] = 0;
    *len = (int)rd;
    return 1;
}

int skip_build_dir(const char *n)
{
    static const char *dirs[] = { "node_modules", "build", "dist", "__pycache__", "target", "obj", 0 };
    int i;
    for (i = 0; dirs[i]; i++) if (!lstrcmpiA(n, dirs[i])) return 1;
    return 0;
}


FARPROC dyn(const char *dll, const char *fn)
{
    HMODULE m = GetModuleHandleA(dll);
    if (!m) m = LoadLibraryA(dll);
    return m ? GetProcAddress(m, fn) : 0;
}

int clip_set(const char *s, int n)
{
    HGLOBAL g;
    if (!OpenClipboard(g_main)) return 0;
    EmptyClipboard();
    g = GlobalAlloc(GMEM_MOVEABLE, n + 1);
    if (g) {
        char *p = (char *)GlobalLock(g);
        memcpy(p, s, n); p[n] = 0;
        GlobalUnlock(g);
        SetClipboardData(CF_TEXT, g);
    }
    CloseClipboard();
    return 1;
}

char *clip_get(void)
{
    HANDLE h; char *r = 0;
    if (!OpenClipboard(g_main)) return 0;
    h = GetClipboardData(CF_TEXT);
    if (h) { char *p = (char *)GlobalLock(h); if (p) { r = xstrdup(p); GlobalUnlock(h); } }
    CloseClipboard();
    return r;
}

int key_mods(void)
{
    return ((GetKeyState(VK_CONTROL) & 0x8000) ? M_CTRL : 0) |
           ((GetKeyState(VK_SHIFT) & 0x8000) ? M_SHIFT : 0) |
           ((GetKeyState(VK_MENU) & 0x8000) ? M_ALT : 0);
}

const char *rel_path(const char *full)
{
    int n = lstrlenA(g_folder);
    if (n && !_strnicmp(full, g_folder, n) && (full[n] == '\\' || full[n] == '/')) return full + n + 1;
    return full;
}

Doc *cur_doc(void) { return (g_cur >= 0 && g_cur < g_ndocs) ? g_docs[g_cur] : 0; }

/* ------------------------------------------------------------------ drawing helpers */

void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r; r.left = x; r.top = y; r.right = x + w; r.bottom = y + h;
    SetBkColor(dc, c);
    ExtTextOutA(dc, 0, 0, ETO_OPAQUE, &r, 0, 0, 0);
}
void hline(HDC dc, int x, int y, int w, COLORREF c) { fill(dc, x, y, w, 1, c); }
void vline(HDC dc, int x, int y, int h, COLORREF c) { fill(dc, x, y, 1, h, c); }

void ui_text(HDC dc, int x, int y, const char *s, COLORREF c)
{
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    TextOutA(dc, x, y, s, lstrlenA(s));
}




static void disc(HDC dc, int x, int y, int d, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c), ob = (HBRUSH)SelectObject(dc, b);
    HPEN p = CreatePen(PS_SOLID, 1, c), op = (HPEN)SelectObject(dc, p);
    Ellipse(dc, x, y, x + d, y + d);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(b); DeleteObject(p);
}

/* icons are tiny vector programs: L line, T thick line, P n points polyline,
 * E ellipse outline, D filled disc, 0 end. 16px boxes; activity icons 24px. */
static const unsigned char
  ic_files[] = {'P',6,9,6,9,2,16,2,20,6,20,17,15,17,'L',16,2,16,6,'L',16,6,20,6,'P',6,4,6,11,6,15,10,15,21,4,21,4,6,0},
  ic_search[] = {'E',8,2,21,15,'T',10,14,3,21,0},
  ic_run[] = {'P',4,6,3,20,12,6,21,6,3,0},
  ic_gear[] = {'E',7,7,18,18,'E',10,10,15,15,'T',12,6,12,3,'T',12,18,12,21,'T',6,12,3,12,'T',18,12,21,12,
               'T',8,8,6,6,'T',16,8,18,6,'T',8,16,6,18,'T',16,16,18,18,0},
  ic_close[] = {'L',4,4,12,12,'L',11,4,3,12,0},
  ic_plus[] = {'L',8,3,8,14,'L',3,8,14,8,0},
  ic_trash[] = {'P',5,4,5,12,5,12,14,4,14,4,5,'L',2,4,15,4,'L',6,2,11,2,'L',7,7,7,12,'L',9,7,9,12,0},
  ic_chevr[] = {'P',3,6,4,10,8,5,13,0},
  ic_chevd[] = {'P',3,4,6,8,10,13,5,0},
  ic_up[] = {'L',8,4,8,14,'P',3,4,8,8,4,13,9,0},
  ic_down[] = {'L',8,3,8,12,'P',3,4,8,8,12,13,7,0},
  ic_max[] = {'P',3,4,10,8,6,13,11,0},
  ic_dot[] = {'D',4,4,8,0},
  ic_refresh[] = {'P',10,13,6,11,3,8,2,5,3,3,6,3,10,5,13,8,14,11,13,13,10,'L',13,2,13,6,'L',13,6,9,6,0},
  ic_newfile[] = {'P',4,9,2,3,2,3,14,9,14,'L',9,2,13,6,'L',13,6,13,8,'L',12,9,12,16,'L',9,12,16,12,0},
  ic_newdir[] = {'P',7,9,13,1,13,1,3,6,3,7,5,14,5,14,8,'L',12,9,12,16,'L',9,12,16,12,0},
  ic_collapse[] = {'P',5,2,2,12,2,12,12,2,12,2,2,'L',5,7,10,7,'L',14,4,14,15,'L',4,14,15,14,0};
static const unsigned char *s_icons[] = { ic_files, ic_search, ic_run, ic_gear, ic_close, ic_plus, ic_trash,
  ic_chevr, ic_chevd, 0, 0, ic_up, ic_down, ic_max, ic_dot, ic_refresh, ic_newfile, ic_newdir, ic_collapse };

void draw_icon(HDC dc, int k, int x, int y, COLORREF c)
{
    POINT pt[12]; int i, n;
    const unsigned char *p = s_icons[k];
    HPEN p1, p2; HGDIOBJ op, ob;
    if (k == IC_ERR) {
        disc(dc, x+1, y+1, 14, XRGB(0xF1,0x4C,0x4C));
        p = ic_close; c = C_BG;
    } else if (k == IC_WARN) {
        for (i = 0; i < 13; i++) hline(dc, x + 8 - i / 2 - 1, y + 2 + i, i + 2, XRGB(0xCC,0xA7,0x00));
        fill(dc, x + 7, y + 5, 2, 5, C_BG); fill(dc, x + 7, y + 11, 2, 2, C_BG);
        return;
    }
    p1 = CreatePen(PS_SOLID, 1, c); p2 = CreatePen(PS_SOLID, 2, c);
    op = SelectObject(dc, p1); ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    while (*p) {
        int op2 = *p++;
        if (op2 == 'D') { disc(dc, x + p[0], y + p[1], p[2], c); p += 3; continue; }
        SelectObject(dc, op2 == 'T' || k == IC_ERR ? p2 : p1);
        if (op2 == 'E') { Ellipse(dc, x + p[0], y + p[1], x + p[2], y + p[3]); p += 4; continue; }
        n = op2 == 'P' ? *p++ : 2;
        for (i = 0; i < n; i++) { pt[i].x = x + *p++; pt[i].y = y + *p++; }
        Polyline(dc, pt, n);
    }
    SelectObject(dc, op); SelectObject(dc, ob);
    DeleteObject(p1); DeleteObject(p2);
}

#define act_icon draw_icon

void file_glyph(HDC dc, const char *name, int isdir, int x, int y)
{
    const char *e = strrchr(name, '.'), *g = "=";
    COLORREF c = XRGB(0x8E,0x9B,0xA6);
    char ext[16] = "";
    HGDIOBJ of;
    if (isdir) return;
    if (e && lstrlenA(e) < 15) { lstrcpyA(ext, e + 1); CharLowerA(ext); }
    if (!lstrcmpA(ext, "c") || !lstrcmpA(ext, "cpp") || !lstrcmpA(ext, "cc")) { g = "C"; c = XRGB(0x51,0x9A,0xBA); }
    else if (!lstrcmpA(ext, "h") || !lstrcmpA(ext, "hpp")) { g = "h"; c = XRGB(0xA0,0x74,0xC4); }
    else if (!lstrcmpA(ext, "py") || !lstrcmpA(ext, "pyw")) { g = "py"; c = XRGB(0x4B,0x8B,0xBE); }
    else if (!lstrcmpA(ext, "js") || !lstrcmpA(ext, "json")) { g = !lstrcmpA(ext, "js") ? "JS" : "{}"; c = XRGB(0xCB,0xCB,0x41); }
    else if (!lstrcmpA(ext, "bat") || !lstrcmpA(ext, "cmd")) { g = ">"; c = XRGB(0xCB,0xCB,0x41); }
    else if (!lstrcmpA(ext, "md")) { g = "M"; c = XRGB(0x51,0x9A,0xBA); }
    else if (!lstrcmpA(ext, "html") || !lstrcmpA(ext, "htm") || !lstrcmpA(ext, "xml")) { g = "<>"; c = XRGB(0xE3,0x79,0x33); }
    else if (!lstrcmpA(ext, "exe") || !lstrcmpA(ext, "dll")) { g = "#"; c = XRGB(0x8E,0x9B,0xA6); }
    else if (!lstrcmpA(ext, "txt")) { g = "="; c = XRGB(0xA0,0xA0,0xA0); }
    of = SelectObject(dc, g_uiBold);
    ui_text(dc, x + (lstrlenA(g) == 1 ? 3 : 0), y, g, c);
    SelectObject(dc, of);
}

/* back buffers kept per window and reused until the size changes */
typedef struct { HWND w; HDC dc; HBITMAP bm; int W, H; } BackBuf;
static BackBuf s_bufs[8];

HDC bb_begin(HWND w, HDC wdc, int W, int H)
{
    int i;
    BackBuf *b = 0;
    for (i = 0; i < 8; i++) if (s_bufs[i].w == w || !s_bufs[i].w) { b = &s_bufs[i]; break; }
    if (!b) b = &s_bufs[7];
    if (b->w != w || b->W != W || b->H != H || !b->dc) {
        if (!b->dc) b->dc = CreateCompatibleDC(wdc);
        if (b->bm) { SelectObject(b->dc, GetStockObject(DEFAULT_GUI_FONT)); DeleteObject(SelectObject(b->dc, CreateCompatibleBitmap(wdc, W, H))); }
        else SelectObject(b->dc, CreateCompatibleBitmap(wdc, W, H));
        b->bm = (HBITMAP)GetCurrentObject(b->dc, OBJ_BITMAP);
        b->w = w; b->W = W; b->H = H;
    }
    return b->dc;
}

/* ------------------------------------------------------------------ custom scroll bar */

static void vs_thumb(VScroll *v, int *ty, int *th)
{
    int H = v->r.bottom - v->r.top, range = v->max - v->page;
    *th = range > 0 ? H * v->page / (v->max > 0 ? v->max : 1) : H;
    if (*th < 20) *th = 20;
    if (*th > H) *th = H;
    *ty = v->r.top + (range > 0 ? (int)((double)(H - *th) * v->pos / range) : 0);
}

int vs_clamp(VScroll *v, int pos)
{
    if (pos > v->max - v->page) pos = v->max - v->page;
    if (pos < 0) pos = 0;
    return pos;
}

void vs_draw(HDC dc, VScroll *v)
{
    int ty, th;
    if (v->max <= v->page) return;
    vs_thumb(v, &ty, &th);
    fill(dc, v->r.left + 1, ty, v->r.right - v->r.left - 1, th, v->drag ? XRGB(0x6E,0x6E,0x6E) : C_SCROLL);
}

int vs_mouse(VScroll *v, HWND w, UINT msg, int x, int y)
{
    int ty, th, H = v->r.bottom - v->r.top, range = v->max - v->page;
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
        if (x < v->r.left || x >= v->r.right || y < v->r.top || y >= v->r.bottom || range <= 0) return 0;
        vs_thumb(v, &ty, &th);
        if (y < ty || y >= ty + th) {
            v->pos = vs_clamp(v, (int)((double)(y - v->r.top - th / 2) * range / (H - th > 0 ? H - th : 1)));
            vs_thumb(v, &ty, &th);
        }
        v->drag = 1; v->dragOff = y - ty;
        SetCapture(w);
        return 1;
    }
    if (msg == WM_MOUSEMOVE && v->drag) {
        vs_thumb(v, &ty, &th);
        v->pos = vs_clamp(v, (int)((double)(y - v->dragOff - v->r.top) * range / (H - th > 0 ? H - th : 1)));
        return 1;
    }
    if (msg == WM_LBUTTONUP && v->drag) { v->drag = 0; ReleaseCapture(); return 1; }
    return 0;
}

/* ------------------------------------------------------------------ commands table */

typedef struct { int id; const char *name; const char *key; } CmdDef;
static const CmdDef s_cmds[] = {
    { CM_NEWFILE, "File: New File", "Ctrl+N" },
    { CM_OPENFILE, "File: Open File...", "Ctrl+O" },
    { CM_OPENFOLDER, "File: Open Folder...", "Ctrl+K Ctrl+O" },
    { CM_SAVE, "File: Save", "Ctrl+S" },
    { CM_SAVEAS, "File: Save As...", "Ctrl+Shift+S" },
    { CM_SAVEALL, "File: Save All", "Ctrl+K S" },
    { CM_REVERT, "File: Revert File", "" },
    { CM_CLOSE, "View: Close Editor", "Ctrl+W" },
    { CM_CLOSEALL, "View: Close All Editors", "Ctrl+K Ctrl+W" },
    { CM_CLOSEFOLDER, "Workspace: Close Folder", "Ctrl+K F" },
    { CM_EXIT, "File: Exit", "Alt+F4" },
    { CM_UNDO, "Edit: Undo", "Ctrl+Z" },
    { CM_REDO, "Edit: Redo", "Ctrl+Y" },
    { CM_CUT, "Edit: Cut", "Ctrl+X" },
    { CM_COPY, "Edit: Copy", "Ctrl+C" },
    { CM_PASTE, "Edit: Paste", "Ctrl+V" },
    { CM_FIND, "Edit: Find", "Ctrl+F" },
    { CM_REPLACE, "Edit: Replace", "Ctrl+H" },
    { CM_FINDNEXT, "Edit: Find Next", "F3" },
    { CM_FINDPREV, "Edit: Find Previous", "Shift+F3" },
    { CM_FINDINFILES, "Search: Find in Files", "Ctrl+Shift+F" },
    { CM_COMMENT, "Edit: Toggle Line Comment", "Ctrl+/" },
    { CM_SELALL, "Selection: Select All", "Ctrl+A" },
    { CM_SELLINE, "Selection: Expand Line Selection", "Ctrl+L" },
    { CM_EXPANDSEL, "Selection: Select Word", "Ctrl+D" },
    { CM_MOVEUP, "Selection: Move Line Up", "Alt+Up" },
    { CM_MOVEDOWN, "Selection: Move Line Down", "Alt+Down" },
    { CM_COPYUP, "Selection: Copy Line Up", "Shift+Alt+Up" },
    { CM_COPYDOWN, "Selection: Copy Line Down", "Shift+Alt+Down" },
    { CM_DELLINE, "Edit: Delete Line", "Ctrl+Shift+K" },
    { CM_INDENT, "Edit: Indent Line", "Ctrl+]" },
    { CM_OUTDENT, "Edit: Outdent Line", "Ctrl+[" },
    { CM_INSLINEBELOW, "Edit: Insert Line Below", "Ctrl+Enter" },
    { CM_INSLINEABOVE, "Edit: Insert Line Above", "Ctrl+Shift+Enter" },
    { CM_TRIMWS, "Edit: Trim Trailing Whitespace", "Ctrl+K Ctrl+X" },
    { CM_PALETTE, "View: Show All Commands", "Ctrl+Shift+P" },
    { CM_QUICKOPEN, "Go: Go to File...", "Ctrl+P" },
    { CM_GOTOLINE, "Go: Go to Line...", "Ctrl+G" },
    { CM_GOTOSYM, "Go: Go to Symbol in Editor...", "Ctrl+Shift+O" },
    { CM_GOTOBRACKET, "Go: Go to Bracket", "Ctrl+Shift+\\" },
    { CM_NEXTTAB, "View: Next Editor", "Ctrl+Tab" },
    { CM_PREVTAB, "View: Previous Editor", "Ctrl+Shift+Tab" },
    { CM_NEXTPROB, "Go: Next Problem", "F8" },
    { CM_PREVPROB, "Go: Previous Problem", "Shift+F8" },
    { CM_EXPLORER, "View: Show Explorer", "Ctrl+Shift+E" },
    { CM_SEARCH, "View: Show Search", "Ctrl+Shift+F" },
    { CM_RUNVIEW, "View: Show Run", "Ctrl+Shift+D" },
    { CM_TOGGLESIDE, "View: Toggle Side Bar", "Ctrl+B" },
    { CM_TOGGLEPANEL, "View: Toggle Panel", "Ctrl+J" },
    { CM_PROBLEMS, "View: Show Problems", "Ctrl+Shift+M" },
    { CM_OUTPUT, "View: Show Output", "Ctrl+Shift+U" },
    { CM_TERMINAL, "View: Toggle Terminal", "Ctrl+`" },
    { CM_ZOOMIN, "View: Zoom In", "Ctrl+=" },
    { CM_ZOOMOUT, "View: Zoom Out", "Ctrl+-" },
    { CM_ZOOMRESET, "View: Reset Zoom", "Ctrl+Num0" },
    { CM_MINIMAP, "View: Toggle Minimap", "" },
    { CM_RUN, "Run: Run Active File", "F5" },
    { CM_BUILD, "Run: Run Build Task (build.bat)", "Ctrl+Shift+B" },
    { CM_STOP, "Run: Stop (send Ctrl+C to terminal)", "Shift+F5" },
    { CM_NEWTERM, "Terminal: Create New Terminal", "Ctrl+Shift+`" },
    { CM_KILLTERM, "Terminal: Kill the Active Terminal", "" },
    { CM_CLEARTERM, "Terminal: Clear", "" },
    { CM_NEXTTERM, "Terminal: Focus Next Terminal", "" },
    { CM_PREVTERM, "Terminal: Focus Previous Terminal", "" },
    { CM_REVEAL, "File: Reveal Active File in Windows Explorer", "" },
    { CM_COPYPATH, "File: Copy Path of Active File", "" },
    { CM_LANG, "Change Language Mode", "" },
    { CM_EOL, "Change End of Line Sequence", "" },
    { CM_INDENTMODE, "Toggle Indent with Tabs / Spaces", "" },
    { CM_ENCODING, "Change File Encoding", "" },
    { CM_SETTINGS, "Preferences: Open Settings (ini)", "Ctrl+," },
    { CM_SHORTCUTS, "Help: Keyboard Shortcuts", "Ctrl+K Ctrl+S" },
    { CM_ABOUT, "Help: About", "" },
};
#define NCMDS (int)(sizeof(s_cmds) / sizeof(s_cmds[0]))

int cmd_count(void) { return NCMDS; }
int cmd_at(int i) { return s_cmds[i].id; }
const char *cmd_name(int id) { int i; for (i = 0; i < NCMDS; i++) if (s_cmds[i].id == id) return s_cmds[i].name; return ""; }
const char *cmd_key(int id) { int i; for (i = 0; i < NCMDS; i++) if (s_cmds[i].id == id) return s_cmds[i].key; return ""; }

/* key bindings: vk, mods, command, editor-only */
typedef struct { BYTE vk, mods; WORD id; BYTE ed; } Bind;
static const Bind s_binds[] = {
    { 'P', M_CTRL|M_SHIFT, CM_PALETTE, 0 }, { VK_F1, 0, CM_PALETTE, 0 },
    { 'P', M_CTRL, CM_QUICKOPEN, 0 }, { 'E', M_CTRL, CM_QUICKOPEN, 0 }, { 'G', M_CTRL, CM_GOTOLINE, 0 }, { 'O', M_CTRL|M_SHIFT, CM_GOTOSYM, 0 },
    { 'N', M_CTRL, CM_NEWFILE, 0 }, { 'O', M_CTRL, CM_OPENFILE, 0 },
    { 'S', M_CTRL, CM_SAVE, 0 }, { 'S', M_CTRL|M_SHIFT, CM_SAVEAS, 0 },
    { 'W', M_CTRL, CM_CLOSE, 0 }, { VK_F4, M_CTRL, CM_CLOSE, 0 },
    { VK_TAB, M_CTRL, CM_NEXTTAB, 0 }, { VK_TAB, M_CTRL|M_SHIFT, CM_PREVTAB, 0 },
    { VK_NEXT, M_CTRL, CM_NEXTTAB, 0 }, { VK_PRIOR, M_CTRL, CM_PREVTAB, 0 },
    { 'B', M_CTRL, CM_TOGGLESIDE, 0 }, { 'J', M_CTRL, CM_TOGGLEPANEL, 0 },
    { VK_OEM_3, M_CTRL, CM_TERMINAL, 0 }, { VK_OEM_3, M_CTRL|M_SHIFT, CM_NEWTERM, 0 },
    { 'E', M_CTRL|M_SHIFT, CM_EXPLORER, 0 }, { 'F', M_CTRL|M_SHIFT, CM_SEARCH, 0 },
    { 'D', M_CTRL|M_SHIFT, CM_RUNVIEW, 0 },
    { 'M', M_CTRL|M_SHIFT, CM_PROBLEMS, 0 }, { 'U', M_CTRL|M_SHIFT, CM_OUTPUT, 0 },
    { VK_OEM_PLUS, M_CTRL, CM_ZOOMIN, 0 }, { VK_OEM_PLUS, M_CTRL|M_SHIFT, CM_ZOOMIN, 0 }, { VK_ADD, M_CTRL, CM_ZOOMIN, 0 },
    { VK_OEM_MINUS, M_CTRL, CM_ZOOMOUT, 0 }, { VK_SUBTRACT, M_CTRL, CM_ZOOMOUT, 0 }, { VK_NUMPAD0, M_CTRL, CM_ZOOMRESET, 0 },
    { VK_F5, 0, CM_RUN, 0 }, { VK_F5, M_SHIFT, CM_STOP, 0 }, { 'B', M_CTRL|M_SHIFT, CM_BUILD, 0 },
    { VK_F8, 0, CM_NEXTPROB, 0 }, { VK_F8, M_SHIFT, CM_PREVPROB, 0 },
    { 'F', M_CTRL, CM_FIND, 0 }, { 'H', M_CTRL, CM_REPLACE, 0 },
    { VK_F3, 0, CM_FINDNEXT, 0 }, { VK_F3, M_SHIFT, CM_FINDPREV, 0 },
    { VK_OEM_COMMA, M_CTRL, CM_SETTINGS, 0 },
    { 'Z', M_CTRL, CM_UNDO, 1 }, { 'Y', M_CTRL, CM_REDO, 1 }, { 'Z', M_CTRL|M_SHIFT, CM_REDO, 1 },
    { 'X', M_CTRL, CM_CUT, 1 }, { 'C', M_CTRL, CM_COPY, 1 }, { 'V', M_CTRL, CM_PASTE, 1 },
    { 'A', M_CTRL, CM_SELALL, 1 }, { 'L', M_CTRL, CM_SELLINE, 1 }, { 'D', M_CTRL, CM_EXPANDSEL, 1 },
    { VK_OEM_2, M_CTRL, CM_COMMENT, 1 }, { VK_DIVIDE, M_CTRL, CM_COMMENT, 1 },
    { VK_UP, M_ALT, CM_MOVEUP, 1 }, { VK_DOWN, M_ALT, CM_MOVEDOWN, 1 },
    { VK_UP, M_ALT|M_SHIFT, CM_COPYUP, 1 }, { VK_DOWN, M_ALT|M_SHIFT, CM_COPYDOWN, 1 },
    { 'K', M_CTRL|M_SHIFT, CM_DELLINE, 1 },
    { VK_OEM_6, M_CTRL, CM_INDENT, 1 }, { VK_OEM_4, M_CTRL, CM_OUTDENT, 1 },
    { VK_OEM_5, M_CTRL|M_SHIFT, CM_GOTOBRACKET, 1 },
};
#ifndef VK_OEM_COMMA
#define VK_OEM_COMMA 0xBC
#endif

static int global_key(MSG *m)
{
    int vk, mods, i;
    char cls[16];
    HWND f;
    if (m->message != WM_KEYDOWN && m->message != WM_SYSKEYDOWN) return 0;
    vk = (int)m->wParam;
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU) return 0;
    mods = key_mods();
    f = GetFocus();
    if (s_chord) {
        s_chord = 0;
        if (vk == 'O') run_cmd(CM_OPENFOLDER);
        else if (vk == 'W') run_cmd(CM_CLOSEALL);
        else if (vk == 'S' && !(mods & M_CTRL)) run_cmd(CM_SAVEALL);
        else if (vk == 'S') run_cmd(CM_SHORTCUTS);
        else if (vk == 'F') run_cmd(CM_CLOSEFOLDER);
        else if (vk == 'X') run_cmd(CM_TRIMWS);
        else if (vk == 'R') run_cmd(CM_REVEAL);
        else if (vk == 'P') run_cmd(CM_COPYPATH);
        else if (vk == 'M') run_cmd(CM_LANG);
        status_dirty();
        return 1;
    }
    if (vk == 'K' && mods == M_CTRL) { s_chord = 1; status_dirty(); return 1; }
    /* in plain edit boxes leave clipboard/undo keys alone */
    cls[0] = 0;
    if (f) GetClassNameA(f, cls, sizeof(cls));
    for (i = 0; i < (int)(sizeof(s_binds) / sizeof(s_binds[0])); i++) {
        const Bind *b = &s_binds[i];
        if (b->vk != vk || b->mods != mods) continue;
        if (b->ed && f != g_edit) return 0;
        if (f == g_term && (b->id == CM_FIND || b->id == CM_REPLACE)) return 0;
        run_cmd(b->id);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ fonts */

static HFONT mkfont(const char *face, int h, int bold)
{
    return CreateFontA(h, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH * 0 | FF_DONTCARE, face);
}

static int font_exists(const char *face)
{
    HDC dc = GetDC(0);
    HFONT f = mkfont(face, -13, 0);
    HGDIOBJ o = SelectObject(dc, f);
    char got[64];
    GetTextFaceA(dc, sizeof(got), got);
    SelectObject(dc, o); DeleteObject(f); ReleaseDC(0, dc);
    return !lstrcmpiA(got, face);
}

void zoom_fonts(void)
{
    int h = -(13 + g_zoom);
    if (h > -6) h = -6;
    if (g_mono) DeleteObject(g_mono);
    g_mono = CreateFontA(h, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                         CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, s_monoFace);
    if (g_edit) ed_font_changed();
    if (g_term) term_font_changed();
}

static void init_fonts(void)
{
    HDC dc; TEXTMETRICA tm; HGDIOBJ o;
    char face[64];
    GetPrivateProfileStringA("editor", "font", "", face, sizeof(face), g_ini);
    if (face[0] && font_exists(face)) lstrcpyA(s_monoFace, face);
    else if (font_exists("Consolas")) lstrcpyA(s_monoFace, "Consolas");
    else if (font_exists("Lucida Console")) lstrcpyA(s_monoFace, "Lucida Console");
    else lstrcpyA(s_monoFace, "Courier New");
    g_ui = mkfont("Tahoma", -11, 0);
    g_uiBold = mkfont("Tahoma", -11, 1);
    g_uiBig = mkfont("Tahoma", -56, 0);
    dc = GetDC(0);
    o = SelectObject(dc, g_ui);
    GetTextMetricsA(dc, &tm);
    g_uiH = tm.tmHeight;
    SelectObject(dc, o);
    ReleaseDC(0, dc);
    zoom_fonts();
}

/* ------------------------------------------------------------------ layout */

void layout(void)
{
    RECT cr; int H, x0, edW, ptop, etop;
    if (!g_main) return;
    GetClientRect(g_main, &cr);
    if (IsIconic(g_main) || cr.right < 200 || cr.bottom < 150) return;   /* keep sizes while minimized */
    H = cr.bottom - STATUS_H;
    if (H < 100) H = 100;
    if (g_sideW < 140) g_sideW = 140;
    if (g_sideW > cr.right - 250) g_sideW = cr.right - 250 > 140 ? cr.right - 250 : 140;
    if (g_panelH < 80) g_panelH = 80;
    if (g_panelH > H - 120) g_panelH = H - 120 > 80 ? H - 120 : 80;
    x0 = ACT_W;
    if (g_sideVis) { MoveWindow(g_side, ACT_W, 0, g_sideW - 3, H, TRUE); ShowWindow(g_side, SW_SHOW); x0 += g_sideW; }
    else ShowWindow(g_side, SW_HIDE);
    edW = cr.right - x0;
    ptop = g_panelVis ? H - g_panelH : H;
    SetRect(&s_tabsR, x0, 0, cr.right, TAB_H);
    etop = TAB_H;
    if (cur_doc()) { SetRect(&s_crumbR, x0, TAB_H, cr.right, TAB_H + CRUMB_H); etop += CRUMB_H; }
    else SetRect(&s_crumbR, 0, 0, 0, 0);
    SetRect(&s_edR, x0, etop, cr.right, ptop);
    MoveWindow(g_edit, x0, etop, edW, ptop - etop > 0 ? ptop - etop : 0, TRUE);
    if (g_panelVis) {
        SetRect(&s_phdrR, x0, ptop, cr.right, ptop + PHDR_H);
        if (g_panelMode == 0) { ShowWindow(g_term, SW_HIDE); MoveWindow(g_prob, x0, ptop + PHDR_H, edW, g_panelH - PHDR_H, TRUE); ShowWindow(g_prob, SW_SHOW); }
        else { ShowWindow(g_prob, SW_HIDE); MoveWindow(g_term, x0 + 12, ptop + PHDR_H, edW - 12, g_panelH - PHDR_H, TRUE); ShowWindow(g_term, SW_SHOW); InvalidateRect(g_term, 0, 0); }
    } else {
        SetRect(&s_phdrR, 0, 0, 0, 0);
        ShowWindow(g_term, SW_HIDE); ShowWindow(g_prob, SW_HIDE);
    }
    SetRect(&s_statusR, 0, H, cr.right, cr.bottom);
    if (g_sideVis) side_layout();
    InvalidateRect(g_main, 0, 0);
}

void chrome_dirty(void) { if (g_main) { InvalidateRect(g_main, 0, 0); set_title(); } }
void status_dirty(void) { if (g_main) InvalidateRect(g_main, &s_statusR, 0); if (g_main) InvalidateRect(g_main, &s_phdrR, 0); }

void set_title(void)
{
    char t[MAX_PATH * 2 + 64];
    Doc *d = cur_doc();
    const char *fn = g_folder[0] ? path_name(g_folder) : 0;
    if (d && fn) wsprintfA(t, "%s%s - %s - " APPNAME, doc_dirty(d) ? "* " : "", d->name, fn);
    else if (d) wsprintfA(t, "%s%s - " APPNAME, doc_dirty(d) ? "* " : "", d->name);
    else if (fn) wsprintfA(t, "%s - " APPNAME, fn);
    else lstrcpyA(t, APPNAME);
    SetWindowTextA(g_main, t);
}

/* ------------------------------------------------------------------ painting */

static int text_w(HDC dc, const char *s) { SIZE z; GetTextExtentPoint32A(dc, s, lstrlenA(s), &z); return z.cx; }

static void paint_tabs(HDC dc)
{
    int i, x, total = 0, avail = s_tabsR.right - s_tabsR.left;
    fill(dc, s_tabsR.left, 0, avail, TAB_H, C_TABBAR);
    SelectObject(dc, g_ui);
    for (i = 0; i < g_ndocs; i++) { s_tabW[i] = text_w(dc, g_docs[i]->name) + 62; if (s_tabW[i] < 90) s_tabW[i] = 90; total += s_tabW[i]; }
    /* keep the active tab in view */
    if (g_cur >= 0) {
        int a = 0; for (i = 0; i < g_cur; i++) a += s_tabW[i];
        if (a - s_tabScroll < 0) s_tabScroll = a;
        if (a + s_tabW[g_cur] - s_tabScroll > avail) s_tabScroll = a + s_tabW[g_cur] - avail;
    }
    if (total <= avail) s_tabScroll = 0;
    x = s_tabsR.left - s_tabScroll;
    for (i = 0; i < g_ndocs; i++) {
        Doc *d = g_docs[i];
        int act = i == g_cur, w = s_tabW[i], dirty = doc_dirty(d);
        s_tabX[i] = x;
        if (x + w > s_tabsR.left && x < s_tabsR.right) {
            fill(dc, x, 0, w, TAB_H, act ? C_BG : C_TABOFF);
            vline(dc, x + w - 1, 0, TAB_H, C_TABBAR);
            if (act && GetFocus() == g_edit) hline(dc, x, 0, w - 1, XRGB(0x00,0x7A,0xCC));
            file_glyph(dc, d->name, 0, x + 12, (TAB_H - g_uiH) / 2);
            ui_text(dc, x + 32, (TAB_H - g_uiH) / 2, d->name, act ? C_WHITE : XRGB(0x96,0x96,0x96));
            if (s_hoverClose == i) fill(dc, x + w - 26, 9, 18, 18, XRGB(0x40,0x40,0x40));
            if (dirty && s_hoverClose != i) draw_icon(dc, IC_DOT, x + w - 25, 10, act ? C_WHITE : C_DIM);
            else if (act || s_hoverTab == i) draw_icon(dc, IC_CLOSE, x + w - 25, 10, act ? C_WHITE : C_DIM);
        }
        x += w;
    }
}

static void paint_crumbs(HDC dc)
{
    Doc *d = cur_doc();
    char buf[MAX_PATH], *p, *seg;
    int x = s_crumbR.left + 14, y = s_crumbR.top + (CRUMB_H - g_uiH) / 2 - 1;
    if (!d) return;
    fill(dc, s_crumbR.left, s_crumbR.top, s_crumbR.right - s_crumbR.left, CRUMB_H, C_BG);
    SelectObject(dc, g_ui);
    lstrcpynA(buf, d->path[0] ? rel_path(d->path) : d->name, sizeof(buf));
    seg = buf;
    for (p = buf;; p++) {
        if (*p == '\\' || *p == '/' || !*p) {
            int last = !*p;
            *p = 0;
            if (last) { file_glyph(dc, seg, 0, x, y); x += 18; }
            ui_text(dc, x, y, seg, XRGB(0xA9,0xA9,0xA9));
            x += text_w(dc, seg) + 4;
            if (last) break;
            draw_icon(dc, IC_CHEVR, x - 2, y - 2, XRGB(0xA9,0xA9,0xA9));
            x += 14;
            seg = p + 1;
        }
    }
}

static void paint_panel_header(HDC dc)
{
    static const char *names[3] = { "PROBLEMS", "OUTPUT", "TERMINAL" };
    int i, x = s_phdrR.left + 18, y = s_phdrR.top + 11, r = s_phdrR.right - 10;
    char buf[64];
    if (!g_panelVis) return;
    fill(dc, s_phdrR.left, s_phdrR.top, s_phdrR.right - s_phdrR.left, PHDR_H, C_BG);
    hline(dc, s_phdrR.left, s_phdrR.top, s_phdrR.right - s_phdrR.left, C_BORDER);
    SelectObject(dc, g_ui);
    for (i = 0; i < 3; i++) {
        int w = text_w(dc, names[i]);
        ui_text(dc, x, y, names[i], g_panelMode == i ? XRGB(0xE7,0xE7,0xE7) : XRGB(0x96,0x96,0x96));
        if (i == 0 && g_nprobs) {
            wsprintfA(buf, "%d", g_nprobs);
            disc(dc, x + w + 5, y - 2, 17, XRGB(0x4D,0x4D,0x4D));
            ui_text(dc, x + w + 13 - text_w(dc, buf) / 2, y, buf, C_WHITE);
            w += 24;
        }
        if (g_panelMode == i) hline(dc, x, s_phdrR.top + 30, w, XRGB(0xE7,0xE7,0xE7)), hline(dc, x, s_phdrR.top + 31, w, XRGB(0xE7,0xE7,0xE7));
        SetRect(&s_phTab[i], x - 8, s_phdrR.top, x + w + 8, s_phdrR.bottom);
        x += w + 28;
    }
    s_nph = 0;
#define PHBTN(icon, cmd) do { r -= 24; SetRect(&s_phBtn[s_nph], r, s_phdrR.top + 6, r + 22, s_phdrR.top + 28); \
        s_phCmd[s_nph++] = cmd; draw_icon(dc, icon, r + 3, s_phdrR.top + 9, XRGB(0xC5,0xC5,0xC5)); } while (0)
    PHBTN(IC_CLOSE, CM_TOGGLEPANEL);
    PHBTN(IC_MAX, -1);
    if (g_panelMode == 2) {
        PHBTN(IC_TRASH, CM_KILLTERM);
        PHBTN(IC_PLUS, CM_NEWTERM);
        if (term_count()) {
            const char *lb = term_label(term_active());
            int w = text_w(dc, lb);
            r -= w + 22;
            SetRect(&s_phBtn[s_nph], r, s_phdrR.top + 6, r + w + 18, s_phdrR.top + 28);
            s_phCmd[s_nph++] = -2;
            ui_text(dc, r + 4, y, lb, XRGB(0xCC,0xCC,0xCC));
            draw_icon(dc, IC_CHEVD, r + w + 4, y - 1, XRGB(0xCC,0xCC,0xCC));
        }
    } else if (g_panelMode == 1) PHBTN(IC_TRASH, CM_CLEARTERM);
#undef PHBTN
}

static void paint_status(HDC dc)
{
    Doc *d = cur_doc();
    int x = 8, r = s_statusR.right - 10, y = s_statusR.top + (STATUS_H - g_uiH) / 2;
    char buf[128];
    COLORREF bg = g_folder[0] ? C_STATUS : C_STATUSNF;
    fill(dc, 0, s_statusR.top, s_statusR.right, STATUS_H, bg);
    SelectObject(dc, g_ui);
    s_nst = 0;
#define ITEM(xx, w, cmd) do { SetRect(&s_stItem[s_nst], xx - 5, s_statusR.top, xx + w + 5, s_statusR.bottom); s_stCmd[s_nst++] = cmd; } while (0)
    if (g_folder[0]) {
        const char *fn = path_name(g_folder);
        ITEM(x, text_w(dc, fn) + 16, CM_EXPLORER);
        draw_icon(dc, IC_NEWDIR, x - 1, y - 2, C_WHITE);
        ui_text(dc, x + 18, y, fn, C_WHITE);
        x += text_w(dc, fn) + 34;
    }
    {
        int x0 = x;
        disc(dc, x, y, 12, C_WHITE); disc(dc, x + 1, y + 1, 10, bg);
        { HPEN p = CreatePen(PS_SOLID, 1, C_WHITE), op = (HPEN)SelectObject(dc, p);
          MoveToEx(dc, x + 3, y + 3, 0); LineTo(dc, x + 9, y + 9); MoveToEx(dc, x + 8, y + 3, 0); LineTo(dc, x + 2, y + 9);
          SelectObject(dc, op); DeleteObject(p); }
        wsprintfA(buf, "%d", g_nerr); ui_text(dc, x + 16, y, buf, C_WHITE); x += 22 + text_w(dc, buf);
        { POINT p[4]; HPEN pn = CreatePen(PS_SOLID, 1, C_WHITE), op = (HPEN)SelectObject(dc, pn);
          p[0].x = x + 6; p[0].y = y; p[1].x = x + 12; p[1].y = y + 11; p[2].x = x; p[2].y = y + 11; p[3] = p[0];
          Polyline(dc, p, 4); MoveToEx(dc, x + 6, y + 4, 0); LineTo(dc, x + 6, y + 8); SelectObject(dc, op); DeleteObject(pn); }
        wsprintfA(buf, "%d", g_nwarn); ui_text(dc, x + 16, y, buf, C_WHITE); x += 22 + text_w(dc, buf);
        ITEM(x0, x - x0, CM_PROBLEMS);
    }
    if (s_chord) { ui_text(dc, x + 10, y, "(Ctrl+K) was pressed. Waiting for second key of chord...", C_WHITE); }
    if (d) {
        const char *items[6]; int cmds[6], n = 0, i;
        char ln[48], ind[24], enc[24];
        int cc = 0, k;
        for (k = 0; k < d->cc && k < d->ln[d->cl].len; k++) cc = d->ln[d->cl].s[k] == '\t' ? (cc / 4 + 1) * 4 : cc + 1;
        if (doc_has_sel(d)) {
            int n2; char *t = doc_sel_text(d, &n2); free(t);
            wsprintfA(ln, "Ln %d, Col %d (%d selected)", d->cl + 1, cc + 1, n2);
        } else wsprintfA(ln, "Ln %d, Col %d", d->cl + 1, cc + 1);
        lstrcpyA(ind, d->tabs ? "Tab Size: 4" : "Spaces: 4");
        if (d->enc == 1) lstrcpyA(enc, "UTF-8 with BOM");
        else if (d->enc == 2) lstrcpyA(enc, "UTF-8");
        else wsprintfA(enc, "Windows %u", GetACP());
        items[n] = lang_name(d->lang); cmds[n++] = CM_LANG;
        items[n] = d->crlf ? "CRLF" : "LF"; cmds[n++] = CM_EOL;
        items[n] = enc; cmds[n++] = CM_ENCODING;
        items[n] = ind; cmds[n++] = CM_INDENTMODE;
        items[n] = ln; cmds[n++] = CM_GOTOLINE;
        for (i = 0; i < n; i++) {
            int w = text_w(dc, items[i]);
            r -= w;
            ui_text(dc, r, y, items[i], C_WHITE);
            ITEM(r, w, cmds[i]);
            r -= 22;
        }
    }
#undef ITEM
}

static void paint_main(HWND w)
{
    PAINTSTRUCT ps;
    HDC wdc = BeginPaint(w, &ps), dc;
    RECT cr; HGDIOBJ of;
    int i, H;
    GetClientRect(w, &cr);
    dc = bb_begin(w, wdc, cr.right, cr.bottom);
    of = SelectObject(dc, g_ui);
    SetBkMode(dc, TRANSPARENT);
    H = s_statusR.top;
    /* activity bar */
    fill(dc, 0, 0, ACT_W, H, C_ACT);
    for (i = 0; i < 3; i++) {
        int act = g_sideVis && g_sideMode == i;
        if (act) fill(dc, 0, i * 48, 2, 48, C_WHITE);
        act_icon(dc, i == 0 ? IC_FILES : i == 1 ? IC_SEARCH : IC_RUN, 12, i * 48 + 12, act ? C_WHITE : XRGB(0x85,0x85,0x85));
    }
    act_icon(dc, IC_GEAR, 12, H - 40, XRGB(0x85,0x85,0x85));
    if (g_sideVis) { fill(dc, ACT_W + g_sideW - 3, 0, 3, H, C_SIDE); vline(dc, ACT_W + g_sideW - 1, 0, H, s_split == 1 ? C_FOCUS : XRGB(0x2B,0x2B,0x2B)); }
    paint_tabs(dc);
    paint_crumbs(dc);
    paint_panel_header(dc);
    if (s_split == 2) hline(dc, s_phdrR.left, s_phdrR.top, s_phdrR.right - s_phdrR.left, C_FOCUS);
    if (g_panelVis && g_panelMode != 0) fill(dc, s_phdrR.left, s_phdrR.bottom, 12, H - s_phdrR.bottom, C_BG);
    paint_status(dc);
    SelectObject(dc, of);
    BitBlt(wdc, ps.rcPaint.left, ps.rcPaint.top, ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top, dc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
    EndPaint(w, &ps);
}

/* ------------------------------------------------------------------ documents */

void focus_editor(void) { if (cur_doc()) SetFocus(g_edit); }

void activate_doc(int i)
{
    if (i < 0 || i >= g_ndocs) { g_cur = g_ndocs ? 0 : -1; if (g_ndocs == 0) { layout(); set_title(); InvalidateRect(g_edit, 0, 0); return; } i = g_cur; }
    g_cur = i;
    layout();
    set_title();
    InvalidateRect(g_edit, 0, 0);
    if (find_visible()) { find_layout(); InvalidateRect(g_find, 0, 0); }
    ed_caret_sync();
    tree_reveal(g_docs[i]->path);
}

int open_file(const char *path, int line, int col)
{
    int i;
    char full[MAX_PATH], *fp;
    Doc *d;
    if (!GetFullPathNameA(path, MAX_PATH, full, &fp)) lstrcpynA(full, path, MAX_PATH);
    for (i = 0; i < g_ndocs; i++) if (!lstrcmpiA(g_docs[i]->path, full)) break;
    if (i == g_ndocs) {
        if (g_ndocs >= MAXDOCS) { MessageBoxA(g_main, "Too many open editors. Close some first.", APPNAME, MB_ICONWARNING); return 0; }
        d = doc_new();
        if (!doc_load(d, full)) {
            char m[MAX_PATH + 64]; wsprintfA(m, "Could not open\n%s", full);
            MessageBoxA(g_main, m, APPNAME, MB_ICONWARNING);
            doc_free(d);
            return 0;
        }
        i = g_cur + 1;
        if (i > g_ndocs) i = g_ndocs;
        memmove(g_docs + i + 1, g_docs + i, (g_ndocs - i) * sizeof(Doc *));
        g_docs[i] = d; g_ndocs++;
    }
    activate_doc(i);
    if (line > 0) ed_goto(g_docs[i], line - 1, col > 0 ? col - 1 : 0, 1);
    SetFocus(g_edit);
    return 1;
}

typedef struct {
    DWORD lStructSize; HWND hwndOwner; HINSTANCE hInstance; LPCSTR lpstrFilter; LPSTR lpstrCustomFilter;
    DWORD nMaxCustFilter; DWORD nFilterIndex; LPSTR lpstrFile; DWORD nMaxFile; LPSTR lpstrFileTitle;
    DWORD nMaxFileTitle; LPCSTR lpstrInitialDir; LPCSTR lpstrTitle; DWORD Flags; WORD nFileOffset;
    WORD nFileExtension; LPCSTR lpstrDefExt; LPARAM lCustData; void *lpfnHook; LPCSTR lpTemplateName;
} XOFN;
typedef BOOL (WINAPI *PGetFileName)(XOFN *);
static const char s_filter[] = "All Files (*.*)\0*.*\0C/C++ (*.c;*.h;*.cpp)\0*.c;*.h;*.cpp;*.hpp\0Python (*.py)\0*.py\0"
    "JavaScript (*.js)\0*.js\0Batch (*.bat;*.cmd)\0*.bat;*.cmd\0Text (*.txt;*.md;*.ini)\0*.txt;*.md;*.ini\0";

static int file_dialog(int save, char *out, const char *title)
{
    XOFN o; PGetFileName fn = (PGetFileName)dyn("comdlg32.dll", save ? "GetSaveFileNameA" : "GetOpenFileNameA");
    if (!fn) return 0;
    memset(&o, 0, sizeof(o));
    o.lStructSize = sizeof(o); o.hwndOwner = g_main; o.lpstrFilter = s_filter; o.nFilterIndex = 1;
    o.lpstrFile = out; o.nMaxFile = MAX_PATH; o.lpstrTitle = title;
    o.lpstrInitialDir = g_folder[0] ? g_folder : 0;
    o.Flags = save ? (0x2 | 0x800 | 0x4) : (0x1000 | 0x800 | 0x4);   /* OVERWRITEPROMPT|PATHMUSTEXIST|HIDEREADONLY / FILEMUSTEXIST */
    return fn(&o);
}

int save_doc(Doc *d, int as)
{
    char path[MAX_PATH];
    int wasNew = !d->path[0];
    lstrcpyA(path, d->path[0] ? d->path : d->name);
    if (as || !d->path[0]) {
        if (!d->path[0] && g_folder[0]) path_join(path, g_folder, d->name);
        if (!file_dialog(1, path, "Save As")) return 0;
    }
    if (d->lossy) {
        char m[MAX_PATH + 200];
        snprintf(m, sizeof(m) - 1, "%s has characters that Windows %u cannot show. Saving will replace them with ?.\n\nSave anyway?", d->name, GetACP());
        m[sizeof(m) - 1] = 0;
        if (MessageBoxA(g_main, m, APPNAME, MB_YESNO | MB_ICONWARNING) != IDYES) return 0;
        d->lossy = 0;
    }
    if (!doc_write(d, path)) {
        char m[MAX_PATH + 64]; wsprintfA(m, "Failed to save\n%s", path);
        MessageBoxA(g_main, m, APPNAME, MB_ICONERROR);
        return 0;
    }
    if (wasNew || as) { tree_refresh(); files_invalidate(); }
    chrome_dirty();
    InvalidateRect(g_edit, 0, 0);
    return 1;
}

int confirm_close(Doc *d)
{
    char m[MAX_PATH + 100]; int r;
    if (!doc_dirty(d)) return 1;
    wsprintfA(m, "Do you want to save the changes you made to %s?\n\nYour changes will be lost if you don't save them.", d->name);
    r = MessageBoxA(g_main, m, APPNAME, MB_YESNOCANCEL | MB_ICONWARNING);
    if (r == IDCANCEL) return 0;
    if (r == IDYES) return save_doc(d, 0);
    return 1;
}

void close_doc(int i)
{
    Doc *d;
    if (i < 0 || i >= g_ndocs) return;
    d = g_docs[i];
    if (!confirm_close(d)) return;
    doc_free(d);
    memmove(g_docs + i, g_docs + i + 1, (g_ndocs - i - 1) * sizeof(Doc *));
    g_ndocs--;
    if (g_cur > i || g_cur >= g_ndocs) g_cur--;
    if (g_cur < 0 && g_ndocs) g_cur = 0;
    if (find_visible() && !g_ndocs) ShowWindow(g_find, SW_HIDE);
    activate_doc(g_cur);
    if (g_ndocs) SetFocus(g_edit);
}

static void new_file(void)
{
    Doc *d;
    if (g_ndocs >= MAXDOCS) return;
    d = doc_new();
    wsprintfA(d->name, "Untitled-%d", ++s_untitled);
    d->untitledNo = s_untitled;
    g_docs[g_ndocs++] = d;
    activate_doc(g_ndocs - 1);
    SetFocus(g_edit);
}

typedef struct { HWND hwndOwner; void *pidlRoot; LPSTR pszDisplayName; LPCSTR lpszTitle; UINT ulFlags; void *lpfn; LPARAM lParam; int iImage; } XBROWSEINFO;
typedef void *(WINAPI *PSHBrowse)(XBROWSEINFO *);
typedef BOOL (WINAPI *PSHGetPath)(void *, LPSTR);
typedef HRESULT (WINAPI *POleInit)(void *);
typedef void (WINAPI *PCoFree)(void *);

static void pick_folder(void)
{
    XBROWSEINFO bi; char name[MAX_PATH], path[MAX_PATH]; void *pidl;
    PSHBrowse br = (PSHBrowse)dyn("shell32.dll", "SHBrowseForFolderA");
    PSHGetPath gp = (PSHGetPath)dyn("shell32.dll", "SHGetPathFromIDListA");
    POleInit oi = (POleInit)dyn("ole32.dll", "OleInitialize");
    if (!br || !gp) return;
    if (oi) oi(0);
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_main; bi.pszDisplayName = name; bi.lpszTitle = "Open Folder";
    bi.ulFlags = 0x1 | 0x10 | 0x40;   /* RETURNONLYFSDIRS | EDITBOX | NEWDIALOGSTYLE */
    pidl = br(&bi);
    if (pidl) {
        PCoFree cf = (PCoFree)dyn("ole32.dll", "CoTaskMemFree");
        int ok = gp(pidl, path);
        if (cf) cf(pidl);
        if (ok) open_folder(path);
    }
}

void open_folder(const char *path)
{
    char full[MAX_PATH], *fp;
    if (!path || !path[0]) {
        g_folder[0] = 0;
        tree_set_root("");
    } else {
        if (!GetFullPathNameA(path, MAX_PATH, full, &fp)) lstrcpynA(full, path, MAX_PATH);
        { int n = lstrlenA(full); if (n > 3 && (full[n-1] == '\\' || full[n-1] == '/')) full[n-1] = 0; }
        if (!dir_exists(full)) return;
        lstrcpyA(g_folder, full);
        SetCurrentDirectoryA(g_folder);
        if (s_watch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(s_watch);
        s_watch = FindFirstChangeNotificationA(g_folder, TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
        tree_set_root(g_folder);
    }
    files_invalidate();
    set_title();
    layout();
}

/* ------------------------------------------------------------------ run / build */

static void quote_into(char *out, const char *s) { wsprintfA(out, "\"%s\"", s); }

static void ini_str(const char *key, const char *def, char *out, int n) { GetPrivateProfileStringA("run", key, def, out, n, g_ini); }

static void run_active(void)
{
    Doc *d = cur_doc();
    char cmd[MAX_PATH * 6], dir[MAX_PATH], base[MAX_PATH], tool[MAX_PATH], q1[MAX_PATH + 2], q2[MAX_PATH + 2], *dot;
    const char *ext;
    if (!d) { run_cmd(CM_BUILD); return; }
    if (!d->path[0] || doc_dirty(d)) if (!save_doc(d, 0)) return;
    path_dir(dir, d->path);
    lstrcpyA(base, d->name);
    dot = strrchr(base, '.');
    ext = dot ? dot + 1 : "";
    if (!lstrcmpiA(ext, "c")) {
        char exe[MAX_PATH];
        path_join(tool, g_exeDir, "tcc\\tcc.exe");
        if (!file_exists(tool)) { path_join(tool, g_exeDir, "tcc.exe"); if (!file_exists(tool)) lstrcpyA(tool, "tcc"); }
        ini_str("tcc", tool, tool, MAX_PATH);
        *dot = 0;
        wsprintfA(exe, "%s.exe", base);
        if (strchr(tool, ' ') && tool[0] != '"') quote_into(q1, tool); else lstrcpyA(q1, tool);
        snprintf(cmd, sizeof(cmd) - 1, "cd /d \"%s\" && %s \"%s\" -o \"%s\" & (if not errorlevel 1 \"%s\")", dir, q1, d->name, exe, exe);
    } else if (!lstrcmpiA(ext, "py") || !lstrcmpiA(ext, "pyw")) {
        ini_str("python", "python", tool, MAX_PATH);
        snprintf(cmd, sizeof(cmd) - 1, "cd /d \"%s\" && %s -u \"%s\"", dir, tool, d->name);
    } else if (!lstrcmpiA(ext, "bat") || !lstrcmpiA(ext, "cmd")) {
        snprintf(cmd, sizeof(cmd) - 1, "cd /d \"%s\" && call \"%s\"", dir, d->name);
    } else if (!lstrcmpiA(ext, "js")) {
        snprintf(cmd, sizeof(cmd) - 1, "cd /d \"%s\" && cscript //nologo \"%s\"", dir, d->name);
    } else if (!lstrcmpiA(ext, "vbs")) {
        snprintf(cmd, sizeof(cmd) - 1, "cd /d \"%s\" && cscript //nologo \"%s\"", dir, d->name);
    } else if (!lstrcmpiA(ext, "html") || !lstrcmpiA(ext, "htm")) {
        quote_into(q2, d->path);
        snprintf(cmd, sizeof(cmd) - 1, "start \"\" %s", q2);
    } else {
        out_log("Don't know how to run .%.40s files. F5 runs .c, .py, .bat, .cmd, .js, .vbs and .html.", ext);
        set_panel(1);
        return;
    }
    cmd[sizeof(cmd) - 1] = 0;
    set_panel(2);
    if (!term_count()) term_new();
    term_send(cmd, 1);
}

static void run_build(void)
{
    char bat[MAX_PATH], cmd[MAX_PATH * 2];
    if (!g_folder[0]) { out_log("Open a folder with a build.bat to use Run Build Task."); set_panel(1); return; }
    path_join(bat, g_folder, "build.bat");
    if (!file_exists(bat)) { out_log("No build.bat found in %s", g_folder); set_panel(1); return; }
    run_cmd(CM_SAVEALL);
    snprintf(cmd, sizeof(cmd) - 1, "cd /d \"%s\" && call build.bat", g_folder);
    cmd[sizeof(cmd) - 1] = 0;
    set_panel(2);
    if (!term_count()) term_new();
    term_send(cmd, 1);
}

/* ------------------------------------------------------------------ panels */

void set_panel(int mode)
{
    g_panelMode = mode;
    g_panelVis = 1;
    layout();
    if (mode == 2) { if (!term_count()) term_new(); SetFocus(g_term); }
    else if (mode == 1) SetFocus(g_term);
    else SetFocus(g_prob);
}

void set_side(int mode)
{
    g_sideMode = mode;
    g_sideVis = 1;
    layout();
    InvalidateRect(g_side, 0, 0);
    if (mode == 1) search_focus(); else SetFocus(g_side);
}

static void enc_pick_cb(const char *t, int i)
{
    Doc *d = cur_doc();
    (void)t;
    if (!d || i < 0) return;
    d->enc = i; d->savedCu = -1;   /* saving writes the new encoding */
    chrome_dirty();
}

static void lang_pick_cb(const char *t, int i)
{
    Doc *d = cur_doc();
    (void)t;
    if (!d || i < 0) return;
    d->lang = i; d->valid = 1;
    InvalidateRect(g_edit, 0, 0); status_dirty();
}

static void show_shortcuts(void)
{
    Doc *d;
    int i, n = 0;
    char *buf, line[200];
    if (g_ndocs >= MAXDOCS) return;
    buf = (char *)malloc(NCMDS * 120 + 200);
    n = wsprintfA(buf, "XP Code keyboard shortcuts\n\n");
    for (i = 0; i < NCMDS; i++) {
        if (!s_cmds[i].key[0]) continue;
        n += wsprintfA(buf + n, "%-44s %s\n", s_cmds[i].name, s_cmds[i].key);
    }
    lstrcpyA(line, "\nTerminal: Ctrl+C interrupt (or copy a selection), Ctrl+V paste, Tab completes file names,\n"
                   "Up/Down history, Ctrl+click a file:line to open it, right-click copies or pastes.\n");
    lstrcpyA(buf + n, line);
    d = doc_new();
    lstrcpyA(d->name, "Keyboard Shortcuts");
    doc_set_text(d, buf);
    free(buf);
    d->savedCu = d->cu;
    g_docs[g_ndocs++] = d;
    activate_doc(g_ndocs - 1);
}

void run_cmd(int id)
{
    Doc *d = cur_doc();
    HWND f = GetFocus();
    char cls[16] = "";
    int i;
    if (f) GetClassNameA(f, cls, sizeof(cls));
    /* clipboard and undo go to whichever edit box has focus */
    if (!lstrcmpiA(cls, "Edit")) {
        switch (id) {
        case CM_UNDO: SendMessageA(f, WM_UNDO, 0, 0); return;
        case CM_CUT: SendMessageA(f, WM_CUT, 0, 0); return;
        case CM_COPY: SendMessageA(f, WM_COPY, 0, 0); return;
        case CM_PASTE: SendMessageA(f, WM_PASTE, 0, 0); return;
        case CM_SELALL: SendMessageA(f, EM_SETSEL, 0, -1); return;
        }
    }
    if (f == g_term && (id == CM_COPY || id == CM_PASTE)) { SendMessageA(g_term, WM_COMMAND, id, 0); return; }
    switch (id) {
    case CM_NEWFILE: new_file(); return;
    case CM_OPENFILE: { char p[MAX_PATH] = ""; if (file_dialog(0, p, "Open File")) open_file(p, 0, 0); return; }
    case CM_OPENFOLDER: pick_folder(); return;
    case CM_SAVE: if (d) save_doc(d, 0); return;
    case CM_SAVEAS: if (d) save_doc(d, 1); return;
    case CM_SAVEALL: for (i = 0; i < g_ndocs; i++) if (doc_dirty(g_docs[i])) { if (!g_docs[i]->path[0]) activate_doc(i); if (!save_doc(g_docs[i], 0)) return; } return;
    case CM_REVERT: if (d && d->path[0]) { doc_load(d, d->path); chrome_dirty(); InvalidateRect(g_edit, 0, 0); } return;
    case CM_CLOSE: if (d) close_doc(g_cur); return;
    case CM_CLOSEALL: while (g_ndocs) { int n = g_ndocs; close_doc(g_ndocs - 1); if (g_ndocs == n) break; } return;
    case CM_CLOSEFOLDER: run_cmd(CM_CLOSEALL); if (!g_ndocs) open_folder(0); return;
    case CM_EXIT: PostMessageA(g_main, WM_CLOSE, 0, 0); return;
    case CM_PALETTE: pal_show(PAL_CMD, ""); return;
    case CM_QUICKOPEN: pal_show(PAL_FILE, ""); return;
    case CM_GOTOLINE: if (d) pal_show(PAL_LINE, ""); return;
    case CM_GOTOSYM: if (d) pal_show(PAL_SYM, ""); return;
    case CM_NEXTTAB: if (g_ndocs) { activate_doc((g_cur + 1) % g_ndocs); SetFocus(g_edit); } return;
    case CM_PREVTAB: if (g_ndocs) { activate_doc((g_cur + g_ndocs - 1) % g_ndocs); SetFocus(g_edit); } return;
    case CM_EXPLORER: set_side(0); return;
    case CM_SEARCH: case CM_FINDINFILES: set_side(1); return;
    case CM_RUNVIEW: set_side(2); return;
    case CM_TOGGLESIDE: g_sideVis = !g_sideVis; layout(); return;
    case CM_TOGGLEPANEL: g_panelVis = !g_panelVis; layout(); if (g_panelVis) set_panel(g_panelMode); else focus_editor(); return;
    case CM_PROBLEMS: set_panel(0); return;
    case CM_OUTPUT: set_panel(1); return;
    case CM_TERMINAL:
        if (g_panelVis && g_panelMode == 2 && GetFocus() == g_term) { g_panelVis = 0; layout(); focus_editor(); }
        else set_panel(2);
        return;
    case CM_NEWTERM: g_panelMode = 2; g_panelVis = 1; term_new(); layout(); SetFocus(g_term); return;
    case CM_KILLTERM: term_kill(term_active()); layout(); return;
    case CM_CLEARTERM: term_clear(); return;
    case CM_NEXTTERM: term_switch(1); return;
    case CM_PREVTERM: term_switch(-1); return;
    case CM_ZOOMIN: if (g_zoom < 20) g_zoom++; zoom_fonts(); return;
    case CM_ZOOMOUT: if (g_zoom > -6) g_zoom--; zoom_fonts(); return;
    case CM_ZOOMRESET: g_zoom = 0; zoom_fonts(); return;
    case CM_MINIMAP: g_minimap = !g_minimap; find_layout(); InvalidateRect(g_edit, 0, 0); return;
    case CM_RUN: run_active(); return;
    case CM_BUILD: run_build(); return;
    case CM_STOP: term_interrupt(); return;
    case CM_NEXTPROB: prob_next(1); return;
    case CM_PREVPROB: prob_next(-1); return;
    case CM_REVEAL: if (d && d->path[0]) { char a[MAX_PATH + 20]; wsprintfA(a, "/select,\"%s\"", d->path);
        { typedef HINSTANCE (WINAPI *PSE)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT); PSE se = (PSE)dyn("shell32.dll", "ShellExecuteA");
          if (se) se(g_main, "open", "explorer.exe", a, 0, SW_SHOWNORMAL); } } return;
    case CM_COPYPATH: if (d && d->path[0]) clip_set(d->path, lstrlenA(d->path)); return;
    case CM_LANG: if (d) { static const char *names[L_COUNT]; for (i = 0; i < L_COUNT; i++) names[i] = lang_name(i); pal_pick("Select Language Mode", names, L_COUNT, lang_pick_cb); } return;
    case CM_ENCODING: if (d) { static const char *e[3] = { "Windows (ANSI)", "UTF-8 with BOM", "UTF-8" }; pal_pick("Save with Encoding", e, 3, enc_pick_cb); } return;
    case CM_EOL: if (d) { d->crlf = !d->crlf; d->savedCu = -1; chrome_dirty(); } return;
    case CM_INDENTMODE: if (d) { d->tabs = !d->tabs; status_dirty(); } return;
    case CM_SETTINGS: {
        if (!file_exists(g_ini)) {
            HANDLE h = CreateFileA(g_ini, GENERIC_WRITE, 0, 0, CREATE_NEW, 0, 0); DWORD wr;
            const char *t = "; XP Code settings\r\n[editor]\r\n; font=Lucida Console\r\n[run]\r\n; tcc=C:\\tcc\\tcc.exe\r\n; python=C:\\Python27\\python.exe\r\n";
            if (h != INVALID_HANDLE_VALUE) { WriteFile(h, t, lstrlenA(t), &wr, 0); CloseHandle(h); }
        }
        open_file(g_ini, 0, 0);
        return;
    }
    case CM_SHORTCUTS: show_shortcuts(); return;
    case CM_ABOUT:
        {
            char m[MAX_PATH + 300];
            snprintf(m, sizeof(m) - 1, APPNAME "\n\nVersion: " APPVER "\nDate: " APPDATE "\nBuilt: " __DATE__ " " __TIME__
                "\n\nA small VS Code-style editor for Windows XP.\nPlain Win32 C, built with Tiny C Compiler.\n\nEditor font: %s\nSettings: %s", s_monoFace, g_ini);
            m[sizeof(m) - 1] = 0;
            MessageBoxA(g_main, m, "About " APPNAME, MB_ICONINFORMATION);
        }
        return;
    }
    if (id == CM_FIND || id == CM_REPLACE || id == CM_FINDNEXT || id == CM_FINDPREV) { if (d) ed_cmd(id); return; }
    if (d && ed_cmd(id)) SetFocus(g_edit);
}

/* ------------------------------------------------------------------ menus */

static void menu_add(HMENU m, int id)
{
    char t[160];
    const char *n = cmd_name(id), *c = strchr(n, ':');
    const char *k = cmd_key(id);
    if (c && id != CM_LANG) n = c + 2;
    if (k[0]) wsprintfA(t, "%s\t%s", n, k); else lstrcpynA(t, n, sizeof(t));
    AppendMenuA(m, MF_STRING, id, t);
}
#define SEP(m) AppendMenuA(m, MF_SEPARATOR, 0, 0)

/* 0 = separator, 1 = end of a menu */
static const short s_menuDef[] = {
    CM_NEWFILE, 0, CM_OPENFILE, CM_OPENFOLDER, 0, CM_SAVE, CM_SAVEAS, CM_SAVEALL, 0, CM_REVERT, CM_CLOSE,
    CM_CLOSEALL, CM_CLOSEFOLDER, 0, CM_SETTINGS, 0, CM_EXIT, 1,
    CM_UNDO, CM_REDO, 0, CM_CUT, CM_COPY, CM_PASTE, 0, CM_FIND, CM_REPLACE, CM_FINDNEXT, CM_FINDPREV, 0,
    CM_FINDINFILES, 0, CM_COMMENT, CM_TRIMWS, 1,
    CM_SELALL, CM_SELLINE, CM_EXPANDSEL, 0, CM_COPYUP, CM_COPYDOWN, CM_MOVEUP, CM_MOVEDOWN, 0,
    CM_DELLINE, CM_INDENT, CM_OUTDENT, CM_INSLINEBELOW, CM_INSLINEABOVE, 1,
    CM_PALETTE, 0, CM_EXPLORER, CM_SEARCH, CM_RUNVIEW, 0, CM_PROBLEMS, CM_OUTPUT, CM_TERMINAL, 0,
    CM_TOGGLESIDE, CM_TOGGLEPANEL, CM_MINIMAP, 0, CM_ZOOMIN, CM_ZOOMOUT, CM_ZOOMRESET, 1,
    CM_QUICKOPEN, CM_GOTOLINE, CM_GOTOSYM, CM_GOTOBRACKET, 0, CM_NEXTTAB, CM_PREVTAB, 0, CM_NEXTPROB, CM_PREVPROB, 1,
    CM_RUN, CM_BUILD, CM_STOP, 1,
    CM_NEWTERM, CM_KILLTERM, CM_CLEARTERM, 0, CM_NEXTTERM, CM_PREVTERM, 1,
    CM_PALETTE, CM_SHORTCUTS, 0, CM_REVEAL, CM_COPYPATH, 0, CM_ABOUT, 1,
    /* gear menu */
    CM_PALETTE, CM_SHORTCUTS, CM_SETTINGS, 0, CM_ZOOMIN, CM_ZOOMOUT, CM_ZOOMRESET, CM_MINIMAP, 0, CM_ABOUT, 1
};
static const char *s_menuNames[] = { "&File", "&Edit", "&Selection", "&View", "&Go", "&Run", "&Terminal", "&Help" };

static HMENU menu_from(const short **pp)
{
    HMENU m = CreatePopupMenu();
    const short *p = *pp;
    for (; *p != 1; p++) if (*p) menu_add(m, *p); else SEP(m);
    *pp = p + 1;
    return m;
}

static HMENU build_menu(void)
{
    HMENU bar = CreateMenu();
    const short *p = s_menuDef;
    int i;
    for (i = 0; i < 8; i++) AppendMenuA(bar, MF_POPUP, (UINT_PTR)menu_from(&p), s_menuNames[i]);
    return bar;
}

/* ------------------------------------------------------------------ session */

static void ini_int(const char *sec, const char *key, int v) { char b[16]; wsprintfA(b, "%d", v); WritePrivateProfileStringA(sec, key, b, g_ini); }

static void save_session(void)
{
    WINDOWPLACEMENT wp; int i, n = 0; char key[32], val[MAX_PATH + 32];
    wp.length = sizeof(wp);
    GetWindowPlacement(g_main, &wp);
    ini_int("window", "x", wp.rcNormalPosition.left); ini_int("window", "y", wp.rcNormalPosition.top);
    ini_int("window", "w", wp.rcNormalPosition.right - wp.rcNormalPosition.left);
    ini_int("window", "h", wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
    ini_int("window", "max", wp.showCmd == SW_SHOWMAXIMIZED || IsZoomed(g_main));
    ini_int("window", "side", g_sideVis); ini_int("window", "sideW", g_sideW); ini_int("window", "sideMode", g_sideMode);
    ini_int("window", "panel", g_panelVis); ini_int("window", "panelH", g_panelH); ini_int("window", "panelMode", g_panelMode);
    ini_int("window", "zoom", g_zoom); ini_int("window", "minimap", g_minimap);
    WritePrivateProfileStringA("session", 0, 0, g_ini);
    WritePrivateProfileStringA("session", "folder", g_folder, g_ini);
    for (i = 0; i < g_ndocs; i++) {
        Doc *d = g_docs[i];
        if (!d->path[0]) continue;
        wsprintfA(key, "file%d", n);
        wsprintfA(val, "%d,%d,%d,%s", d->cl, d->cc, d->top, d->path);
        WritePrivateProfileStringA("session", key, val, g_ini);
        if (i == g_cur) ini_int("session", "active", n);
        n++;
    }
    ini_int("session", "count", n);
}

static void load_session_files(void)
{
    int i, n = GetPrivateProfileIntA("session", "count", 0, g_ini), act = GetPrivateProfileIntA("session", "active", 0, g_ini);
    char key[32], val[MAX_PATH + 40];
    for (i = 0; i < n; i++) {
        int l = 0, c = 0, t = 0; char *p;
        wsprintfA(key, "file%d", i);
        GetPrivateProfileStringA("session", key, "", val, sizeof(val), g_ini);
        p = val;
        l = atoi(p); p = strchr(p, ','); if (!p) continue;
        c = atoi(++p); p = strchr(p, ','); if (!p) continue;
        t = atoi(++p); p = strchr(p, ','); if (!p) continue;
        p++;
        if (!file_exists(p)) continue;
        if (open_file(p, 0, 0)) {
            Doc *d = cur_doc();
            if (l < d->n) { d->cl = d->al = l; d->cc = d->ac = c <= d->ln[l].len ? c : 0; }
            d->top = t < d->n ? t : 0;
        }
    }
    if (act >= 0 && act < g_ndocs) activate_doc(act);
}

/* ------------------------------------------------------------------ main window */

static int hit_split(int x, int y)
{
    if (g_sideVis && x >= ACT_W + g_sideW - 4 && x <= ACT_W + g_sideW + 1 && y < s_statusR.top) return 1;
    if (g_panelVis && y >= s_phdrR.top - 1 && y <= s_phdrR.top + 4 && x >= s_phdrR.left) return 2;
    return 0;
}

static int tab_at(int x, int y, int *onClose)
{
    int i;
    *onClose = 0;
    if (y < 0 || y >= TAB_H || x < s_tabsR.left) return -1;
    for (i = 0; i < g_ndocs; i++)
        if (x >= s_tabX[i] && x < s_tabX[i] + s_tabW[i]) { *onClose = x >= s_tabX[i] + s_tabW[i] - 28 && x < s_tabX[i] + s_tabW[i] - 6; return i; }
    return -1;
}

static void gear_menu(void)
{
    const short *p = s_menuDef; HMENU m; POINT pt; int i;
    for (i = 0; i < 8; i++) { while (*p != 1) p++; p++; }
    m = menu_from(&p);
    pt.x = ACT_W; pt.y = s_statusR.top - 40;
    ClientToScreen(g_main, &pt);
    TrackPopupMenu(m, TPM_LEFTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_main, 0);
    DestroyMenu(m);
}

static void term_menu(void)
{
    HMENU m = CreatePopupMenu(); POINT p; int i, r;
    for (i = 0; i < term_count(); i++) AppendMenuA(m, MF_STRING | (i == term_active() ? MF_CHECKED : 0), 1 + i, term_label(i));
    SEP(m); menu_add(m, CM_NEWTERM); menu_add(m, CM_KILLTERM);
    GetCursorPos(&p);
    r = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTALIGN, p.x, p.y, 0, g_main, 0);
    DestroyMenu(m);
    if (r > 0 && r <= term_count()) term_select(r - 1);
    else if (r) run_cmd(r);
}

static void tab_menu(int i)
{
    HMENU m = CreatePopupMenu(); POINT p; int r, k;
    AppendMenuA(m, MF_STRING, 1, "Close\tCtrl+W");
    AppendMenuA(m, MF_STRING, 2, "Close Others");
    AppendMenuA(m, MF_STRING, 3, "Close to the Right");
    AppendMenuA(m, MF_STRING, 4, "Close All");
    SEP(m);
    AppendMenuA(m, MF_STRING, 5, "Copy Path");
    AppendMenuA(m, MF_STRING, 6, "Reveal in Explorer View");
    AppendMenuA(m, MF_STRING, 7, "Reveal in Windows Explorer");
    GetCursorPos(&p);
    r = TrackPopupMenu(m, TPM_RETURNCMD, p.x, p.y, 0, g_main, 0);
    DestroyMenu(m);
    switch (r) {
    case 1: close_doc(i); break;
    case 2: for (k = g_ndocs - 1; k >= 0; k--) if (k != i) { Doc *keep = g_docs[i]; close_doc(k); for (i = 0; i < g_ndocs && g_docs[i] != keep; i++); } break;
    case 3: for (k = g_ndocs - 1; k > i; k--) close_doc(k); break;
    case 4: run_cmd(CM_CLOSEALL); break;
    case 5: activate_doc(i); run_cmd(CM_COPYPATH); break;
    case 6: activate_doc(i); set_side(0); tree_reveal(g_docs[i]->path); break;
    case 7: activate_doc(i); run_cmd(CM_REVEAL); break;
    }
}

static BOOL WINAPI ctrl_handler(DWORD t) { (void)t; return TRUE; }

static LRESULT CALLBACK MainProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CREATE: return 0;
    case WM_SIZE: if (g_edit && wp != SIZE_MINIMIZED) layout(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: paint_main(w); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)lp)->ptMinTrackSize.x = 500; ((MINMAXINFO *)lp)->ptMinTrackSize.y = 360; return 0;
    case WM_COMMAND:
        if (LOWORD(wp) > CM_FIRST && LOWORD(wp) < CM_LAST) { run_cmd(LOWORD(wp)); return 0; }
        break;
    case WM_ACTIVATEAPP:
        if (wp) { ed_check_disk(); }
        return 0;
    case WM_TIMER:
        if (wp == 1 && GetForegroundWindow() == w) ed_check_disk();
        if (wp == 1 && s_watch != INVALID_HANDLE_VALUE && WaitForSingleObject(s_watch, 0) == WAIT_OBJECT_0) {
            FindNextChangeNotification(s_watch);
            tree_refresh(); files_invalidate();
        }
        return 0;
    case WM_SETCURSOR:
        if ((HWND)wp == w && LOWORD(lp) == HTCLIENT) {
            POINT p; int s;
            GetCursorPos(&p); ScreenToClient(w, &p);
            s = s_split ? s_split : hit_split(p.x, p.y);
            if (s) { SetCursor(LoadCursor(0, s == 1 ? IDC_SIZEWE : IDC_SIZENS)); return TRUE; }
            SetCursor(LoadCursor(0, IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEWHEEL: {   /* over the tab strip: switch tabs */
        POINT p; p.x = (short)LOWORD(lp); p.y = (short)HIWORD(lp);
        ScreenToClient(w, &p);
        if (p.y < TAB_H && p.x >= s_tabsR.left && g_ndocs > 1) run_cmd((short)HIWORD(wp) > 0 ? CM_PREVTAB : CM_NEXTTAB);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONUP: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), i, oc;
        POINT pt; pt.x = x; pt.y = y;
        if (m == WM_LBUTTONDOWN && (s_split = hit_split(x, y))) { SetCapture(w); InvalidateRect(w, 0, 0); return 0; }
        if (x < ACT_W && y < s_statusR.top) {
            if (m != WM_LBUTTONDOWN) return 0;
            i = y / 48;
            if (i < 3) { if (g_sideVis && g_sideMode == i) { g_sideVis = 0; layout(); focus_editor(); } else set_side(i); }
            else if (y > s_statusR.top - 48) gear_menu();
            return 0;
        }
        i = tab_at(x, y, &oc);
        if (i >= 0) {
            if (m == WM_MBUTTONDOWN || (m == WM_LBUTTONDOWN && oc)) close_doc(i);
            else if (m == WM_LBUTTONDOWN) { activate_doc(i); SetFocus(g_edit); }
            else if (m == WM_RBUTTONUP) tab_menu(i);
            return 0;
        }
        if (m != WM_LBUTTONDOWN) return 0;
        if (PtInRect(&s_statusR, pt)) {
            for (i = 0; i < s_nst; i++) if (PtInRect(&s_stItem[i], pt) && s_stCmd[i] > 0) { run_cmd(s_stCmd[i]); return 0; }
            return 0;
        }
        if (PtInRect(&s_phdrR, pt)) {
            for (i = 0; i < 3; i++) if (PtInRect(&s_phTab[i], pt)) { set_panel(i); return 0; }
            for (i = 0; i < s_nph; i++) if (PtInRect(&s_phBtn[i], pt)) {
                if (s_phCmd[i] == -1) { g_panelH = g_panelH < (s_statusR.top - 200) ? s_statusR.top - 120 : 230; layout(); }
                else if (s_phCmd[i] == -2) term_menu();
                else run_cmd(s_phCmd[i]);
                return 0;
            }
            return 0;
        }
        if (PtInRect(&s_crumbR, pt)) { pal_show(PAL_FILE, ""); return 0; }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), oc;
        if (y < TAB_H && x > s_tabsR.left && tab_at(x, y, &oc) < 0) new_file();
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), oc, t;
        if (s_split == 1) { g_sideW = x - ACT_W; layout(); return 0; }
        if (s_split == 2) { g_panelH = s_statusR.top - y; layout(); return 0; }
        t = tab_at(x, y, &oc);
        if (t != s_hoverTab || (oc ? t : -1) != s_hoverClose) {
            TRACKMOUSEEVENT tme;
            s_hoverTab = t; s_hoverClose = oc ? t : -1;
            InvalidateRect(w, &s_tabsR, 0);
            tme.cbSize = sizeof(tme); tme.dwFlags = TME_LEAVE; tme.hwndTrack = w; tme.dwHoverTime = 0;
            TrackMouseEvent(&tme);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (s_hoverTab >= 0) { s_hoverTab = s_hoverClose = -1; InvalidateRect(w, &s_tabsR, 0); }
        return 0;
    case WM_LBUTTONUP:
        if (s_split) { s_split = 0; ReleaseCapture(); InvalidateRect(w, 0, 0); }
        return 0;
    case WM_DROPFILES: {
        typedef UINT (WINAPI *PDQ)(HANDLE, UINT, LPSTR, UINT);
        typedef void (WINAPI *PDF)(HANDLE);
        PDQ q = (PDQ)dyn("shell32.dll", "DragQueryFileA"); PDF fin = (PDF)dyn("shell32.dll", "DragFinish");
        char p[MAX_PATH]; UINT i, n;
        if (!q) return 0;
        n = q((HANDLE)wp, 0xFFFFFFFF, 0, 0);
        for (i = 0; i < n; i++) if (q((HANDLE)wp, i, p, MAX_PATH)) { if (dir_exists(p)) open_folder(p); else open_file(p, 0, 0); }
        if (fin) fin((HANDLE)wp);
        return 0;
    }
    case WM_CLOSE: {
        int i;
        for (i = 0; i < g_ndocs; i++) if (doc_dirty(g_docs[i])) { activate_doc(i); if (!confirm_close(g_docs[i])) return 0; }
        save_session();
        term_kill_all();
        DestroyWindow(w);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

static void get_cmdline_path(char *out)
{
    const char *p = GetCommandLineA();
    int q = 0, n;
    out[0] = 0;
    if (*p == '"') { p++; while (*p && *p != '"') p++; if (*p) p++; }
    else while (*p && *p != ' ' && *p != '\t') p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') { p++; q = 1; }
    lstrcpynA(out, p, MAX_PATH);
    n = lstrlenA(out);
    while (n > 0 && (out[n-1] == ' ' || (q && out[n-1] == '"'))) out[--n] = 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cl, int show)
{
    WNDCLASSA wc; MSG msg; char *p, arg[MAX_PATH], appdata[MAX_PATH];
    int x, y, w, h;
    typedef void (WINAPI *PICC)(void);
    PICC icc;
    (void)prev; (void)cl;
    g_inst = inst;
    icc = (PICC)dyn("comctl32.dll", "InitCommonControls");
    if (icc) icc();
    SetConsoleCtrlHandler(ctrl_handler, TRUE);
    GetModuleFileNameA(0, g_exeDir, MAX_PATH);
    p = strrchr(g_exeDir, '\\'); if (p) *p = 0;
    path_join(g_ini, g_exeDir, "xpcode.ini");
    if (!file_exists(g_ini) && GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH)) {
        path_join(appdata, appdata, "XP Code");
        CreateDirectoryA(appdata, 0);
        path_join(g_ini, appdata, "xpcode.ini");
    }
    {   /* a tcc folder next to the exe goes on PATH (exact entry match, any case) */
        char tdir[MAX_PATH], path[8192], *e; int n = lstrlenA(g_exeDir) + 4, found = 0;
        path_join(tdir, g_exeDir, "tcc");
        if (dir_exists(tdir) && GetEnvironmentVariableA("PATH", path, sizeof(path) - MAX_PATH - 2) < sizeof(path) - MAX_PATH - 2) {
            for (e = path; *e; ) {
                char *semi = strchr(e, ';'); int len = semi ? (int)(semi - e) : lstrlenA(e);
                if (len == n && !_strnicmp(e, tdir, n)) found = 1;
                if (!semi) break;
                e = semi + 1;
            }
            if (!found) { char np[8192 + MAX_PATH]; wsprintfA(np, "%s;", tdir); lstrcatA(np, path); SetEnvironmentVariableA("PATH", np); }
        }
    }
    g_brInput = CreateSolidBrush(C_INPUT); g_brSide = CreateSolidBrush(C_SIDE); g_brBg = CreateSolidBrush(C_BG);
    g_zoom = GetPrivateProfileIntA("window", "zoom", 0, g_ini);
    g_minimap = GetPrivateProfileIntA("window", "minimap", 1, g_ini);
    g_sideVis = GetPrivateProfileIntA("window", "side", 1, g_ini);
    g_sideW = GetPrivateProfileIntA("window", "sideW", 250, g_ini);
    g_sideMode = GetPrivateProfileIntA("window", "sideMode", 0, g_ini) % 3;
    g_panelVis = GetPrivateProfileIntA("window", "panel", 1, g_ini);
    g_panelH = GetPrivateProfileIntA("window", "panelH", 230, g_ini);
    g_panelMode = GetPrivateProfileIntA("window", "panelMode", 2, g_ini) % 3;
    init_fonts();

    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = MainProc; wc.hInstance = inst; wc.lpszClassName = "XPCodeMain";
    wc.hIcon = LoadIconA(inst, MAKEINTRESOURCEA(1));
    if (!wc.hIcon) wc.hIcon = LoadIconA(0, (LPCSTR)IDI_APPLICATION);
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    RegisterClassA(&wc);
    ed_register(); side_register(); term_register(); pal_register();

    w = GetPrivateProfileIntA("window", "w", 1100, g_ini);
    h = GetPrivateProfileIntA("window", "h", 720, g_ini);
    x = GetPrivateProfileIntA("window", "x", CW_USEDEFAULT, g_ini);
    y = GetPrivateProfileIntA("window", "y", CW_USEDEFAULT, g_ini);
    if (x != CW_USEDEFAULT && (x < -50 || y < -50 || x > GetSystemMetrics(SM_CXVIRTUALSCREEN) - 100 || y > GetSystemMetrics(SM_CYVIRTUALSCREEN) - 100)) x = y = CW_USEDEFAULT;
    g_main = CreateWindowExA(WS_EX_ACCEPTFILES, "XPCodeMain", APPNAME, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             x, y, w, h, 0, build_menu(), inst, 0);
    g_edit = CreateWindowExA(0, "XPEdit", "", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10, g_main, 0, inst, 0);
    g_side = CreateWindowExA(0, "XPSide", "", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10, g_main, 0, inst, 0);
    g_term = CreateWindowExA(0, "XPTerm", "", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 10, 10, g_main, 0, inst, 0);
    g_prob = CreateWindowExA(0, "XPProblems", "", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 10, 10, g_main, 0, inst, 0);
    ed_font_changed();
    term_font_changed();

    get_cmdline_path(arg);
    GetPrivateProfileStringA("session", "folder", "", appdata, MAX_PATH, g_ini);
    if (arg[0] && dir_exists(arg)) open_folder(arg);
    else if (appdata[0] && dir_exists(appdata)) open_folder(appdata);
    else open_folder(0);
    layout();
    ShowWindow(g_main, GetPrivateProfileIntA("window", "max", 0, g_ini) ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(g_main);
    if (!(arg[0] && dir_exists(arg))) load_session_files();
    if (arg[0] && file_exists(arg)) open_file(arg, 0, 0);
    if (g_panelVis && g_panelMode == 2) term_new();
    layout();
    if (cur_doc()) SetFocus(g_edit); else SetFocus(g_side);
    SetTimer(g_main, 1, 2000, 0);
    g_ninit = 1;

    while (GetMessageA(&msg, 0, 0, 0) > 0) {
        if (global_key(&msg)) continue;
        if (msg.message == WM_MOUSEWHEEL) {   /* XP sends the wheel to the focus; send it to what is under the mouse */
            POINT p; HWND h;
            p.x = (short)LOWORD(msg.lParam); p.y = (short)HIWORD(msg.lParam);
            h = WindowFromPoint(p);
            if (h && GetWindowThreadProcessId(h, 0) == GetCurrentThreadId()) msg.hwnd = h;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return 0;
}
