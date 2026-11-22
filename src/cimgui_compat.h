/*
 * cimgui_compat.h
 *
 * cimgui's generated API uses the compact `ig*` prefix (igBegin, igButton,
 * ...).  The project keeps the explicit `ImGui_*` C spelling in its UI code,
 * which reads closer to the C++ API while remaining a pure C API.  Each name
 * below is a macro for the corresponding cimgui function - there is no C++
 * wrapper, no name mangling and no extra runtime cost.
 */
#ifndef CIMGUI_COMPAT_H
#define CIMGUI_COMPAT_H

/* cimgui only exposes its enums and structs in C when this is defined. */
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#ifndef CIMGUI_NO_EXPORT
#define CIMGUI_NO_EXPORT
#endif

#include "cimgui.h"

#define ImGui_CreateContext            igCreateContext
#define ImGui_DestroyContext           igDestroyContext
#define ImGui_GetIO                    igGetIO_Nil
#define ImGui_NewFrame                 igNewFrame
#define ImGui_Render                   igRender
#define ImGui_GetDrawData              igGetDrawData
#define ImGui_StyleColorsDark          igStyleColorsDark
#define ImGui_GetStyle                 igGetStyle

#define ImGui_Begin                    igBegin
#define ImGui_End                      igEnd
#define ImGui_SetNextWindowPos         igSetNextWindowPos
#define ImGui_SetNextWindowSize        igSetNextWindowSize
#define ImGui_GetContentRegionAvail    igGetContentRegionAvail
#define ImGui_GetCursorPosX            igGetCursorPosX
#define ImGui_SetCursorPosX            igSetCursorPosX
#define ImGui_SameLine                 igSameLine
#define ImGui_Separator                igSeparator
#define ImGui_Spacing                  igSpacing
#define ImGui_BeginDisabled            igBeginDisabled
#define ImGui_EndDisabled              igEndDisabled
#define ImGui_GetFrameHeight           igGetFrameHeight
#define ImGui_GetFrameHeightWithSpacing igGetFrameHeightWithSpacing
#define ImGui_GetTextLineHeightWithSpacing igGetTextLineHeightWithSpacing
#define ImGui_CalcTextSize             igCalcTextSize
#define ImGui_SetNextItemWidth         igSetNextItemWidth

#define ImGui_TextUnformatted          igTextUnformatted
#define ImGui_TextDisabled             igTextDisabled
#define ImGui_TextWrapped              igTextWrapped
#define ImGui_TextColored              igTextColored
#define ImGui_Button                   igButton
#define ImGui_Selectable               igSelectable_Bool
#define ImGui_IsItemHovered            igIsItemHovered
#define ImGui_SetTooltip               igSetTooltip

#define ImGui_InputText                igInputText
#define ImGui_InputTextWithHint        igInputTextWithHint
#define ImGui_InputTextMultiline       igInputTextMultiline

#define ImGui_BeginListBox             igBeginListBox
#define ImGui_EndListBox               igEndListBox
#define ImGui_BeginTable               igBeginTable
#define ImGui_EndTable                 igEndTable
#define ImGui_TableSetupColumn         igTableSetupColumn
#define ImGui_TableNextRow             igTableNextRow
#define ImGui_TableSetColumnIndex      igTableSetColumnIndex

#define ImGui_BeginPopupModal          igBeginPopupModal
#define ImGui_EndPopup                 igEndPopup
#define ImGui_OpenPopup                igOpenPopup_Str
#define ImGui_CloseCurrentPopup        igCloseCurrentPopup
#define ImGui_IsWindowAppearing        igIsWindowAppearing
#define ImGui_SetKeyboardFocusHere     igSetKeyboardFocusHere
#define ImGui_SetItemDefaultFocus      igSetItemDefaultFocus

#define ImGui_PushStyleColor           igPushStyleColor_Vec4
#define ImGui_PopStyleColor            igPopStyleColor
#define ImGui_SetClipboardText         igSetClipboardText

static inline ImVec2 ImVec2_Make(float x, float y)
{
    ImVec2 value;
    value.x = x;
    value.y = y;
    return value;
}

static inline ImVec4 ImVec4_Make(float x, float y, float z, float w)
{
    ImVec4 value;
    value.x = x;
    value.y = y;
    value.z = z;
    value.w = w;
    return value;
}

#endif /* CIMGUI_COMPAT_H */
