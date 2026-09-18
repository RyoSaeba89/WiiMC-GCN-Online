#include "http_client.h"
#include "radio_stream.h"
#include "webdav_client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s (errno %d)\n", __LINE__, #x, errno); return 1; } } while (0)
static int stop_now(void *p) { (void)p; return 1; }
static int stop_soon(void *p) { return ++*(int *)p > 10; }
static void count_wait(void *p) { ++*(int *)p; }
static int read_all(gc_http *h, char *out, int cap)
{
    int total = 0, n;
    while (total < cap && (n = gc_http_read(h, out+total, cap-total)) > 0) total += n;
    return h->failed ? -1 : total;
}
int main(int argc, char **argv)
{
    CHECK(argc == 2);
    char base[128], url[GC_HTTP_URL_MAX], text[4096], error[160];
    gc_http_url parsed;
    gc_http *h = calloc(1, sizeof(*h));
    CHECK(h);
    snprintf(base, sizeof(base), "http://127.0.0.1:%s", argv[1]);
    CHECK(!gc_http_parse_url("https://example.org:8443/live?q=1#fragment", &parsed));
    CHECK(parsed.tls && parsed.port == 8443 && !strcmp(parsed.path,"/live?q=1"));
    CHECK(gc_http_parse_url("http://user:pass@host/", &parsed) < 0);
    CHECK(gc_http_parse_url("http://host:0/", &parsed) < 0);
    CHECK(gc_http_parse_url("http://host/\r\nX: bad", &parsed) < 0);
    CHECK(!gc_http_encode_path("/ete & 100%.mp3", text, sizeof(text)) && !strcmp(text,"/ete%20%26%20100%25.mp3"));
    CHECK(!gc_http_decode_path(text, url, sizeof(url)) && !strcmp(url,"/ete & 100%.mp3"));
    CHECK(gc_http_decode_path("/a%2fb", text, sizeof(text)) < 0);
    CHECK(gc_http_decode_path("/a%00b", text, sizeof(text)) < 0);
    CHECK(!gc_http_basic("user","pass",text,sizeof(text)) && !strcmp(text,"dXNlcjpwYXNz"));
    CHECK(!gc_http_resolve_url("http://host/dir/list?q=old","../stream",url,sizeof(url)));
    CHECK(!strcmp(url,"http://host:80/dir/../stream"));
    puts("PASS URL validation, escaping, Basic authentication");
    const char *good[] = {"/plain","/chunked","/redirect","/relative/start","/icy-status"};
    for (size_t i=0; i<sizeof(good)/sizeof(*good); ++i) {
        snprintf(url,sizeof(url),"%s%s",base,good[i]);
        CHECK(!gc_http_open(h,url,"GET",NULL,NULL,-1,-1,0,NULL,NULL));
        CHECK(read_all(h,text,sizeof(text)) == 10 && !memcmp(text,"0123456789",10));
        gc_http_close(h);
    }
    const char *bad[] = {"/loop","/negative-length","/duplicate-length","/ambiguous","/bad-range","/error","/encoding","/oversize-header"};
    for (size_t i=0; i<sizeof(bad)/sizeof(*bad); ++i) {
        snprintf(url,sizeof(url),"%s%s",base,bad[i]);
        CHECK(gc_http_open(h,url,"GET",NULL,NULL,-1,-1,0,NULL,NULL) < 0);
        CHECK(!h->conn && h->error[0]);
    }
    const char *truncated[] = {"/truncated","/bad-chunk","/short-chunk"};
    for (size_t i=0; i<sizeof(truncated)/sizeof(*truncated); ++i) {
        snprintf(url,sizeof(url),"%s%s",base,truncated[i]);
        CHECK(!gc_http_open(h,url,"GET",NULL,NULL,-1,-1,0,NULL,NULL));
        CHECK(read_all(h,text,sizeof(text)) < 0);
        gc_http_close(h);
    }
    snprintf(url,sizeof(url),"%s/cross-origin",base);
    CHECK(gc_http_open(h,url,"GET","dXNlcjpwYXNz",NULL,-1,-1,0,NULL,NULL) < 0);
    snprintf(url,sizeof(url),"%s/plain",base);
    CHECK(gc_http_open(h,url,"GET",NULL,NULL,-1,4,0,NULL,NULL) < 0);
    CHECK(gc_http_open(h,url,"GET",NULL,NULL,-1,-1,0,stop_now,NULL) < 0);
    int polls=0;
    snprintf(url,sizeof(url),"%s/silent",base);
    CHECK(gc_http_open(h,url,"GET",NULL,NULL,-1,-1,0,stop_soon,&polls) < 0);
    puts("PASS HTTP framing, redirects, malformed responses, ranges, cancellation");
    gc_radio *radio=calloc(1,sizeof(*radio));
    CHECK(radio);
    const char *stations[]={"/radio","/radio-chunked","/radio-long-title"};
    for(size_t i=0;i<3;++i) {
        snprintf(url,sizeof(url),"%s%s",base,stations[i]);
        CHECK(!gc_radio_open(radio,url,NULL,NULL));
        int n,total=0;
        while((n=gc_radio_read(radio,text+total,sizeof(text)-total))>0) total+=n;
        CHECK(n==0 && total==10 && !memcmp(text,"ABCDEFGHIJ",10));
        CHECK(radio->title_changed && strlen(radio->title)==(i==2 ? 127 : 14));
        if(i<2) CHECK(!strcmp(radio->title,"Artist - Track"));
        gc_radio_close(radio);
    }
    free(radio);
    puts("PASS ICY metadata across TCP and HTTP chunk boundaries, long titles");
    gc_dav_config c;
    CHECK(!gc_dav_config_load(&c,"tests/out/webdav.conf",error,sizeof(error)));
    CHECK(!strcmp(c.authorization,"dXNlcjpwYXNz"));
    gc_dav_entry *entries, stat;
    size_t count;
    CHECK(!gc_dav_list(&c,"/",&entries,&count,error,sizeof(error)));
    CHECK(count==3);
    int saw_file=0,saw_album=0;
    for(size_t i=0;i<count;++i) {
        if(!strcmp(entries[i].name,"ete & 100%.mp3")) saw_file=entries[i].size==10 && !entries[i].directory;
        if(!strcmp(entries[i].name,"Album")) saw_album=entries[i].directory;
    }
    CHECK(saw_file && saw_album); free(entries);
    CHECK(!gc_dav_list(&c,"/Album/",&entries,&count,error,sizeof(error)) && count==0); free(entries);
    int wait_calls=0;
    CHECK(!gc_dav_list_wait(&c,"/Album/",&entries,&count,count_wait,&wait_calls,error,sizeof(error)) && count==0);
    CHECK(wait_calls >= 3); free(entries);
    CHECK(!gc_dav_list(&c,"/Large/",&entries,&count,error,sizeof(error)) && count==1000);
    CHECK(!strcmp(entries[0].name,"Folder-0000") && !strcmp(entries[999].name,"Folder-0999")); free(entries);
    CHECK(gc_dav_list(&c,"/../",&entries,&count,error,sizeof(error)) < 0);
    CHECK(!gc_dav_stat(&c,"/ete & 100%.mp3",&stat,error,sizeof(error)) && stat.size==10);
    CHECK(gc_dav_stat(&c,"/missing.mp3",&stat,error,sizeof(error)) < 0 && errno==ENOENT);
    gc_dav_file file;
    for(int iteration=0;iteration<24;++iteration) {
        CHECK(!gc_dav_open(&file,&c,"/ete & 100%.mp3",error,sizeof(error)));
        CHECK(gc_dav_read(&file,text,3)==3 && !memcmp(text,"012",3));
        CHECK(gc_dav_seek(&file,7,SEEK_SET)==7);
        CHECK(gc_dav_read(&file,text,20)==3 && !memcmp(text,"789",3));
        CHECK(gc_dav_read(&file,text,20)==0);
        CHECK(gc_dav_seek(&file,-8,SEEK_END)==2);
        CHECK(gc_dav_read(&file,text,3)==3 && !memcmp(text,"234",3));
        CHECK(gc_dav_seek(&file,-6,SEEK_CUR)<0);
        CHECK(gc_dav_seek(&file,100,SEEK_SET)==100 && gc_dav_read(&file,text,2)==0);
        CHECK(gc_dav_seek(&file,INT64_MAX,SEEK_CUR)<0);
        gc_dav_close(&file);
    }
    CHECK(!gc_dav_open(&file,&c,"/empty.mp3",error,sizeof(error)));
    CHECK(gc_dav_read(&file,text,1)==0); gc_dav_close(&file);
    CHECK(gc_dav_open(&file,&c,"/Album/",error,sizeof(error))<0 && errno==EISDIR);
    free(h);
    puts("PASS DAV namespaces, escaped names, status filtering, read/seek/EOF, repeated opens");
    return 0;
}
