#define COBJMACROS
#include "catalog/metadata.h"
#include <propkey.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <stdio.h>
#include <winver.h>

// Optional properties exposed by the Windows All Tasks and AppsFolder providers.
static const PROPERTYKEY task_command = {{0x13789837,0x147c,0x4321,{0xb3,0xf0,0x9d,0xe0,0xa0,0x69,0x71,0xd5}},2};
static const PROPERTYKEY package_install_path = {{0x9f4c2855,0x9f79,0x4b39,{0xa8,0xd0,0xe1,0xd4,0x2d,0xe1,0xd5,0xf3}},15};

void w_catalog_caption(wchar_t *out, int cap, const wchar_t *name, const wchar_t *process) {
    int len = (int)wcslen(process);
    if (!len || !_wcsicmp(name,process) || len+4 >= cap) {
        lstrcpynW(out,name,cap);
        return;
    }
    lstrcpynW(out,name,cap-len-3);
    size_t end = wcslen(out);
    if (end && IS_HIGH_SURROGATE(out[end-1])) out[end-1]=0;
    wcscat(out,L" (");
    wcscat(out,process);
    wcscat(out,L")");
}

static IXmlReader *w_open_xml(const wchar_t *path) {
    IStream *stream=NULL;
    IXmlReader *xml=NULL;
    if (SUCCEEDED(SHCreateStreamOnFileEx(path,STGM_READ|STGM_SHARE_DENY_NONE,0,FALSE,NULL,&stream))) {
        if (SUCCEEDED(CreateXmlReader(&IID_IXmlReader,(void **)&xml,NULL)) &&
            FAILED(IXmlReader_SetInput(xml,(IUnknown *)stream))) { IXmlReader_Release(xml); xml=NULL; }
        IStream_Release(stream);
    }
    return xml;
}

static bool w_xml_attribute(IXmlReader *xml, const wchar_t *name, LPCWSTR *value) {
    return IXmlReader_MoveToAttributeByName(xml,name,NULL)==S_OK && SUCCEEDED(IXmlReader_GetValue(xml,value,NULL));
}

static void w_console_title(const wchar_t *path, wchar_t *out, int cap) {
    IXmlReader *xml=w_open_xml(path);
    if (!xml) return;
    // MMC stores the ApplicationTitle as a reference into the console's string table.
    wchar_t title_id[16]=L"";
    XmlNodeType type;
    while (IXmlReader_Read(xml,&type)==S_OK) {
        LPCWSTR value;
        UINT depth;
        if (type!=XmlNodeType_Element || FAILED(IXmlReader_GetLocalName(xml,&value,NULL)) ||
            wcscmp(value,L"String") || FAILED(IXmlReader_GetDepth(xml,&depth))) continue;
        if (depth==2 && w_xml_attribute(xml,L"Name",&value) && !wcscmp(value,L"ApplicationTitle") &&
            w_xml_attribute(xml,L"ID",&value)) lstrcpynW(title_id,value,count_of(title_id));
        if (depth==4 && *title_id && w_xml_attribute(xml,L"ID",&value) && !wcscmp(value,title_id) &&
            IXmlReader_Read(xml,&type)==S_OK && type==XmlNodeType_Text && SUCCEEDED(IXmlReader_GetValue(xml,&value,NULL))) {
            lstrcpynW(out,value,cap);
            break;
        }
    }
    IXmlReader_Release(xml);
}

void w_catalog_file_description(const wchar_t *path, wchar_t *out, int cap) {
    const wchar_t *ext=PathFindExtensionW(path);
    if (!_wcsicmp(ext,L".msc")) { w_console_title(path,out,cap); return; }
    if (_wcsicmp(ext,L".exe") && _wcsicmp(ext,L".com") && _wcsicmp(ext,L".cpl")) return;
    DWORD size=GetFileVersionInfoSizeW(path,NULL);
    void *version=size ? malloc(size) : NULL;
    if (!version) return;
    struct translation { WORD language, codepage; } *tables=NULL;
    UINT bytes=0;
    if (GetFileVersionInfoW(path,0,size,version) &&
        VerQueryValueW(version,L"\\VarFileInfo\\Translation",(void **)&tables,&bytes)) {
        for (size_t i=0; i<bytes/sizeof(*tables); ++i) {
            wchar_t key[64], *description=NULL;
            UINT length;
            swprintf(key,count_of(key),L"\\StringFileInfo\\%04x%04x\\FileDescription",tables[i].language,tables[i].codepage);
            if (VerQueryValueW(version,key,(void **)&description,&length) && length && *description) {
                lstrcpynW(out,description,cap);
                break;
            }
        }
    }
    free(version);
}

static void w_keyword(w_catalog_metadata *meta, const wchar_t *word) {
    size_t end=wcslen(meta->keywords), len=wcslen(word);
    if (!len || end+len+1 >= count_of(meta->keywords)) return;
    if (end) meta->keywords[end++]=L' ';
    wcscpy(meta->keywords+end,word);
    CharLowerBuffW(meta->keywords+end,(DWORD)len);
}

static bool w_process_file(const wchar_t *path, w_catalog_metadata *meta) {
    if (PathIsURLW(path)) return false;
    const wchar_t *ext=PathFindExtensionW(path);
    if (_wcsicmp(ext,L".exe") && _wcsicmp(ext,L".com") && _wcsicmp(ext,L".cmd") && _wcsicmp(ext,L".bat"))
        return false;
    lstrcpynW(meta->process,PathFindFileNameW(path),count_of(meta->process));
    return true;
}

static bool w_module_keyword(wchar_t *argument, w_catalog_metadata *meta) {
    wchar_t *file=PathFindFileNameW(argument);
    file[wcscspn(file,L",")]=0;
    const wchar_t *ext=PathFindExtensionW(file);
    if (_wcsicmp(ext,L".cpl") && _wcsicmp(ext,L".msc") && _wcsicmp(ext,L".dll")) return false;
    w_keyword(meta,file);
    return true;
}

static void w_shortcut_metadata(IShellItem2 *item, w_catalog_metadata *meta) {
    PWSTR path=NULL, arguments=NULL;
    if (SUCCEEDED(IShellItem2_GetString(item,&PKEY_Link_TargetParsingPath,&path)) && path && !PathIsURLW(path) &&
        !w_process_file(path,meta) && w_module_keyword(path,meta)) {
        wchar_t host[os_path_cap];
        DWORD cap=count_of(host);
        if (AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN,ASSOCSTR_EXECUTABLE,PathFindExtensionW(path),L"open",host,&cap)==S_OK)
            w_process_file(host,meta);
    }
    if (SUCCEEDED(IShellItem2_GetString(item,&PKEY_Link_Arguments,&arguments)) && arguments && *arguments) {
        int count=0;
        wchar_t **args=CommandLineToArgvW(arguments,&count);
        if (args) {
            for (int i=0; i<count; ++i) w_module_keyword(args[i],meta);
            LocalFree(args);
        }
    }
    CoTaskMemFree(arguments);
    CoTaskMemFree(path);
}

static void w_command_process(const wchar_t *line, w_catalog_metadata *meta) {
    int count=0;
    wchar_t **args=CommandLineToArgvW(line,&count);
    if (!args) return;
    if (count && w_process_file(args[0],meta)) {
        // Hosted applets and consoles have their own searchable module name.
        for (int i=1; i<count; ++i) w_module_keyword(args[i],meta);
    }
    LocalFree(args);
}

void w_catalog_program_caption(wchar_t *out, int cap, const wchar_t *name, const char *program, bool command_line) {
    wchar_t path[os_path_cap];
    w_catalog_metadata meta={0};
    if (w_os_wide(program,path,count_of(path))) {
        if (command_line) w_command_process(path,&meta);
        else w_process_file(path,&meta);
    }
    w_catalog_caption(out,cap,name,meta.process);
}

// A package can declare several applications. Match the Application Id in the AUMID.
static void w_package_process(IShellItem2 *item, const wchar_t *id, w_catalog_metadata *meta) {
    PWSTR directory=NULL;
    wchar_t path[os_path_cap];
    IXmlReader *xml=NULL;
    if (SUCCEEDED(IShellItem2_GetString(item,&package_install_path,&directory)) && directory &&
        swprintf(path,count_of(path),L"%ls\\AppxManifest.xml",directory)>0 &&
        (xml=w_open_xml(path))) {
        XmlNodeType type;
        while (IXmlReader_Read(xml,&type)==S_OK) {
            LPCWSTR value;
            UINT depth;
            if (type!=XmlNodeType_Element || FAILED(IXmlReader_GetDepth(xml,&depth)) || depth!=2 ||
                FAILED(IXmlReader_GetLocalName(xml,&value,NULL)) || wcscmp(value,L"Application") ||
                !w_xml_attribute(xml,L"Id",&value) || _wcsicmp(id,value)) continue;
            if (w_xml_attribute(xml,L"Executable",&value)) w_process_file(value,meta);
            break;
        }
    }
    if (xml) IXmlReader_Release(xml);
    CoTaskMemFree(directory);
}

void w_catalog_shell_metadata(IShellItem2 *item, const wchar_t *app_id, bool task, w_catalog_metadata *meta) {
    if (task) {
        PWSTR path=NULL;
        if (SUCCEEDED(IShellItem2_GetString(item,&task_command,&path)) && path)
            w_command_process(path,meta);
        CoTaskMemFree(path);
    } else {
        w_shortcut_metadata(item,meta);
        if (app_id && !*meta->process) w_package_process(item,wcschr(app_id,L'!')+1,meta);
    }
    PROPVARIANT keywords={0};
    if (SUCCEEDED(IShellItem2_GetProperty(item,&PKEY_Keywords,&keywords)) &&
        keywords.vt==(VT_VECTOR|VT_LPWSTR)) {
        for (ULONG i=0; i<keywords.calpwstr.cElems; ++i) w_keyword(meta,keywords.calpwstr.pElems[i]);
    }
    PropVariantClear(&keywords);
}
