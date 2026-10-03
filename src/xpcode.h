/* xpcode.h - shared declarations for XP Code, a tiny VS Code-style editor.
 * Plain Win32 C. Builds with Tiny C Compiler 0.9.27 (and MinGW).
 * Only kernel32, user32, gdi32 and msvcrt are linked; everything newer or
 * outside those DLLs is loaded at run time so the exe starts on any XP. */
#ifndef XPCODE_H
#define XPCODE_H

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0501
#define WINVER 0x0501
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APPNAME "XP Code"
#define APPVER  "1.1.0"
#define APPDATE "2026-10-03"   /* release date of APPVER */

#ifndef WM_MOUSEWHEEL
#define WM_MOUSEWHEEL 0x020A
#endif
#ifndef CLEARTYPE_QUALITY
#define CLEARTYPE_QUALITY 5
#endif
#ifndef GET_X_LPARAM
#define GET_X_LPARAM(l) ((int)(short)LOWORD(l))
#define GET_Y_LPARAM(l) ((int)(short)HIWORD(l))
#endif
#ifndef VK_OEM_2
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_PLUS 0xBB
#define VK_OEM_MINUS 0xBD
#endif
#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

/* TCC 0.9.27 ships a trimmed winapi; declare the few missing bits */
#ifndef CP_UTF8
#define CP_UTF8 65001
#define CP_ACP 0
WINBASEAPI int WINAPI MultiByteToWideChar(UINT, DWORD, LPCSTR, int, LPWSTR, int);
WINBASEAPI int WINAPI WideCharToMultiByte(UINT, DWORD, LPCWSTR, int, LPSTR, int, LPCSTR, LPBOOL);
WINBASEAPI UINT WINAPI GetACP(void);
#endif

#define snprintf _snprintf
#define XRGB(r,g,b) ((COLORREF)(((BYTE)(r)|((WORD)((BYTE)(g))<<8))|(((DWORD)(BYTE)(b))<<16)))

/* ---- colours (VS Code Dark+) ---- */
#define C_BG        XRGB(0x1E,0x1E,0x1E)
#define C_SIDE      XRGB(0x25,0x25,0x26)
#define C_ACT       XRGB(0x33,0x33,0x33)
#define C_TABBAR    XRGB(0x25,0x25,0x26)
#define C_TABOFF    XRGB(0x2D,0x2D,0x2D)
#define C_STATUS    XRGB(0x00,0x7A,0xCC)
#define C_STATUSNF  XRGB(0x68,0x21,0x7A)
#define C_TEXT      XRGB(0xD4,0xD4,0xD4)
#define C_DIM       XRGB(0x96,0x96,0x96)
#define C_LINENO    XRGB(0x85,0x85,0x85)
#define C_LINENOA   XRGB(0xC6,0xC6,0xC6)
#define C_SEL       XRGB(0x26,0x4F,0x78)
#define C_SELOFF    XRGB(0x3A,0x3D,0x41)
#define C_FINDHL    XRGB(0x62,0x33,0x15)
#define C_CURLINE   XRGB(0x28,0x28,0x28)
#define C_GUIDE     XRGB(0x40,0x40,0x40)
#define C_LISTSEL   XRGB(0x37,0x37,0x3D)
#define C_LISTFOC   XRGB(0x09,0x47,0x71)
#define C_LISTHOV   XRGB(0x2A,0x2D,0x2E)
#define C_BORDER    XRGB(0x3C,0x3C,0x3C)
#define C_INPUT     XRGB(0x3C,0x3C,0x3C)
#define C_WIDGET    XRGB(0x25,0x25,0x26)
#define C_FOCUS     XRGB(0x00,0x7F,0xD4)
#define C_WHITE     XRGB(0xFF,0xFF,0xFF)
#define C_ERR       XRGB(0xF4,0x87,0x71)
#define C_WARN      XRGB(0xCC,0xA7,0x00)
#define C_SCROLL    XRGB(0x4F,0x4F,0x4F)
#define C_BRACKET   XRGB(0x88,0x88,0x88)

/* token classes */
enum { T_TEXT, T_KEYWORD, T_CONTROL, T_TYPE, T_STRING, T_NUMBER, T_COMMENT,
       T_PREPROC, T_FUNC, T_TAG, T_ATTR, T_HEADING, T_OP, T_CONST, T_PUNCT, T_COUNT };
extern COLORREF g_tokColor[T_COUNT];

/* languages */
enum { L_TEXT, L_C, L_PY, L_JS, L_BAT, L_INI, L_HTML, L_MD, L_COUNT };

/* ---- document ---- */
typedef struct { char *s; int len, cap; unsigned char st; } Line;
typedef struct { char ins; int grp, l, c; char *t; int n; int bl, bc, ba, bb; } Undo;

typedef struct Doc {
    char path[MAX_PATH];           /* empty = untitled */
    char name[MAX_PATH];
    Line *ln; int n, cap;
    int lang, crlf, tabs, untitledNo;
    Undo *u; int nu, cu, capu, grp, savedCu, typing;
    int cl, cc, al, ac, wantX;     /* caret line/col, anchor line/col, sticky x */
    int top, left;                 /* first visible line, horizontal px */
    int valid;                     /* lexer states valid up to this line */
    FILETIME ft; int ftAsked;
    int enc;                       /* 0 ansi, 1 utf-8 bom, 2 utf-8 */
    int lossy;                     /* chars the ANSI code page could not hold became ? */
} Doc;

#define MAXDOCS 64
extern Doc *g_docs[MAXDOCS];
extern int g_ndocs, g_cur;
Doc *cur_doc(void);
int  doc_dirty(Doc *d);

/* ---- globals ---- */
extern HINSTANCE g_inst;
extern HWND g_main, g_edit, g_side, g_term, g_prob, g_find, g_pal;
extern HFONT g_ui, g_uiBold, g_uiBig, g_mono, g_monoMini;
extern int g_cw, g_lh, g_uiH;      /* mono char width, line height, ui text height */
extern int g_zoom, g_minimap;
extern char g_folder[MAX_PATH];
extern char g_exeDir[MAX_PATH], g_ini[MAX_PATH];
extern int g_sideVis, g_sideMode, g_panelVis, g_panelMode, g_sideW, g_panelH;
extern HBRUSH g_brInput, g_brSide, g_brBg;

/* ---- main.c ---- */
void layout(void);
void chrome_dirty(void);
void status_dirty(void);
void set_title(void);
int  open_file(const char *path, int line, int col);
void activate_doc(int i);
void close_doc(int i);
void open_folder(const char *path);
void run_cmd(int id);
void set_panel(int mode);
void set_side(int mode);
void zoom_fonts(void);
const char *rel_path(const char *full);
void ui_text(HDC dc, int x, int y, const char *s, COLORREF c);
void fill(HDC dc, int x, int y, int w, int h, COLORREF c);
void hline(HDC dc, int x, int y, int w, COLORREF c);
void vline(HDC dc, int x, int y, int h, COLORREF c);
void draw_icon(HDC dc, int kind, int x, int y, COLORREF c);
void file_glyph(HDC dc, const char *name, int isdir, int x, int y);
int  key_mods(void);
int  confirm_close(Doc *d);
int  save_doc(Doc *d, int as);
void focus_editor(void);
HDC  bb_begin(HWND w, HDC wdc, int W, int H);
extern int g_ninit;

/* icons for draw_icon */
enum { IC_FILES, IC_SEARCH, IC_RUN, IC_GEAR, IC_CLOSE, IC_PLUS, IC_TRASH, IC_CHEVR, IC_CHEVD,
       IC_ERR, IC_WARN, IC_UP, IC_DOWN, IC_MAX, IC_DOT, IC_REFRESH, IC_NEWFILE, IC_NEWDIR, IC_COLLAPSE };

/* mods */
#define M_CTRL 1
#define M_SHIFT 2
#define M_ALT 4

/* commands */
enum {
  CM_FIRST = 1000,
  CM_NEWFILE, CM_OPENFILE, CM_OPENFOLDER, CM_SAVE, CM_SAVEAS, CM_SAVEALL, CM_REVERT,
  CM_CLOSE, CM_CLOSEALL, CM_CLOSEFOLDER, CM_EXIT,
  CM_UNDO, CM_REDO, CM_CUT, CM_COPY, CM_PASTE, CM_FIND, CM_REPLACE, CM_FINDNEXT, CM_FINDPREV,
  CM_FINDINFILES, CM_COMMENT, CM_BLOCKCOMMENT,
  CM_SELALL, CM_SELLINE, CM_EXPANDSEL, CM_MOVEUP, CM_MOVEDOWN, CM_COPYUP, CM_COPYDOWN, CM_DELLINE,
  CM_INDENT, CM_OUTDENT, CM_INSLINEBELOW, CM_INSLINEABOVE, CM_TRIMWS,
  CM_PALETTE, CM_EXPLORER, CM_SEARCH, CM_RUNVIEW, CM_TOGGLESIDE, CM_TOGGLEPANEL,
  CM_PROBLEMS, CM_OUTPUT, CM_TERMINAL, CM_ZOOMIN, CM_ZOOMOUT, CM_ZOOMRESET, CM_MINIMAP,
  CM_QUICKOPEN, CM_GOTOLINE, CM_GOTOSYM, CM_NEXTTAB, CM_PREVTAB, CM_GOTOBRACKET, CM_NEXTPROB, CM_PREVPROB,
  CM_RUN, CM_BUILD, CM_STOP, CM_NEWTERM, CM_KILLTERM, CM_CLEARTERM, CM_NEXTTERM, CM_PREVTERM,
  CM_ABOUT, CM_SHORTCUTS, CM_REVEAL, CM_SETTINGS, CM_COPYPATH, CM_LANG, CM_EOL, CM_INDENTMODE, CM_ENCODING,
  CM_LAST
};

/* ---- editor.c ---- */
void ed_register(void);
Doc *doc_new(void);
void doc_free(Doc *d);
int  doc_load(Doc *d, const char *path);
int  doc_write(Doc *d, const char *path);
void doc_set_lang(Doc *d);
void doc_set_text(Doc *d, const char *t);
void ed_goto(Doc *d, int line, int col, int center);
int  ed_cmd(int id);
void ed_font_changed(void);
void ed_check_disk(void);
void ed_caret_sync(void);
char *doc_sel_text(Doc *d, int *len);
int  doc_has_sel(Doc *d);
void find_show(int replace);
void find_layout(void);
int  find_visible(void);
void ed_scroll_to_caret(void);

/* ---- syntax.c ---- */
int  lang_from_path(const char *path);
const char *lang_name(int lang);
const char *lang_comment(int lang, const char **blockEnd);
int  lex_line(int lang, const char *s, int n, int st, unsigned char *cls);

/* ---- explorer.c ---- */
void side_register(void);
void tree_set_root(const char *path);
void tree_refresh(void);
void tree_reveal(const char *path);
void side_layout(void);
void search_focus(void);

/* ---- terminal.c ---- */
typedef struct { char file[MAX_PATH]; int line, col, sev, term; char msg[300]; } Problem;
extern Problem *g_probs; extern int g_nprobs, g_nerr, g_nwarn;
void term_register(void);
int  term_new(void);
void term_kill(int i);
void term_kill_all(void);
void term_send(const char *cmd, int show);
void term_interrupt(void);
void term_clear(void);
void term_switch(int delta);
int  term_count(void);
int  term_active(void);
const char *term_label(int i);
void term_select(int i);
void out_log(const char *fmt, ...);
void term_font_changed(void);
void prob_goto(int i);
void prob_next(int dir);

/* ---- palette.c ---- */
enum { PAL_CMD, PAL_FILE, PAL_LINE, PAL_INPUT, PAL_PICK, PAL_SYM };
typedef void (*PalCb)(const char *text, int index);
void pal_register(void);
void pal_show(int mode, const char *init);
void pal_input(const char *title, const char *init, PalCb cb);
void pal_pick(const char *title, const char **items, int n, PalCb cb);
void pal_close(void);
int  fuzzy(const char *pat, const char *s, unsigned char *hits);
void files_invalidate(void);
const char *cmd_name(int id);
const char *cmd_key(int id);
int  cmd_count(void);
int  cmd_at(int i);

/* ---- small helpers (util in main.c) ---- */
void *xalloc(int n);
char *xstrdup(const char *s);
int  file_exists(const char *p);
int  dir_exists(const char *p);
void path_join(char *out, const char *a, const char *b);
const char *path_name(const char *p);
int in_list(const char *list, const char *s, int n, int icase);
void path_dir(char *out, const char *p);
int  read_file(const char *path, char **buf, int *len);
int  skip_build_dir(const char *name);
FARPROC dyn(const char *dll, const char *fn);
int  clip_set(const char *s, int n);
char *clip_get(void);

/* ---- custom scroll bar shared by every pane ---- */
typedef struct { int pos, max, page; int drag, dragOff, hot; RECT r; } VScroll;
void vs_draw(HDC dc, VScroll *v);
int  vs_mouse(VScroll *v, HWND w, UINT msg, int x, int y); /* returns 1 if consumed */
int  vs_clamp(VScroll *v, int pos);

#endif
