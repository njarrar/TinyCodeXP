/* syntax.c - one-pass line lexers. Each line starts in a state carried over
 * from the previous line (block comments, triple quotes, code fences). */
#include "xpcode.h"

COLORREF g_tokColor[T_COUNT] = {
    XRGB(0xD4,0xD4,0xD4), XRGB(0x56,0x9C,0xD6), XRGB(0xC5,0x86,0xC0), XRGB(0x4E,0xC9,0xB0),
    XRGB(0xCE,0x91,0x78), XRGB(0xB5,0xCE,0xA8), XRGB(0x6A,0x99,0x55), XRGB(0xC5,0x86,0xC0),
    XRGB(0xDC,0xDC,0xAA), XRGB(0x56,0x9C,0xD6), XRGB(0x9C,0xDC,0xFE), XRGB(0x56,0x9C,0xD6),
    XRGB(0xD4,0xD4,0xD4), XRGB(0x4F,0xC1,0xFF), XRGB(0x80,0x80,0x80)
};

static const char *ext_map[] = {
    "c",".c.h.cpp.cc.cxx.hpp.hh.hxx.inl.rc.",
    "p",".py.pyw.",
    "j",".js.jsx.ts.tsx.json.mjs.cjs.java.cs.go.",
    "b",".bat.cmd.",
    "i",".ini.cfg.conf.inf.reg.properties.toml.gitattributes.gitignore.editorconfig.",
    "h",".html.htm.xml.xhtml.svg.xaml.manifest.vcproj.csproj.xsl.",
    "m",".md.markdown.",
    0
};

int lang_from_path(const char *path)
{
    const char *n = path_name(path), *dot = strrchr(n, '.');
    char e[40]; int i;
    if (!dot) return L_TEXT;
    if (strlen(dot) > 30) return L_TEXT;
    sprintf(e, "%s.", dot);
    CharLowerA(e);
    for (i = 0; ext_map[i]; i += 2)
        if (strstr(ext_map[i+1], e)) {
            switch (ext_map[i][0]) {
            case 'c': return L_C; case 'p': return L_PY; case 'j': return L_JS;
            case 'b': return L_BAT; case 'i': return L_INI; case 'h': return L_HTML;
            case 'm': return L_MD;
            }
        }
    return L_TEXT;
}

const char *lang_name(int l)
{
    static const char *n[] = { "Plain Text", "C", "Python", "JavaScript", "Batch", "Ini", "HTML", "Markdown" };
    return (l >= 0 && l < L_COUNT) ? n[l] : n[0];
}

const char *lang_comment(int l, const char **end)
{
    *end = 0;
    switch (l) {
    case L_C: case L_JS: return "//";
    case L_PY: return "#";
    case L_BAT: return "REM ";
    case L_INI: return ";";
    case L_HTML: *end = " -->"; return "<!-- ";
    case L_MD: *end = " -->"; return "<!-- ";
    }
    return 0;
}

/* word lists: space separated, leading and trailing space */
static const char *kw_c =
 " struct union enum typedef static extern const volatile sizeof inline register auto signed unsigned"
 " void char short int long float double class public private protected namespace template typename"
 " new delete this virtual operator using bool true false NULL nullptr friend explicit mutable"
 " __int64 __cdecl __stdcall WINAPI CALLBACK APIENTRY __declspec restrict _Bool static_cast"
 " reinterpret_cast const_cast dynamic_cast constexpr noexcept override final ";
static const char *ctl_c =
 " if else for while do switch case default break continue return goto try catch throw ";
static const char *ty_c =
 " DWORD WORD BYTE BOOL UINT LONG ULONG INT HANDLE HWND HDC HINSTANCE HMODULE HMENU HFONT HBRUSH HPEN"
 " HBITMAP HICON HCURSOR LPARAM WPARAM LRESULT LPSTR LPCSTR LPWSTR LPCWSTR LPVOID PVOID SIZE_T COLORREF"
 " RECT POINT SIZE MSG FILE size_t ssize_t ptrdiff_t wchar_t int8_t int16_t int32_t int64_t uint8_t"
 " uint16_t uint32_t uint64_t uintptr_t intptr_t va_list TCHAR CHAR WCHAR FILETIME"
 " std string vector map ";
static const char *kw_js =
 " var let const function class new this typeof instanceof void delete in of true false null"
 " undefined extends super static get set async await import export from as interface type enum"
 " public private protected implements package NaN Infinity ";
static const char *ctl_js =
 " if else for while do switch case default break continue return try catch finally throw yield with ";
static const char *kw_py =
 " def class lambda import from as global nonlocal True False None and or not in is pass del with"
 " async await self cls ";
static const char *ctl_py =
 " if elif else for while try except finally raise return yield break continue assert match case ";
static const char *fn_py =
 " print len range str int float list dict set tuple open input type isinstance enumerate zip map"
 " filter sorted reversed sum min max abs any all repr super object hasattr getattr setattr iter next"
 " bool bytes format round chr ord hex id dir vars exit ";
static const char *kw_bat =
 " echo set if else for in do goto call exit setlocal endlocal not exist defined errorlevel shift pause"
 " cd chdir del erase copy move md mkdir rd rmdir start cls title type ren rename pushd popd equ neq"
 " lss leq gtr geq nul enabledelayedexpansion enableextensions on off xcopy find findstr dir path"
 " prompt ver vol color choice timeout where ";

int in_list(const char *list, const char *s, int n, int icase)
{
    char w[64]; const char *p;
    if (n <= 0 || n > 60) return 0;
    w[0] = ' '; memcpy(w + 1, s, n); w[n+1] = ' '; w[n+2] = 0;
    if (!icase) return strstr(list, w) != 0;
    CharLowerA(w);
    p = strstr(list, w);
    return p != 0;
}

#define ISID(c) (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z') || ((c) >= '0' && (c) <= '9') || (c) == '_' || (unsigned char)(c) >= 0x80)
#define ISDIG(c) ((c) >= '0' && (c) <= '9')

static void set(unsigned char *cls, int a, int b, int t) { while (a < b) cls[a++] = (unsigned char)t; }

static int all_caps(const char *s, int n)
{
    int i, up = 0;
    for (i = 0; i < n; i++) {
        if (s[i] >= 'a' && s[i] <= 'z') return 0;
        if (s[i] >= 'A' && s[i] <= 'Z') up++;
    }
    return up > 0 && n > 1;
}

static int next_nonspace(const char *s, int i, int n)
{
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    return i < n ? s[i] : 0;
}

/* C, C++, JS: st 1 = block comment, 2 = template string */
static int lex_clike(int js, const char *s, int n, int st, unsigned char *cls)
{
    int i = 0, j;
    const char *kw = js ? kw_js : kw_c, *ctl = js ? ctl_js : ctl_c;
    if (st == 1) {
        for (; i < n; i++) if (s[i] == '*' && i + 1 < n && s[i+1] == '/') { i += 2; set(cls, 0, i, T_COMMENT); st = 0; break; }
        if (st) { set(cls, 0, n, T_COMMENT); return 1; }
    } else if (st == 2) {
        for (; i < n; i++) { if (s[i] == '\\') { i++; continue; } if (s[i] == '`') { i++; st = 0; break; } }
        set(cls, 0, i, T_STRING);
        if (st) return 2;
    }
    if (!js) {
        j = i; while (j < n && (s[j] == ' ' || s[j] == '\t')) j++;
        if (j < n && s[j] == '#') {
            int k = j + 1; while (k < n && (s[k] == ' ' || s[k] == '\t')) k++;
            while (k < n && ISID(s[k])) k++;
            set(cls, i, k, T_PREPROC);
            if (k - j >= 8 && !strncmp(s + j, "#include", 8)) {
                int m = k; while (m < n && s[m] == ' ') m++;
                if (m < n && s[m] == '<') { int e = m; while (e < n && s[e] != '>') e++; if (e < n) e++; set(cls, k, e, T_STRING); k = e; }
            }
            i = k;
        }
    }
    while (i < n) {
        char c = s[i];
        if (c == '/' && i + 1 < n && s[i+1] == '/') { set(cls, i, n, T_COMMENT); return 0; }
        if (c == '/' && i + 1 < n && s[i+1] == '*') {
            j = i + 2;
            while (j < n && !(s[j] == '*' && j + 1 < n && s[j+1] == '/')) j++;
            if (j >= n) { set(cls, i, n, T_COMMENT); return 1; }
            set(cls, i, j + 2, T_COMMENT); i = j + 2; continue;
        }
        if (c == '"' || c == '\'' || (js && c == '`')) {
            j = i + 1;
            while (j < n && s[j] != c) { if (s[j] == '\\') j++; j++; }
            if (j >= n && c == '`') { set(cls, i, n, T_STRING); return 2; }
            if (j < n) j++;
            if (j > n) j = n;
            set(cls, i, j, T_STRING); i = j; continue;
        }
        if (ISDIG(c) || (c == '.' && i + 1 < n && ISDIG(s[i+1]))) {
            j = i + 1;
            while (j < n && (ISID(s[j]) || s[j] == '.' || ((s[j] == '+' || s[j] == '-') && (s[j-1] == 'e' || s[j-1] == 'E') && s[i] != '0'))) j++;
            set(cls, i, j, T_NUMBER); i = j; continue;
        }
        if (ISID(c)) {
            int t = T_ATTR, len;
            j = i; while (j < n && ISID(s[j])) j++;
            len = j - i;
            if (in_list(ctl, s + i, len, 0)) t = T_CONTROL;
            else if (in_list(kw, s + i, len, 0)) t = T_KEYWORD;
            else if (!js && in_list(ty_c, s + i, len, 0)) t = T_TYPE;
            else if (next_nonspace(s, j, n) == '(') t = T_FUNC;
            else if (len > 2 && s[j-2] == '_' && s[j-1] == 't') t = T_TYPE;
            else if (all_caps(s + i, len)) t = T_CONST;
            else if (s[i] >= 'A' && s[i] <= 'Z' && (js || next_nonspace(s, j, n) == '*' || ISID(next_nonspace(s, j, n)))) t = T_TYPE;
            set(cls, i, j, t); i = j; continue;
        }
        cls[i++] = T_TEXT;
    }
    return 0;
}

/* Python: st 1 = in ''' string, 2 = in """ string */
static int lex_py(const char *s, int n, int st, unsigned char *cls)
{
    int i = 0, j;
    if (st) {
        char q = st == 1 ? '\'' : '"';
        for (; i + 2 < n || i < n; i++) {
            if (s[i] == '\\') { i++; continue; }
            if (i + 2 < n && s[i] == q && s[i+1] == q && s[i+2] == q) { i += 3; set(cls, 0, i, T_STRING); st = 0; break; }
        }
        if (st) { set(cls, 0, n, T_STRING); return st; }
    }
    while (i < n) {
        char c = s[i];
        if (c == '#') { set(cls, i, n, T_COMMENT); return 0; }
        if (c == '"' || c == '\'' ) {
            int k = i; while (k > 0 && (s[k-1]=='r'||s[k-1]=='b'||s[k-1]=='f'||s[k-1]=='u'||s[k-1]=='R'||s[k-1]=='B'||s[k-1]=='F')) k--;
            if (i + 2 < n && s[i+1] == c && s[i+2] == c) {
                j = i + 3;
                while (j < n) { if (s[j] == '\\') { j += 2; continue; } if (j + 2 < n && s[j] == c && s[j+1] == c && s[j+2] == c) break; j++; }
                if (j >= n) { set(cls, k, n, T_STRING); return c == '\'' ? 1 : 2; }
                set(cls, k, j + 3, T_STRING); i = j + 3; continue;
            }
            j = i + 1;
            while (j < n && s[j] != c) { if (s[j] == '\\') j++; j++; }
            if (j < n) j++;
            if (j > n) j = n;
            set(cls, k, j, T_STRING); i = j; continue;
        }
        if (c == '@' && (i == 0 || next_nonspace(s, 0, n) == '@')) {
            j = i + 1; while (j < n && (ISID(s[j]) || s[j] == '.')) j++;
            set(cls, i, j, T_FUNC); i = j; continue;
        }
        if (ISDIG(c) || (c == '.' && i + 1 < n && ISDIG(s[i+1]))) {
            j = i + 1; while (j < n && (ISID(s[j]) || s[j] == '.')) j++;
            set(cls, i, j, T_NUMBER); i = j; continue;
        }
        if (ISID(c)) {
            int t = T_ATTR, len;
            j = i; while (j < n && ISID(s[j])) j++;
            len = j - i;
            if (in_list(ctl_py, s + i, len, 0)) t = T_CONTROL;
            else if (in_list(kw_py, s + i, len, 0)) t = T_KEYWORD;
            else if (i >= 4 && !strncmp(s + i - 4, "def ", 4)) t = T_FUNC;
            else if (i >= 6 && !strncmp(s + i - 6, "class ", 6)) t = T_TYPE;
            else if (next_nonspace(s, j, n) == '(') t = in_list(fn_py, s + i, len, 0) || !(s[i] >= 'A' && s[i] <= 'Z') ? T_FUNC : T_TYPE;
            else if (all_caps(s + i, len)) t = T_CONST;
            set(cls, i, j, t); i = j; continue;
        }
        cls[i++] = T_TEXT;
    }
    return 0;
}

static int lex_bat(const char *s, int n, unsigned char *cls)
{
    int i = 0, j;
    while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '@')) cls[i++] = T_TEXT;
    if ((i + 3 <= n && !_strnicmp(s + i, "rem", 3) && (i + 3 == n || s[i+3] == ' ' || s[i+3] == '\t')) ||
        (i + 1 < n && s[i] == ':' && s[i+1] == ':')) { set(cls, i, n, T_COMMENT); return 0; }
    if (i < n && s[i] == ':') { set(cls, i, n, T_FUNC); return 0; }
    while (i < n) {
        char c = s[i];
        if (c == '"') {
            j = i + 1; while (j < n && s[j] != '"') j++;
            if (j < n) j++;
            set(cls, i, j, T_STRING); i = j; continue;
        }
        if (c == '%') {
            j = i + 1;
            if (j < n && s[j] == '%') { j++; if (j < n && ISID(s[j])) j++; }
            else if (j < n && s[j] == '~') { while (j < n && ISID(s[j])) j++; while (j < n && s[j] != ' ' && !ISDIG(s[j-1])) j++; }
            else if (j < n && ISDIG(s[j])) j++;
            else { while (j < n && s[j] != '%' && s[j] != ' ') j++; if (j < n && s[j] == '%') j++; }
            set(cls, i, j, T_CONST); i = j; continue;
        }
        if (c == '!' ) {
            j = i + 1; while (j < n && ISID(s[j])) j++;
            if (j < n && s[j] == '!' && j > i + 1) { set(cls, i, j + 1, T_CONST); i = j + 1; continue; }
        }
        if (ISDIG(c) && (i == 0 || !ISID(s[i-1]))) {
            j = i; while (j < n && ISDIG(s[j])) j++;
            if (j == n || !ISID(s[j])) { set(cls, i, j, T_NUMBER); i = j; continue; }
        }
        if (ISID(c)) {
            j = i; while (j < n && (ISID(s[j]))) j++;
            set(cls, i, j, (i > 0 && ISID(s[i-1])) ? T_TEXT : in_list(" goto call exit if for ", s + i, j - i, 1) ? T_CONTROL :
                           in_list(kw_bat, s + i, j - i, 1) ? T_KEYWORD : T_TEXT);
            i = j; continue;
        }
        cls[i++] = (c == '>' || c == '<' || c == '|' || c == '&' || c == '(' || c == ')') ? T_KEYWORD : T_TEXT;
    }
    return 0;
}

static int lex_ini(const char *s, int n, unsigned char *cls)
{
    int i = 0, j;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) cls[i++] = T_TEXT;
    if (i < n && (s[i] == ';' || s[i] == '#')) { set(cls, i, n, T_COMMENT); return 0; }
    if (i < n && s[i] == '[') { set(cls, i, n, T_KEYWORD); return 0; }
    for (j = i; j < n && s[j] != '=' && s[j] != ':'; j++) ;
    if (j < n) { set(cls, i, j, T_ATTR); cls[j] = T_TEXT; set(cls, j + 1, n, T_STRING); }
    else set(cls, i, n, T_TEXT);
    return 0;
}

/* HTML/XML: st 1 = comment, 2 = inside a tag, 3 = inside <script>/<style> body (plain) */
static int lex_html(const char *s, int n, int st, unsigned char *cls)
{
    int i = 0, j;
    while (i < n) {
        if (st == 1) {
            for (j = i; j < n; j++) if (j + 2 < n && s[j] == '-' && s[j+1] == '-' && s[j+2] == '>') break;
            if (j >= n) { set(cls, i, n, T_COMMENT); return 1; }
            set(cls, i, j + 3, T_COMMENT); i = j + 3; st = 0; continue;
        }
        if (st == 2) {
            char c = s[i];
            if (c == '>' || (c == '/' && i + 1 < n && s[i+1] == '>') || (c == '?' && i + 1 < n && s[i+1] == '>')) {
                j = c == '>' ? i + 1 : i + 2; set(cls, i, j, T_PUNCT); i = j; st = 0; continue;
            }
            if (c == '"' || c == '\'') {
                j = i + 1; while (j < n && s[j] != c) j++;
                if (j < n) j++;
                set(cls, i, j, T_STRING); i = j; continue;
            }
            if (ISID(c) || c == '-' || c == ':') {
                j = i; while (j < n && (ISID(s[j]) || s[j] == '-' || s[j] == ':' || s[j] == '.')) j++;
                set(cls, i, j, T_ATTR); i = j; continue;
            }
            cls[i++] = T_TEXT; continue;
        }
        if (s[i] == '<') {
            if (i + 3 < n && s[i+1] == '!' && s[i+2] == '-' && s[i+3] == '-') { st = 1; set(cls, i, i + 4, T_COMMENT); i += 4; continue; }
            j = i + 1;
            if (j < n && (s[j] == '/' || s[j] == '!' || s[j] == '?')) j++;
            set(cls, i, j, T_PUNCT);
            i = j; while (j < n && (ISID(s[j]) || s[j] == '-' || s[j] == ':' || s[j] == '.')) j++;
            set(cls, i, j, T_TAG); i = j; st = 2; continue;
        }
        if (s[i] == '&') {
            j = i + 1; while (j < n && j < i + 10 && (ISID(s[j]) || s[j] == '#')) j++;
            if (j < n && s[j] == ';') { set(cls, i, j + 1, T_CONST); i = j + 1; continue; }
        }
        cls[i++] = T_TEXT;
    }
    return st;
}

/* Markdown: st 1 = inside ``` fence */
static int lex_md(const char *s, int n, int st, unsigned char *cls)
{
    int i = 0, j;
    while (i < n && s[i] == ' ') i++;
    if (i + 2 < n && ((s[i] == '`' && s[i+1] == '`' && s[i+2] == '`') || (s[i] == '~' && s[i+1] == '~' && s[i+2] == '~'))) {
        set(cls, 0, n, T_STRING); return !st;
    }
    if (st) { set(cls, 0, n, T_STRING); return 1; }
    set(cls, 0, n, T_TEXT);
    if (i < n && s[i] == '#') { set(cls, 0, n, T_HEADING); return 0; }
    if (i < n && s[i] == '>') { set(cls, 0, n, T_COMMENT); return 0; }
    if (i + 1 < n && (s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i+1] == ' ') { cls[i] = T_CONST; i += 2; }
    else { j = i; while (j < n && ISDIG(s[j])) j++; if (j > i && j < n && s[j] == '.') { set(cls, i, j + 1, T_CONST); i = j + 1; } }
    if (i == 0 && n >= 3) { for (j = 0; j < n && (s[j] == '-' || s[j] == '=' || s[j] == '*'); j++) ; if (j == n) { set(cls, 0, n, T_HEADING); return 0; } }
    while (i < n) {
        char c = s[i];
        if (c == '`') { j = i + 1; while (j < n && s[j] != '`') j++; if (j < n) j++; set(cls, i, j, T_STRING); i = j; continue; }
        if ((c == '*' || c == '_') && i + 1 < n && s[i+1] == c) {
            j = i + 2; while (j + 1 < n && !(s[j] == c && s[j+1] == c)) j++;
            if (j + 1 < n) { set(cls, i, j + 2, T_KEYWORD); i = j + 2; continue; }
        }
        if (c == '[' ) {
            j = i + 1; while (j < n && s[j] != ']') j++;
            if (j + 1 < n && s[j+1] == '(') {
                int k = j + 2; while (k < n && s[k] != ')') k++;
                set(cls, i, j + 1, T_ATTR); set(cls, j + 1, k < n ? k + 1 : n, T_STRING); i = k < n ? k + 1 : n; continue;
            }
        }
        if (c == '<' && i + 1 < n && (ISID(s[i+1]) || s[i+1] == '/')) {
            j = i + 1; while (j < n && s[j] != '>') j++;
            if (j < n) { set(cls, i, j + 1, T_TAG); i = j + 1; continue; }
        }
        i++;
    }
    return 0;
}

int lex_line(int lang, const char *s, int n, int st, unsigned char *cls)
{
    switch (lang) {
    case L_C:    return lex_clike(0, s, n, st, cls);
    case L_JS:   return lex_clike(1, s, n, st, cls);
    case L_PY:   return lex_py(s, n, st, cls);
    case L_BAT:  return lex_bat(s, n, cls);
    case L_INI:  return lex_ini(s, n, cls);
    case L_HTML: return lex_html(s, n, st, cls);
    case L_MD:   return lex_md(s, n, st, cls);
    }
    memset(cls, T_TEXT, n);
    return 0;
}
