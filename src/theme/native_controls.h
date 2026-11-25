#pragma once
#include <stdbool.h>
#include <windows.h>
#include <imm.h>
#include <wchar.h>

void w_native_controls_theme(HWND parent, bool contrast, COLORREF background);
void w_native_controls_font_changed(HWND parent);
// For report lists with a dedicated LVSIL_SMALL slot and no LVS_SHAREIMAGELISTS.
// Windows owns the attached sizing image list and frees it with the control.
bool w_native_list_row_height(HWND list,int height);
bool w_native_edit_ime_notified(HWND edit);
void w_native_tooltip_text(HWND tip,const wchar_t *text);

// Candidate windows may remain open after the composition string is empty.
static inline bool w_native_edit_ime_active(HWND edit) {
    if (w_native_edit_ime_notified(edit)) return true;
    wchar_t kind[16];
    if (!GetClassNameW(edit,kind,16) || _wcsicmp(kind,L"Edit")) return false;
    HIMC context=ImmGetContext(edit);
    if (!context) return false;
    DWORD lists=0;
    bool active=ImmGetCompositionStringW(context,GCS_COMPSTR,NULL,0)>0 ||
                (ImmGetCandidateListCountW(context,&lists)>0 && lists);
    ImmReleaseContext(edit,context);
    return active;
}

static inline bool w_native_ime_key(const MSG *key) {
    if (key->message!=WM_KEYDOWN ||
        (key->wParam!=VK_RETURN && key->wParam!=VK_ESCAPE && key->wParam!=VK_TAB &&
         (key->wParam<VK_PRIOR || key->wParam>VK_DOWN))) return false;
    return w_native_edit_ime_active(key->hwnd);
}
