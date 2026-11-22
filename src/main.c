/*
 * Snippet Manager - a small, self-contained code-snippet library.
 *
 * Stack: C11 + cimgui (Dear ImGui C API) + SDL2/OpenGL3 + SQLite FTS5.
 *
 * Keyboard/mouse flow:
 *   - Type in the search box: the list is filtered immediately by a
 *     case-insensitive substring match over name + tags.
 *   - New: opens a modal editor, saves to SQLite and selects the new row.
 *   - Delete: asks for confirmation, then deletes the selected row.
 *   - Click a list row: loads and displays its description and full body.
 *   - Copy code: puts the body on the system clipboard.
 *   - Edit: reuses the modal editor to update name/tags/description/body.
 */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_opengl.h>

#include <float.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "cimgui_compat.h"
#include "snippet_db.h"

/* ------------------------------------------------------------------------- */
/* Backend entry points (compiled as C++ with IMGUI_IMPL_API=extern "C").     */
/* ------------------------------------------------------------------------- */
extern bool ImGui_ImplSDL2_InitForOpenGL(SDL_Window *window,
                                         void *sdl_gl_context);
extern void ImGui_ImplSDL2_Shutdown(void);
extern void ImGui_ImplSDL2_NewFrame(void);
extern bool ImGui_ImplSDL2_ProcessEvent(const SDL_Event *event);
extern bool ImGui_ImplOpenGL3_Init(const char *glsl_version);
extern void ImGui_ImplOpenGL3_Shutdown(void);
extern void ImGui_ImplOpenGL3_NewFrame(void);
extern void ImGui_ImplOpenGL3_RenderDrawData(ImDrawData *draw_data);

/* ------------------------------------------------------------------------- */
/* Constants and application state                                           */
/* ------------------------------------------------------------------------- */
#define APP_SEARCH_CAP 256
#define APP_NAME_CAP   160
#define APP_TAGS_CAP   320
#define APP_DESC_CAP   4096
#define APP_BODY_CAP   (1024u * 1024u)

typedef struct UiStrings {
    const char *search_hint;
    const char *new_button;
    const char *delete_button;
    const char *snippet_count;
    const char *no_matches;
    const char *no_snippets;
    const char *select_prompt;
    const char *copy_button;
    const char *edit_button;
    const char *no_tags;
    const char *ready;
    const char *loaded_status;
    const char *copied_status;
    const char *saved_status;
    const char *updated_status;
    const char *deleted_status;
    const char *editor_title;
    const char *editor_new;
    const char *editor_edit;
    const char *name_label;
    const char *tags_label;
    const char *description_label;
    const char *code_label;
    const char *save_button;
    const char *cancel_button;
    const char *editor_hint;
    const char *name_required;
    const char *code_required;
    const char *too_large;
    const char *delete_title;
    const char *delete_question;
    const char *delete_warning;
    const char *delete_confirm;
    const char *composing;
} UiStrings;

static const UiStrings UI_EN = {
    "Search name or tags...",
    "New",
    "Delete",
    "%lu snippet(s)",
    "No matches.",
    "No snippets yet. Click \"New\" to create one.",
    "Select a snippet on the left to view its code.",
    "Copy code",
    "Edit",
    "(no tags)",
    "Ready.",
    "Loaded %lu snippet(s).",
    "Copied %lu characters to the clipboard.",
    "Snippet #%lld saved.",
    "Snippet #%lld updated.",
    "Snippet deleted.",
    "Snippet editor",
    "New snippet",
    "Edit snippet",
    "Name",
    "Tags (comma separated)",
    "Description (optional)",
    "Code",
    "Save",
    "Cancel",
    "Name and code are required. Tags are optional.",
    "Name is required.",
    "Code is required.",
    "This snippet is too large to edit in the GUI.",
    "Delete snippet",
    "Delete \"%s\"?",
    "This action cannot be undone.",
    "Delete",
    "Composing: %s",
};

static const UiStrings UI_ZH = {
    "搜索名称或标签...",
    "新建",
    "删除",
    "%lu 个片段",
    "没有匹配的片段。",
    "还没有片段，点击“新建”创建。",
    "在左侧选择一个片段查看正文。",
    "复制正文",
    "编辑",
    "（无标签）",
    "就绪。",
    "已加载 %lu 个片段。",
    "已复制 %lu 个字符到剪贴板。",
    "片段 #%lld 已保存。",
    "片段 #%lld 已更新。",
    "片段已删除。",
    "片段编辑器",
    "新建片段",
    "编辑片段",
    "名称",
    "标签（逗号分隔）",
    "说明（可选）",
    "正文",
    "保存",
    "取消",
    "名称和正文为必填项，标签可选。",
    "名称不能为空。",
    "正文不能为空。",
    "此片段过大，无法在界面中编辑。",
    "删除片段",
    "确定删除“%s”吗？",
    "此操作不可撤销。",
    "删除",
    "输入中: %s",
};

typedef struct App {
    SnippetDB db;
    SnippetList list;
    Snippet selected;

    char search[APP_SEARCH_CAP];
    char status[768];
    bool status_error;

    /* IME preedit text (e.g. "ni'hao") delivered by SDL_TEXTEDITING.  ImGui
     * does not render the composition inline, so we show it explicitly. */
    char ime_preedit[256];
    bool ime_composing;

    /* Modal editor.  The body is large, so App itself is heap allocated. */
    bool editor_is_edit;
    sqlite3_int64 editor_id;
    bool editor_request_open;
    char form_name[APP_NAME_CAP];
    char form_tags[APP_TAGS_CAP];
    char form_description[APP_DESC_CAP];
    char form_body[APP_BODY_CAP];
    char form_error[768];

    /* Deletion confirmation. */
    char delete_error[768];

    const UiStrings *ui;
    char editor_popup[128];
    char delete_popup[128];
} App;

typedef struct GlWindow {
    SDL_Window *window;
    SDL_GLContext context;
    const char *glsl_version;
} GlWindow;

/* ------------------------------------------------------------------------- */
/* Small helpers                                                             */
/* ------------------------------------------------------------------------- */
static void copy_text(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    snprintf(dst, cap, "%s", src ? src : "");
}

static char *trim_in_place(char *text)
{
    char *start;
    char *end;

    if (!text) {
        return text;
    }
    start = text;
    while (*start && (unsigned char)*start <= 0x20) {
        ++start;
    }
    end = start + strlen(start);
    while (end > start && (unsigned char)end[-1] <= 0x20) {
        --end;
    }
    *end = '\0';
    return start;
}

static bool file_is_readable(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return false;
    }
    fclose(file);
    return true;
}

static void app_set_status(App *app, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(app->status, sizeof(app->status), fmt, args);
    va_end(args);
    app->status_error = false;
}

static void app_set_error(App *app, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vsnprintf(app->status, sizeof(app->status), fmt, args);
    va_end(args);
    app->status_error = true;
    fprintf(stderr, "[snippet-gui] %s\n", app->status);
}

static void set_form_error(App *app, const char *message)
{
    copy_text(app->form_error, sizeof(app->form_error), message);
}

static void app_set_ui(App *app, const UiStrings *ui)
{
    app->ui = ui;
    snprintf(app->editor_popup, sizeof(app->editor_popup),
             "%s###snippet_editor", ui->editor_title);
    snprintf(app->delete_popup, sizeof(app->delete_popup),
             "%s###snippet_delete", ui->delete_title);
}

/*
 * Install a CJK-capable font when one is available.  ImGui 1.92+ rasterizes
 * glyphs on demand, so no glyph-range table is required.  Keeping the default
 * font when no CJK font exists at least leaves the UI usable in English.
 */
static bool load_ui_font(ImFontAtlas *atlas)
{
    static const char *candidates[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\msyh.ttf",
        "C:\\Windows\\Fonts\\simhei.ttf",
        "C:\\Windows\\Fonts\\simsun.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJKsc-Regular.otf",
        "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
        "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
        "/System/Library/Fonts/PingFang.ttc",
        "/System/Library/Fonts/Hiragino Sans GB.ttc",
    };
    size_t i;

    if (!atlas) {
        return false;
    }

    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        ImFont *font;

        if (!file_is_readable(candidates[i])) {
            continue;
        }
        font = ImFontAtlas_AddFontFromFileTTF(atlas, candidates[i], 16.0f, NULL,
                                              NULL);
        if (font) {
            fprintf(stderr, "[snippet-gui] UI font: %s\n", candidates[i]);
            return true;
        }
        fprintf(stderr, "[snippet-gui] warning: could not load font: %s\n",
                candidates[i]);
    }

    fprintf(stderr,
            "[snippet-gui] no CJK font found; using the built-in font and an "
            "English UI.\n");
    return false;
}

/* ------------------------------------------------------------------------- */
/* Model operations                                                          */
/* ------------------------------------------------------------------------- */
static bool app_refresh(App *app)
{
    SnippetList fresh;
    char err[768];

    snippet_list_init(&fresh);
    if (!snippet_db_list(&app->db, app->search[0] ? app->search : NULL, &fresh,
                         err, sizeof(err))) {
        app_set_error(app, "%s", err);
        snippet_list_free(&fresh);
        return false;
    }

    snippet_list_free(&app->list);
    app->list = fresh;

    if (app->selected.valid &&
        !snippet_list_contains(&app->list, app->selected.id)) {
        snippet_free(&app->selected);
    }
    return true;
}

static void app_select(App *app, sqlite3_int64 id)
{
    Snippet loaded;
    char err[768];

    if (!snippet_db_get(&app->db, id, &loaded, err, sizeof(err))) {
        app_set_error(app, "%s", err);
        return;
    }

    snippet_free(&app->selected);
    app->selected = loaded;
}

static void app_copy_selected(App *app)
{
    Snippet *snippet = &app->selected;

    if (!snippet->valid) {
        return;
    }

    /* igSetClipboardText goes through the SDL2 backend; the direct SDL call
     * makes the behaviour explicit and also covers backends that do not set
     * the platform clipboard callbacks. */
    ImGui_SetClipboardText(snippet->body);
    if (SDL_SetClipboardText(snippet->body) != 0) {
        app_set_error(app, "Clipboard error: %s", SDL_GetError());
        return;
    }
    app_set_status(app, app->ui->copied_status,
                   (unsigned long)strlen(snippet->body));
}

static void app_open_editor_new(App *app)
{
    app->editor_is_edit = false;
    app->editor_id = 0;
    app->editor_request_open = true;
    app->form_name[0] = '\0';
    app->form_tags[0] = '\0';
    app->form_description[0] = '\0';
    app->form_body[0] = '\0';
    app->form_error[0] = '\0';
}

static void app_open_editor_edit(App *app)
{
    Snippet *snippet = &app->selected;

    if (!snippet->valid) {
        return;
    }
    if (strlen(snippet->name) >= sizeof(app->form_name) ||
        strlen(snippet->tags) >= sizeof(app->form_tags) ||
        strlen(snippet->description) >= sizeof(app->form_description) ||
        strlen(snippet->body) >= sizeof(app->form_body)) {
        app_set_error(app, "%s", app->ui->too_large);
        return;
    }

    app->editor_is_edit = true;
    app->editor_id = snippet->id;
    app->editor_request_open = true;
    copy_text(app->form_name, sizeof(app->form_name), snippet->name);
    copy_text(app->form_tags, sizeof(app->form_tags), snippet->tags);
    copy_text(app->form_description, sizeof(app->form_description),
              snippet->description);
    copy_text(app->form_body, sizeof(app->form_body), snippet->body);
    app->form_error[0] = '\0';
}

static void app_save_editor(App *app)
{
    char name[APP_NAME_CAP];
    char tags[APP_TAGS_CAP];
    char err[768];
    sqlite3_int64 id = app->editor_id;
    bool refreshed;
    bool ok;

    copy_text(name, sizeof(name), app->form_name);
    copy_text(tags, sizeof(tags), app->form_tags);
    {
        char *trimmed_name = trim_in_place(name);
        char *trimmed_tags = trim_in_place(tags);
        if (trimmed_name != name) {
            memmove(name, trimmed_name, strlen(trimmed_name) + 1);
        }
        if (trimmed_tags != tags) {
            memmove(tags, trimmed_tags, strlen(trimmed_tags) + 1);
        }
    }

    if (!name[0]) {
        set_form_error(app, app->ui->name_required);
        return;
    }
    if (!app->form_body[0]) {
        set_form_error(app, app->ui->code_required);
        return;
    }

    if (app->editor_is_edit) {
        ok = snippet_db_update(&app->db, app->editor_id, name, tags,
                               app->form_description, app->form_body, err,
                               sizeof(err));
    } else {
        ok = snippet_db_insert(&app->db, name, tags, app->form_description,
                               app->form_body, &id, err, sizeof(err));
    }

    if (!ok) {
        set_form_error(app, err);
        app_set_error(app, "%s", err);
        return;
    }

    ImGui_CloseCurrentPopup();
    app->form_error[0] = '\0';

    /* Clearing the filter guarantees the new/updated row is visible and
     * selected, which is what the user expects after pressing Save. */
    app->search[0] = '\0';
    refreshed = app_refresh(app);
    snippet_free(&app->selected);
    app_select(app, id);

    if (refreshed && app->selected.valid && app->selected.id == id) {
        app_set_status(app, app->editor_is_edit ? app->ui->updated_status
                                                : app->ui->saved_status,
                       (long long)id);
    }
}

static void app_open_delete(App *app)
{
    if (!app->selected.valid) {
        return;
    }
    app->delete_error[0] = '\0';
    ImGui_OpenPopup(app->delete_popup, ImGuiPopupFlags_None);
}

static void app_confirm_delete(App *app)
{
    char err[768];

    if (!app->selected.valid) {
        ImGui_CloseCurrentPopup();
        return;
    }

    if (!snippet_db_delete(&app->db, app->selected.id, err, sizeof(err))) {
        copy_text(app->delete_error, sizeof(app->delete_error), err);
        app_set_error(app, "%s", err);
        return;
    }

    ImGui_CloseCurrentPopup();
    snippet_free(&app->selected);
    if (app_refresh(app)) {
        app_set_status(app, "%s", app->ui->deleted_status);
    }
}

static bool app_init(App *app, const char *db_path, char *err,
                     size_t err_size)
{
    memset(app, 0, sizeof(*app));
    snippet_list_init(&app->list);
    app->ui = &UI_EN; /* replaced once we know whether a CJK font loaded */
    app_set_ui(app, &UI_EN);
    app_set_status(app, "%s", app->ui->ready);

    if (!snippet_db_open(&app->db, db_path, err, err_size)) {
        return false;
    }
    if (!app_refresh(app)) {
        copy_text(err, err_size, app->status);
        return false;
    }
    if (app->list.count > 0) {
        app_select(app, app->list.items[0].id);
    }
    return true;
}

static void app_destroy(App *app)
{
    if (!app) {
        return;
    }
    snippet_free(&app->selected);
    snippet_list_free(&app->list);
    snippet_db_close(&app->db);
}

/*
 * GUI-subsystem build support.
 *
 * The normal executable keeps a console so database errors are visible.  The
 * no-console SnippetManager.exe redirects stderr to snippet_manager.log next
 * to the executable and reports startup failures in a MessageBox.
 */
static void gui_prepare_logging(void)
{
#if defined(_WIN32) && defined(SNIPGUI_GUI_APP)
    char path[MAX_PATH];
    DWORD length = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));

    if (length > 0 && length < (DWORD)sizeof(path)) {
        char *slash = strrchr(path, '\\');
        if (slash) {
            char log_path[MAX_PATH + 32];
            *slash = '\0';
            snprintf(log_path, sizeof(log_path), "%s\\snippet_manager.log",
                     path);
            (void)freopen(log_path, "a", stderr);
        }
    }
#endif
}

static void gui_show_error(const char *message)
{
#if defined(_WIN32) && defined(SNIPGUI_GUI_APP)
    MessageBoxA(NULL, message ? message : "Unknown error", "Snippet Manager",
                MB_OK | MB_ICONERROR);
#else
    (void)message;
#endif
}

/* ------------------------------------------------------------------------- */
/* UI helpers                                                                */
/* ------------------------------------------------------------------------- */
static void sanitize_label(const char *src, char *dst, size_t cap)
{
    size_t in = 0;
    size_t out = 0;

    if (!dst || cap == 0) {
        return;
    }
    while (src && src[in] && out + 1 < cap) {
        unsigned char c = (unsigned char)src[in];

        if (c == '#' && src[in + 1] == '#') {
            if (out + 4 < cap) {
                dst[out++] = '#';
                dst[out++] = ' ';
                dst[out++] = '#';
            }
            in += 2;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            dst[out++] = ' ';
            ++in;
        } else {
            dst[out++] = (char)c;
            ++in;
        }
    }
    dst[out] = '\0';
}

static void build_row_label(const SnippetSummary *item, char *label,
                            size_t cap)
{
    char name[512];
    char tags[1024];

    sanitize_label(item->name, name, sizeof(name));
    sanitize_label(item->tags, tags, sizeof(tags));

    if (tags[0]) {
        snprintf(label, cap, "%s  -  %s##snippet_row_%lld", name, tags,
                 (long long)item->id);
    } else {
        snprintf(label, cap, "%s##snippet_row_%lld", name,
                 (long long)item->id);
    }
}

static void setup_style(void)
{
    ImGuiStyle *style;

    ImGui_StyleColorsDark(NULL);
    style = ImGui_GetStyle();

    style->WindowRounding = 6.0f;
    style->ChildRounding = 5.0f;
    style->FrameRounding = 4.0f;
    style->PopupRounding = 6.0f;
    style->ScrollbarRounding = 8.0f;
    style->GrabRounding = 4.0f;
    style->WindowBorderSize = 1.0f;
    style->FrameBorderSize = 1.0f;
    style->FramePadding = ImVec2_Make(9.0f, 5.0f);
    style->ItemSpacing = ImVec2_Make(8.0f, 7.0f);

    style->Colors[ImGuiCol_FrameBg] = ImVec4_Make(0.16f, 0.19f, 0.24f, 1.00f);
    style->Colors[ImGuiCol_FrameBgHovered] =
        ImVec4_Make(0.21f, 0.25f, 0.31f, 1.00f);
    style->Colors[ImGuiCol_FrameBgActive] =
        ImVec4_Make(0.24f, 0.29f, 0.36f, 1.00f);
    style->Colors[ImGuiCol_Border] = ImVec4_Make(0.34f, 0.39f, 0.47f, 1.00f);
    style->Colors[ImGuiCol_PopupBg] = ImVec4_Make(0.105f, 0.115f, 0.135f, 1.00f);
    style->Colors[ImGuiCol_Button] = ImVec4_Make(0.20f, 0.43f, 0.68f, 1.00f);
    style->Colors[ImGuiCol_ButtonHovered] =
        ImVec4_Make(0.26f, 0.53f, 0.82f, 1.00f);
    style->Colors[ImGuiCol_ButtonActive] =
        ImVec4_Make(0.16f, 0.36f, 0.60f, 1.00f);
    style->Colors[ImGuiCol_Header] = ImVec4_Make(0.20f, 0.43f, 0.68f, 0.85f);
    style->Colors[ImGuiCol_HeaderHovered] =
        ImVec4_Make(0.26f, 0.53f, 0.82f, 0.85f);
    style->Colors[ImGuiCol_HeaderActive] =
        ImVec4_Make(0.16f, 0.36f, 0.60f, 1.00f);
}

/* ------------------------------------------------------------------------- */
/* UI drawing                                                                */
/* ------------------------------------------------------------------------- */
static void draw_toolbar(App *app)
{
    const UiStrings *ui = app->ui;
    ImGuiStyle *style = ImGui_GetStyle();
    float new_width =
        ImGui_CalcTextSize(ui->new_button, NULL, false, -1.0f).x +
        style->FramePadding.x * 2.0f;
    float delete_width =
        ImGui_CalcTextSize(ui->delete_button, NULL, false, -1.0f).x +
        style->FramePadding.x * 2.0f;
    float available = ImGui_GetContentRegionAvail().x;
    float search_width = available - new_width - delete_width -
                         style->ItemSpacing.x * 2.0f - 8.0f;

    if (search_width < 140.0f) {
        search_width = 140.0f;
    }

    ImGui_SetNextItemWidth(search_width);
    if (ImGui_InputTextWithHint("##search", ui->search_hint, app->search,
                                sizeof(app->search), 0, NULL, NULL)) {
        app_refresh(app);
    }

    ImGui_SameLine(0.0f, 8.0f);
    if (ImGui_Button(ui->new_button, ImVec2_Make(0.0f, 0.0f))) {
        app_open_editor_new(app);
    }

    ImGui_SameLine(0.0f, 4.0f);
    ImGui_BeginDisabled(!app->selected.valid);
    if (ImGui_Button(ui->delete_button, ImVec2_Make(0.0f, 0.0f))) {
        app_open_delete(app);
    }
    ImGui_EndDisabled();
}

static void draw_list_panel(App *app, float panel_height)
{
    const UiStrings *ui = app->ui;
    float list_height;
    size_t i;

    ImGui_TextDisabled(ui->snippet_count, (unsigned long)app->list.count);

    /* The table cell can report the full window height as "available", which
     * would push the status bar off-screen.  Size the scroll area from the
     * panel height explicitly. */
    list_height = panel_height - ImGui_GetTextLineHeightWithSpacing() - 30.0f;
    if (list_height < 80.0f) {
        list_height = 80.0f;
    }

    if (ImGui_BeginListBox("##snippet_list",
                           ImVec2_Make(0.0f, list_height))) {
        if (app->list.count == 0) {
            ImGui_TextWrapped("%s", app->search[0] ? ui->no_matches
                                                   : ui->no_snippets);
        } else {
            for (i = 0; i < app->list.count; ++i) {
                const SnippetSummary *item = &app->list.items[i];
                char label[2048];
                bool selected = app->selected.valid &&
                                app->selected.id == item->id;
                ImVec2 row_size = ImVec2_Make(
                    ImGui_GetContentRegionAvail().x, 0.0f);

                build_row_label(item, label, sizeof(label));
                if (ImGui_Selectable(label, selected, ImGuiSelectableFlags_None,
                                     row_size)) {
                    app_select(app, item->id);
                }
                if (ImGui_IsItemHovered(ImGuiHoveredFlags_None)) {
                    if (item->tags[0]) {
                        ImGui_SetTooltip("%s\nTags: %s", item->name,
                                         item->tags);
                    } else {
                        ImGui_SetTooltip("%s", item->name);
                    }
                }
            }
        }
        ImGui_EndListBox();
    }
}

static void draw_detail_panel(App *app, float panel_height)
{
    const UiStrings *ui = app->ui;
    ImGuiStyle *style = ImGui_GetStyle();
    Snippet *snippet = &app->selected;
    float copy_width;
    float edit_width;
    float buttons_width;
    float remaining;

    if (!snippet->valid) {
        ImGui_TextDisabled("%s", ui->select_prompt);
        return;
    }

    ImGui_TextUnformatted(snippet->name, NULL);

    copy_width = ImGui_CalcTextSize(ui->copy_button, NULL, false, -1.0f).x +
                 style->FramePadding.x * 2.0f;
    edit_width = ImGui_CalcTextSize(ui->edit_button, NULL, false, -1.0f).x +
                 style->FramePadding.x * 2.0f;
    buttons_width = copy_width + edit_width + style->ItemSpacing.x;
    remaining = ImGui_GetContentRegionAvail().x;

    if (remaining > buttons_width + 12.0f) {
        float x = ImGui_GetCursorPosX() + remaining - buttons_width;
        ImGui_SameLine(0.0f, 0.0f);
        ImGui_SetCursorPosX(x);
    } else {
        ImGui_Spacing();
    }

    if (ImGui_Button(ui->copy_button, ImVec2_Make(copy_width, 0.0f))) {
        app_copy_selected(app);
    }
    ImGui_SameLine(0.0f, style->ItemSpacing.x);
    if (ImGui_Button(ui->edit_button, ImVec2_Make(edit_width, 0.0f))) {
        app_open_editor_edit(app);
    }

    ImGui_TextDisabled("%s", snippet->tags[0] ? snippet->tags : ui->no_tags);

    {
        float description_space = 0.0f;
        if (snippet->description[0]) {
            ImGui_TextDisabled("%s", ui->description_label);
            ImGui_InputTextMultiline(
                "##snippet_description", snippet->description,
                strlen(snippet->description) + 1, ImVec2_Make(-FLT_MIN, 64.0f),
                ImGuiInputTextFlags_ReadOnly, NULL, NULL);
            description_space = 64.0f +
                                ImGui_GetTextLineHeightWithSpacing() +
                                style->ItemSpacing.y;
        }

        ImGui_Separator();

        {
            float body_height =
                panel_height - 90.0f - description_space;
            if (body_height < 80.0f) {
                body_height = 80.0f;
            }
            /* ReadOnly gives selection, Ctrl+C and independent scrolling for
             * free, while still showing the complete, unwrapped body. */
            ImGui_InputTextMultiline("##snippet_body", snippet->body,
                                     strlen(snippet->body) + 1,
                                     ImVec2_Make(-FLT_MIN, body_height),
                                     ImGuiInputTextFlags_ReadOnly, NULL, NULL);
        }
    }
}

static void draw_body(App *app)
{
    ImGuiStyle *style = ImGui_GetStyle();
    float footer_height = ImGui_GetTextLineHeightWithSpacing() +
                          style->ItemSpacing.y * 2.0f + 6.0f;
    float table_height = ImGui_GetContentRegionAvail().y - footer_height;
    ImGuiTableFlags table_flags = ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_SizingStretchProp |
                                  ImGuiTableFlags_NoSavedSettings;

    if (table_height < 140.0f) {
        table_height = 140.0f;
    }

    if (ImGui_BeginTable("##main_layout", 2, table_flags,
                         ImVec2_Make(0.0f, table_height), 0.0f)) {
        ImGui_TableSetupColumn("Snippets", ImGuiTableColumnFlags_WidthFixed,
                               360.0f, 0);
        ImGui_TableSetupColumn("Detail", ImGuiTableColumnFlags_WidthStretch,
                               0.0f, 0);
        ImGui_TableNextRow(ImGuiTableRowFlags_None, 0.0f);

        ImGui_TableSetColumnIndex(0);
        draw_list_panel(app, table_height);

        ImGui_TableSetColumnIndex(1);
        draw_detail_panel(app, table_height);

        ImGui_EndTable();
    }
}

static void draw_status(App *app)
{
    if (app->ime_composing) {
        ImGui_TextColored(ImVec4_Make(0.55f, 0.78f, 1.0f, 1.0f),
                          app->ui->composing, app->ime_preedit);
    } else if (app->status_error) {
        ImGui_TextColored(ImVec4_Make(1.0f, 0.38f, 0.38f, 1.0f), "%s",
                          app->status);
    } else {
        ImGui_TextDisabled("%s", app->status);
    }
}

static void draw_editor_popup(App *app)
{
    const UiStrings *ui = app->ui;
    ImGuiStyle *style = ImGui_GetStyle();
    ImGuiIO *io = ImGui_GetIO();
    float width = 780.0f;
    float height = 620.0f;

    if (io->DisplaySize.x < width + 40.0f) {
        width = io->DisplaySize.x - 40.0f;
    }
    if (io->DisplaySize.y < height + 40.0f) {
        height = io->DisplaySize.y - 40.0f;
    }
    if (width < 420.0f) {
        width = 420.0f;
    }
    if (height < 360.0f) {
        height = 360.0f;
    }

    ImGui_SetNextWindowPos(
        ImVec2_Make(io->DisplaySize.x * 0.5f, io->DisplaySize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2_Make(0.5f, 0.5f));
    ImGui_SetNextWindowSize(ImVec2_Make(width, height), ImGuiCond_Appearing);

    if (!ImGui_BeginPopupModal(app->editor_popup, NULL,
                               ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    ImGui_TextUnformatted(app->editor_is_edit ? ui->editor_edit
                                              : ui->editor_new,
                          NULL);
    ImGui_Separator();

    ImGui_TextUnformatted(ui->name_label, NULL);
    if (ImGui_IsWindowAppearing()) {
        ImGui_SetKeyboardFocusHere(0);
    }
    ImGui_SetNextItemWidth(-FLT_MIN);
    ImGui_InputText("##form_name", app->form_name, sizeof(app->form_name), 0,
                    NULL, NULL);

    ImGui_Spacing();
    ImGui_TextUnformatted(ui->tags_label, NULL);
    ImGui_SetNextItemWidth(-FLT_MIN);
    ImGui_InputText("##form_tags", app->form_tags, sizeof(app->form_tags), 0,
                    NULL, NULL);

    if (app->ime_composing) {
        ImGui_TextColored(ImVec4_Make(0.55f, 0.78f, 1.0f, 1.0f),
                          ui->composing, app->ime_preedit);
    }

    ImGui_Spacing();
    ImGui_TextUnformatted(ui->description_label, NULL);
    ImGui_InputTextMultiline("##form_description", app->form_description,
                             sizeof(app->form_description),
                             ImVec2_Make(-FLT_MIN, 64.0f), 0, NULL, NULL);

    ImGui_Spacing();
    ImGui_TextUnformatted(ui->code_label, NULL);

    {
        float reserve = ImGui_GetFrameHeightWithSpacing() +
                        ImGui_GetTextLineHeightWithSpacing() +
                        style->ItemSpacing.y * 2.0f;
        float body_height = ImGui_GetContentRegionAvail().y - reserve;

        if (body_height < 80.0f) {
            body_height = 80.0f;
        }
        ImGui_InputTextMultiline("##form_body", app->form_body,
                                 sizeof(app->form_body),
                                 ImVec2_Make(-FLT_MIN, body_height), 0, NULL,
                                 NULL);
    }

    if (app->form_error[0]) {
        ImGui_TextColored(ImVec4_Make(1.0f, 0.45f, 0.45f, 1.0f), "%s",
                          app->form_error);
    } else {
        ImGui_TextDisabled("%s", ui->editor_hint);
    }

    if (ImGui_Button(ui->save_button, ImVec2_Make(110.0f, 0.0f))) {
        app_save_editor(app);
    }
    ImGui_SameLine(0.0f, style->ItemSpacing.x);
    if (ImGui_Button(ui->cancel_button, ImVec2_Make(110.0f, 0.0f))) {
        app->form_error[0] = '\0';
        ImGui_CloseCurrentPopup();
    }

    ImGui_EndPopup();
}

static void draw_delete_popup(App *app)
{
    const UiStrings *ui = app->ui;
    ImGuiStyle *style = ImGui_GetStyle();
    ImGuiIO *io = ImGui_GetIO();
    const char *name = app->selected.valid ? app->selected.name : "";

    ImGui_SetNextWindowPos(
        ImVec2_Make(io->DisplaySize.x * 0.5f, io->DisplaySize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2_Make(0.5f, 0.5f));

    if (!ImGui_BeginPopupModal(app->delete_popup, NULL,
                               ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    ImGui_TextWrapped(ui->delete_question, name);
    ImGui_Spacing();
    ImGui_TextDisabled("%s", ui->delete_warning);

    if (app->delete_error[0]) {
        ImGui_TextColored(ImVec4_Make(1.0f, 0.45f, 0.45f, 1.0f), "%s",
                          app->delete_error);
    }

    ImGui_Spacing();
    ImGui_PushStyleColor(ImGuiCol_Button, ImVec4_Make(0.64f, 0.20f, 0.20f, 1.0f));
    ImGui_PushStyleColor(ImGuiCol_ButtonHovered,
                         ImVec4_Make(0.78f, 0.28f, 0.28f, 1.0f));
    ImGui_PushStyleColor(ImGuiCol_ButtonActive,
                         ImVec4_Make(0.55f, 0.16f, 0.16f, 1.0f));
    if (ImGui_Button(ui->delete_confirm, ImVec2_Make(100.0f, 0.0f))) {
        app_confirm_delete(app);
    }
    ImGui_PopStyleColor(3);

    ImGui_SameLine(0.0f, style->ItemSpacing.x);
    if (ImGui_Button(ui->cancel_button, ImVec2_Make(100.0f, 0.0f))) {
        app->delete_error[0] = '\0';
        ImGui_CloseCurrentPopup();
    }

    ImGui_EndPopup();
}

static void app_draw(App *app)
{
    ImGuiIO *io = ImGui_GetIO();
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui_SetNextWindowPos(ImVec2_Make(0.0f, 0.0f), ImGuiCond_Always,
                           ImVec2_Make(0.0f, 0.0f));
    ImGui_SetNextWindowSize(io->DisplaySize, ImGuiCond_Always);

    if (ImGui_Begin("Snippet Manager##main_window", NULL, flags)) {
        draw_toolbar(app);
        ImGui_Separator();
        draw_body(app);
        ImGui_Separator();
        draw_status(app);

        /* Open the editor at the main window's ID-stack level.  Opening it
         * directly from the Edit button (inside a table cell) would bind the
         * popup to the table's stack and BeginPopupModal would not find it. */
        if (app->editor_request_open) {
            app->editor_request_open = false;
            ImGui_OpenPopup(app->editor_popup, ImGuiPopupFlags_None);
        }

        draw_editor_popup(app);
        draw_delete_popup(app);
    }
    ImGui_End();
}

/* ------------------------------------------------------------------------- */
/* SDL / OpenGL bootstrap                                                    */
/* ------------------------------------------------------------------------- */
static bool create_gl_window(GlWindow *out, char *err, size_t err_size)
{
#if defined(__APPLE__)
    const int versions[1][2] = {{3, 2}};
    const char *glsl_versions[1] = {"#version 150"};
    const int attempt_count = 1;
#else
    const int versions[2][2] = {{3, 0}, {2, 1}};
    const char *glsl_versions[2] = {"#version 130", "#version 120"};
    const int attempt_count = 2;
#endif
    int attempt;

    memset(out, 0, sizeof(*out));

    /* SDL2 suppresses the native IME candidate/composition window by default.
     * Both hints must be set before SDL_CreateWindow(); otherwise a Chinese
     * IME commits text without showing what is being composed.
     *
     * INTERNAL_EDITING asks the IME to draw the in-progress composition in its
     * own UI instead of sending SDL_TEXTEDITING events (which this backend
     * does not render inline). */
#ifdef SDL_HINT_IME_SHOW_UI
    SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
#endif
#ifdef SDL_HINT_IME_INTERNAL_EDITING
    SDL_SetHint(SDL_HINT_IME_INTERNAL_EDITING, "1");
#endif

    for (attempt = 0; attempt < attempt_count; ++attempt) {
        SDL_Window *window;
        SDL_GLContext context;

        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, versions[attempt][0]);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, versions[attempt][1]);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
        SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
#if defined(__APPLE__)
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                            SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                            SDL_GL_CONTEXT_PROFILE_CORE);
#endif

        window = SDL_CreateWindow(
            "Snippet Manager", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            1180, 760,
            SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
        if (!window) {
            continue;
        }

        context = SDL_GL_CreateContext(window);
        if (!context) {
            SDL_DestroyWindow(window);
            continue;
        }

        if (SDL_GL_MakeCurrent(window, context) != 0) {
            SDL_GL_DeleteContext(context);
            SDL_DestroyWindow(window);
            continue;
        }

        SDL_GL_SetSwapInterval(1);
        SDL_SetWindowMinimumSize(window, 760, 480);

        out->window = window;
        out->context = context;
        out->glsl_version = glsl_versions[attempt];
        return true;
    }

    snprintf(err, err_size, "could not create an OpenGL window: %s",
             SDL_GetError());
    fprintf(stderr, "[snippet-gui] %s\n", err);
    return false;
}

int main(int argc, char **argv)
{
    App *app = NULL;
    GlWindow gl = {0};
    ImGuiContext *imgui_context = NULL;
    bool sdl2_backend = false;
    bool opengl3_backend = false;
    bool running = true;
    bool ime_started = false;
    int exit_code = EXIT_FAILURE;
    char err[1024] = {0};
    const char *db_path;

    gui_prepare_logging();

    if (argc > 2) {
        fprintf(stderr, "usage: %s [database-file]\n", argv[0]);
        return 2;
    }
    db_path = (argc == 2 && argv[1] && argv[1][0]) ? argv[1] : "snippets.db";

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
        snprintf(err, sizeof(err), "SDL_Init failed: %s", SDL_GetError());
        fprintf(stderr, "[snippet-gui] %s\n", err);
        gui_show_error(err);
        return EXIT_FAILURE;
    }

    app = (App *)calloc(1, sizeof(*app));
    if (!app) {
        fprintf(stderr, "[snippet-gui] out of memory\n");
        gui_show_error("Out of memory.");
        SDL_Quit();
        return EXIT_FAILURE;
    }

    if (!app_init(app, db_path, err, sizeof(err))) {
        fprintf(stderr, "[snippet-gui] startup failed: %s\n", err);
        gui_show_error(err);
        goto cleanup;
    }

    if (!create_gl_window(&gl, err, sizeof(err))) {
        gui_show_error(err);
        goto cleanup;
    }

    imgui_context = ImGui_CreateContext(NULL);
    if (!imgui_context) {
        snprintf(err, sizeof(err), "Could not create the ImGui context.");
        fprintf(stderr, "[snippet-gui] %s\n", err);
        gui_show_error(err);
        goto cleanup;
    }

    {
        ImGuiIO *io = ImGui_GetIO();
        io->IniFilename = NULL;
        io->LogFilename = NULL;
        io->ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        setup_style();

        if (!ImGui_ImplSDL2_InitForOpenGL(gl.window, gl.context)) {
            snprintf(err, sizeof(err), "SDL2 backend init failed: %s",
                     SDL_GetError());
            fprintf(stderr, "[snippet-gui] %s\n", err);
            gui_show_error(err);
            goto cleanup;
        }
        sdl2_backend = true;

        if (!ImGui_ImplOpenGL3_Init(gl.glsl_version)) {
            snprintf(err, sizeof(err), "OpenGL3 backend init failed.");
            fprintf(stderr, "[snippet-gui] %s\n", err);
            gui_show_error(err);
            goto cleanup;
        }
        opengl3_backend = true;

        /* Load the font before the first frame so it becomes the default. */
        app_set_ui(app, load_ui_font(io->Fonts) ? &UI_ZH : &UI_EN);
        app_set_status(app, app->ui->loaded_status,
                       (unsigned long)app->list.count);
    }

    while (running) {
        SDL_Event event;
        int drawable_width = 0;
        int drawable_height = 0;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_TEXTEDITING) {
                copy_text(app->ime_preedit, sizeof(app->ime_preedit),
                          event.edit.text);
                app->ime_composing = app->ime_preedit[0] != '\0';
            } else if (event.type == SDL_TEXTINPUT) {
                app->ime_composing = false;
                app->ime_preedit[0] = '\0';
            }
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                running = false;
            } else if (event.type == SDL_WINDOWEVENT) {
                if (event.window.event == SDL_WINDOWEVENT_CLOSE &&
                    event.window.windowID == SDL_GetWindowID(gl.window)) {
                    running = false;
                } else if (event.window.event == SDL_WINDOWEVENT_FOCUS_GAINED &&
                           event.window.windowID == SDL_GetWindowID(gl.window)) {
                    /* WIN_StartTextInput() uses SDL_GetKeyboardFocus(), so it
                     * must run after the window actually has focus.  This is
                     * what associates the Windows IME context with the window
                     * and lets the native candidate/composition UI appear. */
                    SDL_StartTextInput();
                    ime_started = true;
                }
            }
        }

        if (!ime_started && SDL_GetKeyboardFocus() == gl.window) {
            SDL_StartTextInput();
            ime_started = true;
            fprintf(stderr, "[snippet-gui] IME text input enabled\n");
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui_NewFrame();

        app_draw(app);

        ImGui_Render();

        SDL_GL_GetDrawableSize(gl.window, &drawable_width, &drawable_height);
        if (drawable_width > 0 && drawable_height > 0) {
            glViewport(0, 0, drawable_width, drawable_height);
            glClearColor(0.075f, 0.082f, 0.095f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui_GetDrawData());
            SDL_GL_SwapWindow(gl.window);
        }
    }

    exit_code = EXIT_SUCCESS;

cleanup:
    if (opengl3_backend) {
        ImGui_ImplOpenGL3_Shutdown();
    }
    if (sdl2_backend) {
        ImGui_ImplSDL2_Shutdown();
    }
    if (imgui_context) {
        ImGui_DestroyContext(imgui_context);
    }
    if (gl.context) {
        SDL_GL_DeleteContext(gl.context);
    }
    if (gl.window) {
        SDL_DestroyWindow(gl.window);
    }
    if (app) {
        app_destroy(app);
        free(app);
    }
    SDL_Quit();
    return exit_code;
}

#if defined(_WIN32) && defined(SNIPGUI_GUI_APP)
int WINAPI WinMain(HINSTANCE instance, HINSTANCE previous_instance,
                   LPSTR command_line, int show_command)
{
    (void)instance;
    (void)previous_instance;
    (void)command_line;
    (void)show_command;

    /* The CRT's __argv/__argc are not reliably initialized in a GUI-subsystem
     * MinGW process, so calling main(__argc, __argv) can crash before main().
     * The shortcut always opens the default snippets.db from its working
     * directory, which is exactly the behaviour we want here. */
    return main(0, NULL);
}
#endif
