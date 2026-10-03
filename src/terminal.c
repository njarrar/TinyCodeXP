/* terminal.c - the bottom panel: PROBLEMS, OUTPUT and the integrated TERMINAL.
 * Each terminal is a real cmd.exe connected through anonymous pipes, with a
 * hidden console of its own so Ctrl+C can be delivered, all inside a Job
 * Object so killing the terminal kills every process it started. */
#include "xpcode.h"
#include <stdarg.h>

/* tlhelp32.h is not part of TCC 0.9.27's headers */
#define TH32CS_SNAPPROCESS 0x2
typedef struct {
    DWORD dwSize, cntUsage, th32ProcessID; ULONG_PTR th32DefaultHeapID; DWORD th32ModuleID, cntThreads,
    th32ParentProcessID; LONG pcPriClassBase; DWORD dwFlags; CHAR szExeFile[MAX_PATH];
} XPROCESSENTRY32;
WINBASEAPI HANDLE WINAPI CreateToolhelp32Snapshot(DWORD, DWORD);
WINBASEAPI BOOL WINAPI Process32First(HANDLE, XPROCESSENTRY32 *);
WINBASEAPI BOOL WINAPI Process32Next(HANDLE, XPROCESSENTRY32 *);

#define MAXTERMS 8
#define SCROLLBACK 5000
#define SB_W 14
#define WM_TERMDATA (WM_APP + 1)
#define WM_TERMEXIT (WM_APP + 2)
#define ROW_H 22

typedef struct { char *s; int len, cap; } TLine;
typedef struct Chunk { struct Chunk *next; int serial, n; char data[1]; } Chunk;

typedef struct Term {
    HANDLE proc, inW, job; DWORD pid;
    TLine *ln; int n, cap;
    int col, esc;
    char in[1024]; int inlen, incur;
    char hist[50][256]; int nhist, histpos;
    int top, follow, alive, number, serial, atPrompt, isOutput, ready;
    char pend[1024];   /* command waiting for the first prompt */
    char cwd[MAX_PATH];
    char echo[1100]; int echoLen, echoPos;
    char **tabList; int tabN, tabI, tabStart; char tabSaved[1024];
    char pyFile[MAX_PATH]; int pyLine;
} Term;

static Term *s_terms[MAXTERMS];
static int s_nterms, s_active = -1, s_nextNum = 1, s_nextSerial = 1;
static Term s_out;
static int s_selA[2], s_selB[2], s_hasSel, s_selecting;
static VScroll s_vs;
static int s_hasCaret;
static char s_lineBuf[4096];
static CRITICAL_SECTION s_qlock;
static Chunk *s_qhead, *s_qtail;   /* output waiting for the UI thread; one post per batch */
static void submit2(Term *t, const char *suffix);
static void term_free(Term *t);

Problem *g_probs; int g_nprobs, g_nerr, g_nwarn;
static int s_pcap, s_probSel = -1, s_probTop, s_probIdx = -1;
static VScroll s_pvs;

/* ------------------------------------------------------------------ dynamic kernel32 (XP-safe) */

typedef HANDLE (WINAPI *PCreateJob)(LPSECURITY_ATTRIBUTES, LPCSTR);
typedef BOOL (WINAPI *PAssignJob)(HANDLE, HANDLE);
typedef BOOL (WINAPI *PSetJobInfo)(HANDLE, int, LPVOID, DWORD);
typedef BOOL (WINAPI *PTermJob)(HANDLE, UINT);
typedef BOOL (WINAPI *PAttachConsole)(DWORD);

typedef struct {
    LARGE_INTEGER PerProcessUserTimeLimit, PerJobUserTimeLimit;
    DWORD LimitFlags; SIZE_T MinimumWorkingSetSize, MaximumWorkingSetSize;
    DWORD ActiveProcessLimit; ULONG_PTR Affinity; DWORD PriorityClass, SchedulingClass;
} XJOBBASIC;
typedef struct {
    XJOBBASIC b;
    ULONGLONG io[6];
    SIZE_T ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
} XJOBEXT;

/* ------------------------------------------------------------------ problems */

static int is_abs(const char *p) { return (p[0] && p[1] == ':') || p[0] == '\\' || p[0] == '/'; }

static void resolve(char *out, const char *path, const char *cwd)
{
    char t[MAX_PATH * 2], *fp;
    if (is_abs(path) || !cwd[0]) lstrcpynA(t, path, MAX_PATH);
    else path_join(t, cwd, path);
    if (!GetFullPathNameA(t, MAX_PATH, out, &fp)) lstrcpynA(out, t, MAX_PATH);
}

static void prob_add(const char *file, int line, int col, int sev, const char *msg, int term, const char *cwd)
{
    Problem *p; int i; char full[MAX_PATH];
    resolve(full, file, cwd);
    for (i = 0; i < g_nprobs; i++)
        if (g_probs[i].line == line && !lstrcmpiA(g_probs[i].file, full) && !lstrcmpA(g_probs[i].msg, msg)) return;
    if (g_nprobs >= s_pcap) { s_pcap = s_pcap * 2 + 32; g_probs = (Problem *)realloc(g_probs, s_pcap * sizeof(Problem)); }
    p = &g_probs[g_nprobs++];
    lstrcpynA(p->file, full, MAX_PATH);
    p->line = line; p->col = col; p->sev = sev; p->term = term;
    lstrcpynA(p->msg, msg, sizeof(p->msg));
    if (sev == 0) g_nerr++; else if (sev == 1) g_nwarn++;
    if (g_prob) InvalidateRect(g_prob, 0, 0);
    status_dirty();
}

static void prob_clear_term(int serial)
{
    int i, j = 0;
    g_nerr = g_nwarn = 0;
    for (i = 0; i < g_nprobs; i++) {
        if (g_probs[i].term == serial) continue;
        g_probs[j] = g_probs[i];
        if (g_probs[j].sev == 0) g_nerr++; else if (g_probs[j].sev == 1) g_nwarn++;
        j++;
    }
    g_nprobs = j; s_probSel = -1; s_probIdx = -1;
    if (g_prob) InvalidateRect(g_prob, 0, 0);
    status_dirty();
}

static int digits_back(const char *s, int end, int *val)
{
    int i = end;
    while (i > 0 && s[i-1] >= '0' && s[i-1] <= '9') i--;
    if (i == end || end - i > 9) return -1;
    *val = atoi(s + i);
    return i;
}

/* Parses one terminal line. Returns 1 and fills file/line/col/sev/msg when it
 * holds a source location. sev: 0 error, 1 warning, 2 just a location. */
static int parse_line(const char *s, char *file, int *line, int *col, int *sev, const char **msg)
{
    static const char *keys[] = { ": fatal error", ": error", ": warning", ": note", 0 };
    int i, k = -1, kl = 0, which = -1, a, b;
    const char *p;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return 0;
    /* Python: File "x.py", line 12 */
    if (!strncmp(s, "File \"", 6)) {
        const char *q = strchr(s + 6, '"');
        if (q && !strncmp(q, "\", line ", 8)) {
            int n = (int)(q - s - 6);
            if (n >= MAX_PATH) return 0;
            memcpy(file, s + 6, n); file[n] = 0;
            *line = atoi(q + 8); *col = 1; *sev = 2; *msg = "";
            return *line > 0;
        }
    }
    /* MSVC: file(12) : error C2065: ... or file(12,5): warning ... */
    p = strchr(s, '(');
    while (p) {
        const char *q = p + 1;
        int ln = 0, cl = 1;
        while (*q >= '0' && *q <= '9') ln = ln * 10 + (*q++ - '0');
        if (ln && *q == ',') { cl = 0; q++; while (*q >= '0' && *q <= '9') cl = cl * 10 + (*q++ - '0'); }
        if (ln && *q == ')') {
            q++; while (*q == ' ') q++;
            if (*q == ':') {
                q++; while (*q == ' ') q++;
                *sev = !_strnicmp(q, "warning", 7) ? 1 : (!_strnicmp(q, "error", 5) || !_strnicmp(q, "fatal error", 11)) ? 0 : 2;
                if (*sev < 2 && p - s < MAX_PATH && p > s) {
                    memcpy(file, s, p - s); file[p - s] = 0;
                    *line = ln; *col = cl ? cl : 1; *msg = q;
                    return 1;
                }
            }
        }
        p = strchr(p + 1, '(');
    }
    /* GCC / TCC: file:12:5: error: ... or file:12: warning: ... */
    for (i = 0; keys[i]; i++) {
        const char *f = strstr(s, keys[i]);
        if (f && (k < 0 || f - s < k)) { k = (int)(f - s); kl = lstrlenA(keys[i]); which = i; }
    }
    if (k > 0) {
        int ln = 0, cl = 0;
        a = digits_back(s, k, &ln);
        if (a > 1 && s[a-1] == ':') {
            b = digits_back(s, a - 1, &cl);
            if (b > 1 && s[b-1] == ':') { int t = cl; cl = ln; ln = t; a = b; }
            else cl = 1;
            if (a - 1 > 0 && a - 1 < MAX_PATH) {
                memcpy(file, s, a - 1); file[a - 1] = 0;
                *line = ln; *col = cl ? cl : 1;
                *sev = which <= 1 ? 0 : which == 2 ? 1 : 2;
                *msg = s + k + kl;
                while (**msg == ':' || **msg == ' ') (*msg)++;
                return 1;
            }
        }
    }
    /* plain file:line: (for Ctrl+click only) */
    for (p = s; *p; p++) {
        if (*p == ':' && p > s + 1 && p[1] >= '0' && p[1] <= '9') {
            int ln = atoi(p + 1), n = (int)(p - s);
            const char *e = p + 1; while (*e >= '0' && *e <= '9') e++;
            if ((*e == ':' || *e == 0 || *e == ' ') && n < MAX_PATH && ln > 0) {
                memcpy(file, s, n); file[n] = 0;
                if (strchr(file, ' ')) return 0;
                *line = ln; *col = 1; *sev = 2; *msg = "";
                if (*e == ':' && e[1] >= '0' && e[1] <= '9') *col = atoi(e + 1);
                return 1;
            }
        }
    }
    return 0;
}

static int py_exception(const char *s)
{
    const char *p = s;
    if (*s == ' ' || *s == '\t' || !*s) return 0;
    while ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '.') p++;
    if (p == s || (*p != ':' && *p != 0)) return 0;
    return (p - s >= 5 && (!strncmp(p - 5, "Error", 5) || !strncmp(p - 4, "Exit", 4))) ||
           (p - s >= 9 && !strncmp(p - 9, "Exception", 9)) || (p - s >= 9 && !strncmp(p - 9, "Interrupt", 9)) ||
           (p - s >= 7 && !strncmp(p - 7, "Warning", 7));
}

static void scan_line(Term *t, const char *s)
{
    char file[MAX_PATH]; int line, col, sev; const char *msg;
    if (t->isOutput) return;
    if (parse_line(s, file, &line, &col, &sev, &msg)) {
        if (sev < 2) { prob_add(file, line, col, sev, msg, t->serial, t->cwd); t->pyFile[0] = 0; }
        else if (!strncmp(s + strspn(s, " \t"), "File \"", 6)) { lstrcpynA(t->pyFile, file, MAX_PATH); t->pyLine = line; }
        return;
    }
    if (t->pyFile[0] && py_exception(s)) {
        prob_add(t->pyFile, t->pyLine, 1, strstr(s, "Warning") ? 1 : 0, s, t->serial, t->cwd);
        t->pyFile[0] = 0;
    }
}

/* ------------------------------------------------------------------ buffer */

static Term *cur_term(void)
{
    if (g_panelMode == 1) return &s_out;
    return (s_active >= 0 && s_active < s_nterms) ? s_terms[s_active] : 0;
}

static void tl_reserve(TLine *L, int n) { if (n + 1 > L->cap) { L->cap = n + 64 + L->cap / 2; L->s = (char *)realloc(L->s, L->cap); } }

static void new_line(Term *t)
{
    if (t->n >= t->cap) { t->cap = t->cap * 2 + 256; t->ln = (TLine *)realloc(t->ln, t->cap * sizeof(TLine)); }
    memset(&t->ln[t->n], 0, sizeof(TLine));
    tl_reserve(&t->ln[t->n], 16);
    t->n++;
    t->col = 0;
    if (t->n > SCROLLBACK) {
        int k, drop = 500;
        for (k = 0; k < drop; k++) free(t->ln[k].s);
        memmove(t->ln, t->ln + drop, (t->n - drop) * sizeof(TLine));
        t->n -= drop;
        if (t == cur_term()) s_hasSel = 0;
    }
}

static void finish_line(Term *t)
{
    TLine *L = &t->ln[t->n - 1];
    L->s[L->len] = 0;
    scan_line(t, L->s);
    new_line(t);
}

static void put_char(Term *t, char c)
{
    TLine *L = &t->ln[t->n - 1];
    if (t->col < L->len) L->s[t->col] = c;
    else { tl_reserve(L, L->len + 1); L->s[L->len++] = c; }
    t->col++;
    L->s[L->len] = 0;
}

static void feed(Term *t, const char *d, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)d[i];
        if (t->echoLen) {
            if (c == (unsigned char)t->echo[t->echoPos]) { if (++t->echoPos >= t->echoLen) t->echoLen = 0; continue; }
            t->echoLen = 0;
        }
        if (t->esc == 1) { t->esc = c == '[' ? 2 : 0; continue; }
        if (t->esc == 2) { if (c >= '@' && c <= '~') t->esc = 0; continue; }
        switch (c) {
        case 27: t->esc = 1; break;
        case '\r': t->col = 0; break;
        case '\n': finish_line(t); break;
        case '\b': if (t->col > 0) t->col--; break;
        case '\t': do put_char(t, ' '); while (t->col % 8); break;
        case 7: case 0: break;
        default: if (c >= 32 || c >= 128) put_char(t, (char)c); break;
        }
    }
    /* sitting at a cmd prompt?  "C:\some\dir>" */
    {
        TLine *L = &t->ln[t->n - 1];
        t->atPrompt = 0;
        if (L->len >= 3 && L->len < MAX_PATH && L->s[1] == ':' && L->s[2] == '\\' && L->s[L->len - 1] == '>' && t->col == L->len) {
            memcpy(t->cwd, L->s, L->len - 1); t->cwd[L->len - 1] = 0;
            t->atPrompt = t->ready = 1;
        }
    }
    if (t->atPrompt && t->pend[0]) {
        lstrcpyA(t->in, t->pend); t->pend[0] = 0;
        t->inlen = t->incur = lstrlenA(t->in);
        submit2(t, " & echo on");
    }
}

static int cols_avail(void)
{
    RECT r; int c;
    GetClientRect(g_term, &r);
    c = (r.right - SB_W - 4) / (g_cw ? g_cw : 8);
    return c < 10 ? 10 : c;
}

static const char *line_text(Term *t, int l, int *len)
{
    TLine *L = &t->ln[l];
    if (l == t->n - 1 && t->alive && !t->isOutput && t->inlen) {
        int n = L->len < 3000 ? L->len : 3000;
        memcpy(s_lineBuf, L->s, n);
        memcpy(s_lineBuf + n, t->in, t->inlen);
        *len = n + t->inlen;
        s_lineBuf[*len] = 0;
        return s_lineBuf;
    }
    *len = L->len;
    return L->s;
}

static int rows_of(int len, int cols) { return len <= 0 ? 1 : (len + cols - 1) / cols; }

static int total_rows(Term *t, int cols)
{
    int i, r = 0, len;
    for (i = 0; i < t->n; i++) { line_text(t, i, &len); r += rows_of(len, cols); }
    return r;
}

static int vis_rows(void) { RECT r; GetClientRect(g_term, &r); return r.bottom / (g_lh ? g_lh : 16); }

static void follow_bottom(Term *t)
{
    int tr = total_rows(t, cols_avail()), vr = vis_rows();
    t->top = tr - vr > 0 ? tr - vr : 0;
}

/* map a visual row to line/segment */
static int row_to_line(Term *t, int row, int cols, int *seg)
{
    int i, r = 0, len;
    for (i = 0; i < t->n; i++) {
        int k;
        line_text(t, i, &len);
        k = rows_of(len, cols);
        if (row < r + k) { *seg = row - r; return i; }
        r += k;
    }
    *seg = 0;
    return -1;
}

static int line_to_row(Term *t, int line, int cols)
{
    int i, r = 0, len;
    for (i = 0; i < line && i < t->n; i++) { line_text(t, i, &len); r += rows_of(len, cols); }
    return r;
}

/* ------------------------------------------------------------------ output log */

void out_log(const char *fmt, ...)
{
    char buf[1200]; va_list ap; SYSTEMTIME st; int n;
    if (!s_out.n) { new_line(&s_out); s_out.isOutput = 1; s_out.follow = 1; }
    GetLocalTime(&st);
    n = wsprintfA(buf, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    _vsnprintf(buf + n, sizeof(buf) - n - 3, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 3] = 0;
    lstrcatA(buf, "\n");
    feed(&s_out, buf, lstrlenA(buf));
    if (s_out.follow && g_term) follow_bottom(&s_out);
    if (g_term && g_panelMode == 1) InvalidateRect(g_term, 0, 0);
}

/* ------------------------------------------------------------------ processes */

static DWORD WINAPI reader(LPVOID arg)
{
    HANDLE h = ((HANDLE *)arg)[0];
    int serial = (int)(INT_PTR)((HANDLE *)arg)[1];
    char buf[4096]; DWORD rd;
    free(arg);
    while (ReadFile(h, buf, sizeof(buf), &rd, 0) && rd) {
        Chunk *c = (Chunk *)malloc(sizeof(Chunk) + rd);
        int wasEmpty;
        c->serial = serial; c->n = (int)rd; c->next = 0;
        memcpy(c->data, buf, rd);
        EnterCriticalSection(&s_qlock);
        wasEmpty = !s_qhead;
        if (s_qtail) s_qtail->next = c; else s_qhead = c;
        s_qtail = c;
        LeaveCriticalSection(&s_qlock);
        if (wasEmpty) PostMessageA(g_term, WM_TERMDATA, 0, 0);
    }
    CloseHandle(h);
    PostMessageA(g_term, WM_TERMEXIT, (WPARAM)serial, 0);
    return 0;
}

static void kill_tree(DWORD pid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    XPROCESSENTRY32 pe;
    HANDLE h;
    if (snap != INVALID_HANDLE_VALUE) {
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe)) do {
            if (pe.th32ParentProcessID == pid && pe.th32ProcessID != pid) kill_tree(pe.th32ProcessID);
        } while (Process32Next(snap, &pe));
        CloseHandle(snap);
    }
    h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h) { TerminateProcess(h, 1); CloseHandle(h); }
}

int term_new(void)
{
    SECURITY_ATTRIBUTES sa; STARTUPINFOA si; PROCESS_INFORMATION pi;
    HANDLE inR, inW, outR, outW, *arg;
    char cmd[MAX_PATH + 16], comspec[MAX_PATH];
    const char *dir;
    Term *t;
    PCreateJob cj = (PCreateJob)dyn("kernel32.dll", "CreateJobObjectA");
    PAssignJob aj = (PAssignJob)dyn("kernel32.dll", "AssignProcessToJobObject");
    PSetJobInfo sj = (PSetJobInfo)dyn("kernel32.dll", "SetInformationJobObject");
    if (s_nterms >= MAXTERMS) { out_log("At most %d terminals can be open.", MAXTERMS); return -1; }
    sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = 0; sa.bInheritHandle = TRUE;
    if (!CreatePipe(&inR, &inW, &sa, 65536)) return -1;
    if (!CreatePipe(&outR, &outW, &sa, 65536)) { CloseHandle(inR); CloseHandle(inW); return -1; }
    SetHandleInformation(inW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = inR; si.hStdOutput = outW; si.hStdError = outW;
    if (!GetEnvironmentVariableA("COMSPEC", comspec, MAX_PATH)) lstrcpyA(comspec, "cmd.exe");
    wsprintfA(cmd, "\"%s\"", comspec);
    dir = g_folder[0] ? g_folder : 0;
    if (!CreateProcessA(0, cmd, 0, 0, TRUE, CREATE_NEW_CONSOLE | CREATE_SUSPENDED, 0, dir, &si, &pi)) {
        out_log("Could not start %s (error %lu)", comspec, GetLastError());
        CloseHandle(inR); CloseHandle(inW); CloseHandle(outR); CloseHandle(outW);
        return -1;
    }
    CloseHandle(inR); CloseHandle(outW);
    t = (Term *)xalloc(sizeof(Term));
    t->proc = pi.hProcess; t->pid = pi.dwProcessId; t->inW = inW;
    t->alive = 1; t->follow = 1; t->number = s_nextNum++; t->serial = s_nextSerial++;
    lstrcpyA(t->cwd, g_folder[0] ? g_folder : g_exeDir);
    new_line(t);
    if (cj && aj) {
        t->job = cj(0, 0);
        if (t->job) {
            XJOBEXT x; memset(&x, 0, sizeof(x));
            x.b.LimitFlags = 0x2000;   /* JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE */
            if (sj) sj(t->job, 9, &x, sizeof(x));
            if (!aj(t->job, pi.hProcess)) { CloseHandle(t->job); t->job = 0; }
        }
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    arg = (HANDLE *)malloc(2 * sizeof(HANDLE));
    arg[0] = outR; arg[1] = (HANDLE)(INT_PTR)t->serial;
    {
        HANDLE th = CreateThread(0, 0, reader, arg, 0, 0);
        if (!th) { out_log("Could not start the terminal reader (error %lu)", GetLastError()); free(arg); CloseHandle(outR); term_free(t); return -1; }
        CloseHandle(th);
    }
    s_terms[s_nterms++] = t;
    s_active = s_nterms - 1;
    s_hasSel = 0;
    if (g_term) InvalidateRect(g_term, 0, 0);
    status_dirty();
    return s_active;
}

static void term_free(Term *t)
{
    int i;
    if (t->job) { PTermJob tj = (PTermJob)dyn("kernel32.dll", "TerminateJobObject"); if (tj) tj(t->job, 1); }
    if (t->alive) kill_tree(t->pid);
    if (t->job) CloseHandle(t->job);
    if (t->proc) CloseHandle(t->proc);
    if (t->inW) CloseHandle(t->inW);
    for (i = 0; i < t->n; i++) free(t->ln[i].s);
    free(t->ln);
    for (i = 0; i < t->tabN; i++) free(t->tabList[i]);
    free(t->tabList);
    free(t);
}

void term_kill(int i)
{
    Term *t;
    if (i < 0 || i >= s_nterms) return;
    t = s_terms[i];
    prob_clear_term(t->serial);
    memmove(s_terms + i, s_terms + i + 1, (s_nterms - i - 1) * sizeof(Term *));
    s_nterms--;
    term_free(t);
    if (s_active >= s_nterms) s_active = s_nterms - 1;
    s_hasSel = 0;
    if (g_term) InvalidateRect(g_term, 0, 0);
    status_dirty();
    if (g_main) InvalidateRect(g_main, 0, 0);
}

void term_kill_all(void) { while (s_nterms) term_kill(s_nterms - 1); }
int term_count(void) { return s_nterms; }
int term_active(void) { return s_active; }

const char *term_label(int i)
{
    static char b[32];
    if (i < 0 || i >= s_nterms) return "";
    wsprintfA(b, "%d: cmd", s_terms[i]->number);
    return b;
}

void term_select(int i)
{
    if (i < 0 || i >= s_nterms) return;
    s_active = i; s_hasSel = 0;
    InvalidateRect(g_term, 0, 0); status_dirty();
    SetFocus(g_term);
}

void term_switch(int delta)
{
    if (!s_nterms) return;
    set_panel(2);
    term_select((s_active + delta + s_nterms) % s_nterms);
}

static void write_in(Term *t, const char *s, int n)
{
    DWORD w; char *o;
    if (!t->alive || !t->inW) return;
    o = (char *)malloc(n + 1);
    CharToOemBuffA(s, o, n);
    WriteFile(t->inW, o, n, &w, 0);
    free(o);
}

/* suffix is sent to cmd but not shown (used to restore ECHO after a batch file) */
static void submit2(Term *t, const char *suffix)
{
    TLine *L;
    char line[1200];
    int n = t->inlen, k = suffix ? lstrlenA(suffix) : 0;
    memcpy(line, t->in, n); line[n] = 0;
    if (n && (!t->nhist || lstrcmpA(t->hist[(t->nhist - 1) % 50], line))) {
        lstrcpynA(t->hist[t->nhist % 50], line, 256);
        t->nhist++;
    }
    t->histpos = t->nhist;
    if (t->atPrompt) prob_clear_term(t->serial);
    /* show what was typed now; cmd's own echo of it is swallowed in feed() */
    L = &t->ln[t->n - 1];
    tl_reserve(L, L->len + n);
    memcpy(L->s + L->len, line, n); L->len += n; L->s[L->len] = 0;
    t->col = L->len;
    new_line(t);
    memcpy(line + n, suffix, k); n += k;
    if (t->atPrompt) { memcpy(t->echo, line, n); t->echo[n] = '\r'; t->echo[n+1] = '\n'; t->echoLen = n + 2; t->echoPos = 0; }
    t->atPrompt = 0;
    t->inlen = t->incur = 0;
    line[n] = '\r'; line[n+1] = '\n';
    write_in(t, line, n + 2);
    t->follow = 1;
}

static void submit(Term *t) { submit2(t, 0); }

void term_send(const char *cmd, int show)
{
    Term *t;
    (void)show;
    if (s_active < 0 || s_active >= s_nterms) { if (term_new() < 0) return; }
    t = s_terms[s_active];
    if (!t->alive) { if (term_new() < 0) return; t = s_terms[s_active]; }
    /* a program is still running here: don't type into its stdin, use a new terminal */
    if (t->ready && !t->atPrompt) { if (term_new() < 0) return; t = s_terms[s_active]; }
    if (!t->atPrompt) lstrcpynA(t->pend, cmd, sizeof(t->pend));   /* sent once cmd shows its prompt */
    else {
        lstrcpynA(t->in, cmd, sizeof(t->in));
        t->inlen = t->incur = lstrlenA(t->in);
        submit2(t, " & echo on");
    }
    follow_bottom(t);
    InvalidateRect(g_term, 0, 0);
}

void term_interrupt(void)
{
    Term *t = (s_active >= 0 && s_active < s_nterms) ? s_terms[s_active] : 0;
    PAttachConsole ac = (PAttachConsole)dyn("kernel32.dll", "AttachConsole");
    TLine *L;
    if (!t || !t->alive) return;
    FreeConsole();
    if (ac && ac(t->pid)) {
        GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
        FreeConsole();
    } else kill_tree(t->pid), out_log("Could not send Ctrl+C; the terminal was stopped.");
    L = &t->ln[t->n - 1];
    tl_reserve(L, L->len + t->inlen + 2);
    memcpy(L->s + L->len, t->in, t->inlen); L->len += t->inlen;
    L->s[L->len++] = '^'; L->s[L->len++] = 'C'; L->s[L->len] = 0;
    t->col = L->len;
    t->inlen = t->incur = 0;
    t->echoLen = 0;
    if (t->atPrompt) { new_line(t); write_in(t, "\r\n", 2); t->atPrompt = 0; }
    InvalidateRect(g_term, 0, 0);
}

void term_clear(void)
{
    Term *t = cur_term();
    int i;
    TLine keep;
    if (!t || !t->n) return;
    keep = t->ln[t->n - 1];
    for (i = 0; i < t->n - 1; i++) free(t->ln[i].s);
    t->ln[0] = keep; t->n = 1; t->top = 0; s_hasSel = 0;
    InvalidateRect(g_term, 0, 0);
}

void term_font_changed(void) { if (g_term) { Term *t = cur_term(); if (t && t->follow) follow_bottom(t); InvalidateRect(g_term, 0, 0); } }

/* ------------------------------------------------------------------ tab completion */

static int cmp_str(const void *a, const void *b) { return lstrcmpiA(*(char **)a, *(char **)b); }

static void complete(Term *t, int back)
{
    int i;
    if (!t->tabN) {
        char pat[MAX_PATH * 2], dirpart[MAX_PATH], tok[MAX_PATH];
        WIN32_FIND_DATAA fd; HANDLE h; int s = t->incur, q = 0, cap = 0;
        /* token start: after the last space not inside quotes */
        s = 0;
        for (i = 0; i < t->incur; i++) { if (t->in[i] == '"') q = !q; else if (t->in[i] == ' ' && !q) s = i + 1; }
        t->tabStart = s;
        i = 0;
        for (q = s; q < t->incur && i < MAX_PATH - 1; q++) if (t->in[q] != '"') tok[i++] = t->in[q];
        tok[i] = 0;
        lstrcpyA(dirpart, tok);
        { char *a = strrchr(dirpart, '\\'), *b = strrchr(dirpart, '/'); if (b > a) a = b; if (a) a[1] = 0; else dirpart[0] = 0; }
        if (is_abs(tok)) wsprintfA(pat, "%s*", tok); else wsprintfA(pat, "%s\\%s*", t->cwd, tok);
        h = FindFirstFileA(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            char full[MAX_PATH * 2]; int sp;
            if (!lstrcmpA(fd.cFileName, ".") || !lstrcmpA(fd.cFileName, "..")) continue;
            wsprintfA(full, "%s%s", dirpart, fd.cFileName);
            sp = strchr(full, ' ') != 0;
            if (t->tabN >= cap) { cap = cap * 2 + 16; t->tabList = (char **)realloc(t->tabList, cap * sizeof(char *)); }
            if (sp) { char qd[MAX_PATH * 2 + 3]; wsprintfA(qd, "\"%s\"", full); t->tabList[t->tabN++] = xstrdup(qd); }
            else t->tabList[t->tabN++] = xstrdup(full);
        } while (FindNextFileA(h, &fd) && t->tabN < 2000);
        FindClose(h);
        if (!t->tabN) return;
        qsort(t->tabList, t->tabN, sizeof(char *), cmp_str);
        memcpy(t->tabSaved, t->in + t->incur, t->inlen - t->incur);
        t->tabSaved[t->inlen - t->incur] = 0;
        t->tabI = back ? t->tabN - 1 : 0;
    } else t->tabI = (t->tabI + (back ? -1 : 1) + t->tabN) % t->tabN;
    {
        char nb[1024]; int n = t->tabStart, k = lstrlenA(t->tabList[t->tabI]), r = lstrlenA(t->tabSaved);
        if (n + k + r >= (int)sizeof(nb) - 1) return;
        memcpy(nb, t->in, n);
        memcpy(nb + n, t->tabList[t->tabI], k);
        memcpy(nb + n + k, t->tabSaved, r);
        t->inlen = n + k + r; t->incur = n + k;
        memcpy(t->in, nb, t->inlen);
    }
}

static void tab_reset(Term *t)
{
    int i;
    for (i = 0; i < t->tabN; i++) free(t->tabList[i]);
    t->tabN = 0;
}

/* ------------------------------------------------------------------ selection */

static void sel_order(int *a, int *b)
{
    if (s_selA[0] < s_selB[0] || (s_selA[0] == s_selB[0] && s_selA[1] <= s_selB[1])) { a[0] = s_selA[0]; a[1] = s_selA[1]; b[0] = s_selB[0]; b[1] = s_selB[1]; }
    else { a[0] = s_selB[0]; a[1] = s_selB[1]; b[0] = s_selA[0]; b[1] = s_selA[1]; }
}

static void copy_sel(Term *t)
{
    int a[2], b[2], l, n = 0, len;
    char *buf;
    if (!s_hasSel) return;
    sel_order(a, b);
    if (a[0] >= t->n) return;
    if (b[0] >= t->n) { b[0] = t->n - 1; b[1] = 1 << 30; }
    for (l = a[0]; l <= b[0]; l++) { line_text(t, l, &len); n += len + 2; }
    buf = (char *)malloc(n + 1); n = 0;
    for (l = a[0]; l <= b[0]; l++) {
        const char *s = line_text(t, l, &len);
        int x = l == a[0] ? a[1] : 0, y = l == b[0] ? b[1] : len;
        if (x > len) x = len;
        if (y > len) y = len;
        if (y > x) { memcpy(buf + n, s + x, y - x); n += y - x; }
        if (l < b[0]) { while (n > 0 && buf[n-1] == ' ') n--; buf[n++] = '\r'; buf[n++] = '\n'; }
    }
    clip_set(buf, n);
    free(buf);
}

static void paste(Term *t)
{
    char *c = clip_get(), *p;
    if (!c || !t->alive) { free(c); return; }
    tab_reset(t);
    for (p = c; *p; p++) {
        if (*p == '\r') continue;
        if (*p == '\n') { submit(t); continue; }
        if (*p == '\t') *p = ' ';
        if (t->inlen < (int)sizeof(t->in) - 3) {
            memmove(t->in + t->incur + 1, t->in + t->incur, t->inlen - t->incur);
            t->in[t->incur++] = *p; t->inlen++;
        }
    }
    free(c);
    follow_bottom(t);
}

/* ------------------------------------------------------------------ terminal view */

static void caret_rc(Term *t, int cols, int *row, int *col)
{
    TLine *L = &t->ln[t->n - 1];
    int pos = L->len + t->incur, r0 = line_to_row(t, t->n - 1, cols);
    if (t->col < L->len && !t->inlen) pos = t->col;
    *row = r0 + pos / cols - t->top;
    *col = pos % cols;
}

static void caret_sync(void)
{
    Term *t = cur_term();
    int r, c;
    if (!s_hasCaret) return;
    if (!t || t->isOutput || !t->alive) { SetCaretPos(-100, -100); return; }
    caret_rc(t, cols_avail(), &r, &c);
    SetCaretPos(r >= 0 && r < vis_rows() + 1 ? 4 + c * g_cw : -100, r * g_lh);
}

static void term_paint(HWND w)
{
    PAINTSTRUCT ps; HDC wdc = BeginPaint(w, &ps), dc;
    RECT r; HGDIOBJ of;
    Term *t = cur_term();
    int cols, rows, i, a[2], b[2], tr;
    GetClientRect(w, &r);
    dc = bb_begin(w, wdc, r.right, r.bottom);
    fill(dc, 0, 0, r.right, r.bottom, C_BG);
    of = SelectObject(dc, g_mono);
    SetBkMode(dc, TRANSPARENT);
    if (!t) {
        SelectObject(dc, g_ui);
        ui_text(dc, 8, 8, "No terminal is open. Press Ctrl+Shift+` or click + to create one.", C_DIM);
    } else {
        cols = cols_avail(); rows = vis_rows() + 1;
        tr = total_rows(t, cols);
        if (t->top > tr - 1) t->top = tr - 1;
        if (t->top < 0) t->top = 0;
        sel_order(a, b);
        for (i = 0; i < rows; i++) {
            int seg, l = row_to_line(t, t->top + i, cols, &seg), len, x0, x1;
            const char *s;
            if (l < 0) break;
            s = line_text(t, l, &len);
            x0 = seg * cols; x1 = x0 + cols; if (x1 > len) x1 = len;
            if (s_hasSel && l >= a[0] && l <= b[0]) {
                int sa = l == a[0] ? a[1] : 0, sb = l == b[0] ? b[1] : len + 1;
                if (sa < x0) sa = x0;
                if (sb > x0 + cols) sb = x0 + cols;
                if (sb > sa) fill(dc, 4 + (sa - x0) * g_cw, i * g_lh, (sb - sa) * g_cw, g_lh, GetFocus() == w ? C_SEL : C_SELOFF);
            }
            if (x1 > x0) {
                SetTextColor(dc, t->isOutput ? XRGB(0xBB,0xBB,0xBB) : C_TEXT);
                TextOutA(dc, 4, i * g_lh + 2, s + x0, x1 - x0);
            }
        }
        if (!t->isOutput && t->alive && GetFocus() != w) {
            /* hollow caret when not focused, like VS Code */
            int cr, cc; caret_rc(t, cols, &cr, &cc);
            if (cr >= 0 && cr < rows) {
                HPEN p = CreatePen(PS_SOLID, 1, XRGB(0xAE,0xAF,0xAD)), op = (HPEN)SelectObject(dc, p);
                HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, 4 + cc * g_cw, cr * g_lh + 1, 4 + (cc + 1) * g_cw, (cr + 1) * g_lh);
                SelectObject(dc, obr); SelectObject(dc, op); DeleteObject(p);
            }
        }
        s_vs.r.left = r.right - SB_W; s_vs.r.top = 0; s_vs.r.right = r.right; s_vs.r.bottom = r.bottom;
        s_vs.page = vis_rows(); s_vs.max = tr; s_vs.pos = t->top;
        vs_draw(dc, &s_vs);
    }
    SelectObject(dc, of);
    BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    
    EndPaint(w, &ps);
    caret_sync();
}

static void hit(Term *t, int x, int y, int *l, int *c)
{
    int cols = cols_avail(), seg, row = y / g_lh + t->top, len;
    if (y < 0) row = t->top - 1;
    if (row < 0) row = 0;
    *l = row_to_line(t, row, cols, &seg);
    if (*l < 0) { *l = t->n - 1; line_text(t, *l, &len); *c = len; return; }
    *c = seg * cols + (x - 4 + g_cw / 2) / g_cw;
    if (*c < seg * cols) *c = seg * cols;
    line_text(t, *l, &len);
    if (*c > len) *c = len;
}

static void open_location(Term *t, int l)
{
    char file[MAX_PATH], full[MAX_PATH]; int line, col, sev, len; const char *msg;
    const char *s = line_text(t, l, &len);
    if (!parse_line(s, file, &line, &col, &sev, &msg)) return;
    resolve(full, file, t->cwd);
    if (file_exists(full)) open_file(full, line, col);
}

static void scroll_by(Term *t, int d)
{
    int tr = total_rows(t, cols_avail()), vr = vis_rows();
    t->top += d;
    if (t->top > tr - vr) t->top = tr - vr;
    if (t->top < 0) t->top = 0;
    t->follow = t->top >= tr - vr;
    InvalidateRect(g_term, 0, 0);
}

static void term_key(HWND w, int vk)
{
    Term *t = cur_term();
    int mods = key_mods(), ctrl = mods & M_CTRL, shift = mods & M_SHIFT;
    if (!t) return;
    if (ctrl && vk == 'C') { if (s_hasSel) { copy_sel(t); s_hasSel = 0; InvalidateRect(w, 0, 0); } else if (!t->isOutput) term_interrupt(); return; }
    if ((ctrl && vk == 'V') || (shift && vk == VK_INSERT)) { if (!t->isOutput) paste(t); InvalidateRect(w, 0, 0); return; }
    if (ctrl && vk == VK_INSERT) { copy_sel(t); return; }
    if (ctrl && vk == 'A') { s_selA[0] = 0; s_selA[1] = 0; s_selB[0] = t->n - 1; s_selB[1] = 1 << 20; s_hasSel = 1; InvalidateRect(w, 0, 0); return; }
    if (vk == VK_PRIOR) { scroll_by(t, -vis_rows()); return; }
    if (vk == VK_NEXT) { scroll_by(t, vis_rows()); return; }
    if (ctrl && vk == VK_UP) { scroll_by(t, -1); return; }
    if (ctrl && vk == VK_DOWN) { scroll_by(t, 1); return; }
    if (ctrl && vk == VK_HOME) { scroll_by(t, -(1 << 20)); return; }
    if (ctrl && vk == VK_END) { scroll_by(t, 1 << 20); return; }
    if (vk == VK_ESCAPE && s_hasSel) { s_hasSel = 0; InvalidateRect(w, 0, 0); return; }
    if (t->isOutput || !t->alive) return;
    if (vk != VK_TAB) tab_reset(t);
    switch (vk) {
    case VK_RETURN: submit(t); break;
    case VK_BACK:
        if (t->incur > 0) {
            int k = 1;
            if (ctrl) { k = 0; while (t->incur - k > 0 && t->in[t->incur - k - 1] == ' ') k++; while (t->incur - k > 0 && t->in[t->incur - k - 1] != ' ') k++; }
            memmove(t->in + t->incur - k, t->in + t->incur, t->inlen - t->incur);
            t->incur -= k; t->inlen -= k;
        }
        break;
    case VK_DELETE: if (t->incur < t->inlen) { memmove(t->in + t->incur, t->in + t->incur + 1, t->inlen - t->incur - 1); t->inlen--; } break;
    case VK_LEFT: if (t->incur > 0) t->incur--; break;
    case VK_RIGHT: if (t->incur < t->inlen) t->incur++; break;
    case VK_HOME: t->incur = 0; break;
    case VK_END: t->incur = t->inlen; break;
    case VK_ESCAPE: t->inlen = t->incur = 0; break;
    case VK_UP: case VK_DOWN: {
        int lo = t->nhist > 50 ? t->nhist - 50 : 0;
        if (!t->nhist) return;
        if (vk == VK_UP) { if (t->histpos > lo) t->histpos--; }
        else { if (t->histpos < t->nhist) t->histpos++; }
        if (t->histpos == t->nhist) t->inlen = 0;
        else { lstrcpynA(t->in, t->hist[t->histpos % 50], sizeof(t->in)); t->inlen = lstrlenA(t->in); }
        t->incur = t->inlen;
        break;
    }
    case VK_TAB: complete(t, shift); break;
    default: return;
    }
    s_hasSel = 0;
    t->follow = 1; follow_bottom(t);
    InvalidateRect(w, 0, 0);
}

static LRESULT CALLBACK TermProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    Term *t = cur_term();
    switch (m) {
    case WM_PAINT: term_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: if (t && t->follow) follow_bottom(t); InvalidateRect(w, 0, 0); return 0;
    case WM_SETFOCUS: CreateCaret(w, 0, 2, g_lh); s_hasCaret = 1; ShowCaret(w); InvalidateRect(w, 0, 0); return 0;
    case WM_KILLFOCUS: s_hasCaret = 0; DestroyCaret(); InvalidateRect(w, 0, 0); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_KEYDOWN: term_key(w, (int)wp); return 0;
    case WM_CHAR:
        if (!t || t->isOutput || !t->alive || wp < 32 || wp == 127) return 0;
        if ((key_mods() & M_CTRL) && !(key_mods() & M_ALT)) return 0;
        tab_reset(t);
        if (t->inlen < (int)sizeof(t->in) - 3) {
            memmove(t->in + t->incur + 1, t->in + t->incur, t->inlen - t->incur);
            t->in[t->incur++] = (char)wp; t->inlen++;
        }
        s_hasSel = 0;
        t->follow = 1; follow_bottom(t);
        InvalidateRect(w, 0, 0);
        return 0;
    case WM_COMMAND:
        if (!t) return 0;
        if (wp == CM_COPY) copy_sel(t);
        else if (wp == CM_PASTE && !t->isOutput) { paste(t); InvalidateRect(w, 0, 0); }
        return 0;
    case WM_MOUSEWHEEL:
        if (LOWORD(wp) & MK_CONTROL) { run_cmd((short)HIWORD(wp) > 0 ? CM_ZOOMIN : CM_ZOOMOUT); return 0; }
        if (t) scroll_by(t, -(short)HIWORD(wp) / 120 * 3);
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), l, c;
        SetFocus(w);
        if (!t) return 0;
        if (vs_mouse(&s_vs, w, m, x, y)) { t->top = s_vs.pos; scroll_by(t, 0); return 0; }
        hit(t, x, y, &l, &c);
        if (key_mods() & M_CTRL) { open_location(t, l); return 0; }
        if (m == WM_LBUTTONDBLCLK) {
            int len, a = c, b = c; const char *s = line_text(t, l, &len);
            while (a > 0 && s[a-1] != ' ' && s[a-1] != '"') a--;
            while (b < len && s[b] != ' ' && s[b] != '"') b++;
            s_selA[0] = s_selB[0] = l; s_selA[1] = a; s_selB[1] = b; s_hasSel = b > a;
            InvalidateRect(w, 0, 0);
            return 0;
        }
        if (key_mods() & M_SHIFT && s_hasSel) { s_selB[0] = l; s_selB[1] = c; }
        else { s_selA[0] = s_selB[0] = l; s_selA[1] = s_selB[1] = c; s_hasSel = 0; }
        s_selecting = 1; SetCapture(w);
        InvalidateRect(w, 0, 0);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), l, c;
        if (!t) return 0;
        if (s_vs.drag) { vs_mouse(&s_vs, w, m, x, y); t->top = s_vs.pos; scroll_by(t, 0); return 0; }
        if (!s_selecting) return 0;
        { RECT r; GetClientRect(w, &r); if (y < 0) scroll_by(t, -1); else if (y > r.bottom) scroll_by(t, 1); }
        hit(t, x, y, &l, &c);
        s_selB[0] = l; s_selB[1] = c;
        s_hasSel = s_selA[0] != l || s_selA[1] != c;
        InvalidateRect(w, 0, 0);
        return 0;
    }
    case WM_LBUTTONUP:
        if (s_vs.drag) vs_mouse(&s_vs, w, m, 0, 0);
        if (s_selecting) { s_selecting = 0; ReleaseCapture(); }
        return 0;
    case WM_RBUTTONUP:
        /* Windows console style: right-click copies a selection, otherwise pastes */
        if (!t) return 0;
        SetFocus(w);
        if (s_hasSel) { copy_sel(t); s_hasSel = 0; } else if (!t->isOutput) paste(t);
        InvalidateRect(w, 0, 0);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursor(0, IDC_IBEAM)); return TRUE; }
        break;
    case WM_TERMDATA: {
        Chunk *c, *next; int i;
        EnterCriticalSection(&s_qlock);
        c = s_qhead; s_qhead = s_qtail = 0;
        LeaveCriticalSection(&s_qlock);
        for (; c; c = next) {
            next = c->next;
            for (i = 0; i < s_nterms; i++) if (s_terms[i]->serial == c->serial) {
                Term *x = s_terms[i];
                OemToCharBuffA(c->data, c->data, c->n);
                feed(x, c->data, c->n);
                if (x->follow) follow_bottom(x);
                if (x == cur_term()) InvalidateRect(w, 0, 0);
                break;
            }
            free(c);
        }
        return 0;
    }
    case WM_TERMEXIT: {
        int i;
        SendMessageA(w, WM_TERMDATA, 0, 0);
        for (i = 0; i < s_nterms; i++) if (s_terms[i]->serial == (int)wp) {
            Term *x = s_terms[i];
            x->alive = 0;
            out_log("Terminal %d (cmd) exited.", x->number);
            term_kill(i);
            if (g_main) InvalidateRect(g_main, 0, 0);
            break;
        }
        return 0;
    }
    }
    return DefWindowProcA(w, m, wp, lp);
}

/* ------------------------------------------------------------------ problems view */

static int prob_rows(void) { int i, n = 0; for (i = 0; i < g_nprobs; i++) { if (!i || lstrcmpiA(g_probs[i].file, g_probs[i-1].file)) n++; n++; } return n; }

/* row -> problem index; *hdr set for file header rows */
static int prob_at(int row, int *hdr)
{
    int i, r = 0;
    for (i = 0; i < g_nprobs; i++) {
        if (!i || lstrcmpiA(g_probs[i].file, g_probs[i-1].file)) { if (r == row) { *hdr = 1; return i; } r++; }
        if (r == row) { *hdr = 0; return i; }
        r++;
    }
    *hdr = 0;
    return -1;
}

static int prob_row_of(int idx)
{
    int i, r = 0;
    for (i = 0; i < g_nprobs; i++) {
        if (!i || lstrcmpiA(g_probs[i].file, g_probs[i-1].file)) r++;
        if (i == idx) return r;
        r++;
    }
    return -1;
}

void prob_goto(int i)
{
    if (i < 0 || i >= g_nprobs) return;
    s_probIdx = i;
    s_probSel = prob_row_of(i);
    if (file_exists(g_probs[i].file)) open_file(g_probs[i].file, g_probs[i].line, g_probs[i].col);
    else out_log("File not found: %s", g_probs[i].file);
    if (g_prob) InvalidateRect(g_prob, 0, 0);
}

void prob_next(int dir)
{
    if (!g_nprobs) return;
    s_probIdx = s_probIdx < 0 ? (dir > 0 ? 0 : g_nprobs - 1) : (s_probIdx + dir + g_nprobs) % g_nprobs;
    prob_goto(s_probIdx);
}

static void prob_paint(HWND w)
{
    PAINTSTRUCT ps; HDC wdc = BeginPaint(w, &ps), dc;
    RECT r; HGDIOBJ of;
    int i, rows, n = prob_rows();
    char buf[400];
    GetClientRect(w, &r);
    dc = bb_begin(w, wdc, r.right, r.bottom);
    fill(dc, 0, 0, r.right, r.bottom, C_BG);
    of = SelectObject(dc, g_ui);
    SetBkMode(dc, TRANSPARENT);
    if (!g_nprobs) ui_text(dc, 20, 6, "No problems have been detected in the workspace.", C_TEXT);
    rows = r.bottom / ROW_H + 1;
    if (s_probTop > n - rows + 1) s_probTop = n - rows + 1;
    if (s_probTop < 0) s_probTop = 0;
    for (i = 0; i < rows; i++) {
        int hdr, k = prob_at(s_probTop + i, &hdr), y = i * ROW_H, ty = y + (ROW_H - g_uiH) / 2;
        Problem *p;
        if (k < 0) break;
        p = &g_probs[k];
        if (s_probTop + i == s_probSel) fill(dc, 0, y, r.right - SB_W, ROW_H, GetFocus() == w ? C_LISTFOC : C_LISTSEL);
        if (hdr) {
            char dir[MAX_PATH]; SIZE z; int cnt = 0, j;
            for (j = k; j < g_nprobs && !lstrcmpiA(g_probs[j].file, p->file); j++) cnt++;
            draw_icon(dc, IC_CHEVD, 8, y + 3, C_TEXT);
            file_glyph(dc, path_name(p->file), 0, 26, ty);
            ui_text(dc, 46, ty, path_name(p->file), C_TEXT);
            GetTextExtentPoint32A(dc, path_name(p->file), lstrlenA(path_name(p->file)), &z);
            path_dir(dir, p->file);
            ui_text(dc, 52 + z.cx, ty, rel_path(dir) == dir ? dir : rel_path(dir), C_DIM);
            GetTextExtentPoint32A(dc, dir, lstrlenA(rel_path(dir) == dir ? dir : rel_path(dir)), &z);
            wsprintfA(buf, "%d", cnt);
            {
                SIZE b; int bx; GetTextExtentPoint32A(dc, path_name(p->file), lstrlenA(path_name(p->file)), &b);
                bx = 60 + b.cx + z.cx;
                fill(dc, bx, y + 3, 18, ROW_H - 6, XRGB(0x4D,0x4D,0x4D));
                ui_text(dc, bx + 5, ty, buf, C_WHITE);
            }
        } else {
            SIZE z;
            draw_icon(dc, p->sev == 0 ? IC_ERR : IC_WARN, 40, y + 3, 0);
            ui_text(dc, 62, ty, p->msg, C_TEXT);
            GetTextExtentPoint32A(dc, p->msg, lstrlenA(p->msg), &z);
            wsprintfA(buf, "[Ln %d, Col %d]", p->line, p->col);
            ui_text(dc, 70 + z.cx, ty, buf, C_DIM);
        }
    }
    s_pvs.r.left = r.right - SB_W; s_pvs.r.top = 0; s_pvs.r.right = r.right; s_pvs.r.bottom = r.bottom;
    s_pvs.page = rows - 1; s_pvs.max = n; s_pvs.pos = s_probTop;
    vs_draw(dc, &s_pvs);
    SelectObject(dc, of);
    BitBlt(wdc, 0, 0, r.right, r.bottom, dc, 0, 0, SRCCOPY);
    
    EndPaint(w, &ps);
}

static LRESULT CALLBACK ProbProc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_PAINT: prob_paint(w); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_SIZE: InvalidateRect(w, 0, 0); return 0;
    case WM_GETDLGCODE: return DLGC_WANTALLKEYS;
    case WM_MOUSEWHEEL: s_probTop -= (short)HIWORD(wp) / 120 * 3; if (s_probTop < 0) s_probTop = 0; InvalidateRect(w, 0, 0); return 0;
    case WM_KEYDOWN: {
        int n = prob_rows(), hdr, k;
        RECT r; GetClientRect(w, &r);
        if (wp == VK_UP && s_probSel > 0) s_probSel--;
        else if (wp == VK_DOWN && s_probSel < n - 1) s_probSel++;
        else if (wp == VK_RETURN) { k = prob_at(s_probSel, &hdr); if (k >= 0 && !hdr) prob_goto(k); return 0; }
        else if (wp == VK_ESCAPE) { focus_editor(); return 0; }
        if (s_probSel < s_probTop) s_probTop = s_probSel;
        if (s_probSel >= s_probTop + r.bottom / ROW_H) s_probTop = s_probSel - r.bottom / ROW_H + 1;
        InvalidateRect(w, 0, 0);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), hdr, k;
        SetFocus(w);
        if (vs_mouse(&s_pvs, w, m, x, y)) { s_probTop = s_pvs.pos; InvalidateRect(w, 0, 0); return 0; }
        s_probSel = s_probTop + y / ROW_H;
        k = prob_at(s_probSel, &hdr);
        InvalidateRect(w, 0, 0);
        if (k >= 0 && !hdr) prob_goto(k);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (s_pvs.drag) { vs_mouse(&s_pvs, w, m, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); s_probTop = s_pvs.pos; InvalidateRect(w, 0, 0); }
        return 0;
    case WM_LBUTTONUP: if (s_pvs.drag) vs_mouse(&s_pvs, w, m, 0, 0); return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

void term_register(void)
{
    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = TermProc; wc.hInstance = g_inst; wc.lpszClassName = "XPTerm";
    wc.hCursor = LoadCursor(0, IDC_IBEAM);
    RegisterClassA(&wc);
    wc.lpfnWndProc = ProbProc; wc.lpszClassName = "XPProblems";
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    RegisterClassA(&wc);
    InitializeCriticalSection(&s_qlock);
    SetEnvironmentVariableA("PYTHONUNBUFFERED", "1");
    new_line(&s_out); s_out.isOutput = 1; s_out.follow = 1;
}

