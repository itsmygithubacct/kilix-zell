/* Theater helper. Its own process group and memfd, never the station service.
 * Frames are copied only when seq is even, nonzero, and unchanged after the
 * copy, and the generation is the active one. use/close/open drop the picture
 * first so a late frame from the previous generation cannot stay on screen. */
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <signal.h>

#define TH_ITEMS 8
#define TH_LINE 8192
#define TH_QUEUE 4
#define TH_ACCEPT_NONE 0xffffffffu
#define TH_MAP_BYTES (64u+(size_t)2048*2048*4u)

typedef struct {
    pid_t pid;
    int in_fd,out_fd,frame_fd;
    void *map;
    uint32_t *shown,*scratch;
    size_t cap;
    uint32_t seq,accept,retired;
    int frame_w,frame_h,sent_w,sent_h;
    bool ready,stopping,hold,fullscreen,popout,menu,cc,paused;
    bool install_stored,discard;
    int focus,items,next_serial,q_len,install_argc,listing_n;
    char path[4096],url[2048],caption[256],error[180],notice[180],browser[16],active[33];
    char item_id[TH_ITEMS][33];
    char queue[TH_QUEUE][TH_LINE];
    char line[TH_LINE],last[TH_LINE];
    size_t line_n;
    char install_id[33],install_argv[8][512];
    char listing_name[8][64],listing_path[8][512];
} th_state;

static th_state th={.in_fd=-1,.out_fd=-1,.frame_fd=-1,.accept=TH_ACCEPT_NONE,.cc=true,.paused=true,.next_serial=1};

static uint32_t th_u32(const unsigned char *p){
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void th_set_error(const char *text){
    snprintf(th.error,sizeof th.error,"%s",text?text:"");
    th.notice[0]=0;
}
static void th_set_notice(const char *text){
    snprintf(th.notice,sizeof th.notice,"%s",text?text:"");
    th.error[0]=0;
}
/* Bounded copy. Ids are at most 32 characters; callers already checked. */
static void th_copy(char *dst,size_t cap,const char *src){
    size_t n=0;
    if(!dst||cap<1)return;
    if(src)while(src[n]&&n+1<cap){dst[n]=src[n];n++;}
    dst[n]=0;
}
static bool th_id_ok(const char *id){
    size_t n;
    if(!id)return false;
    n=strlen(id);
    if(n<1||n>32)return false;
    for(size_t i=0;i<n;i++){
        char ch=id[i];
        if(!((ch>='A'&&ch<='Z')||(ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='-'))return false;
    }
    return true;
}
static const char *th_key(const char *json,const char *key){
    char pat[64];
    const char *p;
    size_t n;
    if(!json||!key||strlen(key)>48)return NULL;
    snprintf(pat,sizeof pat,"\"%s\"",key);
    n=strlen(pat);
    for(p=json;(p=strstr(p,pat))!=NULL;p+=n){
        const char *q=p+n;
        while(*q==' '||*q=='\t'||*q=='\n'||*q=='\r')q++;
        if(*q==':'){
            q++;
            while(*q==' '||*q=='\t'||*q=='\n'||*q=='\r')q++;
            return q;
        }
    }
    return NULL;
}
static bool th_parse_string(const char **pp,char *dst,size_t cap){
    const char *p=*pp;
    size_t n=0;
    if(*p!='"')return false;
    p++;
    while(*p&&*p!='"'){
        unsigned char ch=(unsigned char)*p++;
        if(ch=='\\'){
            char esc=*p++;
            if(esc=='"'||esc=='\\'||esc=='/')ch=(unsigned char)esc;
            else if(esc=='n')ch='\n';
            else if(esc=='t')ch='\t';
            else if(esc=='u'){
                int value=0;
                for(int i=0;i<4;i++){
                    char h=*p++;
                    int d=h>='0'&&h<='9'?h-'0':h>='a'&&h<='f'?h-'a'+10:h>='A'&&h<='F'?h-'A'+10:-1;
                    if(d<0)return false;
                    value=value*16+d;
                }
                ch=(unsigned char)(value>255?'?':value);
            }else return false;
        }
        if(n+1>=cap)return false;
        dst[n++]=(char)ch;
    }
    if(*p!='"')return false;
    dst[n]=0;
    *pp=p+1;
    return true;
}
static bool th_json_string(const char *json,const char *key,char *dst,size_t cap){
    const char *p=th_key(json,key);
    if(!p)return false;
    return th_parse_string(&p,dst,cap);
}
static bool th_json_uint(const char *json,const char *key,uint32_t *out){
    const char *p=th_key(json,key);
    unsigned long value=0;
    if(!p||*p<'0'||*p>'9')return false;
    while(*p>='0'&&*p<='9'){
        value=value*10ul+(unsigned long)(*p-'0');
        if(value>0xfffffffful)return false;
        p++;
    }
    *out=(uint32_t)value;
    return true;
}
static int th_json_argv(const char *json,char argv[][512],int cap){
    const char *p=th_key(json,"argv");
    int n=0;
    if(!p||*p!='[')return -1;
    p++;
    while(*p&&*p!=']'){
        while(*p==' '||*p==','||*p=='\n'||*p=='\t'||*p=='\r')p++;
        if(*p==']')break;
        if(n>=cap||!th_parse_string(&p,argv[n],512))return -1;
        n++;
    }
    return n;
}
static void th_close_fds(void){
    if(th.map&&th.map!=MAP_FAILED){munmap(th.map,TH_MAP_BYTES);th.map=NULL;}
    if(th.frame_fd>=0){close(th.frame_fd);th.frame_fd=-1;}
    if(th.in_fd>=0){close(th.in_fd);th.in_fd=-1;}
    if(th.out_fd>=0){close(th.out_fd);th.out_fd=-1;}
}
static bool th_ensure_pixels(size_t n){
    uint32_t *fresh,*spare,*old_shown,*old_scratch;
    if(n<1)n=1;
    if(th.cap>=n&&th.shown&&th.scratch)return true;
    fresh=calloc(n,sizeof *fresh);
    spare=calloc(n,sizeof *spare);
    if(!fresh||!spare){free(fresh);free(spare);return false;}
    old_shown=th.shown;old_scratch=th.scratch;
    th.shown=fresh;th.scratch=spare;th.cap=n;
    free(old_shown);free(old_scratch);
    return true;
}
static void th_retire(void){
    th.hold=true;
    th.retired=th.accept;
    th.accept=TH_ACCEPT_NONE;
    th.seq=0;
    th.frame_w=th.frame_h=0;
    th.caption[0]=0;
}
static bool th_enqueue(const char *json){
    int n;
    if(!json||th.q_len>=TH_QUEUE)return false;
    n=snprintf(th.queue[th.q_len],TH_LINE,"%s\n",json);
    if(n<0||n>=TH_LINE)return false;
    th.q_len++;
    return true;
}
static void th_flush(void){
    while(th.q_len>0&&th.ready&&th.in_fd>=0){
        size_t n=strlen(th.queue[0]);
        ssize_t wrote=write(th.in_fd,th.queue[0],n);
        if(wrote<0){
            if(errno==EINTR)continue;
            if(errno==EAGAIN||errno==EWOULDBLOCK)return;
            th_set_error("The theater helper pipe closed.");
            return;
        }
        if((size_t)wrote<n){memmove(th.queue[0],th.queue[0]+wrote,n-(size_t)wrote+1);return;}
        if(th.q_len>1)memmove(th.queue[0],th.queue[1],(size_t)(th.q_len-1)*sizeof th.queue[0]);
        th.q_len--;
    }
}
static bool th_cmd_is(const char *json,const char *name){
    char pat[48];
    snprintf(pat,sizeof pat,"\"cmd\":\"%s\"",name);
    return json&&strstr(json,pat)!=NULL;
}
static bool th_script(char *dst,size_t cap,const char *name){
    ssize_t n=readlink("/proc/self/exe",dst,cap-48);
    char *slash;
    if(n<0||(size_t)n+48>=cap)return false;
    dst[n]=0;
    slash=strrchr(dst,'/');
    if(!slash)return false;
    snprintf(slash+1,(size_t)(dst+cap-(slash+1)),"tools/%s",name);
    return access(dst,R_OK)==0;
}
static bool th_start(zell_scene *s){
    char script[4096],fd_text[24];
    int to_child[2],from_child[2];
    (void)s;
    if(th.pid>0)return true;
    if(!th_script(script,sizeof script,"zell_theater.py")){
        th_set_error("Could not start the theater helper. tools/zell_theater.py is not beside this program.");
        return false;
    }
    if(!th_ensure_pixels((size_t)2048*2048)){th_set_error("Could not allocate the theater frame.");return false;}
    if(pipe(to_child)!=0){th_set_error("Could not start the theater helper.");return false;}
    if(pipe(from_child)!=0){close(to_child[0]);close(to_child[1]);th_set_error("Could not start the theater helper.");return false;}
    th.frame_fd=(int)syscall(SYS_memfd_create,"zell-theater",0);
    if(th.frame_fd<0||ftruncate(th.frame_fd,(off_t)TH_MAP_BYTES)!=0){
        if(th.frame_fd>=0)close(th.frame_fd);
        th.frame_fd=-1;
        close(to_child[0]);close(to_child[1]);close(from_child[0]);close(from_child[1]);
        th_set_error("Could not create the theater frame map.");
        return false;
    }
    th.map=mmap(NULL,TH_MAP_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,th.frame_fd,0);
    if(th.map==MAP_FAILED){th.map=NULL;th_close_fds();close(to_child[0]);close(to_child[1]);close(from_child[0]);close(from_child[1]);th_set_error("Could not map the theater frame.");return false;}
    memset(th.map,0,64);
    th.pid=fork();
    if(th.pid==0){
        if(setsid()<0)_exit(127);
        dup2(to_child[0],STDIN_FILENO);
        dup2(from_child[1],STDOUT_FILENO);
        int nullfd=open("/dev/null",O_WRONLY);
        if(nullfd>=0){dup2(nullfd,STDERR_FILENO);close(nullfd);}
        close(to_child[0]);close(to_child[1]);close(from_child[0]);close(from_child[1]);
        snprintf(fd_text,sizeof fd_text,"%d",th.frame_fd);
        setenv("ZELL_THEATER_FD",fd_text,1);
        signal(SIGINT,SIG_DFL);signal(SIGTERM,SIG_DFL);signal(SIGPIPE,SIG_DFL);
        execlp("python3","python3",script,(char *)NULL);
        _exit(127);
    }
    close(to_child[0]);close(from_child[1]);
    if(th.pid<0){
        close(to_child[1]);close(from_child[0]);
        th_close_fds();
        th_set_error("Could not start the theater helper.");
        return false;
    }
    th.in_fd=to_child[1];th.out_fd=from_child[0];
    fcntl(th.frame_fd,F_SETFD,FD_CLOEXEC);
    fcntl(th.in_fd,F_SETFL,fcntl(th.in_fd,F_GETFL)|O_NONBLOCK);
    fcntl(th.out_fd,F_SETFL,fcntl(th.out_fd,F_GETFL)|O_NONBLOCK);
    fcntl(th.in_fd,F_SETFD,FD_CLOEXEC);
    fcntl(th.out_fd,F_SETFD,FD_CLOEXEC);
    th.ready=false;th.stopping=false;th.q_len=0;th.line_n=0;th.discard=false;
    return true;
}
static void th_send_install_result(bool ok);
static void th_send_install_result_body(bool ok){
    char line[128];
    if(!th_id_ok(th.install_id))return;
    snprintf(line,sizeof line,"{\"cmd\":\"install_result\",\"id\":\"%s\",\"ok\":%s}",th.install_id,ok?"true":"false");
    snprintf(th.last,sizeof th.last,"%s",line);
    if(th.pid>0)th_enqueue(line);
}
static int th_take_entries(const char *json){
    const char *p=th_key(json,"entries");
    th.listing_n=0;
    if(!p||*p!='[')return 0;
    p++;
    while(*p&&*p!=']'&&th.listing_n<8){
        char obj[1200],name[64],path[512];
        const char *end;
        size_t len;
        while(*p&&*p!='{'&&*p!=']')p++;
        if(*p!='{')break;
        end=strchr(p,'}');
        if(!end)break;
        len=(size_t)(end-p)+1;
        if(len>=sizeof obj){p=end+1;continue;}
        memcpy(obj,p,len);obj[len]=0;
        if(th_json_string(obj,"name",name,sizeof name)&&th_json_string(obj,"path",path,sizeof path)&&path[0]){
            snprintf(th.listing_name[th.listing_n],sizeof th.listing_name[0],"%s",name);
            snprintf(th.listing_path[th.listing_n],sizeof th.listing_path[0],"%s",path);
            th.listing_n++;
        }
        p=end+1;
    }
    return th.listing_n;
}
int zell_theater_apply_line(zell_scene *s,const char *line){
    char event[32];
    if(!line||!th_json_string(line,"event",event,sizeof event))return 0;
    if(strcmp(event,"ready")==0){th.ready=true;th_flush();return 0;}
    if(strcmp(event,"fatal")==0){
        char err[160];
        if(!th_json_string(line,"error",err,sizeof err))snprintf(err,sizeof err,"Theater runtime is missing.");
        th_set_error(err);th_retire();return 0;
    }
    if(strcmp(event,"error")==0){
        char detail[160];
        if(!th_json_string(line,"detail",detail,sizeof detail)&&!th_json_string(line,"error",detail,sizeof detail))
            snprintf(detail,sizeof detail,"Theater error.");
        th_set_error(detail);return 0;
    }
    if(strcmp(event,"caption")==0){
        uint32_t gen=0;
        if(th_json_uint(line,"generation",&gen)){
            if(th.hold&&gen==th.retired)return 0;
            th.accept=gen;th.hold=false;
        }
        if(!th_json_string(line,"text",th.caption,sizeof th.caption))th.caption[0]=0;
        return 0;
    }
    if(strcmp(event,"status")==0){
        uint32_t gen=0;
        char id[40],state[32],browser[16];
        const char *browser_at;
        if(th_json_uint(line,"generation",&gen)){
            if(th.hold&&gen==th.retired)return 0;
            th.accept=gen;th.hold=false;
        }
        if(th_json_string(line,"id",id,sizeof id)&&th_id_ok(id))th_copy(th.active,sizeof th.active,id);
        if(th_json_string(line,"state",state,sizeof state))th.paused=strcmp(state,"playing")!=0;
        browser_at=th_key(line,"browser");
        if(browser_at&&*browser_at=='n')th.browser[0]=0;
        else if(th_json_string(line,"browser",browser,sizeof browser))snprintf(th.browser,sizeof th.browser,"%s",browser);
        return 0;
    }
    if(strcmp(event,"listing")==0){th_take_entries(line);th.menu=true;return 0;}
    if(strcmp(event,"install")==0){
        char phase[32],id[40],argv[8][512];
        char *ptrs[8];
        int argc;
        if(!th_json_string(line,"phase",phase,sizeof phase)||strcmp(phase,"request")!=0)return 0;
        if(!th_json_string(line,"id",id,sizeof id)||!th_id_ok(id)){th_set_error("Theater install id is not valid.");return 0;}
        th_copy(th.install_id,sizeof th.install_id,id);
        argc=th_json_argv(line,argv,8);
        for(int i=0;i<8;i++)ptrs[i]=argv[i];
        if(argc<1||!zell_theater_install_argv_ok(argc,ptrs)){
            th_set_error("Theater install command was refused.");
            th_send_install_result(false);
            return 0;
        }
        th.install_argc=argc;
        for(int i=0;i<argc;i++)th_copy(th.install_argv[i],sizeof th.install_argv[0],argv[i]);
        th.install_stored=true;
        if(s)s->theater_install_request=true;
        th_set_notice("Install requested.");
        return 1;
    }
    return 0;
}
static bool th_add_item(const char *id){
    if(!th_id_ok(id)||th.items>=TH_ITEMS)return false;
    for(int i=0;i<th.items;i++)if(strcmp(th.item_id[i],id)==0)return true;
    th_copy(th.item_id[th.items],sizeof th.item_id[0],id);
    th.items++;
    th_copy(th.active,sizeof th.active,id);
    return true;
}
static void th_read(zell_scene *s){
    if(th.out_fd<0)return;
    for(int batch=0;batch<8;batch++){
        char bytes[4096];
        ssize_t count=read(th.out_fd,bytes,sizeof bytes);
        if(count<0){if(errno==EINTR)continue;break;}
        if(count==0)break;
        for(ssize_t i=0;i<count;i++){
            if(bytes[i]=='\n'){
                if(!th.discard&&th.line_n>0&&th.line_n<sizeof th.line){
                    th.line[th.line_n]=0;
                    zell_theater_apply_line(s,th.line);
                }
                th.line_n=0;th.discard=false;
            }else if(!th.discard){
                if(th.line_n+1<sizeof th.line)th.line[th.line_n++]=(char)bytes[i];
                else {th.discard=true;th.line_n=0;th_set_error("A theater reply was too long.");}
            }
        }
    }
}
static void th_reap(void){
    int status=0;
    pid_t ended;
    if(th.pid<=0)return;
    ended=waitpid(th.pid,&status,WNOHANG);
    if(ended!=th.pid)return;
    th.pid=0;th.ready=false;
    th_close_fds();
    if(!th.stopping)th_set_error("The theater helper stopped.");
}
static void th_window(const zell_scene *s,zell_rect *frame,zell_rect *image,zell_rect *dock,zell_rect *close_r){
    int width=s->width>0?s->width:1,height=s->height>0?s->height:1;
    int w=width*4/10,h;
    if(w<200)w=200;
    if(w>640)w=640;
    h=w*9/16;
    if(h>height-160)h=height-160;
    if(h<112)h=112;
    *frame=(zell_rect){width-w-24,88,w,h+36};
    *image=(zell_rect){frame->x+8,frame->y+28,w-16,h};
    *dock=(zell_rect){frame->x+w-62,frame->y+4,26,20};
    *close_r=(zell_rect){dock->x-30,dock->y,26,20};
}
static void th_resize_target(const zell_scene *s,int *w,int *h){
    if(th.fullscreen){
        int width=s->width>0?s->width:1408,height=s->height>0?s->height:585,fh=height-44-28;
        if(fh<1)fh=1;
        if(width>16384)width=16384;
        if(fh>16384)fh=16384;
        *w=width;*h=fh;return;
    }
    if(th.popout){
        zell_rect frame,image,dock,close_r;
        th_window(s,&frame,&image,&dock,&close_r);
        *w=image.w>0?image.w:320;*h=image.h>0?image.h:180;
        if(*w>16384)*w=16384;
        if(*h>16384)*h=16384;
        return;
    }
    *w=1408;*h=585;
}
static void th_maybe_resize(const zell_scene *s){
    int w,h;
    char line[96];
    if(!th.ready)return;
    th_resize_target(s,&w,&h);
    if(w==th.sent_w&&h==th.sent_h)return;
    snprintf(line,sizeof line,"{\"cmd\":\"resize\",\"width\":%d,\"height\":%d}",w,h);
    if(th_enqueue(line)){th.sent_w=w;th.sent_h=h;}
}
static void th_pump(zell_scene *s){
    if(s&&s->theater_install_finished){
        int code=s->theater_install_code;
        s->theater_install_finished=false;
        if(code==-2)th_set_error("The station terminal is busy.");
        else if(code==0)th_set_notice("Install finished.");
        else th_set_error("Install did not finish.");
        th_send_install_result(code==0);
    }
    th_read(s);
    if(th.map)zell_theater_present(th.map,TH_MAP_BYTES);
    th_maybe_resize(s);
    th_flush();
    th_reap();
}
bool zell_theater_submit(zell_scene *s,const char *json){
    bool open_cmd,use_cmd,close_cmd;
    char id[40];
    if(!json||!json[0]||strlen(json)>=TH_LINE-2)return false;
    snprintf(th.last,sizeof th.last,"%s",json);
    open_cmd=th_cmd_is(json,"open");
    use_cmd=th_cmd_is(json,"use");
    close_cmd=th_cmd_is(json,"close");
    if(!th_start(s)){
        if(use_cmd||close_cmd)th_retire();
        return false;
    }
    if(open_cmd||use_cmd||close_cmd)th_retire();
    if(open_cmd){
        if(!th_json_string(json,"id",id,sizeof id)||!th_add_item(id)){th_set_error("The queue is full.");return false;}
    }
    if(!th_enqueue(json)){th_set_error("The theater command queue is full.");return false;}
    th_flush();
    return true;
}
static int th_quote(char *dst,size_t cap,const char *src){
    size_t n=0;
    if(!dst||cap<3||!src)return -1;
    dst[n++]='"';
    for(const unsigned char *p=(const unsigned char *)src;*p;p++){
        char buf[8];
        const char *esc;
        if(*p=='"'||*p=='\\'){buf[0]='\\';buf[1]=(char)*p;buf[2]=0;esc=buf;}
        else if(*p=='\n')esc="\\n";
        else if(*p<0x20){snprintf(buf,sizeof buf,"\\u%04x",*p);esc=buf;}
        else {buf[0]=(char)*p;buf[1]=0;esc=buf;}
        size_t k=strlen(esc);
        if(n+k+2>=cap)return -1;
        memcpy(dst+n,esc,k);
        n+=k;
    }
    dst[n++]='"';dst[n]=0;
    return (int)n;
}
static int th_format_open(char *dst,size_t cap,const char *id,const char *kind,const char *path,const char *url,const char *browser){
    char qid[80],qpath[TH_LINE],qurl[TH_LINE];
    int n;
    if(!th_id_ok(id)||th_quote(qid,sizeof qid,id)<0)return -1;
    if(url){
        if(!browser||(strcmp(browser,"chrome")!=0&&strcmp(browser,"chromium")!=0&&strcmp(browser,"firefox-esr")!=0))return -1;
        if(th_quote(qurl,sizeof qurl,url)<0)return -1;
        n=snprintf(dst,cap,"{\"cmd\":\"open\",\"id\":%s,\"browser\":\"%s\",\"source\":{\"kind\":\"url\",\"url\":%s}}",qid,browser,qurl);
    }else{
        if(!kind||(strcmp(kind,"file")!=0&&strcmp(kind,"folder")!=0)||!path)return -1;
        if(th_quote(qpath,sizeof qpath,path)<0)return -1;
        n=snprintf(dst,cap,"{\"cmd\":\"open\",\"id\":%s,\"browser\":null,\"source\":{\"kind\":\"%s\",\"path\":%s}}",qid,kind,qpath);
    }
    if(n<0||(size_t)n>=cap)return -1;
    return n;
}
int zell_theater_format_open_file(char *dst,size_t cap,const char *id,const char *path){
    return th_format_open(dst,cap,id,"file",path,NULL,NULL);
}
static bool th_url_ok(const char *url){
    size_t n;
    if(!url)return false;
    n=strlen(url);
    if(n<8||n>2048)return false;
    if(strncmp(url,"https://",8)!=0&&strncmp(url,"http://",7)!=0)return false;
    for(const unsigned char *p=(const unsigned char *)url;*p;p++)if(*p<=0x20)return false;
    return true;
}
static bool th_open_path(zell_scene *s,const char *kind,const char *path){
    char id[16],line[TH_LINE];
    if(!path||!path[0]){th_set_error(strcmp(kind,"folder")==0?"Type a folder path first.":"Type a file path first.");return false;}
    if(strlen(path)>4096){th_set_error("That path is too long.");return false;}
    if(th.items>=TH_ITEMS){th_set_error("The queue is full.");return false;}
    snprintf(id,sizeof id,"q%d",th.next_serial);
    if(th_format_open(line,sizeof line,id,kind,path,NULL,NULL)<0){th_set_error("Could not build the theater command.");return false;}
    if(!zell_theater_submit(s,line))return false;
    th.next_serial++;
    return true;
}
static bool th_open_browser(zell_scene *s,const char *browser){
    char id[16],line[TH_LINE];
    if(!th_url_ok(th.url)){th_set_error("Type an http or https URL first.");return false;}
    if(th.items>=TH_ITEMS){th_set_error("The queue is full.");return false;}
    snprintf(id,sizeof id,"q%d",th.next_serial);
    if(th_format_open(line,sizeof line,id,NULL,NULL,th.url,browser)<0){th_set_error("Could not build the theater command.");return false;}
    if(!zell_theater_submit(s,line))return false;
    th.next_serial++;
    return true;
}
static void th_use(zell_scene *s,const char *id){
    char line[80];
    if(!th_id_ok(id))return;
    snprintf(th.active,sizeof th.active,"%s",id);
    snprintf(line,sizeof line,"{\"cmd\":\"use\",\"id\":\"%s\"}",id);
    zell_theater_submit(s,line);
}
static void th_close_active(zell_scene *s){
    char line[80];
    int at=-1;
    if(!th.active[0]){th_set_error("Nothing is playing.");return;}
    snprintf(line,sizeof line,"{\"cmd\":\"close\",\"id\":\"%s\"}",th.active);
    zell_theater_submit(s,line);
    for(int i=0;i<th.items;i++)if(strcmp(th.item_id[i],th.active)==0)at=i;
    if(at>=0){
        for(int i=at;i+1<th.items;i++)snprintf(th.item_id[i],sizeof th.item_id[0],"%s",th.item_id[i+1]);
        th.items--;
    }
    th.active[0]=0;
    if(th.items>0)snprintf(th.active,sizeof th.active,"%s",th.item_id[0]);
    if(th.items==0){th.popout=false;th.fullscreen=false;}
}
static void th_toggle_playback(zell_scene *s){
    if(!th.active[0]){th_set_error("Nothing is playing.");return;}
    zell_theater_submit(s,th.paused?"{\"cmd\":\"resume\"}":"{\"cmd\":\"pause\"}");
}
static void th_toggle_cc(zell_scene *s){
    th.cc=!th.cc;
    zell_theater_submit(s,th.cc?"{\"cmd\":\"cc\",\"on\":true}":"{\"cmd\":\"cc\",\"on\":false}");
}
static void th_set_fullscreen(zell_scene *s,bool on){
    th.fullscreen=on;
    if(on)th.popout=false;
    th.sent_w=th.sent_h=0;
    (void)s;
}
static void th_dock(zell_scene *s){
    th.popout=false;th.fullscreen=false;th.sent_w=th.sent_h=0;
    s->room=6;s->mode=ZELL_MODE_WALK;
    s->x=(float)ZELL_WORLD_W*.50f;s->y=(float)ZELL_WORLD_H*ZELL_ROOM_ENTRY_Y;
    if(th.browser[0])zell_theater_submit(s,"{\"cmd\":\"dock\"}");
}
static void th_pop(zell_scene *s){
    if(th.popout){th_dock(s);return;}
    th.popout=true;th.fullscreen=false;th.sent_w=th.sent_h=0;
    if(th.browser[0])zell_theater_submit(s,"{\"cmd\":\"popout\"}");
}
static void th_pointer(zell_scene *s,int x,int y){
    char line[128];
    if(!th.browser[0])return;
    if(x<0)x=0;
    if(y<0)y=0;
    snprintf(line,sizeof line,"{\"cmd\":\"pointer\",\"x\":%d,\"y\":%d,\"button\":0,\"action\":\"down\"}",x,y);
    zell_theater_submit(s,line);
    snprintf(line,sizeof line,"{\"cmd\":\"pointer\",\"x\":%d,\"y\":%d,\"button\":0,\"action\":\"up\"}",x,y);
    zell_theater_submit(s,line);
}
static void th_label(sr_canvas *c,int x,int y,const char *text,uint32_t color,int width){
    char shown[160];
    int cap=width/8;
    if(cap<1)return;
    if(cap>159)cap=159;
    snprintf(shown,sizeof shown,"%s",text?text:"");
    if((int)strlen(shown)>cap){shown[cap]=0;if(cap>3)memcpy(shown+cap-3,"...",3);}
    sr_text(c,(float)x,(float)y,shown,color,1.f,1);
}
static void th_paint(sr_canvas *c,int dx,int dy,int dw,int dh){
    int x,y,w,h;
    sr_canvas src;
    if(th.frame_w<1||th.frame_h<1||!th.shown||dw<1||dh<1)return;
    sr_fill_rect(c,(float)dx,(float)dy,(float)dw,(float)dh,0x000000u,1.f);
    zell_theater_contain(th.frame_w,th.frame_h,dw,dh,&x,&y,&w,&h);
    if(w<1||h<1)return;
    sr_canvas_wrap(&src,th.shown,th.frame_w,th.frame_h);
    sr_blit_scaled(c,&src,dx+x,dy+y,w,h,1.f);
}
static const char *th_row_label(int row){
    switch(row){
    case 0:return th.path[0]?th.path:"File path";
    case 1:return "Open file";
    case 2:return "Open folder";
    case 3:return th.url[0]?th.url:"Video URL";
    case 4:return "Chrome";
    case 5:return "Chromium";
    case 6:return "Firefox ESR";
    case 7:return th.paused||!th.active[0]?"Play":"Pause";
    case 8:return th.cc?"Captions on":"Captions off";
    case 9:return "Fullscreen";
    case 10:return th.popout?"Dock in the theater":"Pop out";
    default:return "Close";
    }
}
static void th_row_action(zell_scene *s,int row){
    if(row==0){th.focus=1;return;}
    if(row==3){th.focus=2;return;}
    th.focus=0;
    if(row==1)th_open_path(s,"file",th.path);
    else if(row==2)th_open_path(s,"folder",th.path);
    else if(row==4)th_open_browser(s,"chrome");
    else if(row==5)th_open_browser(s,"chromium");
    else if(row==6)th_open_browser(s,"firefox-esr");
    else if(row==7)th_toggle_playback(s);
    else if(row==8)th_toggle_cc(s);
    else if(row==9)th_set_fullscreen(s,!th.fullscreen);
    else if(row==10)th_pop(s);
    else if(row==11)th_close_active(s);
}
void zell_theater_menu_layout(const zell_scene *s,zell_rect *panel,zell_rect *rows,int row_cap,int *row_count){
    float sx,sy;
    int width,height,pw=300,row_h=22,rows_n=12,ph,x,y,n;
    if(row_count)*row_count=0;
    if(panel)*panel=(zell_rect){0,0,0,0};
    if(!s||!th.menu||th.fullscreen)return;
    width=s->width>0?s->width:1;height=s->height>0?s->height:1;
    ph=16+rows_n*row_h;
    zell_world_to_screen(s,840.f+92.f,790.f,&sx,&sy);
    x=(int)sx-pw-8;y=(int)sy;
    if(x+pw>width-8)x=width-pw-8;
    if(x<8)x=8;
    if(y+ph>height-40)y=height-40-ph;
    if(y<80)y=80;
    if(panel)*panel=(zell_rect){x,y,pw,ph};
    if(!rows||row_cap<=0||!row_count)return;
    n=rows_n;if(n>row_cap)n=row_cap;
    for(int i=0;i<n;i++)rows[i]=(zell_rect){x+8,y+8+i*row_h,pw-16,row_h-2};
    *row_count=n;
}
static bool th_menu_click(zell_scene *s){
    zell_rect panel,rows[12];
    int n=0;
    if(!th.menu)return false;
    zell_theater_menu_layout(s,&panel,rows,12,&n);
    for(int i=0;i<n;i++)if(rect_holds(&rows[i],s->mouse_x,s->mouse_y)){th_row_action(s,i);return true;}
    if(th.listing_n>0&&panel.w>0){
        for(int i=0;i<th.listing_n;i++){
            zell_rect row={panel.x+8,panel.y+panel.h+4+i*22,panel.w-16,20};
            if(!rect_holds(&row,s->mouse_x,s->mouse_y))continue;
            th_open_path(s,"file",th.listing_path[i]);
            return true;
        }
    }
    th.menu=false;th.focus=0;
    return true;
}
bool zell_theater_click(zell_scene *s){
    zell_rect remote,frame,image,dock,close_r;
    if(!s||!s->desktop||!s->mouse_valid||!s->mouse_click||s->mode!=ZELL_MODE_WALK)return false;
    if(th.fullscreen){
        int top=32,dh=s->height-top-44,local_x,local_y;
        if(dh<1)dh=1;
        local_x=s->mouse_x;local_y=s->mouse_y-top;
        if(th.browser[0]&&local_x>=0&&local_y>=0&&local_x<s->width&&local_y<dh)th_pointer(s,local_x,local_y);
        s->mouse_click=false;return true;
    }
    if(th.popout){
        th_window(s,&frame,&image,&dock,&close_r);
        if(rect_holds(&close_r,s->mouse_x,s->mouse_y)){th_close_active(s);s->mouse_click=false;return true;}
        if(rect_holds(&dock,s->mouse_x,s->mouse_y)){th_dock(s);s->mouse_click=false;return true;}
        if(rect_holds(&image,s->mouse_x,s->mouse_y)){
            if(th.browser[0])th_pointer(s,s->mouse_x-image.x,s->mouse_y-image.y);
            s->mouse_click=false;return true;
        }
    }
    for(int i=0;i<th.items;i++){
        zell_rect button;
        zell_theater_queue_rect(i,s->width,s->height,&button);
        if(button.x+button.w>s->width-4)continue;
        if(!rect_holds(&button,s->mouse_x,s->mouse_y))continue;
        th_use(s,th.item_id[i]);
        s->mouse_click=false;return true;
    }
    if(th.menu&&th_menu_click(s)){s->mouse_click=false;return true;}
    if(s->room==6&&!th.popout){
        float wx,wy;
        zell_screen_to_world(s,(float)s->mouse_x,(float)s->mouse_y,&wx,&wy);
        zell_theater_remote_rect(&remote);
        if(rect_holds(&remote,(int)floorf(wx),(int)floorf(wy))){th.menu=!th.menu;th.focus=0;s->mouse_click=false;return true;}
        if(th.browser[0]){
            int lx=(int)floorf(wx)-64,ly=(int)floorf(wy)-54;
            if(lx>=0&&ly>=0&&lx<1408&&ly<585){th_pointer(s,lx,ly);s->mouse_click=false;return true;}
        }
    }
    return false;
}
int zell_theater_input(zell_scene *s,const zell_controls *c){
    char *field;
    size_t cap,n;
    th_pump(s);
    if(!s||!c)return 0;
    /* The station terminal owns the keyboard, including phase 5. */
    if(s->mode==ZELL_MODE_SERVICE)return 0;
    if(th.fullscreen&&c->cancel){th_set_fullscreen(s,false);return 1;}
    if(c->fullscreen_key&&(th.items>0||th.fullscreen||th.menu)){th_set_fullscreen(s,!th.fullscreen);return 1;}
    if(th.focus&&c->cancel){th.focus=0;return 1;}
    if(th.focus&&(c->backspace||(c->character>=32&&c->character<127)||c->confirm)){
        field=th.focus==2?th.url:th.path;
        cap=th.focus==2?sizeof th.url:sizeof th.path;
        n=strlen(field);
        if(c->backspace){if(n)field[n-1]=0;}
        else if(c->character>=32&&c->character<127){if(n+1<cap){field[n]=(char)c->character;field[n+1]=0;}}
        else if(th.focus==1)th_open_path(s,"file",th.path);
        return 1;
    }
    return 0;
}
bool zell_theater_typing(const zell_scene *s){(void)s;return th.focus==1||th.focus==2;}
void zell_theater_shutdown(void){
    const char *stop="{\"cmd\":\"stop\"}\n";
    int64_t deadline;
    struct timespec delay={.tv_nsec=10000000};
    if(th.pid<=0){th_close_fds();return;}
    th.stopping=true;
    if(th.in_fd>=0){
        ssize_t wrote=write(th.in_fd,stop,strlen(stop));
        (void)wrote;
    }
    deadline=zell_notice_clock_ms()+500;
    while(zell_notice_clock_ms()<deadline){
        pid_t ended=waitpid(th.pid,NULL,WNOHANG);
        if(ended==th.pid||(ended<0&&errno==ECHILD)){th.pid=0;break;}
        nanosleep(&delay,NULL);
    }
    if(th.pid>0){
        if(kill(-th.pid,SIGTERM)<0)kill(th.pid,SIGTERM);
        deadline=zell_notice_clock_ms()+500;
        while(zell_notice_clock_ms()<deadline){
            pid_t ended=waitpid(th.pid,NULL,WNOHANG);
            if(ended==th.pid||(ended<0&&errno==ECHILD)){th.pid=0;break;}
            nanosleep(&delay,NULL);
        }
    }
    if(th.pid>0){
        if(kill(-th.pid,SIGKILL)<0)kill(th.pid,SIGKILL);
        while(waitpid(th.pid,NULL,0)<0&&errno==EINTR){}
        th.pid=0;
    }
    th.ready=false;
    th_close_fds();
}
bool zell_theater_install_json(char *dst,size_t cap){
    char q[8][1100];
    size_t used=0;
    int n;
    if(!dst||cap<16||!th.install_stored||th.install_argc<1)return false;
    n=snprintf(dst,cap,"{\"title\":\"Theater install\",\"argv\":[");
    if(n<0||(size_t)n>=cap)return false;
    used=(size_t)n;
    for(int i=0;i<th.install_argc;i++){
        if(th_quote(q[i],sizeof q[i],th.install_argv[i])<0)return false;
        n=snprintf(dst+used,cap-used,"%s%s",i?",":"",q[i]);
        if(n<0||(size_t)n>=cap-used)return false;
        used+=(size_t)n;
    }
    n=snprintf(dst+used,cap-used,"]}");
    return n>0&&(size_t)n<cap-used;
}
int zell_theater_copy_frame(const void *map,size_t map_bytes,uint32_t want_generation,uint32_t *last_seq,uint32_t *dst,size_t dst_pixels,int *out_w,int *out_h){
    const unsigned char *p;
    uint32_t version,width,height,seq,stride,format,generation;
    size_t pixels,bytes;
    if(!map||!last_seq||!out_w||!out_h||map_bytes<64)return 0;
    p=map;
    if(p[0]!='Z'||p[1]!='T'||p[2]!='H'||p[3]!='R')return 0;
    version=th_u32(p+4);width=th_u32(p+8);height=th_u32(p+12);seq=th_u32(p+16);
    stride=th_u32(p+32);format=th_u32(p+36);generation=th_u32(p+40);
    if(version!=1||format!=1||generation!=want_generation)return 0;
    if(seq==0)return -1;
    if((seq&1u)||seq==*last_seq)return 0;
    if(width<1||height<1||width>2048||height>2048||stride!=width*4u)return 0;
    pixels=(size_t)width*(size_t)height;
    if(!dst||pixels>dst_pixels)return 0;
    bytes=pixels*4u;
    if(map_bytes<64u+bytes)return 0;
    memcpy(dst,p+64,bytes);
    if(th_u32(p+16)!=seq)return 0;
    *last_seq=seq;*out_w=(int)width;*out_h=(int)height;
    return 1;
}
int zell_theater_present(const void *map,size_t map_bytes){
    const unsigned char *p;
    uint32_t width,height;
    size_t need=1;
    int w=0,h=0,rc;
    if(!map||map_bytes<64||th.accept==TH_ACCEPT_NONE)return 0;
    p=map;width=th_u32(p+8);height=th_u32(p+12);
    if(width>=1&&height>=1&&width<=2048&&height<=2048)need=(size_t)width*(size_t)height;
    if(!th_ensure_pixels(need))return 0;
    rc=zell_theater_copy_frame(map,map_bytes,th.accept,&th.seq,th.scratch,th.cap,&w,&h);
    if(rc==1){
        uint32_t *swap=th.shown;th.shown=th.scratch;th.scratch=swap;
        th.frame_w=w;th.frame_h=h;
    }else if(rc==-1)th.frame_w=th.frame_h=0;
    return rc;
}
void zell_theater_contain(int src_w,int src_h,int dest_w,int dest_h,int *x,int *y,int *w,int *h){
    long long sw,sh,dw,dh,cw,ch;
    if(!x||!y||!w||!h)return;
    *x=*y=*w=*h=0;
    if(src_w<1||src_h<1||dest_w<1||dest_h<1)return;
    sw=src_w;sh=src_h;dw=dest_w;dh=dest_h;
    if(sw*dh>=dw*sh){cw=dw;ch=dw*sh/sw;}
    else {ch=dh;cw=dh*sw/sh;}
    if(cw<1)cw=1;
    if(ch<1)ch=1;
    if(cw>dw)cw=dw;
    if(ch>dh)ch=dh;
    *w=(int)cw;*h=(int)ch;
    *x=(int)((dw-cw)/2);*y=(int)((dh-ch)/2);
}
void zell_theater_video_rect(zell_rect *r){if(r)*r=(zell_rect){64,54,1408,585};}
void zell_theater_caption_rect(zell_rect *r){if(r)*r=(zell_rect){82,677,1370,44};}
void zell_theater_remote_rect(zell_rect *r){if(r)*r=(zell_rect){840,790,92,145};}
void zell_theater_queue_rect(int index,int width,int height,zell_rect *r){
    (void)width;
    if(!r)return;
    *r=(zell_rect){0,0,0,0};
    if(index<0||index>=TH_ITEMS)return;
    *r=(zell_rect){260+index*78,height-ZELL_ROOM_STATUS_STRIP-28,72,24};
}
static void th_send_install_result(bool ok){th_send_install_result_body(ok);}
bool zell_theater_install_argv_ok(int argc,char **argv){
    if(!argv||argc<1||argc>8)return false;
    for(int i=0;i<argc;i++){
        if(!argv[i]||!argv[i][0]||strlen(argv[i])>511||strchr(argv[i],'\n'))return false;
        if(strcmp(argv[i],"sudo")==0||strcmp(argv[i],"google-chrome-stable")==0)return false;
    }
    return true;
}
void zell_theater_test_reset(void){
    if(th.pid>0){
        if(kill(-th.pid,SIGKILL)<0)kill(th.pid,SIGKILL);
        while(waitpid(th.pid,NULL,0)<0&&errno==EINTR){}
    }
    th_close_fds();
    free(th.shown);free(th.scratch);
    memset(&th,0,sizeof th);
    th.in_fd=th.out_fd=th.frame_fd=-1;
    th.accept=TH_ACCEPT_NONE;
    th.cc=true;th.paused=true;th.next_serial=1;
}
bool zell_theater_menu_open(void){return th.menu;}
bool zell_theater_fullscreen(void){return th.fullscreen;}
int zell_theater_queue_count(void){return th.items;}
const char *zell_theater_error(void){return th.error;}
const char *zell_theater_caption(void){return th.caption;}
const char *zell_theater_last_command(void){return th.last;}
static void th_screen_span(const zell_scene *s,float x0,float y0,float x1,float y1,int *dx,int *dy,int *dw,int *dh){
    float sx0,sy0,sx1,sy1;
    zell_world_to_screen(s,x0,y0,&sx0,&sy0);
    zell_world_to_screen(s,x1,y1,&sx1,&sy1);
    *dx=(int)floorf(sx0);*dy=(int)floorf(sy0);
    *dw=(int)floorf(sx1)-*dx;*dh=(int)floorf(sy1)-*dy;
    if(*dw<1)*dw=1;
    if(*dh<1)*dh=1;
}
void zell_theater_draw(const zell_scene *s,sr_canvas *c){
    zell_rect panel,rows[12],frame,image,dock,close_r,button;
    int n=0,dx,dy,dw,dh;
    if(!s||!c||!s->desktop)return;
    if(th.fullscreen){
        int top=32,cap_h=44,vh=c->h-top-cap_h;
        if(vh<1)vh=1;
        sr_fill_rect(c,0,0,(float)c->w,(float)c->h,0x05080cu,1.f);
        th_paint(c,0,top,c->w,vh);
        sr_fill_rect(c,0,(float)(top+vh),(float)c->w,(float)cap_h,0x101820u,1.f);
        th_label(c,12,top+vh+14,th.caption[0]?th.caption:"",0xe7e2d4u,c->w-24);
        th_label(c,12,8,"F11 toggles fullscreen. Escape leaves it.",0xf0d69bu,c->w-24);
    }else if(s->room==6&&!th.popout){
        th_screen_span(s,64.f,54.f,64.f+1408.f,54.f+585.f,&dx,&dy,&dw,&dh);
        th_paint(c,dx,dy,dw,dh);
        th_screen_span(s,82.f,677.f,82.f+1370.f,677.f+44.f,&dx,&dy,&dw,&dh);
        if(th.caption[0]){
            sr_fill_rect(c,(float)dx,(float)dy,(float)dw,(float)dh,0x101820u,.72f);
            th_label(c,dx+8,dy+12,th.caption,0xe7e2d4u,dw-16);
        }
    }
    if(th.popout&&!th.fullscreen){
        th_window(s,&frame,&image,&dock,&close_r);
        sr_fill_rect(c,(float)frame.x,(float)frame.y,(float)frame.w,(float)frame.h,0x10232cu,.98f);
        sr_stroke_rect(c,(float)frame.x,(float)frame.y,(float)frame.w,(float)frame.h,1.f,0x5b9989u,1.f);
        th_label(c,frame.x+8,frame.y+6,"Theater",0xf0d69bu,frame.w-80);
        th_paint(c,image.x,image.y,image.w,image.h);
        sr_stroke_rect(c,(float)dock.x,(float)dock.y,(float)dock.w,(float)dock.h,1.f,0xd9ba79u,1.f);
        sr_stroke_rect(c,(float)close_r.x,(float)close_r.y,(float)close_r.w,(float)close_r.h,1.f,0xd9ba79u,1.f);
    }
    for(int i=0;i<th.items;i++){
        zell_theater_queue_rect(i,c->w,c->h,&button);
        if(button.x+button.w>c->w-4)break;
        sr_fill_rect(c,(float)button.x,(float)button.y,(float)button.w,(float)button.h,strcmp(th.item_id[i],th.active)==0?0x254b50u:0x10232cu,.95f);
        th_label(c,button.x+4,button.y+4,th.item_id[i],0xe8ffe9u,button.w-8);
    }
    if(th.menu&&!th.fullscreen){
        zell_theater_menu_layout(s,&panel,rows,12,&n);
        if(panel.w>0){
            sr_fill_rect(c,(float)panel.x,(float)panel.y,(float)panel.w,(float)panel.h,0x10232cu,.98f);
            sr_stroke_rect(c,(float)panel.x,(float)panel.y,(float)panel.w,(float)panel.h,1.f,0xd9ba79u,1.f);
            for(int i=0;i<n;i++)th_label(c,rows[i].x,rows[i].y+2,th_row_label(i),i==0||i==3?0xf5dda7u:0xd5e4e6u,rows[i].w);
            for(int i=0;i<th.listing_n;i++){
                int y=panel.y+panel.h+4+i*22;
                th_label(c,panel.x+8,y,th.listing_name[i],0xc7d8dau,panel.w-16);
            }
        }
    }
    if(th.error[0])th_label(c,12,c->h-ZELL_ROOM_STATUS_STRIP-18,th.error,0xffb4a8u,c->w-24);
    else if(th.notice[0])th_label(c,12,c->h-ZELL_ROOM_STATUS_STRIP-18,th.notice,0xf0d69bu,c->w-24);
}
