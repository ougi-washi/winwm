#include "lua_config/lua_config.h"
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    w_config_language *language;
    const char *text, *label;
    size_t memory;
    int budget;
    bool once;
} w_lua;

static w_lua *w_state(lua_State *L) {
    return *(w_lua **)lua_getextraspace(L);
}

static void *w_allocate(void *userdata, void *ptr, size_t old, size_t bytes) {
    w_lua *ctx = userdata;
    if (!ptr) old = 0;
    if (bytes > old && bytes-old > 8*1024*1024-ctx->memory) return NULL;
    if (!bytes) { free(ptr); ctx->memory -= old; return NULL; }
    void *result = realloc(ptr,bytes);
    if (result) ctx->memory = ctx->memory-old+bytes;
    return result;
}

static void w_charge(lua_State *L, int cost) {
    w_state(L)->budget -= cost;
    if (w_state(L)->budget <= 0) luaL_error(L,"configuration exceeded its instruction limit");
}

static void w_hook(lua_State *L, lua_Debug *debug) {
    (void)debug;
    w_charge(L,1000);
}

static void w_args(lua_State *L, int low, int high) {
    int n = lua_gettop(L);
    if (n < low || n > high) luaL_error(L,"unexpected number of arguments");
}

static void w_declaration(lua_State *L, int low, int high) {
    w_args(L,low,high);
    if (w_state(L)->once) luaL_error(L,"only startup commands can be scheduled by the start event");
}

static const char *w_string(lua_State *L, int i) {
    luaL_checktype(L,i,LUA_TSTRING);
    size_t len;
    const char *s = lua_tolstring(L,i,&len);
    if (memchr(s,0,len)) luaL_error(L,"embedded NUL in configuration string");
    return s;
}

static const char *w_scalar(lua_State *L, int i) {
    if (lua_isboolean(L,i)) return lua_toboolean(L,i) ? "true" : "false";
    if (lua_type(L,i) == LUA_TNUMBER) return lua_tostring(L,i);
    return w_string(L,i);
}

static int w_name_order(const void *a, const void *b) {
    return strcmp(*(const char *const *)a,*(const char *const *)b);
}

// Lua hash iteration order varies between runtimes. Keep configured app, style
// and provider indices stable across reloads and across identical user files.
static int w_names(lua_State *L, int i, const char **names, size_t cap) {
    i = lua_absindex(L,i);
    luaL_checktype(L,i,LUA_TTABLE);
    size_t n = 0;
    lua_pushnil(L);
    while (lua_next(L,i)) {
        if (n == cap) luaL_error(L,"configuration table has too many entries");
        names[n++] = w_string(L,-2);
        lua_pop(L,1);
    }
    qsort(names,n,sizeof(*names),w_name_order);
    return (int)n;
}

static void w_fields(lua_State *L, int i, const char *allowed) {
    i = lua_absindex(L,i);
    luaL_checktype(L,i,LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L,i)) {
        const char *key = w_string(L,-2);
        size_t n = strlen(key);
        bool found = false;
        for (const char *p = allowed; *p;) {
            size_t len = strcspn(p," ");
            if (len == n && !memcmp(p,key,n)) { found = true; break; }
            p += len + (p[len] != 0);
        }
        if (!found) luaL_error(L,"unsupported field '%s'",key);
        lua_pop(L,1);
    }
}

static int w_array(lua_State *L, int i, int min, int max) {
    i = lua_absindex(L,i);
    luaL_checktype(L,i,LUA_TTABLE);
    size_t n = lua_rawlen(L,i);
    if (n < (size_t)min || n > (size_t)max) luaL_error(L,"invalid array length");
    int keys = 0;
    lua_pushnil(L);
    while (lua_next(L,i)) {
        lua_Integer k = lua_tointeger(L,-2);
        if (!lua_isinteger(L,-2) || k < 1 || (lua_Unsigned)k > n) luaL_error(L,"expected a dense array");
        ++keys;
        lua_pop(L,1);
    }
    if ((size_t)keys != n) luaL_error(L,"expected a dense array");
    return keys;
}

static bool w_has(lua_State *L, int i, const char *key) {
    bool found = lua_getfield(L,i,key) != LUA_TNIL;
    lua_pop(L,1);
    return found;
}

static const char *w_field(lua_State *L, int i, const char *key, const char *fallback) {
    lua_getfield(L,i,key);
    const char *s = lua_isnil(L,-1) && fallback ? fallback : w_string(L,-1);
    lua_pop(L,1);
    return s;
}

static bool w_flag(lua_State *L, int i, const char *key, bool fallback) {
    if (lua_isnoneornil(L,i)) return fallback;
    lua_getfield(L,i,key);
    if (!lua_isnil(L,-1)) {
        luaL_checktype(L,-1,LUA_TBOOLEAN);
        fallback = lua_toboolean(L,-1) != 0;
    }
    lua_pop(L,1);
    return fallback;
}

static int w_integer(lua_State *L, int i, int lo, int hi) {
    lua_Integer n = luaL_checkinteger(L,i);
    if (n < lo || n > hi) luaL_error(L,"integer out of range");
    return (int)n;
}

static int w_int_field(lua_State *L, int i, const char *key, int lo, int hi, int fallback) {
    lua_getfield(L,i,key);
    int val = lua_isnil(L,-1) ? fallback : w_integer(L,-1,lo,hi);
    lua_pop(L,1);
    return val;
}

static void w_copy(lua_State *L, char *out, size_t cap, const char *text) {
    if (!w_str_copy(out,cap,text)) luaL_error(L,"configuration value is too long");
}

static void w_set(lua_State *L, const char *key, const char *value) {
    char buffer[4096];
    w_copy(L,buffer,sizeof(buffer),value);
    if (!w_config_language_set(w_state(L)->language,key,buffer))
        luaL_error(L,"unsupported option or invalid value for '%s'",key);
}

static void w_list_value(lua_State *L, int i, int max, char *out, size_t cap) {
    i = lua_absindex(L,i);
    int n = w_array(L,i,0,max);
    size_t used = 0;
    for (int j = 1; j <= n; ++j) {
        lua_rawgeti(L,i,j);
        const char *s = w_string(L,-1);
        int wrote = snprintf(out+used,cap-used,"%s%s",used ? " " : "",s);
        if (!*s || strpbrk(s," \r\n\t") || wrote < 0 || (size_t)wrote >= cap-used)
            luaL_error(L,"invalid array element");
        used += (size_t)wrote;
        lua_pop(L,1);
    }
}

static void w_option(lua_State *L, const char *path, int i, int depth) {
    if (depth > 5) luaL_error(L,"invalid configuration nesting");
    i = lua_absindex(L,i);
    w_charge(L,50);
    if (lua_istable(L,i) && (!*path || !strchr(path,'.') || !strcmp(path,"general.col") || !strcmp(path,"decoration.panels") ||
        (!strncmp(path,"bar.",4) && !strchr(path+4,'.') && strncmp(path+4,"modules_",8)))) {
        if (!*path) w_fields(L,i,"general decoration input animations bar programs notifications");
        const char *names[96];
        int count = w_names(L,i,names,count_of(names));
        for (int j=0; j<count; ++j) {
            char nested[180];
            const char *part = names[j];
            int n = snprintf(nested,sizeof(nested),"%s%s%s",path,*path ? "." : "",part);
            if (!*part || strchr(part,':') || n < 0 || (size_t)n >= sizeof(nested)) luaL_error(L,"invalid option name");
            lua_getfield(L,i,part);
            w_option(L,nested,-1,depth+1);
            lua_pop(L,1);
        }
        if (*path && !count) luaL_error(L,"empty option group '%s'",path);
        return;
    }
    char key[180], value[512] = {0};
    w_copy(L,key,sizeof(key),path);
    char *sep = strchr(key,'.');
    if (!sep || strchr(path,':')) luaL_error(L,"invalid option '%s'",path);
    *sep++ = ':';
    if (!strncmp(key,"bar:",4)) for (char *p=sep; *p; ++p) if (*p == '.') *p = ':';
    if (!lua_istable(L,i)) {
        // Lua hexadecimal colors are integers in the upstream configuration API.
        if (lua_isinteger(L,i) && (!strncmp(key,"general:col.",12) ||
            !strcmp(key,"bar:background") || !strcmp(key,"bar:foreground") || strstr(key,":color"))) {
            lua_Integer color = lua_tointeger(L,i);
            if (color < 0 || (lua_Unsigned)color > UINT32_MAX) luaL_error(L,"invalid color");
            snprintf(value,sizeof(value),"0x%08x",(unsigned)color);
            w_set(L,key,value);
        } else w_set(L,key,w_scalar(L,i));
    } else if (!strncmp(key,"bar:modules_",12)) {
        w_list_value(L,i,64,value,sizeof(value));
        w_set(L,key,value);
    } else if (!strncmp(key,"general:col.",12)) {
        w_fields(L,i,"colors angle");
        lua_getfield(L,i,"colors");
        w_array(L,-1,1,2);
        w_list_value(L,-1,2,value,sizeof(value));
        int n = (int)lua_rawlen(L,-1);
        lua_pop(L,1);
        int angle = w_int_field(L,i,"angle",0,360,0);
        if (n == 2) snprintf(value+strlen(value),sizeof(value)-strlen(value)," %ddeg",angle);
        w_set(L,key,value);
    } else luaL_error(L,"unsupported option '%s'",path);
}

static int w_config(lua_State *L) {
    w_declaration(L,1,2);
    if (lua_gettop(L) == 2) w_option(L,w_string(L,1),2,0);
    else { luaL_checktype(L,1,LUA_TTABLE); w_option(L,"",1,0); }
    return 0;
}

static int w_descriptor(lua_State *L) {
    return luaL_error(L,"pass the dispatcher to hl.bind");
}

static int w_action(lua_State *L, const char *name, const char *argument, enum w_mouse_action mouse) {
    if (argument && *argument) lua_pushfstring(L,"%s %s",name,argument);
    else lua_pushstring(L,name);
    const char *action = lua_tostring(L,-1);
    w_command checked;
    if (mouse == mouse_inherit && !w_command_parse(action,&checked))
        return luaL_error(L,"invalid winwm action '%s'",action);
    lua_pushinteger(L,mouse);
    lua_pushcclosure(L,w_descriptor,2);
    return 1;
}

static int w_native(lua_State *L) {
    w_args(L,1,1);
    return w_action(L,w_string(L,1),NULL,mouse_inherit);
}

static int w_exec_dispatch(lua_State *L) {
    w_args(L,1,1);
    return w_action(L,"program",w_string(L,1),mouse_inherit);
}

static int w_simple(lua_State *L) {
    w_args(L,0,0);
    return w_action(L,lua_tostring(L,lua_upvalueindex(1)),NULL,mouse_inherit);
}

static int w_close_window(lua_State *L) {
    w_args(L,0,1);
    if (lua_gettop(L)) w_fields(L,1,"");
    return w_action(L,"close",NULL,mouse_inherit);
}

static int w_toggle(lua_State *L) {
    const char *action = lua_tostring(L,lua_upvalueindex(1));
    w_args(L,0,1);
    if (lua_gettop(L)) {
        bool full = !strcmp(action,"fullscreen");
        w_fields(L,1,full ? "action mode" : "action");
        if (strcmp(w_field(L,1,"action","toggle"),"toggle") || (full && strcmp(w_field(L,1,"mode","fullscreen"),"fullscreen")))
            return luaL_error(L,"only toggling the focused window is supported");
    }
    return w_action(L,action,NULL,mouse_inherit);
}

static int w_move_focus(lua_State *L) {
    w_args(L,1,1);
    bool move = lua_toboolean(L,lua_upvalueindex(1)) != 0;
    luaL_checktype(L,1,LUA_TTABLE);
    if (w_has(L,1,"direction")) {
        w_fields(L,1,"direction");
        return w_action(L,move ? "move" : "focus",w_field(L,1,"direction",NULL),mouse_inherit);
    }
    w_fields(L,1,move ? "workspace follow" : "workspace");
    lua_getfield(L,1,"workspace");
    const char *ws = w_scalar(L,-1);
    if (!move && !strcmp(ws,"previous")) return w_action(L,"workspace_back",NULL,mouse_inherit);
    if (!strncmp(ws,"name:",5)) ws += 5;
    else {
        if (strspn(ws,"0123456789") != strlen(ws)) return luaL_error(L,"unsupported workspace selector");
        w_integer(L,-1,1,workspace_limit);
    }
    const char *action = !move ? "workspace_select" : w_flag(L,1,"follow",true) ? "workspace_follow" : "workspace_send";
    return w_action(L,action,ws,mouse_inherit);
}

static int w_resize(lua_State *L) {
    w_args(L,0,1);
    if (!lua_gettop(L)) return w_action(L,"",NULL,mouse_resize);
    w_fields(L,1,"x y relative");
    if (!w_has(L,1,"x") && !w_has(L,1,"y") && !w_has(L,1,"relative")) return w_action(L,"",NULL,mouse_resize);
    if (!w_flag(L,1,"relative",false) || !w_has(L,1,"x") || !w_has(L,1,"y"))
        return luaL_error(L,"resize requires x, y and relative=true");
    int x = w_int_field(L,1,"x",-10000,10000,0), y = w_int_field(L,1,"y",-10000,10000,0);
    char delta[32];
    snprintf(delta,sizeof(delta),"%d %d",x,y);
    return w_action(L,"resize_by",delta,mouse_inherit);
}

static int w_drag(lua_State *L) {
    w_args(L,0,0);
    return w_action(L,"",NULL,mouse_move);
}

static int w_layout(lua_State *L) {
    w_args(L,1,1);
    if (strcmp(w_string(L,1),"togglesplit")) return luaL_error(L,"unsupported layout message");
    return w_action(L,"layout","toggle_split",mouse_inherit);
}

static int w_submap_dispatch(lua_State *L) {
    w_args(L,1,1);
    return w_action(L,"set_mode",w_string(L,1),mouse_inherit);
}

static int w_submap(lua_State *L) {
    w_declaration(L,2,2);
    w_config_language *lang = w_state(L)->language;
    if (lang->resize || strcmp(w_string(L,1),"resize")) return luaL_error(L,"only the resize submap is supported");
    luaL_checktype(L,2,LUA_TFUNCTION);
    lang->resize = true;
    int status = lua_pcall(L,0,0,0);
    lang->resize = false;
    if (status != LUA_OK) return lua_error(L);
    return 0;
}

static int w_bind(lua_State *L) {
    w_declaration(L,2,3);
    const char *chord = w_string(L,1);
    if (lua_tocfunction(L,2) != w_descriptor) return luaL_error(L,"bind requires a supported dispatcher");
    if (lua_gettop(L) == 3) w_fields(L,3,"mouse repeating release");
    bool repeat = w_flag(L,3,"repeating",false), release = w_flag(L,3,"release",false);
    bool drag = w_flag(L,3,"mouse",false);
    lua_getupvalue(L,2,1);
    const char *action = lua_tostring(L,-1);
    lua_getupvalue(L,2,2);
    enum w_mouse_action mouse = (enum w_mouse_action)lua_tointeger(L,-1);
    if (drag != (mouse != mouse_inherit) || (drag && (repeat || release)) ||
        !w_config_language_bind(w_state(L)->language,chord,action,repeat,release,mouse))
        return luaL_error(L,"invalid binding for '%s'",chord);
    return 0;
}

static int w_unbind(lua_State *L) {
    w_declaration(L,1,1);
    const char *chord = w_string(L,1);
    enum w_mouse_action mode = strstr(chord,"mouse:272") || strstr(chord,"mouse:273") ? mouse_unbound : mouse_inherit;
    if (!w_config_language_bind(w_state(L)->language,chord,"unbound",false,false,mode))
        return luaL_error(L,"invalid unbind for '%s'",chord);
    return 0;
}

static int w_curve_config(lua_State *L) {
    w_declaration(L,2,2);
    const char *name = w_string(L,1);
    w_fields(L,2,"type points");
    if (!*name || strcmp(w_field(L,2,"type",NULL),"bezier")) return luaL_error(L,"expected a named bezier curve");
    lua_getfield(L,2,"points");
    w_array(L,-1,2,2);
    double p[4];
    for (int i=0; i<2; ++i) {
        lua_rawgeti(L,-1,i+1);
        w_array(L,-1,2,2);
        for (int j=0; j<2; ++j) {
            lua_rawgeti(L,-1,j+1);
            double n = luaL_checknumber(L,-1);
            if (!isfinite(n) || n < (j ? -4 : 0) || n > (j ? 4 : 1)) return luaL_error(L,"invalid bezier coordinate");
            p[2*i+j] = n;
            lua_pop(L,1);
        }
        lua_pop(L,1);
    }
    w_config_language *lang = w_state(L)->language;
    int i=0;
    while (i<lang->curve_count && strcmp(lang->curves[i].name,name)) ++i;
    if (i == (int)count_of(lang->curves)) return luaL_error(L,"too many curves");
    w_copy(L,lang->curves[i].name,sizeof(lang->curves[i].name),name);
    lang->curves[i].curve = (w_curve){p[0],p[1],p[2],p[3]};
    if (i == lang->curve_count) ++lang->curve_count;
    return 0;
}

static int w_animation(lua_State *L) {
    w_declaration(L,1,1);
    w_fields(L,1,"leaf enabled speed bezier style");
    const char *leaf = w_field(L,1,"leaf",NULL);
    const char *curve = w_field(L,1,"bezier","default"), *style = w_field(L,1,"style","");
    bool enabled = w_flag(L,1,"enabled",true);
    lua_getfield(L,1,"speed");
    double speed = luaL_optnumber(L,-1,1);
    if (!isfinite(speed) || !w_config_language_animation(w_state(L)->language,leaf,enabled,speed,curve,style))
        return luaL_error(L,"unsupported animation '%s' or invalid timing/style",leaf);
    return 0;
}

static int w_rule(lua_State *L) {
    w_declaration(L,1,1);
    w_fields(L,1,"name match float workspace monitor border_color layout exclude");
    w_desktop_config *cfg = w_state(L)->language->config;
    if (cfg->rule_count >= rule_limit) return luaL_error(L,"too many window rules");
    (void)w_field(L,1,"name","");
    w_window_rule r = {.workspace=-1,.output=-1,.layout=-1};
    lua_getfield(L,1,"match");
    w_fields(L,2,"class title executable");
    const char *match[] = {"class","title"};
    bool filtered = false;
    for (int j=0; j<2; ++j) {
        if (!w_has(L,2,match[j])) continue;
        const char *pattern = w_field(L,2,match[j],NULL);
        char literal[512];
        w_copy(L,literal,sizeof(literal),pattern);
        if (strcmp(literal,".*") && !w_config_literal_filter(literal,j ? r.title : r.class_name,
                                                              j ? sizeof(r.title) : sizeof(r.class_name)))
            return luaL_error(L,"unsupported %s match '%s'",match[j],pattern);
        filtered = true;
    }
    w_copy(L,r.executable,sizeof(r.executable),w_field(L,2,"executable",""));
    if (!filtered && !*r.executable) return luaL_error(L,"window rule needs a match");
    r.sets_floating = w_has(L,1,"float");
    r.floating = w_flag(L,1,"float",false);
    r.exclude = w_flag(L,1,"exclude",false);
    if (w_has(L,1,"workspace")) {
        lua_getfield(L,1,"workspace");
        const char *ws = w_scalar(L,-1);
        char *end;
        long n = strtol(ws,&end,10);
        if (end == ws || n < 1 || n > workspace_limit || (*end && strcmp(end," silent")))
            return luaL_error(L,"invalid rule workspace");
        r.workspace = (int)n-1;
        lua_pop(L,1);
    }
    r.output = w_int_field(L,1,"monitor",0,output_limit-1,-1);
    if (w_has(L,1,"layout")) {
        const char *layouts[] = {"tree","alternate","master","tabs","stack"};
        const char *layout = w_field(L,1,"layout",NULL);
        for (size_t i=0; i<count_of(layouts); ++i) if (!strcmp(layouts[i],layout)) r.layout = (int)i;
        if (r.layout < 0) return luaL_error(L,"unsupported rule layout");
    }
    if (w_has(L,1,"border_color")) {
        lua_getfield(L,1,"border_color");
        if (lua_isinteger(L,-1)) {
            lua_Integer color = lua_tointeger(L,-1);
            if (color < 0 || (lua_Unsigned)color > UINT32_MAX) return luaL_error(L,"invalid border color");
            r.border = (uint32_t)color & 0xffffff;
        } else if (!w_config_color(w_string(L,-1),&r.border)) return luaL_error(L,"invalid border color");
        r.colored = true;
    }
    if (!r.sets_floating && !r.exclude && r.workspace == -1 && r.output == -1 && r.layout == -1 && !r.colored)
        return luaL_error(L,"window rule needs an effect");
    cfg->rules[cfg->rule_count++] = r;
    return 0;
}

static int w_apps(lua_State *L) {
    w_declaration(L,1,1);
    w_desktop_config *cfg = w_state(L)->language->config;
    const char *names[app_limit];
    int count = w_names(L,1,names,count_of(names));
    for (int j=0; j<count; ++j) {
        const char *name = names[j];
        lua_getfield(L,1,name);
        const char *path = w_string(L,-1);
        int i=0;
        while (i < cfg->app_count && strcmp(cfg->apps[i].name,name)) ++i;
        if (i == app_limit || !*name || !*path) return luaL_error(L,"invalid app '%s' or too many apps",name);
        w_copy(L,cfg->apps[i].name,sizeof(cfg->apps[i].name),name);
        w_copy(L,cfg->apps[i].path,sizeof(cfg->apps[i].path),path);
        if (i == cfg->app_count) ++cfg->app_count;
        lua_pop(L,1);
    }
    return 0;
}

static int w_providers(lua_State *L) {
    w_declaration(L,1,1);
    w_desktop_config *cfg = w_state(L)->language->config;
    const char *names[provider_limit];
    int count = w_names(L,1,names,count_of(names));
    for (int j=0; j<count; ++j) {
        const char *name = names[j];
        lua_getfield(L,1,name);
        int i=0;
        while (i < cfg->provider_count && strcmp(cfg->providers[i].name,name)) ++i;
        if (i == provider_limit || !*name) return luaL_error(L,"invalid or excessive provider");
        for (const char *p=name; *p; ++p)
            if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-') return luaL_error(L,"invalid provider name");
        w_fields(L,-1,"executable interval timeout stream action");
        w_provider_config entry = i == cfg->provider_count ? (w_provider_config){.timeout=10} : cfg->providers[i];
        w_copy(L,entry.name,sizeof(entry.name),name);
        if (w_has(L,-1,"executable")) w_copy(L,entry.executable,sizeof(entry.executable),w_field(L,-1,"executable",NULL));
        entry.interval = w_int_field(L,-1,"interval",0,86400,entry.interval);
        entry.timeout = w_int_field(L,-1,"timeout",1,86400,entry.timeout);
        entry.stream = w_flag(L,-1,"stream",entry.stream);
        if (w_has(L,-1,"action")) w_copy(L,entry.action,sizeof(entry.action),w_field(L,-1,"action",NULL));
        w_command checked;
        if (!*entry.executable || (entry.stream && entry.interval) || (*entry.action && !w_command_parse(entry.action,&checked)))
            return luaL_error(L,"invalid provider '%s'",name);
        cfg->providers[i] = entry;
        if (i == cfg->provider_count) ++cfg->provider_count;
        lua_pop(L,1);
    }
    return 0;
}

static int w_workspaces(lua_State *L) {
    w_declaration(L,1,1);
    luaL_checktype(L,1,LUA_TTABLE);
    w_desktop_config *cfg = w_state(L)->language->config;
    lua_pushnil(L);
    while (lua_next(L,1)) {
        if (!lua_isinteger(L,-2)) return luaL_error(L,"expected a workspace number");
        int slot = w_integer(L,-2,1,workspace_limit)-1;
        const char *name = w_string(L,-1);
        if (isdigit((unsigned char)*name)) return luaL_error(L,"workspace names cannot begin with digits");
        w_copy(L,cfg->workspace_names[slot],sizeof(cfg->workspace_names[slot]),name);
        lua_pop(L,1);
    }
    for (int i=0; i<workspace_limit; ++i) for (int j=0; j<i; ++j)
        if (*cfg->workspace_names[i] && !strcmp(cfg->workspace_names[i],cfg->workspace_names[j]))
            return luaL_error(L,"duplicate workspace name");
    return 0;
}

static int w_source(lua_State *L) {
    w_args(L,1,1);
    w_config_language *lang = w_state(L)->language;
    char err[512] = {0};
    const char *path = w_string(L,1);
    if (!*path || !lang->source || !lang->source(lang,path,err,sizeof(err)))
        return luaL_error(L,"source '%s': %s",path,*err ? err : "cannot load");
    return 0;
}

static char module_cache;

static int w_require(lua_State *L) {
    w_args(L,1,1);
    const char *name = w_string(L,1);
    char path[512], err[512] = {0};
    size_t n = strlen(name);
    if (!n || n+4 >= sizeof(path)) return luaL_error(L,"invalid module name");
    for (size_t i=0; i<n; ++i) {
        unsigned char c = (unsigned char)name[i];
        if (c == '.') {
            if (!i || i+1 == n || name[i-1] == '.') return luaL_error(L,"invalid module name");
            path[i] = '/';
        } else if (isalnum(c) || c == '_' || c == '-' || c >= 128) path[i] = (char)c;
        else return luaL_error(L,"invalid module name");
    }
    memcpy(path+n,".lua",5);
    lua_rawgetp(L,LUA_REGISTRYINDEX,&module_cache);
    lua_getfield(L,2,name);
    if (lua_touserdata(L,-1) == &module_cache) return luaL_error(L,"cyclic require '%s'",name);
    if (lua_toboolean(L,-1)) return 1;
    lua_pop(L,1);
    lua_pushlightuserdata(L,&module_cache);
    lua_setfield(L,2,name);
    w_config_language *lang = w_state(L)->language;
    lang->module = true;
    bool ok = lang->source && lang->source(lang,path,err,sizeof(err));
    lang->module = false;
    if (!ok || lua_gettop(L) != 3) {
        lua_pushnil(L);
        lua_setfield(L,2,name);
        return luaL_error(L,"module '%s': %s",name,*err ? err : "cannot load");
    }
    if (lua_isnil(L,-1)) { lua_pop(L,1); lua_pushboolean(L,1); }
    lua_pushvalue(L,-1);
    lua_setfield(L,2,name);
    return 1;
}

static int w_start_command(lua_State *L) {
    w_args(L,1,1);
    w_set(L,w_state(L)->once ? "exec-once" : "exec",w_string(L,1));
    return 0;
}

static int w_on(lua_State *L) {
    w_args(L,2,2);
    if (w_state(L)->once || strcmp(w_string(L,1),"hyprland.start")) return luaL_error(L,"unsupported startup event");
    luaL_checktype(L,2,LUA_TFUNCTION);
    w_state(L)->once = true;
    int status = lua_pcall(L,0,0,0);
    w_state(L)->once = false;
    if (status != LUA_OK) return lua_error(L);
    return 0;
}

static void w_register_text(lua_State *L, const char *key, lua_CFunction fn, const char *upvalue) {
    lua_pushstring(L,upvalue);
    lua_pushcclosure(L,fn,1);
    lua_setfield(L,-2,key);
}

static void w_register_direction(lua_State *L, const char *key, bool move) {
    lua_pushboolean(L,move);
    lua_pushcclosure(L,w_move_focus,1);
    lua_setfield(L,-2,key);
}

static int w_call_library(lua_State *L) {
    int argc = lua_gettop(L);
    lua_pushvalue(L,lua_upvalueindex(1));
    lua_insert(L,1);
    lua_call(L,argc,LUA_MULTRET);
    return lua_gettop(L);
}

static int w_protected(lua_State *L) {
    int results = w_call_library(L);
    // A caught error may be handled by the profile. Exhausting the shared VM
    // budget terminates the complete evaluation, even across nested pcall.
    w_charge(L,1);
    return results;
}

static void w_native_cost(lua_State *L, double work) {
    // Charge native library work in batches. The ordinary instruction hook
    // cannot interrupt a pattern matcher or a table operation running in C.
    if (work > 16000000.) luaL_error(L,"operation exceeds the native work limit");
    w_charge(L,1+(int)(work/16.));
}

static int w_repeat(lua_State *L) {
    lua_Integer n = luaL_checkinteger(L,2);
    // The standard implementation still loops n times for an empty result.
    // Return that result directly for arbitrarily large repetition counts.
    size_t len, separator;
    luaL_checklstring(L,1,&len);
    luaL_optlstring(L,3,"",&separator);
    if (!len && (!separator || n <= 1)) { lua_pushliteral(L,""); return 1; }
    if (n > 0) w_native_cost(L,(double)n*(1.+len+separator));
    return w_call_library(L);
}

static int w_table_move(lua_State *L) {
    lua_Integer first = luaL_checkinteger(L,2), last = luaL_checkinteger(L,3);
    if (last >= first) w_native_cost(L,(double)last-(double)first+1.);
    return w_call_library(L);
}

static int w_pattern(lua_State *L) {
    size_t bytes, length;
    luaL_checklstring(L,1,&bytes);
    const char *pattern = luaL_checklstring(L,2,&length);
    bool find = lua_toboolean(L,lua_upvalueindex(2)) != 0;
    double per_attempt = (double)length+1., choices = 1.;
    if (lua_toboolean(L,lua_upvalueindex(4)) && lua_type(L,3) == LUA_TSTRING) per_attempt += lua_rawlen(L,3);
    bool plain = find && lua_toboolean(L,4);
    // For k variable-length repetitions over n input bytes, the possible
    // ordered partitions (including empty matches) are bounded by C(n+k,k).
    unsigned repetitions = 0;
    if (!plain) {
        for (size_t i=0; i<length; ++i) {
            char c = pattern[i];
            if (c == '%' && i+1 < length) {
                char escaped = pattern[++i];
                if (escaped == 'b' || isdigit((unsigned char)escaped)) per_attempt += bytes;
                if (escaped == 'b' && length-i > 2) i += 2;
            } else if (c == '[') {
                size_t j = i+1;
                if (j < length && pattern[j] == '^') ++j;
                while (j < length) {
                    if (pattern[j] == '%' && j+1 < length) ++j;
                    ++j;
                    if (j < length && pattern[j] == ']') break;
                }
                i = j;
            } else if (c == '*' || c == '+' || c == '-') {
                ++repetitions;
                choices *= ((double)bytes+repetitions)/repetitions;
            } else if (c == '?') choices *= 2.;
            if (choices*per_attempt > 16000000.) break;
        }
    }
    // Non-anchored searches try each input position. gmatch treats '^'
    // literally, so its iterator always takes the unanchored bound.
    bool anchors = !plain && length && *pattern == '^' && !lua_toboolean(L,lua_upvalueindex(3));
    if (!anchors) choices *= (double)bytes+1.;
    w_native_cost(L,choices*per_attempt);
    return w_call_library(L);
}

static void w_wrap(lua_State *L, const char *name, lua_CFunction wrapper) {
    lua_getfield(L,-1,name);
    lua_pushboolean(L,!strcmp(name,"find"));
    lua_pushboolean(L,!strcmp(name,"gmatch"));
    lua_pushboolean(L,!strcmp(name,"gsub"));
    lua_pushcclosure(L,wrapper,4);
    lua_setfield(L,-2,name);
}

static int w_chunk(lua_State *L) {
    w_lua *chunk = lua_touserdata(L,1);
    const char *name = lua_pushfstring(L,"@%s",chunk->label);
    if (luaL_loadbufferx(L,chunk->text,strlen(chunk->text),name,"t") != LUA_OK) return lua_error(L);
    lua_call(L,0,1);
    return 1;
}

static int w_run(lua_State *L) {
    static const luaL_Reg api[] = {{"config",w_config},{"curve",w_curve_config},{"animation",w_animation},
        {"bind",w_bind},{"unbind",w_unbind},{"define_submap",w_submap},{"window_rule",w_rule},{"source",w_source},
        {"on",w_on},{"exec_cmd",w_start_command},{NULL,NULL}};
    static const luaL_Reg dsp[] = {{"winwm",w_native},{"exec_cmd",w_exec_dispatch},
        {"layout",w_layout},{"submap",w_submap_dispatch},{NULL,NULL}};
    static const luaL_Reg window[] = {{"close",w_close_window},{"drag",w_drag},{"resize",w_resize},{NULL,NULL}};
    static const luaL_Reg native[] = {{"apps",w_apps},{"providers",w_providers},{"workspaces",w_workspaces},{NULL,NULL}};
    luaL_requiref(L,LUA_GNAME,luaopen_base,1);
    const char *blocked[] = {"collectgarbage","dofile","load","loadfile","getmetatable","setmetatable","print","warn"};
    for (size_t i=0; i<count_of(blocked); ++i) { lua_pushnil(L); lua_setfield(L,-2,blocked[i]); }
    w_wrap(L,"pcall",w_protected);
    w_wrap(L,"xpcall",w_protected);
    lua_pop(L,1);
    lua_newtable(L);
    lua_rawsetp(L,LUA_REGISTRYINDEX,&module_cache);
    lua_pushcfunction(L,w_require);
    lua_setglobal(L,"require");
    luaL_requiref(L,LUA_MATHLIBNAME,luaopen_math,1); lua_pop(L,1);
    luaL_requiref(L,LUA_UTF8LIBNAME,luaopen_utf8,1); lua_pop(L,1);
    luaL_requiref(L,LUA_TABLIBNAME,luaopen_table,1);
    w_wrap(L,"move",w_table_move);
    lua_pop(L,1);
    luaL_requiref(L,LUA_STRLIBNAME,luaopen_string,1);
    const char *patterns[] = {"find","match","gmatch","gsub"};
    for (size_t i=0; i<count_of(patterns); ++i) w_wrap(L,patterns[i],w_pattern);
    w_wrap(L,"rep",w_repeat);
    lua_pop(L,1);
    luaL_newlib(L,native); lua_setglobal(L,"winwm");
    luaL_newlib(L,api);
    luaL_newlib(L,dsp);
    w_register_direction(L,"focus",false);
    w_register_text(L,"reload_config",w_simple,"reload");
    w_register_text(L,"exit",w_simple,"quit");
    luaL_newlib(L,window);
    w_register_text(L,"fullscreen",w_toggle,"fullscreen");
    w_register_text(L,"float",w_toggle,"floating");
    w_register_direction(L,"move",true);
    lua_setfield(L,-2,"window");
    lua_setfield(L,-2,"dsp");
    lua_setglobal(L,"hl");
    w_lua *ctx = w_state(L);
    lua_pushcfunction(L,w_chunk);
    lua_pushlightuserdata(L,ctx);
    lua_call(L,1,0);
    return 0;
}

bool w_lua_config_parse(w_config_language *language, const char *text, const char *label,
                        char *error, size_t capacity) {
    w_lua ctx = {.language=language,.text=text,.label=label ? label : "config",.budget=1000000};
    bool resize = language->resize, module = language->module, first = !language->runtime;
    lua_State *L = first ? lua_newstate(w_allocate,&ctx) : language->runtime;
    if (!L) { snprintf(error,capacity,"cannot allocate Lua runtime"); return false; }
    if (!lua_checkstack(L,3)) { snprintf(error,capacity,"Lua stack exhausted"); if (first) lua_close(L); return false; }
    language->module = false;
    if (first) {
        *(w_lua **)lua_getextraspace(L) = &ctx;
        lua_sethook(L,w_hook,LUA_MASKCOUNT,1000);
        language->runtime = L;
        lua_pushcfunction(L,w_run);
    } else {
        lua_pushcfunction(L,w_chunk);
        lua_pushlightuserdata(L,&ctx);
    }
    bool ok = lua_pcall(L,!first,module ? 1 : 0,0) == LUA_OK;
    if (!ok) {
        const char *message = lua_type(L,-1) == LUA_TSTRING ? lua_tostring(L,-1) : NULL;
        snprintf(error,capacity,"%s",message ? message : "Lua configuration error");
        lua_pop(L,1);
    }
    if (first) { lua_close(L); language->runtime = NULL; }
    language->resize = resize;
    language->module = module;
    return ok;
}
