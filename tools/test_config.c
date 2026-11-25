#include "lua_config/lua_config.h"
#include <stdio.h>

static w_desktop_config config;
static w_config_language language = { .config = &config };
static char error[512];

static bool parse(const char *text) {
    error[0] = 0;
    return w_lua_config_parse(&language,text,"config-test.lua",error,sizeof(error));
}

static bool shipped(void) {
    static char source[256 * 1024];
    FILE *file = fopen("assets/winwm.lua","rb");
    if (!file) return false;
    size_t n = fread(source,1,sizeof(source),file);
    bool ok = !ferror(file) && n > 0 && n < sizeof(source) && !memchr(source,0,n);
    fclose(file);
    if (!ok || !parse(source)) return false;
    return config.panel_material.opacity == 240 && config.panel_material.blur && config.panel_material.shadow &&
           config.binding_count > 50 && config.mouse_actions[0][8] == mouse_move &&
           config.mouse_actions[1][8] == mouse_resize && config.active_gradient.enabled &&
           config.motion[motion_workspace].ms == 180 && w_config_validate_providers(&config,error,sizeof(error));
}

static bool custom(void) {
    w_config_defaults(&config);
    language = (w_config_language){ .config = &config };
    if (!parse("local mod = 'ALT + '\n"
               "for i = 1, 4 do hl.bind(mod .. i, hl.dsp.focus({workspace=i})) end\n"
               "hl.bind(mod .. 'mouse:273',hl.dsp.window.drag(),{mouse=true})\n"
               "hl.config({general={gaps_in=3*4},decoration={panels={opacity=0.5,blur=false,shadow=true}},bar={modules_left={},"
               "modules_right={'audio#desk','custom/status'},['audio#desk']={tooltip_enabled=false}}})\n"
               "winwm.apps({Editor='editor.exe'})\n"
               "winwm.providers({status={executable='status.exe'}})\n"
               "winwm.workspaces({[1]='Dev'})\n")) return false;
    if (config.panel_material.opacity != 128 || config.panel_material.blur || !config.panel_material.shadow ||
        config.inactive_alpha != 250 ||
        config.binding_count != 4 || config.gap != 12 || config.mouse_actions[1][1] != mouse_move ||
        config.mouse_actions[1][8] != mouse_unbound || *config.bar_left || config.style_count != 1 ||
        !config.styles[0].hide_tooltip || !w_config_has_module(&config,"audio") ||
        strcmp(config.workspace_names[0],"Dev") || config.app_count != 1 ||
        !w_config_validate_providers(&config,error,sizeof(error))) return false;
    for (int i = 0; i < 4; ++i) {
        const w_key_binding *b = &config.bindings[i];
        if (b->key != (unsigned)('1'+i) || b->mods != 1 || b->action.kind != cmd_workspace_select)
            return false;
    }
    return parse("hl.bind('ALT + 1',hl.dsp.winwm('menu'),{release=true})\n"
                 "hl.unbind('ALT + 1')\n") && config.binding_count == 3;
}

int main(void) {
    w_config_defaults(&config);
    if (!shipped() || !custom()) {
        fprintf(stderr,"Lua profile did not produce the expected configuration. %s\n",error);
        return 1;
    }
    if (!parse("hl.config({notifications={enabled=false,capture='off',corner='bottom_left',"
               "duration_ms=0,fade_ms=0,history_limit=2,visible_limit=1,margin=0,width=240}})") ||
        config.notifications.enabled || config.notifications.capture != notice_capture_off || config.notifications.corner != notice_bottom_left ||
        config.notifications.duration_ms || config.notifications.fade_ms || config.notifications.history_limit != 2 ||
        config.notifications.visible_limit != 1 || config.notifications.margin || config.notifications.width != 240)
        return 1;
    const char *invalid[] = {
        "hl.config({general={gaps_in=-1}})", "hl.config({general={inexistent=1}})",
        "hl.config({bar={modules_left={'clock','unknown'}}})", "hl.bind(",
        "while true do end", "os.execute('')",
        "hl.config({notifications={history_limit=513}})", "hl.config({notifications={corner='center'}})",
        "hl.config('decoration.panels.opacity',1.01)", "hl.config('decoration.panels.opacity',0/0)",
        "hl.config({decoration={panels={blur=2}}})", "hl.config('decoration.panels.shadow','invalid')",
    };
    for (size_t i = 0; i < count_of(invalid); ++i)
        if (parse(invalid[i]) || !*error) {
            fprintf(stderr,"Expected a config diagnostic for %s\n",invalid[i]);
            return 2;
        }
    puts("Lua defaults, custom profiles and error handling passed.");
    return 0;
}
