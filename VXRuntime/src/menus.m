// Menus (user32): menu objects with their items (CreatePopupMenu, AppendMenu, InsertMenuItem, SetMenuItemInfo ...), and
// TrackPopupMenu(Ex) as an NSMenu at the point given: what a program shows from its notification-area icon (tray.m)
// or as a context menu. Windows here have no menu bar (SetMenu stays user32.c's).
//
// TrackPopupMenu is modal for the calling thread, as on Windows: WM_ENTERMENULOOP and WM_INITMENUPOPUP (for the menu
// and, before it opens, each submenu) go to the owner window first, the NSMenu runs on the main thread, and the
// calling thread meanwhile keeps handling its messages (sent, posted, timers) until the menu closes. The choice comes
// back as the return value (TPM_RETURNCMD) or as WM_COMMAND / WM_MENUCOMMAND posted to the owner.
#import <Cocoa/Cocoa.h>
#include "ui.h"

typedef int32_t WINBOOL;                        // (Objective-C has a BOOL of its own, one byte)
#define MENU_BASE 0xE00000                      // below 16 MB, where the vCPU's low memory never is (vx.h)
#define MAX_MENUS 65536
enum { MF_GRAYED = 0x1, MF_DISABLED = 0x2, MF_BITMAP = 0x4, MF_CHECKED = 0x8, MF_POPUP = 0x10, MF_HILITE = 0x80,
       MF_OWNERDRAW = 0x100, MF_RADIOCHECK = 0x200, MF_BYPOSITION = 0x400, MF_SEPARATOR = 0x800, MF_DEFAULT = 0x1000 };
enum { MFT_TYPE_BITS = MF_BITMAP | 0x20 | 0x40 | MF_OWNERDRAW | MF_RADIOCHECK | MF_SEPARATOR | 0x2000 | 0x4000,
       MFS_STATE_BITS = MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_HILITE | MF_DEFAULT };
enum { MIIM_STATE = 0x1, MIIM_ID = 0x2, MIIM_SUBMENU = 0x4, MIIM_CHECKMARKS = 0x8, MIIM_TYPE = 0x10, MIIM_DATA = 0x20,
       MIIM_STRING = 0x40, MIIM_BITMAP = 0x80, MIIM_FTYPE = 0x100 };
enum { TPM_CENTERALIGN = 0x4, TPM_RIGHTALIGN = 0x8, TPM_VCENTERALIGN = 0x10, TPM_BOTTOMALIGN = 0x20, TPM_NONOTIFY = 0x80,
       TPM_RETURNCMD = 0x100 };
enum { WM_COMMAND = 0x111, WM_INITMENUPOPUP = 0x117, WM_UNINITMENUPOPUP = 0x125, WM_MENUCOMMAND = 0x126,
       WM_ENTERMENULOOP = 0x211, WM_EXITMENULOOP = 0x212 };
enum { MIM_MAXHEIGHT = 0x1, MIM_BACKGROUND = 0x2, MIM_HELPID = 0x4, MIM_MENUDATA = 0x8, MIM_STYLE = 0x10 };
#define MNS_NOTIFYBYPOS 0x08000000u
enum { ERROR_INVALID_WINDOW_HANDLE = 1400, ERROR_INVALID_MENU_HANDLE = 1401, ERROR_POPUP_ALREADY_ACTIVE = 1446,
       ERROR_MENU_ITEM_NOT_FOUND = 1456, ERROR_INVALID_PARAMETER = 87 };

typedef struct {
    uint32_t type, state, id;                   // MFT_*, MFS_*, the command
    HANDLE sub;                                 // a submenu, or NULL
    uintptr_t data;                             // dwItemData
    char *text;                                 // UTF-8; NULL for none
    HANDLE bitmap, checked_bmp, unchecked_bmp;
} item;
typedef struct {
    item *v; int n, cap;
    uint32_t style, max_height, help; HANDLE background; uintptr_t menu_data;   // MENUINFO
} menu;

static menu *g_menus[MAX_MENUS];
static int g_menu_next;
static pthread_mutex_t g_menu_lock = PTHREAD_MUTEX_INITIALIZER;

static menu *get(HANDLE h) {                    // (g_menu_lock held)
    uintptr_t v = (uintptr_t)h - MENU_BASE;
    return (uintptr_t)h >= MENU_BASE && v % 4 == 0 && v / 4 < MAX_MENUS ? g_menus[v / 4] : NULL;
}
static menu *lock_menu(HANDLE h) {              // the menu, with g_menu_lock held; NULL (and unlocked) if there is none
    pthread_mutex_lock(&g_menu_lock);
    menu *m = get(h);
    if (!m) { pthread_mutex_unlock(&g_menu_lock); vx_set_last_error(ERROR_INVALID_MENU_HANDLE); }
    return m;
}
static void unlock(void) { pthread_mutex_unlock(&g_menu_lock); }

static void item_free(item *it) { free(it->text); it->text = NULL; }
static char *dup_text(const void *s, int wide) {
    if (!s) return NULL;
    return wide ? vx_utf8_dup(VX_LOW(s)) : strdup(VX_LOW(s));
}

// The item a command or a position names: by position in this menu, or by command here and in its submenus.
static item *find(menu *m, uint32_t which, int by_pos, menu **owner, int *index, int depth) {
    if (by_pos) {
        if (which >= (uint32_t)m->n) return NULL;
        if (owner) *owner = m;
        if (index) *index = (int)which;
        return &m->v[which];
    }
    for (int i = 0; i < m->n; i++) {
        item *it = &m->v[i];
        if (it->id == which) { if (owner) *owner = m; if (index) *index = i; return it; }   // (a submenu's item too, by its wID)
        menu *s = it->sub ? get(it->sub) : NULL;
        item *r = s && depth < 16 ? find(s, which, 0, owner, index, depth + 1) : NULL;
        if (r) return r;
    }
    return NULL;
}
static item *insert_at(menu *m, int pos) {      // a new zeroed item before pos (past the end: appended)
    if (m->n == m->cap) { m->cap = m->cap ? 2 * m->cap : 8; m->v = realloc(m->v, (size_t)m->cap * sizeof *m->v); }
    if (pos < 0 || pos > m->n) pos = m->n;
    memmove(&m->v[pos + 1], &m->v[pos], (size_t)(m->n - pos) * sizeof *m->v);
    m->n++;
    memset(&m->v[pos], 0, sizeof m->v[pos]);
    return &m->v[pos];
}

// ---- creating and destroying ---------------------------------------------------------------------------------------------------
static WINAPI HANDLE CreateMenu(void) {
    menu *m = calloc(1, sizeof *m);
    pthread_mutex_lock(&g_menu_lock);
    int i = g_menu_next;
    for (int k = 0; k < MAX_MENUS && g_menus[i]; k++) i = (i + 1) % MAX_MENUS;
    HANDLE h = NULL;
    if (!g_menus[i]) { g_menus[i] = m; g_menu_next = (i + 1) % MAX_MENUS; h = (HANDLE)(uintptr_t)(MENU_BASE + i * 4); }
    unlock();
    if (!h) { free(m); vx_set_last_error(8); }
    return h;
}
static WINAPI HANDLE CreatePopupMenu(void) { return CreateMenu(); }
static void destroy(HANDLE h, int depth) {      // (g_menu_lock held) the menu and its submenus
    menu *m = get(h);
    if (!m) return;
    g_menus[((uintptr_t)h - MENU_BASE) / 4] = NULL;
    for (int i = 0; i < m->n; i++) { if (m->v[i].sub && depth < 16) destroy(m->v[i].sub, depth + 1); item_free(&m->v[i]); }
    free(m->v); free(m);
}
static WINAPI WINBOOL DestroyMenu(HANDLE h) {
    if (!lock_menu(h)) return 0;
    destroy(h, 0);
    unlock();
    return 1;
}
static WINAPI WINBOOL IsMenu(HANDLE h) { pthread_mutex_lock(&g_menu_lock); WINBOOL r = get(h) != NULL; unlock(); return r; }

// ---- the MF_* forms: AppendMenu, InsertMenu, ModifyMenu --------------------------------------------------------------------------
static void set_from_flags(item *it, uint32_t flags, uintptr_t id, const void *new_item, int wide) {
    item_free(it);
    it->type = flags & (MFT_TYPE_BITS & ~(MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR));
    it->state = flags & (MF_GRAYED | MF_DISABLED | MF_CHECKED | MF_HILITE | MF_DEFAULT);
    it->sub = flags & MF_POPUP ? (HANDLE)id : NULL;
    it->id = (uint32_t)id;
    if (flags & MF_SEPARATOR) it->type |= MF_SEPARATOR;
    else if (flags & MF_BITMAP) { it->type |= MF_BITMAP; it->bitmap = (HANDLE)new_item; }
    else if (flags & MF_OWNERDRAW) { it->type |= MF_OWNERDRAW; it->data = (uintptr_t)new_item; }
    else it->text = dup_text(new_item, wide);
}
static WINBOOL insert_menu(HANDLE h, uint32_t pos, uint32_t flags, uintptr_t id, const void *new_item, int wide) {
    menu *m = lock_menu(h), *owner = m;
    if (!m) return 0;
    int at = -1;
    if (pos != 0xffffffffu) {
        if (flags & MF_BYPOSITION) at = (int)pos;
        else if (!find(m, pos, 0, &owner, &at, 0)) { unlock(); vx_set_last_error(ERROR_MENU_ITEM_NOT_FOUND); return 0; }
    }
    set_from_flags(insert_at(owner, at), flags, id, new_item, wide);
    unlock();
    return 1;
}
static WINAPI WINBOOL AppendMenuW(HANDLE h, uint32_t flags, uintptr_t id, const WCHAR *s) { return insert_menu(h, 0xffffffffu, flags & ~MF_BYPOSITION, id, s, 1); }
static WINAPI WINBOOL AppendMenuA(HANDLE h, uint32_t flags, uintptr_t id, const char *s) { return insert_menu(h, 0xffffffffu, flags & ~MF_BYPOSITION, id, s, 0); }
static WINAPI WINBOOL InsertMenuW(HANDLE h, uint32_t pos, uint32_t flags, uintptr_t id, const WCHAR *s) { return insert_menu(h, pos, flags, id, s, 1); }
static WINAPI WINBOOL InsertMenuA(HANDLE h, uint32_t pos, uint32_t flags, uintptr_t id, const char *s) { return insert_menu(h, pos, flags, id, s, 0); }
static WINBOOL modify_menu(HANDLE h, uint32_t pos, uint32_t flags, uintptr_t id, const void *new_item, int wide) {
    menu *m = lock_menu(h);
    if (!m) return 0;
    item *it = find(m, pos, (flags & MF_BYPOSITION) != 0, NULL, NULL, 0);
    if (it) set_from_flags(it, flags, id, new_item, wide);
    unlock();
    if (!it) vx_set_last_error(ERROR_MENU_ITEM_NOT_FOUND);
    return it != NULL;
}
static WINAPI WINBOOL ModifyMenuW(HANDLE h, uint32_t pos, uint32_t flags, uintptr_t id, const WCHAR *s) { return modify_menu(h, pos, flags, id, s, 1); }
static WINAPI WINBOOL ModifyMenuA(HANDLE h, uint32_t pos, uint32_t flags, uintptr_t id, const char *s) { return modify_menu(h, pos, flags, id, s, 0); }
static WINBOOL remove_item(HANDLE h, uint32_t pos, uint32_t flags, int destroy_sub) {
    menu *m = lock_menu(h), *owner;
    if (!m) return 0;
    int at;
    item *it = find(m, pos, (flags & MF_BYPOSITION) != 0, &owner, &at, 0);
    if (it) {
        if (destroy_sub && it->sub) destroy(it->sub, 0);
        item_free(it);
        memmove(&owner->v[at], &owner->v[at + 1], (size_t)(owner->n - at - 1) * sizeof *owner->v);
        owner->n--;
    }
    unlock();
    if (!it) vx_set_last_error(ERROR_MENU_ITEM_NOT_FOUND);
    return it != NULL;
}
static WINAPI WINBOOL DeleteMenu(HANDLE h, uint32_t pos, uint32_t flags) { return remove_item(h, pos, flags, 1); }
static WINAPI WINBOOL RemoveMenu(HANDLE h, uint32_t pos, uint32_t flags) { return remove_item(h, pos, flags, 0); }

// ---- states -------------------------------------------------------------------------------------------------------------------------
static WINAPI DWORD CheckMenuItem(HANDLE h, uint32_t which, uint32_t flags) {
    menu *m = lock_menu(h);
    if (!m) return 0xffffffffu;
    item *it = find(m, which, (flags & MF_BYPOSITION) != 0, NULL, NULL, 0);
    DWORD was = it ? it->state & MF_CHECKED : 0xffffffffu;
    if (it) it->state = (it->state & ~MF_CHECKED) | (flags & MF_CHECKED);
    unlock();
    return was;
}
static WINAPI WINBOOL EnableMenuItem(HANDLE h, uint32_t which, uint32_t flags) {
    menu *m = lock_menu(h);
    if (!m) return -1;
    item *it = find(m, which, (flags & MF_BYPOSITION) != 0, NULL, NULL, 0);
    int was = it ? (int)(it->state & (MF_GRAYED | MF_DISABLED)) : -1;
    if (it) it->state = (it->state & ~(MF_GRAYED | MF_DISABLED)) | (flags & (MF_GRAYED | MF_DISABLED));
    unlock();
    return was;
}
static WINAPI WINBOOL CheckMenuRadioItem(HANDLE h, uint32_t first, uint32_t last, uint32_t check, uint32_t flags) {
    menu *m = lock_menu(h), *owner = NULL, *o2 = NULL;
    if (!m) return 0;
    int by_pos = (flags & MF_BYPOSITION) != 0, a, b;
    item *fi = find(m, first, by_pos, &owner, &a, 0), *li = find(m, last, by_pos, &o2, &b, 0);
    WINBOOL ok = fi && li && owner == o2 && a <= b;
    for (int i = a; ok && i <= b; i++) {
        item *it = &owner->v[i];
        int on = by_pos ? (uint32_t)i == check : it->id == check;
        if (on) it->type |= MF_RADIOCHECK;
        it->state = (it->state & ~MF_CHECKED) | (on ? MF_CHECKED : 0);
    }
    unlock();
    if (!ok) vx_set_last_error(ERROR_MENU_ITEM_NOT_FOUND);
    return ok;
}
static WINAPI WINBOOL SetMenuDefaultItem(HANDLE h, uint32_t which, uint32_t by_pos) {
    menu *m = lock_menu(h);
    if (!m) return 0;
    item *target = which == 0xffffffffu ? NULL : find(m, which, by_pos != 0, NULL, NULL, 1000);   // (this menu only)
    for (int i = 0; i < m->n; i++) m->v[i].state &= ~MF_DEFAULT;
    if (target) target->state |= MF_DEFAULT;
    unlock();
    if (which != 0xffffffffu && !target) { vx_set_last_error(ERROR_MENU_ITEM_NOT_FOUND); return 0; }
    return 1;
}
static WINAPI uint32_t GetMenuDefaultItem(HANDLE h, uint32_t by_pos, uint32_t flags) {
    menu *m = lock_menu(h);
    if (!m) return 0xffffffffu;
    uint32_t r = 0xffffffffu;
    for (int i = 0; i < m->n && r == 0xffffffffu; i++)
        if ((m->v[i].state & MF_DEFAULT) && ((flags & 1 /* GMDI_USEDISABLED */) || !(m->v[i].state & (MF_GRAYED | MF_DISABLED))))
            r = by_pos ? (uint32_t)i : m->v[i].id;
    unlock();
    return r;
}

// ---- questions ---------------------------------------------------------------------------------------------------------------------
static WINAPI int GetMenuItemCount(HANDLE h) { menu *m = lock_menu(h); if (!m) return -1; int n = m->n; unlock(); return n; }
static WINAPI uint32_t GetMenuItemID(HANDLE h, int pos) {
    menu *m = lock_menu(h);
    if (!m) return 0xffffffffu;
    uint32_t id = pos >= 0 && pos < m->n && !m->v[pos].sub ? m->v[pos].id : 0xffffffffu;
    unlock();
    return id;
}
static WINAPI HANDLE GetSubMenu(HANDLE h, int pos) {
    menu *m = lock_menu(h);
    if (!m) return NULL;
    HANDLE s = pos >= 0 && pos < m->n ? m->v[pos].sub : NULL;
    unlock();
    return s;
}
static WINAPI uint32_t GetMenuState(HANDLE h, uint32_t which, uint32_t flags) {
    menu *m = lock_menu(h);
    if (!m) return 0xffffffffu;
    item *it = find(m, which, (flags & MF_BYPOSITION) != 0, NULL, NULL, 0);
    uint32_t r = 0xffffffffu;
    if (it) {
        r = (it->type | it->state) & 0xffff;
        menu *s = it->sub ? get(it->sub) : NULL;
        if (s) r = (uint32_t)(s->n & 0xff) << 8 | ((r | MF_POPUP) & 0xff);
    }
    unlock();
    if (!it) vx_set_last_error(ERROR_MENU_ITEM_NOT_FOUND);
    return r;
}
// The item's text, as Windows' GetMenuString: copied up to cap - 1 units; the length when buf is NULL.
static int menu_string(HANDLE h, uint32_t which, void *buf, int cap, uint32_t flags, int wide) {
    menu *m = lock_menu(h);
    if (!m) return 0;
    item *it = find(m, which, (flags & MF_BYPOSITION) != 0, NULL, NULL, 0);
    char *t = it && it->text ? strdup(it->text) : NULL;
    unlock();
    if (!it) { vx_set_last_error(ERROR_MENU_ITEM_NOT_FOUND); return 0; }
    int len = t ? (int)(wide ? vx_utf8_to_utf16(t, strlen(t), NULL, 0) : strlen(t)) : 0;
    if (!buf || cap <= 0) { free(t); return len; }
    int n = len < cap - 1 ? len : cap - 1;
    if (wide) { if (t) vx_utf8_to_utf16(t, strlen(t), buf, (size_t)n); ((WCHAR *)buf)[n] = 0; }
    else { if (t) memcpy(buf, t, (size_t)n); ((char *)buf)[n] = 0; }
    free(t);
    return n;
}
static WINAPI int GetMenuStringW(HANDLE h, uint32_t which, WCHAR *buf, int cap, uint32_t flags) { return menu_string(h, which, buf, cap, flags, 1); }
static WINAPI int GetMenuStringA(HANDLE h, uint32_t which, char *buf, int cap, uint32_t flags) { return menu_string(h, which, buf, cap, flags, 0); }

// ---- MENUITEMINFO: cbSize, fMask, fType, fState, wID, hSubMenu, hbmpChecked, hbmpUnchecked, dwItemData, dwTypeData, cch,
// hbmpItem (80 bytes; 72 without hbmpItem) -----------------------------------------------------------------------------------
typedef struct {
    uint32_t size, mask, type, state, id; HANDLE sub, checked, unchecked; uintptr_t data; void *type_data; uint32_t cch; HANDLE bitmap;
} MENUITEMINFO;
static int is_text(uint32_t type) { return !(type & (MF_BITMAP | MF_OWNERDRAW | MF_SEPARATOR)); }
static void set_from_info(item *it, const MENUITEMINFO *mi, int wide) {
    uint32_t mask = mi->mask;
    if (mask & MIIM_TYPE) {                                       // (the older form: the type and its data together)
        item_free(it);
        it->type = mi->type & MFT_TYPE_BITS;
        if (is_text(it->type)) it->text = dup_text(mi->type_data, wide);
        else if (it->type & MF_BITMAP) it->bitmap = (HANDLE)(uintptr_t)(uint32_t)(uintptr_t)mi->type_data;
    }
    if (mask & MIIM_FTYPE) it->type = mi->type & MFT_TYPE_BITS;
    if (mask & MIIM_STRING) { item_free(it); it->text = dup_text(mi->type_data, wide); }
    if (mask & MIIM_STATE) it->state = mi->state & MFS_STATE_BITS;
    if (mask & MIIM_ID) it->id = mi->id;
    if (mask & MIIM_SUBMENU) it->sub = mi->sub;
    if (mask & MIIM_CHECKMARKS) { it->checked_bmp = mi->checked; it->unchecked_bmp = mi->unchecked; }
    if (mask & MIIM_DATA) it->data = mi->data;
    if ((mask & MIIM_BITMAP) && mi->size >= 80) it->bitmap = mi->bitmap;
}
static WINBOOL item_info(HANDLE h, uint32_t which, WINBOOL by_pos, MENUITEMINFO *mi, int wide, int set) {
    if (!mi || mi->size < 72) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    menu *m = lock_menu(h);
    if (!m) return 0;
    item *it = find(m, which, by_pos != 0, NULL, NULL, 0);
    if (!it) { unlock(); vx_set_last_error(ERROR_MENU_ITEM_NOT_FOUND); return 0; }
    if (set) { set_from_info(it, mi, wide); unlock(); return 1; }
    uint32_t mask = mi->mask;
    if (mask & (MIIM_TYPE | MIIM_FTYPE)) mi->type = it->type;
    if (mask & MIIM_STATE) mi->state = it->state;
    if (mask & MIIM_ID) mi->id = it->id;
    if (mask & MIIM_SUBMENU) mi->sub = it->sub;
    if (mask & MIIM_CHECKMARKS) { mi->checked = it->checked_bmp; mi->unchecked = it->unchecked_bmp; }
    if (mask & MIIM_DATA) mi->data = it->data;
    if ((mask & MIIM_BITMAP) && mi->size >= 80) mi->bitmap = it->bitmap;
    if ((mask & MIIM_TYPE) && !is_text(it->type)) mi->type_data = it->type & MF_BITMAP ? it->bitmap : NULL;
    else if (mask & (MIIM_STRING | MIIM_TYPE)) {
        // The text: its length in cch when dwTypeData is NULL, else copied (cch - 1 units at most) and cch set to it.
        const char *t = it->text ? it->text : "";
        size_t len = wide ? vx_utf8_to_utf16(t, strlen(t), NULL, 0) : strlen(t);
        void *out = mi->type_data ? VX_LOW(mi->type_data) : NULL;
        if (!out || !mi->cch) mi->cch = (uint32_t)len;
        else {
            size_t n = len < mi->cch - 1 ? len : mi->cch - 1;
            if (wide) { vx_utf8_to_utf16(t, strlen(t), out, n); ((WCHAR *)out)[n] = 0; }
            else { memcpy(out, t, n); ((char *)out)[n] = 0; }
            mi->cch = (uint32_t)n;
        }
    }
    unlock();
    return 1;
}
static WINAPI WINBOOL GetMenuItemInfoW(HANDLE h, uint32_t which, WINBOOL by_pos, MENUITEMINFO *mi) { return item_info(h, which, by_pos, mi, 1, 0); }
static WINAPI WINBOOL GetMenuItemInfoA(HANDLE h, uint32_t which, WINBOOL by_pos, MENUITEMINFO *mi) { return item_info(h, which, by_pos, mi, 0, 0); }
static WINAPI WINBOOL SetMenuItemInfoW(HANDLE h, uint32_t which, WINBOOL by_pos, const MENUITEMINFO *mi) { return item_info(h, which, by_pos, (MENUITEMINFO *)mi, 1, 1); }
static WINAPI WINBOOL SetMenuItemInfoA(HANDLE h, uint32_t which, WINBOOL by_pos, const MENUITEMINFO *mi) { return item_info(h, which, by_pos, (MENUITEMINFO *)mi, 0, 1); }
static WINBOOL insert_item(HANDLE h, uint32_t which, WINBOOL by_pos, const MENUITEMINFO *mi, int wide) {
    if (!mi || mi->size < 72) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    menu *m = lock_menu(h), *owner = m;
    if (!m) return 0;
    int at = by_pos ? (int)which : -1;
    if (!by_pos && !find(m, which, 0, &owner, &at, 0)) { owner = m; at = -1; }   // (an unknown command: at the end)
    set_from_info(insert_at(owner, at), mi, wide);
    unlock();
    return 1;
}
static WINAPI WINBOOL InsertMenuItemW(HANDLE h, uint32_t which, WINBOOL by_pos, const MENUITEMINFO *mi) { return insert_item(h, which, by_pos, mi, 1); }
static WINAPI WINBOOL InsertMenuItemA(HANDLE h, uint32_t which, WINBOOL by_pos, const MENUITEMINFO *mi) { return insert_item(h, which, by_pos, mi, 0); }

// MENUINFO: cbSize, fMask, dwStyle, cyMax, hbrBack, dwContextHelpID, dwMenuData (40 bytes).
typedef struct { uint32_t size, mask, style, max_height; HANDLE background; uint32_t help; uintptr_t data; } MENUINFO;
static WINAPI WINBOOL GetMenuInfo(HANDLE h, MENUINFO *mi) {
    if (!mi || mi->size < sizeof *mi) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    menu *m = lock_menu(h);
    if (!m) return 0;
    if (mi->mask & MIM_STYLE) mi->style = m->style;
    if (mi->mask & MIM_MAXHEIGHT) mi->max_height = m->max_height;
    if (mi->mask & MIM_BACKGROUND) mi->background = m->background;
    if (mi->mask & MIM_HELPID) mi->help = m->help;
    if (mi->mask & MIM_MENUDATA) mi->data = m->menu_data;
    unlock();
    return 1;
}
static WINAPI WINBOOL SetMenuInfo(HANDLE h, const MENUINFO *mi) {
    if (!mi || mi->size < sizeof *mi) { vx_set_last_error(ERROR_INVALID_PARAMETER); return 0; }
    menu *m = lock_menu(h);
    if (!m) return 0;
    if (mi->mask & MIM_STYLE) m->style = mi->style;
    if (mi->mask & MIM_MAXHEIGHT) m->max_height = mi->max_height;
    if (mi->mask & MIM_BACKGROUND) m->background = mi->background;
    if (mi->mask & MIM_HELPID) m->help = mi->help;
    if (mi->mask & MIM_MENUDATA) m->menu_data = mi->data;
    unlock();
    return 1;
}

// ---- TrackPopupMenu ----------------------------------------------------------------------------------------------------------------
// What is shown: a copy of the menu taken after WM_INITMENUPOPUP (the program may change it meanwhile), and every item
// that can be chosen in a flat list (the NSMenuItem's tag is its index there).
typedef struct snap snap;
typedef struct { char *title; uint32_t type, state, id; int pick; snap *sub; uint32_t *px; int pw, ph; } snap_item;
struct snap { HANDLE menu; int n; snap_item *v; };
typedef struct { HANDLE menu; int index; uint32_t id; } choice;
typedef struct {
    snap *top;
    choice *picks; int npicks;
    HANDLE hwnd; uint32_t flags; int x, y;
    HANDLE done;                                // an event: the menu has closed
    int chosen;                                 // index into picks, -1 for none
    void *nsmenu;                               // the NSMenu while it is open (main thread)
    int serial;                                 // which TrackPopupMenu (g_finished counts those that have returned)
} track;

// Text as a string: UTF-8 (the ANSI code page here), else read as Windows-1252 (an A string from elsewhere); never nil.
static NSString *ns_text(const char *s) {
    if (!s) return @"";
    return [NSString stringWithUTF8String:s] ?: [[NSString alloc] initWithBytes:s length:strlen(s) encoding:NSWindowsCP1252StringEncoding] ?: @"";
}
// The title AppKit shows: '&' marks the mnemonic ("&&" is one '&'), and what follows a tab is the shortcut's name.
static char *menu_title(const char *s) {
    char *out = malloc(strlen(s) + 1), *o = out;
    for (const char *c = s; *c && *c != '\t' && *c != '\b'; c++) {
        if (*c == '&') { if (c[1] == '&') *o++ = *c++; continue; }
        *o++ = *c;
    }
    *o = 0;
    return out;
}
static snap *take_snap(HANDLE h, track *t, int depth) {        // (g_menu_lock held)
    menu *m = get(h);
    snap *s = calloc(1, sizeof *s);
    s->menu = h;
    if (!m) return s;
    s->v = calloc((size_t)(m->n ? m->n : 1), sizeof *s->v);
    s->n = m->n;
    for (int i = 0; i < m->n; i++) {
        item *it = &m->v[i];
        snap_item *si = &s->v[i];
        si->type = it->type; si->state = it->state; si->id = it->id; si->pick = -1;
        si->title = it->text ? menu_title(it->text) : NULL;
        if (it->bitmap && (uintptr_t)it->bitmap > 11) {           // (HBMMENU_* 1-11: the system's pictures, not drawn)
            int bpp;
            si->px = vx_gdi_bitmap_bgra(it->bitmap, &si->pw, &si->ph, &bpp);
        }
        if (it->sub && depth < 16) si->sub = take_snap(it->sub, t, depth + 1);
        else if (!(it->type & MF_SEPARATOR)) {
            t->picks = realloc(t->picks, (size_t)(t->npicks + 1) * sizeof *t->picks);
            t->picks[t->npicks] = (choice){ h, i, it->id };
            si->pick = t->npicks++;
        }
    }
    return s;
}
static void free_snap(snap *s) {
    if (!s) return;
    for (int i = 0; i < s->n; i++) { free(s->v[i].title); free(s->v[i].px); free_snap(s->v[i].sub); }
    free(s->v); free(s);
}

@interface VXMenuTarget : NSObject { @public track *t; }
- (void)picked:(NSMenuItem *)item;
@end
@implementation VXMenuTarget
- (void)picked:(NSMenuItem *)item { t->chosen = (int)item.tag; }
@end

static NSImage *item_image(const snap_item *si) {
    if (!si->px || si->pw <= 0 || si->ph <= 0) return nil;
    int any_alpha = 0;
    for (size_t i = 0; i < (size_t)si->pw * si->ph; i++) any_alpha |= (si->px[i] >> 24) != 0;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef c = CGBitmapContextCreate(si->px, (size_t)si->pw, (size_t)si->ph, 8, (size_t)si->pw * 4, cs,
                                           (any_alpha ? kCGImageAlphaPremultipliedFirst : kCGImageAlphaNoneSkipFirst) | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    CGImageRef img = c ? CGBitmapContextCreateImage(c) : NULL;   // (menu bitmaps with alpha are premultiplied)
    if (c) CGContextRelease(c);
    if (!img) return nil;
    NSImage *r = [[NSImage alloc] initWithCGImage:img size:NSMakeSize(si->pw, si->ph)];
    CGImageRelease(img);
    return r;
}
static NSMenu *build(const snap *s, VXMenuTarget *target) {
    NSMenu *m = [[NSMenu alloc] initWithTitle:@""];
    m.autoenablesItems = NO;
    static int told_owner_draw;
    for (int i = 0; i < s->n; i++) {
        const snap_item *si = &s->v[i];
        if (si->type & MF_SEPARATOR) { [m addItem:[NSMenuItem separatorItem]]; continue; }
        if ((si->type & MF_OWNERDRAW) && !si->title && !told_owner_draw++) vx_log("menu: owner-drawn items are shown as plain items (no WM_DRAWITEM)");
        NSString *title = si->title ? ns_text(si->title) : nil;
        NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title ?: @"" action:NULL keyEquivalent:@""];
        it.image = item_image(si);
        if (!title.length && !it.image) it.title = @"•";
        if (si->state & MF_DEFAULT) it.attributedTitle = [[NSAttributedString alloc] initWithString:it.title attributes:@{ NSFontAttributeName: [NSFont boldSystemFontOfSize:0] }];
        it.state = si->state & MF_CHECKED ? NSControlStateValueOn : NSControlStateValueOff;
        it.enabled = !(si->state & (MF_GRAYED | MF_DISABLED));
        if (si->sub) it.submenu = build(si->sub, target);
        else { it.target = target; it.action = @selector(picked:); it.tag = si->pick; }
        [m addItem:it];
    }
    return m;
}

static pthread_mutex_t g_open_lock = PTHREAD_MUTEX_INITIALIZER;
static track *g_open;                           // the menu being shown (for Command Clips), under g_open_lock
static int g_tracking;                          // a TrackPopupMenu is running (Windows allows one)
static int g_started, g_finished;               // TrackPopupMenu calls begun / returned to the program

// Main thread: shows the menu and returns when it has closed.
static void show(track *t) {
    VXMenuTarget *target = [VXMenuTarget new];
    target->t = t;
    NSMenu *m = build(t->top, target);
    NSSize size = m.size;
    CGFloat screen_h = NSScreen.screens.count ? NSScreen.screens[0].frame.size.height : 1080;
    NSPoint p = NSMakePoint(t->x, screen_h - t->y);              // the menu's top left corner, in Cocoa's coordinates
    if (t->flags & TPM_CENTERALIGN) p.x -= size.width / 2; else if (t->flags & TPM_RIGHTALIGN) p.x -= size.width;
    if (t->flags & TPM_VCENTERALIGN) p.y += size.height / 2; else if (t->flags & TPM_BOTTOMALIGN) p.y += size.height;
    // AppKit moves a menu that would cross a screen's edge, but shows none at a point outside every screen: such a
    // point comes into the main screen (Windows keeps menus in the work area too).
    int on_screen = 0;
    for (NSScreen *s in NSScreen.screens) on_screen |= NSPointInRect(NSMakePoint(p.x + 1, p.y - 1), s.frame);
    if (!on_screen && NSScreen.screens.count) {
        NSRect f = NSScreen.screens[0].visibleFrame;
        p.x = MAX(NSMinX(f), MIN(p.x, NSMaxX(f) - size.width));
        p.y = MAX(NSMinY(f) + size.height, MIN(p.y, NSMaxY(f)));
    }
    pthread_mutex_lock(&g_open_lock);
    t->nsmenu = (__bridge void *)m;
    g_open = t;
    pthread_mutex_unlock(&g_open_lock);
    [m popUpMenuPositioningItem:nil atLocation:p inView:nil];    // (AppKit keeps it on the screen)
    pthread_mutex_lock(&g_open_lock);
    g_open = NULL;
    t->nsmenu = NULL;
    pthread_mutex_unlock(&g_open_lock);
}
static void on_main_async(void (^b)(void)) {
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, b);   // (runs during menu tracking and modal panels too)
    CFRunLoopWakeUp(CFRunLoopGetMain());
}
static void cancel(track *t) {
    on_main_async(^{
        pthread_mutex_lock(&g_open_lock);
        NSMenu *m = g_open == t ? (__bridge NSMenu *)t->nsmenu : nil;
        pthread_mutex_unlock(&g_open_lock);
        [m cancelTrackingWithoutAnimation];     // (with the fade, it would close a quarter of a second later)
    });
}

typedef struct { HANDLE hwnd; uint32_t message; WPARAM wparam; LPARAM lparam; DWORD time; POINT pt; DWORD priv; } MSG;
// Waits for the menu to close, handling this thread's messages meanwhile as Windows' menu loop does. A WM_QUIT ends
// the menu and is posted again afterwards.
static void wait_closed(track *t) {
    static DWORD (WINAPI *msg_wait)(DWORD, const HANDLE *, DWORD, DWORD, DWORD);
    static WINBOOL (WINAPI *peek)(MSG *, HANDLE, uint32_t, uint32_t, uint32_t);
    static WINBOOL (WINAPI *translate)(const MSG *);
    static LRESULT (WINAPI *dispatch)(const MSG *);
    static void (WINAPI *post_quit)(int);
    static DWORD (WINAPI *wait_one)(HANDLE, DWORD);
    if (!msg_wait) {
        peek = (WINBOOL (WINAPI *)(MSG *, HANDLE, uint32_t, uint32_t, uint32_t))vx_find_export("user32.dll", "PeekMessageW");
        translate = (WINBOOL (WINAPI *)(const MSG *))vx_find_export("user32.dll", "TranslateMessage");
        dispatch = (LRESULT (WINAPI *)(const MSG *))vx_find_export("user32.dll", "DispatchMessageW");
        post_quit = (void (WINAPI *)(int))vx_find_export("user32.dll", "PostQuitMessage");
        wait_one = (DWORD (WINAPI *)(HANDLE, DWORD))vx_find_export("kernel32.dll", "WaitForSingleObject");
        msg_wait = (DWORD (WINAPI *)(DWORD, const HANDLE *, DWORD, DWORD, DWORD))vx_find_export("user32.dll", "MsgWaitForMultipleObjectsEx");
    }
    int quit = 0; WPARAM quit_code = 0;
    while (!quit) {
        DWORD r = msg_wait(1, &t->done, 0xffffffffu, 0x4ff /* QS_ALLINPUT */, 0);
        if (r != 1) break;                                        // the menu closed (or the wait failed: see below)
        MSG m;
        while (peek(&m, NULL, 0, 0, 1 /* PM_REMOVE */)) {
            if (m.message == WM_QUIT) { quit = 1; quit_code = m.wparam; cancel(t); break; }
            translate(&m);
            dispatch(&m);
        }
    }
    wait_one(t->done, 0xffffffffu);                               // (t is the main thread's until the menu has closed)
    if (quit) post_quit((int)quit_code);
}

// WM_INITMENUPOPUP for the menu and each submenu (Windows sends it as each opens; AppKit's menu is built beforehand).
static void init_popups(HANDLE hwnd, HANDLE h, int index, int depth, HANDLE *sent, int *nsent) {
    if (*nsent < 64) sent[(*nsent)++] = h;
    vx_send_message(hwnd, WM_INITMENUPOPUP, (WPARAM)(uintptr_t)h, MAKELPARAM(index, 0));
    HANDLE subs[64]; int idx[64], n = 0;
    pthread_mutex_lock(&g_menu_lock);
    menu *m = get(h);
    for (int i = 0; m && i < m->n && n < 64; i++) if (m->v[i].sub) { subs[n] = m->v[i].sub; idx[n++] = i; }
    unlock();
    for (int i = 0; i < n && depth < 16; i++) init_popups(hwnd, subs[i], idx[i], depth + 1, sent, nsent);
}

static WINBOOL track_popup(HANDLE h, uint32_t flags, int x, int y, HANDLE hwnd) {
    if (!IsMenu(h)) { vx_set_last_error(ERROR_INVALID_MENU_HANDLE); return 0; }
    if (!vx_window_get(hwnd)) { vx_set_last_error(ERROR_INVALID_WINDOW_HANDLE); return 0; }
    if (__atomic_exchange_n(&g_tracking, 1, __ATOMIC_ACQ_REL)) { vx_set_last_error(ERROR_POPUP_ALREADY_ACTIVE); return 0; }
    int notify = !(flags & TPM_NONOTIFY);
    HANDLE sent[64]; int nsent = 0;
    if (notify) {
        vx_send_message(hwnd, WM_ENTERMENULOOP, 1, 0);
        init_popups(hwnd, h, 0, 0, sent, &nsent);
    }
    static HANDLE (WINAPI *create_event)(void *, WINBOOL, WINBOOL, const WCHAR *);
    static WINBOOL (WINAPI *close_handle)(HANDLE);
    if (!create_event) {
        close_handle = (WINBOOL (WINAPI *)(HANDLE))vx_find_export("kernel32.dll", "CloseHandle");
        create_event = (HANDLE (WINAPI *)(void *, WINBOOL, WINBOOL, const WCHAR *))vx_find_export("kernel32.dll", "CreateEventW");
    }
    track *t = calloc(1, sizeof *t);
    t->hwnd = hwnd; t->flags = flags; t->x = x; t->y = y; t->chosen = -1;
    t->serial = __atomic_add_fetch(&g_started, 1, __ATOMIC_ACQ_REL);
    t->done = create_event(NULL, 1, 0, NULL);
    pthread_mutex_lock(&g_menu_lock);
    t->top = take_snap(h, t, 0);
    uint32_t style = get(h) ? get(h)->style : 0;
    unlock();
    vx_cocoa_ensure();
    HANDLE done = t->done;
    // In the default mode only: a menu opened inside other tracking (the status button's, which runs on for a moment
    // after its action) closes at once.
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopDefaultMode, ^{ @autoreleasepool { show(t); } vx_set_event(done); });
    CFRunLoopWakeUp(CFRunLoopGetMain());
    wait_closed(t);
    close_handle(t->done);
    if (notify) {
        for (int i = nsent - 1; i >= 0; i--) vx_send_message(hwnd, WM_UNINITMENUPOPUP, (WPARAM)(uintptr_t)sent[i], 0);
        vx_send_message(hwnd, WM_EXITMENULOOP, 1, 0);
    }
    choice c = t->chosen >= 0 && t->chosen < t->npicks ? t->picks[t->chosen] : (choice){ NULL, -1, 0 };
    free_snap(t->top); free(t->picks); free(t);
    WINBOOL r = 1;
    if (flags & TPM_RETURNCMD) r = c.menu ? (WINBOOL)c.id : 0;
    else if (c.menu && notify) {
        if (style & MNS_NOTIFYBYPOS) vx_window_post(hwnd, WM_MENUCOMMAND, (WPARAM)c.index, (LPARAM)(uintptr_t)c.menu);
        else vx_window_post(hwnd, WM_COMMAND, c.id & 0xffff, 0);
    }
    __atomic_store_n(&g_tracking, 0, __ATOMIC_RELEASE);
    __atomic_add_fetch(&g_finished, 1, __ATOMIC_ACQ_REL);
    return r;
}
static WINAPI WINBOOL TrackPopupMenu(HANDLE h, uint32_t flags, int x, int y, int reserved, HANDLE hwnd, const RECT *rect) {
    (void)reserved; (void)rect;
    return track_popup(h, flags, x, y, hwnd);
}
static WINAPI WINBOOL TrackPopupMenuEx(HANDLE h, uint32_t flags, int x, int y, HANDLE hwnd, const void *params) {
    (void)params;                               // (TPMPARAMS' rectangle to keep clear: AppKit places the menu)
    return track_popup(h, flags, x, y, hwnd);
}
static WINAPI WINBOOL EndMenu(void) {
    pthread_mutex_lock(&g_open_lock);
    track *t = g_open;
    pthread_mutex_unlock(&g_open_lock);
    if (t) cancel(t);
    return 1;
}

// ---- Command Clips (control.c) ---------------------------------------------------------------------------------------------------------
static NSArray *snap_json(const snap *s) {
    NSMutableArray *a = [NSMutableArray array];
    for (int i = 0; i < s->n; i++) {
        const snap_item *si = &s->v[i];
        NSMutableDictionary *d = [@{ @"index": @(i), @"id": @(si->id), @"text": ns_text(si->title),
            @"separator": @((si->type & MF_SEPARATOR) != 0), @"enabled": @(!(si->state & (MF_GRAYED | MF_DISABLED))),
            @"checked": @((si->state & MF_CHECKED) != 0), @"default": @((si->state & MF_DEFAULT) != 0) } mutableCopy];
        if (si->sub) d[@"items"] = snap_json(si->sub);
        [a addObject:d];
    }
    return a;
}
static char *json(id obj) {
    NSData *d = [NSJSONSerialization dataWithJSONObject:obj options:0 error:nil];
    char *s = malloc(d.length + 1);
    memcpy(s, d.bytes, d.length); s[d.length] = 0;
    return s;
}
char *vx_menu_state(void) {
    @autoreleasepool {
        pthread_mutex_lock(&g_open_lock);
        NSDictionary *d = g_open ? @{ @"ok": @YES, @"open": @YES, @"hwnd": @((long long)(intptr_t)g_open->hwnd), @"items": snap_json(g_open->top) }
                                 : @{ @"ok": @YES, @"open": @NO };
        pthread_mutex_unlock(&g_open_lock);
        return json(d);
    }
}
static int find_pick(const snap *s, const char *text) {         // the first enabled item whose title contains text, or "#id"
    for (int i = 0; i < s->n; i++) {
        const snap_item *si = &s->v[i];
        if (si->sub) { int r = find_pick(si->sub, text); if (r >= 0) return r; continue; }
        if (si->pick < 0 || (si->state & (MF_GRAYED | MF_DISABLED))) continue;
        if (text[0] == '#' ? si->id == (uint32_t)strtoul(text + 1, NULL, 0) : si->title && strstr(si->title, text)) return si->pick;
    }
    return -1;
}
// Chooses an item of the menu that is open (waiting up to wait_ms for one to open), as a click on it would; NULL text
// closes the menu without a choice. Answers once TrackPopupMenu has returned to the program with the choice, so that
// whatever is asked next sees the program after it (wait-idle then waits for what the program does with it).
int vx_menu_pick(const char *text, int wait_ms, char *why, size_t cap) {
    for (int waited = 0;; waited += 5) {
        pthread_mutex_lock(&g_open_lock);
        track *t = g_open;
        if (t && t->nsmenu) {
            int pick = text ? find_pick(t->top, text) : -1;
            if (text && pick < 0) { pthread_mutex_unlock(&g_open_lock); snprintf(why, cap, "no enabled item of the open menu has that text"); return 0; }
            t->chosen = pick;
            int serial = t->serial;
            pthread_mutex_unlock(&g_open_lock);
            cancel(t);
            for (int w = 0; __atomic_load_n(&g_finished, __ATOMIC_ACQUIRE) < serial && w < 5000; w += 2) usleep(2000);
            return 1;
        }
        pthread_mutex_unlock(&g_open_lock);
        if (waited >= wait_ms) { snprintf(why, cap, "no popup menu is open"); return 0; }
        usleep(5000);
    }
}

static const vx_export user32[] = {
    VX_EXPORT(CreateMenu), VX_EXPORT(CreatePopupMenu), VX_EXPORT(DestroyMenu), VX_EXPORT(IsMenu),
    VX_EXPORT(AppendMenuA), VX_EXPORT(AppendMenuW), VX_EXPORT(InsertMenuA), VX_EXPORT(InsertMenuW), VX_EXPORT(ModifyMenuA), VX_EXPORT(ModifyMenuW),
    VX_EXPORT(DeleteMenu), VX_EXPORT(RemoveMenu), VX_EXPORT(CheckMenuItem), VX_EXPORT(EnableMenuItem), VX_EXPORT(CheckMenuRadioItem),
    VX_EXPORT(SetMenuDefaultItem), VX_EXPORT(GetMenuDefaultItem), VX_EXPORT(GetMenuItemCount), VX_EXPORT(GetMenuItemID),
    VX_EXPORT(GetSubMenu), VX_EXPORT(GetMenuState), VX_EXPORT(GetMenuStringA), VX_EXPORT(GetMenuStringW),
    VX_EXPORT(GetMenuItemInfoA), VX_EXPORT(GetMenuItemInfoW), VX_EXPORT(SetMenuItemInfoA), VX_EXPORT(SetMenuItemInfoW),
    VX_EXPORT(InsertMenuItemA), VX_EXPORT(InsertMenuItemW), VX_EXPORT(GetMenuInfo), VX_EXPORT(SetMenuInfo),
    VX_EXPORT(TrackPopupMenu), VX_EXPORT(TrackPopupMenuEx), VX_EXPORT(EndMenu),
};
void vx_menus_init(void) { vx_register_module("user32.dll", user32, sizeof user32 / sizeof user32[0]); }

#ifdef __aarch64__
#include "thunks/menus.m.inc"
#endif
