#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <psapi.h>

typedef unsigned long long u64;

void *memset(void *d, int c, size_t n)
{
    unsigned char *p = (unsigned char *)d;
    while (n--) *p++ = (unsigned char)c;
    return d;
}

void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    while (n--) *dp++ = *sp++;
    return d;
}

void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *dp = (unsigned char *)d;
    const unsigned char *sp = (const unsigned char *)s;
    if (dp < sp) {
        while (n--) *dp++ = *sp++;
    } else if (dp > sp) {
        dp += n; sp += n;
        while (n--) *--dp = *--sp;
    }
    return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    while (n--) {
        if (*pa != *pb) return *pa - *pb;
        pa++; pb++;
    }
    return 0;
}

#define IDC_SEARCH  101
#define IDC_REFRESH 102
#define IDC_AUTO    103
#define IDC_LIST    104
#define IDC_KILL    105
#define IDC_OPENDIR 106
#define IDC_COPY    107
#define IDC_STATUS  108

#define COL_PID  0
#define COL_NAME 1
#define COL_PATH 2
#define COL_MEM  3
#define COL_CPU  4

#define IDT_AUTO 1
#define IDT_SEED 2

#define TM_PATH 1024
#define TM_NAME 128

typedef struct PSLOT {
    DWORD     pid;
    wchar_t   name[TM_NAME];
    wchar_t   path[TM_PATH];
    int       pathOk;
    SIZE_T    mem;
    int       cpu10;
    ULONGLONG lastCpu;
    FILETIME  lastCreate;
    int       hasTimes;
} PSLOT;

static HWND g_hwnd, g_search, g_refresh, g_auto, g_list;
static HWND g_kill, g_opendir, g_copy, g_status;
static PSLOT *g_slots;
static int g_nslots, g_cap;
static int *g_disp;
static int g_ndisp, g_dcap;
static int g_sortCol = COL_PID, g_sortAsc = 1;
static int g_autoOn;
static int g_selftest;
static ULONGLONG g_lastWall;

static u64 FileTimeU(const FILETIME *ft)
{
    return ((u64)ft->dwHighDateTime << 32) | ft->dwLowDateTime;
}

static void SetStatus(const wchar_t *s)
{
    if (g_status) SetWindowTextW(g_status, s);
}

static int ContainsI(const wchar_t *hay, const wchar_t *needle)
{
    int nlen, i;
    const wchar_t *p;

    if (!needle || !needle[0]) return 1;
    if (!hay) return 0;
    nlen = lstrlenW(needle);
    for (p = hay; *p; p++) {
        for (i = 0; i < nlen; i++) {
            wchar_t a = p[i], b = needle[i];
            if (!a) break;
            if (a >= L'A' && a <= L'Z') a += 32;
            if (b >= L'A' && b <= L'Z') b += 32;
            if (a != b) break;
        }
        if (i == nlen) return 1;
    }
    return 0;
}

static void FmtMem(wchar_t *b, SIZE_T bytes)
{
    ULONGLONG t = ((ULONGLONG)bytes * 10 + 524288ULL) / 1048576ULL;
    wsprintfW(b, L"%u.%u", (unsigned)(t / 10), (unsigned)(t % 10));
}

static void FmtCpu(wchar_t *b, int tenths)
{
    if (tenths < 0) tenths = 0;
    if (tenths > 9999) tenths = 9999;
    wsprintfW(b, L"%d.%d", tenths / 10, tenths % 10);
}

static DWORD PidFromText(const wchar_t *b)
{
    DWORD v = 0;
    while (*b >= L'0' && *b <= L'9') {
        v = v * 10 + (DWORD)(*b - L'0');
        b++;
    }
    return v;
}

static PSLOT *FindSlot(DWORD pid)
{
    int i;
    for (i = 0; i < g_nslots; i++)
        if (g_slots[i].pid == pid) return &g_slots[i];
    return NULL;
}

static PSLOT *AddSlot(DWORD pid, const wchar_t *name)
{
    PSLOT *s;
    if (g_nslots >= g_cap) {
        int nc = g_cap ? g_cap * 2 : 256;
        PSLOT *ns;
        if (g_slots)
            ns = (PSLOT *)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                      g_slots, nc * sizeof(PSLOT));
        else
            ns = (PSLOT *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    nc * sizeof(PSLOT));
        if (!ns) return NULL;
        g_slots = ns;
        g_cap = nc;
    }
    s = &g_slots[g_nslots++];
    memset(s, 0, sizeof(*s));
    s->pid = pid;
    lstrcpynW(s->name, name, TM_NAME);
    return s;
}

static void RemoveSlotAt(int i)
{
    int j;
    for (j = i; j < g_nslots - 1; j++)
        g_slots[j] = g_slots[j + 1];
    g_nslots--;
}

static void QueryPath(PSLOT *s)
{
    HANDLE h;
    DWORD n = TM_PATH;

    s->pathOk = 0;
    s->path[0] = 0;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, s->pid);
    if (!h) return;
    if (QueryFullProcessImageNameW(h, 0, s->path, &n))
        s->pathOk = 1;
    CloseHandle(h);
}

static void QuerySample(PSLOT *s, ULONGLONG nowWall, int ncpu)
{
    HANDLE h;
    PROCESS_MEMORY_COUNTERS_EX pmc;
    FILETIME cr, ke, us, dummy;

    s->mem = 0;
    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, s->pid);
    if (!h) {
        s->cpu10 = 0;
        s->hasTimes = 0;
        return;
    }

    memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc)))
        s->mem = pmc.PrivateUsage;

    if (GetProcessTimes(h, &cr, &dummy, &ke, &us)) {
        ULONGLONG cpu = FileTimeU(&ke) + FileTimeU(&us);
        if (s->hasTimes &&
            s->lastCreate.dwLowDateTime == cr.dwLowDateTime &&
            s->lastCreate.dwHighDateTime == cr.dwHighDateTime &&
            g_lastWall && nowWall > g_lastWall &&
            cpu >= s->lastCpu) {
            ULONGLONG w = nowWall - g_lastWall;
            ULONGLONG d = cpu - s->lastCpu;
            u64 v;
            if (ncpu < 1) ncpu = 1;
            v = (u64)(d * 1000ULL / (w * (ULONGLONG)ncpu));
            if (v > 9999) v = 9999;
            s->cpu10 = (int)v;
        } else {
            s->cpu10 = 0;
        }
        s->lastCpu = cpu;
        s->lastCreate = cr;
        s->hasTimes = 1;
    } else {
        s->cpu10 = 0;
        s->hasTimes = 0;
    }
    CloseHandle(h);
}

static int SnapshotRefresh(void)
{
    HANDLE snap;
    PROCESSENTRY32W pe;
    DWORD *pids = NULL;
    int np = 0, cnt = 0, i, j, ncpu;
    ULONGLONG nowWall;
    FILETIME nowFt;
    SYSTEM_INFO si;

    GetSystemTimeAsFileTime(&nowFt);
    nowWall = FileTimeU(&nowFt);
    GetSystemInfo(&si);
    ncpu = (int)si.dwNumberOfProcessors;
    if (ncpu < 1) ncpu = 1;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do { cnt++; } while (Process32NextW(snap, &pe));
    }
    pids = (DWORD *)HeapAlloc(GetProcessHeap(), 0, (cnt + 1) * sizeof(DWORD));
    if (!pids) {
        CloseHandle(snap);
        return 0;
    }

    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            PSLOT *s = FindSlot(pe.th32ProcessID);
            if (!s)
                s = AddSlot(pe.th32ProcessID, pe.szExeFile);
            else
                lstrcpynW(s->name, pe.szExeFile, TM_NAME);
            if (s) {
                QuerySample(s, nowWall, ncpu);
                if (!s->pathOk && !s->path[0])
                    QueryPath(s);
            }
            pids[np++] = pe.th32ProcessID;
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);

    for (i = 0; i < g_nslots; ) {
        int found = 0;
        for (j = 0; j < np; j++) {
            if (pids[j] == g_slots[i].pid) { found = 1; break; }
        }
        if (!found) RemoveSlotAt(i);
        else i++;
    }
    HeapFree(GetProcessHeap(), 0, pids);
    g_lastWall = nowWall;
    return 1;
}

static int CmpSlot(const PSLOT *a, const PSLOT *b)
{
    int r = 0;
    switch (g_sortCol) {
    case COL_PID:
        r = (a->pid > b->pid) - (a->pid < b->pid);
        break;
    case COL_NAME:
        r = lstrcmpiW(a->name, b->name);
        if (r == 0) r = (a->pid > b->pid) - (a->pid < b->pid);
        break;
    case COL_PATH:
        r = lstrcmpiW(a->path, b->path);
        if (r == 0) r = (a->pid > b->pid) - (a->pid < b->pid);
        break;
    case COL_MEM:
        r = (a->mem > b->mem) - (a->mem < b->mem);
        if (r == 0) r = (a->pid > b->pid) - (a->pid < b->pid);
        break;
    case COL_CPU:
        r = (a->cpu10 > b->cpu10) - (a->cpu10 < b->cpu10);
        if (r == 0) r = (a->pid > b->pid) - (a->pid < b->pid);
        break;
    }
    return g_sortAsc ? r : -r;
}

static void SortDisp(int *arr, int n)
{
    int i, j;
    for (i = 1; i < n; i++) {
        int key = arr[i];
        j = i - 1;
        while (j >= 0 && CmpSlot(&g_slots[arr[j]], &g_slots[key]) > 0) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

static int MatchFilter(const PSLOT *s, const wchar_t *f)
{
    wchar_t pidbuf[16];
    if (!f[0]) return 1;
    if (ContainsI(s->name, f)) return 1;
    if (ContainsI(s->path, f)) return 1;
    wsprintfW(pidbuf, L"%u", s->pid);
    if (ContainsI(pidbuf, f)) return 1;
    return 0;
}

static DWORD SelPid(void)
{
    int idx;
    wchar_t b[16];
    if (!g_list) return 0;
    idx = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (idx < 0) return 0;
    b[0] = 0;
    ListView_GetItemText(g_list, idx, COL_PID, b, 16);
    return PidFromText(b);
}

static int SelPath(wchar_t *out, int cap)
{
    int idx;
    out[0] = 0;
    if (!g_list) return -1;
    idx = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (idx < 0) return -1;
    ListView_GetItemText(g_list, idx, COL_PATH, out, cap);
    return idx;
}

static void RebuildList(void)
{
    wchar_t filter[128];
    DWORD selPid = 0, topPid = 0;
    int i, n, topIdx;
    LVITEMW li;
    wchar_t buf[64];

    if (!g_list) return;

    selPid = SelPid();
    topIdx = ListView_GetTopIndex(g_list);
    if (topIdx >= 0 && topIdx < ListView_GetItemCount(g_list)) {
        buf[0] = 0;
        ListView_GetItemText(g_list, topIdx, COL_PID, buf, 16);
        topPid = PidFromText(buf);
    }

    filter[0] = 0;
    if (g_search) GetWindowTextW(g_search, filter, 128);

    n = 0;
    for (i = 0; i < g_nslots; i++) {
            if (MatchFilter(&g_slots[i], filter)) {
                if (n >= g_dcap) {
                    int nc = g_dcap ? g_dcap * 2 : 256;
                    int *nd;
                    if (g_disp)
                        nd = (int *)HeapReAlloc(GetProcessHeap(), 0, g_disp, nc * sizeof(int));
                    else
                        nd = (int *)HeapAlloc(GetProcessHeap(), 0, nc * sizeof(int));
                    if (!nd) break;
                    g_disp = nd;
                    g_dcap = nc;
                }
                g_disp[n++] = i;
            }
    }
    g_ndisp = n;
    SortDisp(g_disp, g_ndisp);

    SendMessageW(g_list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_list);

    ZeroMemory(&li, sizeof(li));
    for (i = 0; i < g_ndisp; i++) {
        PSLOT *s = &g_slots[g_disp[i]];
        li.mask = LVIF_TEXT;
        li.iItem = i;
        li.iSubItem = 0;
        wsprintfW(buf, L"%u", s->pid);
        li.pszText = buf;
        li.iItem = ListView_InsertItem(g_list, &li);
        if (li.iItem < 0) break;
        ListView_SetItemText(g_list, li.iItem, COL_NAME, s->name);
        ListView_SetItemText(g_list, li.iItem, COL_PATH,
                             s->pathOk ? s->path : L"(无法访问)");
        FmtMem(buf, s->mem);
        ListView_SetItemText(g_list, li.iItem, COL_MEM, buf);
        FmtCpu(buf, s->cpu10);
        ListView_SetItemText(g_list, li.iItem, COL_CPU, buf);
    }

    if (selPid) {
        for (i = 0; i < g_ndisp; i++) {
            if (g_slots[g_disp[i]].pid == selPid) {
                ListView_SetItemState(g_list, i, LVIS_SELECTED | LVIS_FOCUSED,
                                      LVIS_SELECTED | LVIS_FOCUSED);
                break;
            }
        }
    }
    if (topPid) {
        for (i = 0; i < g_ndisp; i++) {
            if (g_slots[g_disp[i]].pid == topPid) {
                ListView_EnsureVisible(g_list, i, FALSE);
                break;
            }
        }
    }

    SendMessageW(g_list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_list, NULL, TRUE);

    {
        wchar_t st[128];
        wsprintfW(st, L"共 %d 个进程 · 显示 %d 个", g_nslots, g_ndisp);
        if (g_status) SetWindowTextW(g_status, st);
    }
}

static void DoRefresh(void)
{
    SnapshotRefresh();
    RebuildList();
}

static void DoKill(void)
{
    DWORD pid = SelPid();
    HANDLE h;
    wchar_t msg[320];
    DWORD err;

    if (!pid) {
        SetStatus(L"请先选中一个进程");
        return;
    }
    if (MessageBoxW(g_hwnd, L"确定要结束所选进程吗？", L"结束任务",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;

    h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (!h) {
        err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
            wsprintfW(msg,
                L"无法结束进程 %u：访问被拒绝。\n"
                L"该进程可能是受保护的系统进程（PPL），或需要更高权限。",
                pid);
        else
            wsprintfW(msg, L"无法打开进程 %u 进行结束（错误 %u）。", pid, err);
        MessageBoxW(g_hwnd, msg, L"结束失败", MB_OK | MB_ICONWARNING);
        SetStatus(L"结束进程失败");
        return;
    }
    if (!TerminateProcess(h, 1)) {
        err = GetLastError();
        if (err == ERROR_ACCESS_DENIED)
            wsprintfW(msg,
                L"无法结束进程 %u：访问被拒绝。\n"
                L"该进程可能是受保护的系统进程（PPL），TerminateProcess 被拒绝。",
                pid);
        else
            wsprintfW(msg, L"结束进程 %u 失败（错误 %u）。", pid, err);
        MessageBoxW(g_hwnd, msg, L"结束失败", MB_OK | MB_ICONWARNING);
        SetStatus(L"结束进程失败");
    } else {
        SetStatus(L"已请求结束进程");
    }
    CloseHandle(h);
    DoRefresh();
}

static void DoOpenDir(void)
{
    wchar_t path[TM_PATH], cmd[TM_PATH + 32];
    if (SelPath(path, TM_PATH) < 0 || !path[0] || path[0] == L'(') {
        SetStatus(L"请先选中一个有路径的进程");
        return;
    }
    wsprintfW(cmd, L"/select,\"%s\"", path);
    ShellExecuteW(g_hwnd, L"open", L"explorer.exe", cmd, NULL, SW_SHOWNORMAL);
    SetStatus(L"已打开所在目录");
}

static void DoCopy(void)
{
    wchar_t path[TM_PATH];
    HGLOBAL h;
    wchar_t *p;

    if (SelPath(path, TM_PATH) < 0 || !path[0] || path[0] == L'(') {
        SetStatus(L"请先选中一个有路径的进程");
        return;
    }
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    h = GlobalAlloc(GMEM_MOVEABLE, (lstrlenW(path) + 1) * sizeof(wchar_t));
    if (h) {
        p = (wchar_t *)GlobalLock(h);
        if (p) {
            lstrcpyW(p, path);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
    SetStatus(L"已复制完整路径");
}

static void AddCol(int i, const wchar_t *t, int w, int fmt)
{
    LVCOLUMNW c;
    ZeroMemory(&c, sizeof(c));
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    if (fmt) c.mask |= LVCF_FMT;
    c.pszText = (LPWSTR)t;
    c.cx = w;
    c.iSubItem = i;
    c.fmt = fmt;
    ListView_InsertColumn(g_list, i, &c);
}

static void Layout(void)
{
    RECT rc;
    int w, top, listH;
    HDWP d;

    if (!g_hwnd) return;
    GetClientRect(g_hwnd, &rc);
    w = rc.right - rc.left;
    top = 8;
    d = BeginDeferWindowPos(8);
    d = DeferWindowPos(d, g_search, NULL, 8, top, 240, 24, SWP_NOZORDER);
    d = DeferWindowPos(d, g_refresh, NULL, 256, top, 72, 24, SWP_NOZORDER);
    d = DeferWindowPos(d, g_auto, NULL, 336, top, 150, 24, SWP_NOZORDER);
    top += 32;
    listH = rc.bottom - top - 48;
    if (listH < 40) listH = 40;
    d = DeferWindowPos(d, g_list, NULL, 8, top, w - 16, listH, SWP_NOZORDER);
    top += listH + 8;
    d = DeferWindowPos(d, g_kill, NULL, 8, top, 100, 26, SWP_NOZORDER);
    d = DeferWindowPos(d, g_opendir, NULL, 116, top, 120, 26, SWP_NOZORDER);
    d = DeferWindowPos(d, g_copy, NULL, 244, top, 110, 26, SWP_NOZORDER);
    d = DeferWindowPos(d, g_status, NULL, 364, top, w - 372, 26, SWP_NOZORDER);
    EndDeferWindowPos(d);
}

static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX ic = { sizeof(ic), ICC_LISTVIEW_CLASSES };
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        g_hwnd = wnd;
        InitCommonControlsEx(&ic);
        g_search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            0, 0, 0, 0, wnd, (HMENU)(UINT_PTR)IDC_SEARCH, cs->hInstance, NULL);
        SendMessageW(g_search, EM_SETCUEBANNER, TRUE, (LPARAM)L"搜索名称 / PID / 路径");
        g_refresh = CreateWindowExW(0, L"BUTTON", L"刷新",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, wnd, (HMENU)(UINT_PTR)IDC_REFRESH, cs->hInstance, NULL);
        g_auto = CreateWindowExW(0, L"BUTTON", L"自动刷新 (2秒)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            0, 0, 0, 0, wnd, (HMENU)(UINT_PTR)IDC_AUTO, cs->hInstance, NULL);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, NULL,
            WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL,
            0, 0, 0, 0, wnd, (HMENU)(UINT_PTR)IDC_LIST, cs->hInstance, NULL);
        ListView_SetExtendedListViewStyle(g_list,
            LVS_EX_FULLROWSELECT | LVS_EX_INFOTIP | LVS_EX_HEADERDRAGDROP |
            LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        AddCol(COL_PID,  L"PID",       70,  LVCFMT_RIGHT);
        AddCol(COL_NAME, L"名称",      160, 0);
        AddCol(COL_PATH, L"完整路径",  380, 0);
        AddCol(COL_MEM,  L"内存 (MB)", 90,  LVCFMT_RIGHT);
        AddCol(COL_CPU,  L"CPU (%)",   80,  LVCFMT_RIGHT);
        g_kill = CreateWindowExW(0, L"BUTTON", L"结束任务",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, wnd, (HMENU)(UINT_PTR)IDC_KILL, cs->hInstance, NULL);
        g_opendir = CreateWindowExW(0, L"BUTTON", L"打开文件位置",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, wnd, (HMENU)(UINT_PTR)IDC_OPENDIR, cs->hInstance, NULL);
        g_copy = CreateWindowExW(0, L"BUTTON", L"复制路径",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            0, 0, 0, 0, wnd, (HMENU)(UINT_PTR)IDC_COPY, cs->hInstance, NULL);
        g_status = CreateWindowExW(0, L"STATIC", L"就绪",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
            0, 0, 0, 0, wnd, (HMENU)(UINT_PTR)IDC_STATUS, cs->hInstance, NULL);
        Layout();
        DoRefresh();
        SetTimer(wnd, IDT_SEED, 500, NULL);
        return 0;
    }
    case WM_SIZE:
        Layout();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_REFRESH: DoRefresh(); return 0;
        case IDC_KILL: DoKill(); return 0;
        case IDC_OPENDIR: DoOpenDir(); return 0;
        case IDC_COPY: DoCopy(); return 0;
        case IDC_AUTO:
            g_autoOn = (SendMessageW(g_auto, BM_GETCHECK, 0, 0) == BST_CHECKED);
            if (g_autoOn) {
                SetTimer(wnd, IDT_AUTO, 2000, NULL);
                SetStatus(L"自动刷新已开启（2 秒）");
            } else {
                KillTimer(wnd, IDT_AUTO);
                SetStatus(L"自动刷新已关闭");
            }
            return 0;
        case IDC_SEARCH:
            if (HIWORD(wp) == EN_CHANGE)
                RebuildList();
            return 0;
        }
        break;
    case WM_TIMER:
        if (wp == IDT_AUTO) {
            if (g_autoOn) DoRefresh();
            return 0;
        }
        if (wp == IDT_SEED) {
            KillTimer(wnd, IDT_SEED);
            DoRefresh();
            return 0;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->hwndFrom == g_list && nm->code == LVN_COLUMNCLICK) {
            NMLISTVIEW *lv = (NMLISTVIEW *)lp;
            if (g_sortCol == lv->iSubItem)
                g_sortAsc = !g_sortAsc;
            else {
                g_sortCol = lv->iSubItem;
                g_sortAsc = 1;
            }
            RebuildList();
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        KillTimer(wnd, IDT_AUTO);
        KillTimer(wnd, IDT_SEED);
        DestroyWindow(wnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(wnd, msg, wp, lp);
}

static int RunSelfTest(void)
{
    HANDLE snap;
    PROCESSENTRY32W pe;
    int n = 0;
    static PSLOT probe;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 2;
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do { n++; } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    if (n < 2) return 3;

    memset(&probe, 0, sizeof(probe));
    probe.pid = GetCurrentProcessId();
    lstrcpynW(probe.name, L"self", TM_NAME);
    QueryPath(&probe);
    if (!probe.pathOk) return 4;

    if (!SnapshotRefresh()) return 5;
    if (g_nslots < 2) return 6;

    /* Simulate window-less rebuild target setup is skipped in selftest;
       verify filter/sort path with a minimal list allocation. */
    {
        wchar_t filterTest[] = L".exe";
        int i, m = 0;
        for (i = 0; i < g_nslots; i++)
            if (MatchFilter(&g_slots[i], filterTest)) m++;
        if (m < 1) return 7;
    }
    return 0;
}

void Entry(void)
{
    WNDCLASSEXW wc;
    MSG msg;
    HWND hwnd;
    const wchar_t *cls = L"TaskMgrLiteClass";
    int argc = 0;
    LPWSTR *argv;
    int rc;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        int i;
        for (i = 1; i < argc; i++) {
            if (lstrcmpiW(argv[i], L"-selftest") == 0)
                g_selftest = 1;
        }
        LocalFree(argv);
    }

    if (g_selftest) {
        rc = RunSelfTest();
        ExitProcess((UINT)rc);
    }

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);

    hwnd = CreateWindowExW(0, cls, L"任务管理器",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 560,
        NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) ExitProcess(1);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (IsDialogMessageW(hwnd, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    ExitProcess((UINT)msg.wParam);
}
