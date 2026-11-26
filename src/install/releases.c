#include "install/releases.h"
#include "base/json.h"
#include <bcrypt.h>
#include <winhttp.h>
#include <stdlib.h>
#include <stdio.h>

#define release_api L"/repos/ougi-washi/winwm/releases"
enum { metadata_limit = 8*1024*1024, executable_limit = 128*1024*1024, page_size = 30 };

// Asset IDs are taken from this repository's metadata. WinHTTP follows GitHub's
// signed HTTPS redirect and rejects HTTPS-to-HTTP redirects by default.
static DWORD w_fetch(const wchar_t *path, bool binary, DWORD limit, HANDLE cancel,
                     BYTE **body, DWORD *length) {
    HINTERNET session = WinHttpOpen(L"winwm-release-manager",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if (!session) return GetLastError();
    WinHttpSetTimeouts(session,10000,15000,15000,30000);
    HINTERNET server = WinHttpConnect(session,L"api.github.com",INTERNET_DEFAULT_HTTPS_PORT,0);
    HINTERNET request = server ? WinHttpOpenRequest(server,L"GET",path,NULL,WINHTTP_NO_REFERER,
                                                     WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE) : NULL;
    const wchar_t *headers = binary ? L"Accept: application/octet-stream" : L"Accept: application/vnd.github+json";
    DWORD error = ERROR_SUCCESS, status = 0, size = sizeof(status);
    if (!request || !WinHttpSendRequest(request,headers,(DWORD)-1,NULL,0,0,0) ||
        !WinHttpReceiveResponse(request,NULL) ||
        !WinHttpQueryHeaders(request,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
                             NULL,&status,&size,NULL)) error = GetLastError();
    else if (status != 200)
        error = status == 404 ? ERROR_NOT_FOUND : status == 403 || status == 429 ? ERROR_RETRY :
                ERROR_WINHTTP_INVALID_SERVER_RESPONSE;
    BYTE *data = NULL, block[32768];
    DWORD used = 0, capacity = 0;
    while (!error) {
        DWORD n;
        if (WaitForSingleObject(cancel,0) == WAIT_OBJECT_0) error = ERROR_CANCELLED;
        else if (!WinHttpReadData(request,block,sizeof(block),&n)) error = GetLastError();
        else if (!n) break;
        else if (n > limit-used) error = ERROR_FILE_TOO_LARGE;
        else {
            if (used+n > capacity) {
                capacity = min(limit,max(used+n,capacity*2));
                BYTE *more = realloc(data,(size_t)capacity+1);
                if (!more) { error = ERROR_OUTOFMEMORY; break; }
                data = more;
            }
            memcpy(data+used,block,n);
            used += n;
        }
    }
    if (request) WinHttpCloseHandle(request);
    if (server) WinHttpCloseHandle(server);
    WinHttpCloseHandle(session);
    if (!error && !used) error = ERROR_INVALID_DATA;
    if (error) free(data);
    else {
        data[used] = 0;
        *body = data;
        *length = used;
    }
    return error;
}

static const w_json_token *w_member(const w_json_doc *doc, const w_json_token *object, const char *name) {
    if (object && object->type == json_object)
        for (const w_json_token *key = object+1; key < doc->tokens+object->after; key = doc->tokens+key[1].after)
            if (key->length == strlen(name) && !memcmp(key->text,name,key->length)) return key+1;
    return NULL;
}

static bool w_token_is(const w_json_token *t, enum json_type type, const char *value) {
    return t && t->type == type && t->length == strlen(value) && !memcmp(t->text,value,t->length);
}

static unsigned long long w_number(const w_json_token *t) {
    if (!t || t->type != json_scalar || !t->length || t->length > 19 ||
        t->length != strspn(t->text,"0123456789")) return 0;
    return strtoull(t->text,NULL,10);
}

static DWORD w_add_release(w_release_catalog *catalog, const w_json_doc *doc,
                           const w_json_token *object, bool latest) {
    if (!w_token_is(w_member(doc,object,"draft"),json_scalar,"false")) return ERROR_SUCCESS;
    const w_json_token *assets = w_member(doc,object,"assets"), *exe = NULL;
    if (!assets || assets->type != json_array) return ERROR_INVALID_DATA;
    for (const w_json_token *a = assets+1; a < doc->tokens+assets->after; a = doc->tokens+a->after) {
        const w_json_token *name = w_member(doc,a,"name");
        if (w_token_is(name,json_string,"winwm_recovery.exe")) return ERROR_SUCCESS;
        if (w_token_is(name,json_string,"winwm.exe")) exe = a;
    }
    if (!exe || !w_token_is(w_member(doc,exe,"state"),json_string,"uploaded")) return ERROR_SUCCESS;
    w_release item = {.latest=latest,.asset=w_number(w_member(doc,exe,"id")),
                      .prerelease=w_token_is(w_member(doc,object,"prerelease"),json_scalar,"true")};
    unsigned long long size = w_number(w_member(doc,exe,"size"));
    const w_json_token *tag = w_member(doc,object,"tag_name"), *digest = w_member(doc,exe,"digest");
    char tag_utf8[512], hash[72];
    if (!item.asset || !size || size > executable_limit || !tag || !w_json_read(tag,tag_utf8,sizeof(tag_utf8)) ||
        !*tag_utf8 || !w_os_wide(tag_utf8,item.tag,count_of(item.tag)) ||
        !digest || !w_json_read(digest,hash,sizeof(hash)) || strncmp(hash,"sha256:",7) ||
        strspn(hash+7,"0123456789abcdef") != 64) return ERROR_SUCCESS;
    item.size = (DWORD)size;
    strcpy(item.digest,hash+7);
    for (size_t i=0; i<catalog->count; ++i)
        if (catalog->items[i].asset == item.asset) return ERROR_SUCCESS;
    w_release *more = realloc(catalog->items,(catalog->count+1)*sizeof(item));
    if (!more) return ERROR_OUTOFMEMORY;
    catalog->items = more;
    more[catalog->count++] = item;
    return ERROR_SUCCESS;
}

static DWORD w_read_page(w_release_catalog *catalog, unsigned page, HANDLE cancel, bool *more) {
    wchar_t path[160];
    if (page) swprintf(path,count_of(path),release_api L"?per_page=%u&page=%u",page_size,page);
    else wcscpy(path,release_api L"/latest");
    BYTE *data;
    DWORD size, error = w_fetch(path,false,metadata_limit,cancel,&data,&size);
    if (error) return error;
    // A token starts at a distinct input byte. Grow only as needed for this page.
    int cap = min(size+1,4096u);
    w_json_doc doc;
    w_json_token *tokens = NULL;
    for (;;) {
        w_json_token *grown = realloc(tokens,(size_t)cap*sizeof(*tokens));
        if (!grown) { error = ERROR_OUTOFMEMORY; break; }
        tokens = grown;
        if (w_json_parse(&doc,(const char *)data,tokens,cap)) break;
        if (doc.count != cap || cap >= 262144) { error = ERROR_INVALID_DATA; break; }
        cap *= 2;
    }
    if (!error) {
        if (!page && tokens->type == json_object) error = w_add_release(catalog,&doc,tokens,true);
        else if (page && tokens->type == json_array) {
            unsigned count = 0;
            for (int i=1; !error && i<tokens->after; i=tokens[i].after) {
                error = w_add_release(catalog,&doc,tokens+i,false);
                ++count;
            }
            *more = count == page_size;
        } else error = ERROR_INVALID_DATA;
    }
    free(tokens);
    free(data);
    return error;
}

static DWORD w_digest(w_install_image image, char hex[65]) {
    BYTE hash[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE,NULL,0,(BYTE *)image.bytes,image.size,hash,sizeof(hash)) < 0)
        return ERROR_GEN_FAILURE;
    for (size_t i=0; i<sizeof(hash); ++i) sprintf(hex+2*i,"%02x",hash[i]);
    return ERROR_SUCCESS;
}

DWORD w_release_list(w_release_catalog *catalog, HANDLE cancel) {
    wchar_t current[os_path_cap];
    DWORD error = w_install_current(current);
    if (error) return error;
    catalog->startup = !!*current;
    if (*current) {
        w_install_image image;
        if (!w_install_read(current,&image)) {
            w_digest(image,catalog->installed);
            UnmapViewOfFile(image.bytes);
        }
    }
    bool more = true;
    error = w_read_page(catalog,0,cancel,&more);
    if (error == ERROR_NOT_FOUND) error = ERROR_SUCCESS;
    for (unsigned page=1; !error && more; ++page)
        error = w_read_page(catalog,page,cancel,&more);
    return error;
}

DWORD w_release_apply(const w_release *release, HANDLE cancel) {
    wchar_t path[128];
    swprintf(path,count_of(path),release_api L"/assets/%llu",release->asset);
    BYTE *bytes;
    DWORD size, error = w_fetch(path,true,release->size,cancel,&bytes,&size);
    if (error) return error;
    w_install_image image = {bytes,size};
    char digest[65];
    error = w_digest(image,digest);
    if (!error && (size != release->size || strcmp(digest,release->digest))) error = ERROR_CRC;
    if (!error && WaitForSingleObject(cancel,0) == WAIT_OBJECT_0) error = ERROR_CANCELLED;
    if (!error) error = w_install_release(image);
    free(bytes);
    return error;
}
