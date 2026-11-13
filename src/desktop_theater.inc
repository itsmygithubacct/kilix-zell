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
#define TH_CMD 8192
#define TH_LINE 65540
#define TH_QUEUE 4
#define TH_LIST 200
#define TH_NAME 4096
#define TH_ACCEPT_NONE 0xffffffffu
#define TH_MAP_BYTES (64u+(size_t)2048*2048*4u)
#define TH_BAR 28
#define TH_CAP_H 44
#define TH_CHROME_DEB "google-chrome-stable_current_amd64.deb"

typedef struct {
    pid_t pid;
    int in_fd,out_fd,frame_fd;
    void *map;
    uint32_t *shown,*scratch;
    size_t cap;
    uint32_t seq,accept,retired,expect;
    int frame_w,frame_h,sent_w,sent_h;
    bool ready,stopping,hold,fullscreen,popout,menu,cc,paused,playing;
    bool install_stored,discard,expect_set,helper_active,page_keys,help;
    int focus,items,next_serial,q_len,listing_n,list_scroll,queue_page;
    char path[4096],url[2048],caption[1024],error[180],notice[180],browser[16];
    char active[33],picture[33],selected[33],pending_id[33],pending_label[TH_NAME];
    char item_id[TH_ITEMS][33];
    char item_label[TH_ITEMS][TH_NAME];
    char queue[TH_QUEUE][TH_CMD];
    char line[TH_LINE],last[TH_CMD];
    size_t line_n;
    char install_id[33],install_browser[16],install_action[32],install_deb[80];
    char listing_name[TH_LIST][TH_NAME];
    char listing_path[TH_LIST][4096];
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
static int th_hex4(const char **pp){
    int value=0;
    for(int i=0;i<4;i++){
        char h=*(*pp)++;
        int d=h>='0'&&h<='9'?h-'0':h>='a'&&h<='f'?h-'a'+10:h>='A'&&h<='F'?h-'A'+10:-1;
        if(d<0)return -1;
        value=value*16+d;
    }
    return value;
}
static int th_utf8_put(char *dst,size_t cap,size_t n,unsigned code){
    unsigned char tmp[4];
    int len;
    if(code>0x10ffffu)return -1;
    if(code<=0x7fu){tmp[0]=(unsigned char)code;len=1;}
    else if(code<=0x7ffu){tmp[0]=(unsigned char)(0xc0u|(code>>6));tmp[1]=(unsigned char)(0x80u|(code&63u));len=2;}
    else if(code<=0xffffu){
        tmp[0]=(unsigned char)(0xe0u|(code>>12));
        tmp[1]=(unsigned char)(0x80u|((code>>6)&63u));
        tmp[2]=(unsigned char)(0x80u|(code&63u));
        len=3;
    }else{
        tmp[0]=(unsigned char)(0xf0u|(code>>18));
        tmp[1]=(unsigned char)(0x80u|((code>>12)&63u));
        tmp[2]=(unsigned char)(0x80u|((code>>6)&63u));
        tmp[3]=(unsigned char)(0x80u|(code&63u));
        len=4;
    }
    if(n+(size_t)len>=cap)return -1;
    for(int i=0;i<len;i++)dst[n+(size_t)i]=(char)tmp[i];
    return len;
}
static bool th_parse_string(const char **pp,char *dst,size_t cap){
    const char *p=*pp;
    size_t n=0;
    if(!dst||cap<1||*p!='"')return false;
    p++;
    while(*p&&*p!='"'){
        unsigned char ch=(unsigned char)*p++;
        if(ch=='\\'){
            char esc=*p++;
            if(!esc)return false;
            if(esc=='"'||esc=='\\'||esc=='/')ch=(unsigned char)esc;
            else if(esc=='b')ch='\b';
            else if(esc=='f')ch='\f';
            else if(esc=='n')ch='\n';
            else if(esc=='r')ch='\r';
            else if(esc=='t')ch='\t';
            else if(esc=='u'){
                int unit=th_hex4(&p);
                unsigned code;
                int wrote;
                if(unit<0)return false;
                code=(unsigned)unit;
                if(code>=0xd800u&&code<=0xdbffu){
                    int low;
                    if(p[0]!='\\'||p[1]!='u')return false;
                    p+=2;
                    low=th_hex4(&p);
                    if(low<0xdc00||low>0xdfff)return false;
                    code=0x10000u+(((code-0xd800u)<<10)|((unsigned)low-0xdc00u));
                }else if(code>=0xdc00u&&code<=0xdfffu)return false;
                wrote=th_utf8_put(dst,cap,n,code);
                if(wrote<0)return false;
                n+=(size_t)wrote;
                continue;
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
static const char *th_object_end(const char *p){
    int depth=0;
    bool str=false,esc=false;
    if(!p||*p!='{')return NULL;
    for(;*p;p++){
        if(str){
            if(esc)esc=false;
            else if(*p=='\\')esc=true;
            else if(*p=='"')str=false;
            continue;
        }
        if(*p=='"')str=true;
        else if(*p=='{')depth++;
        else if(*p=='}'){if(--depth==0)return p;}
    }
    return NULL;
}
static bool th_gen_ok(uint32_t gen){
    if(th.hold&&gen==th.retired)return false;
    if(th.expect_set&&gen<th.expect)return false;
    return true;
}
static void th_gen_take(uint32_t gen){
    th.expect=gen;
    th.expect_set=true;
    th.accept=gen;
    th.hold=false;
}
static const char *th_base(const char *path){
    const char *slash;
    if(!path||!path[0])return "";
    slash=strrchr(path,'/');
    return slash&&slash[1]?slash+1:path;
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
    n=snprintf(th.queue[th.q_len],TH_CMD,"%s\n",json);
    if(n<0||n>=TH_CMD)return false;
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
static void th_send_install_cancel(void);
static void th_send_install_result_body(bool ok){
    char line[160];
    if(!th_id_ok(th.install_id))return;
    snprintf(line,sizeof line,"{\"cmd\":\"install_result\",\"id\":\"%s\",\"ok\":%s}",th.install_id,ok?"true":"false");
    th_copy(th.last,sizeof th.last,line);
    if(th.pid>0)th_enqueue(line);
    th.install_id[0]=0;
    th.install_stored=false;
}
static void th_send_install_cancel(void){
    char line[96];
    if(!th_id_ok(th.install_id))return;
    snprintf(line,sizeof line,"{\"cmd\":\"install_cancel\",\"id\":\"%s\"}",th.install_id);
    th_copy(th.last,sizeof th.last,line);
    if(th.pid>0)th_enqueue(line);
    th.install_id[0]=0;
    th.install_stored=false;
}
static void th_clear_pending(void){
    th.pending_id[0]=0;
    th.pending_label[0]=0;
}
static bool th_note_loaded(const char *id){
    const char *label;
    if(!th_id_ok(id))return false;
    for(int i=0;i<th.items;i++)if(strcmp(th.item_id[i],id)==0)return true;
    if(th.items>=TH_ITEMS){th_set_error("The queue is full.");return false;}
    label=th.pending_label[0]&&strcmp(th.pending_id,id)==0?th.pending_label:id;
    th_copy(th.item_id[th.items],sizeof th.item_id[0],id);
    th_copy(th.item_label[th.items],sizeof th.item_label[0],label);
    th.items++;
    if(!th.selected[0])th_copy(th.selected,sizeof th.selected,id);
    if(strcmp(th.pending_id,id)==0)th_clear_pending();
    return true;
}
static int th_take_entries(const char *json){
    const char *p=th_key(json,"entries");
    th.listing_n=0;
    th.list_scroll=0;
    if(!p||*p!='[')return 0;
    p++;
    while(*p&&*p!=']'&&th.listing_n<TH_LIST){
        char obj[16384],name[TH_NAME],path[4096];
        const char *end;
        size_t len;
        while(*p&&*p!='{'&&*p!=']')p++;
        if(*p!='{')break;
        end=th_object_end(p);
        if(!end)break;
        len=(size_t)(end-p)+1;
        if(len>=sizeof obj){p=end+1;continue;}
        memcpy(obj,p,len);obj[len]=0;
        if(th_json_string(obj,"name",name,sizeof name)&&th_json_string(obj,"path",path,sizeof path)&&path[0]&&strlen(path)<=4096){
            th_copy(th.listing_name[th.listing_n],sizeof th.listing_name[0],name);
            th_copy(th.listing_path[th.listing_n],sizeof th.listing_path[0],path);
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
        th_set_error(err);th_retire();th_clear_pending();return 0;
    }
    if(strcmp(event,"error")==0){
        char detail[160];
        if(!th_json_string(line,"detail",detail,sizeof detail)&&!th_json_string(line,"error",detail,sizeof detail))
            snprintf(detail,sizeof detail,"Theater error.");
        th_set_error(detail);
        th_clear_pending();
        return 0;
    }
    if(strcmp(event,"caption")==0){
        uint32_t gen=0;
        char id[40];
        bool has_gen=th_json_uint(line,"generation",&gen);
        bool has_id=th_json_string(line,"id",id,sizeof id);
        if(has_gen&&!th_gen_ok(gen))return 0;
        if(has_gen)th_gen_take(gen);
        if(has_id&&th.picture[0]&&strcmp(id,th.picture)!=0)return 0;
        if(!th_json_string(line,"text",th.caption,sizeof th.caption))th.caption[0]=0;
        return 0;
    }
    if(strcmp(event,"status")==0){
        uint32_t gen=0;
        char id[40],state[32],browser[16];
        const char *browser_at;
        bool has_gen=th_json_uint(line,"generation",&gen);
        bool has_id;
        if(has_gen&&!th_gen_ok(gen))return 0;
        if(has_gen)th_gen_take(gen);
        has_id=th_json_string(line,"id",id,sizeof id)&&th_id_ok(id);
        if(!th_json_string(line,"state",state,sizeof state))state[0]=0;
        browser_at=th_key(line,"browser");
        if(has_id&&strcmp(state,"playing")==0){
            th_note_loaded(id);
            th_copy(th.picture,sizeof th.picture,id);
            th_copy(th.active,sizeof th.active,id);
            th_copy(th.selected,sizeof th.selected,id);
            th.playing=true;
            th.paused=false;
            th.helper_active=true;
            if(browser_at&&*browser_at=='n')th.browser[0]=0;
            else if(th_json_string(line,"browser",browser,sizeof browser))th_copy(th.browser,sizeof th.browser,browser);
        }else if(has_id&&(strcmp(state,"paused")==0||strcmp(state,"ended")==0||strcmp(state,"error")==0)){
            if(th.picture[0]&&strcmp(id,th.picture)!=0){
                if(strcmp(state,"paused")==0)th_note_loaded(id);
            }else{
                th_note_loaded(id);
                th.playing=false;
                th.paused=true;
                if(strcmp(state,"paused")==0){
                    th_copy(th.picture,sizeof th.picture,id);
                    th_copy(th.active,sizeof th.active,id);
                    if(browser_at&&*browser_at=='n')th.browser[0]=0;
                    else if(th_json_string(line,"browser",browser,sizeof browser))th_copy(th.browser,sizeof th.browser,browser);
                }else{
                    th.helper_active=false;
                    if(strcmp(id,th.picture)==0){th.picture[0]=0;th.active[0]=0;}
                }
            }
        }else if(!has_id){
            th.playing=false;
            th.paused=true;
            th.helper_active=false;
            th.picture[0]=0;
            th.active[0]=0;
            th.browser[0]=0;
        }
        return 0;
    }
    if(strcmp(event,"listing")==0){
        char id[40];
        th_take_entries(line);
        if(th_json_string(line,"id",id,sizeof id)&&th.pending_id[0]&&strcmp(id,th.pending_id)==0)th_clear_pending();
        th.menu=true;
        return 0;
    }
    if(strcmp(event,"install")==0){
        char phase[32],id[40],action[40],browser[16],deb[80];
        bool has_deb;
        if(!th_json_string(line,"phase",phase,sizeof phase)||strcmp(phase,"request")!=0)return 0;
        if(th_key(line,"argv")){
            th_set_error("Theater install command was refused.");
            if(th_json_string(line,"id",id,sizeof id)&&th_id_ok(id)){th_copy(th.install_id,sizeof th.install_id,id);th_send_install_result(false);}
            return 0;
        }
        if(!th_json_string(line,"id",id,sizeof id)||!th_id_ok(id)){th_set_error("Theater install id is not valid.");return 0;}
        if(!th_json_string(line,"action",action,sizeof action))action[0]=0;
        if(!th_json_string(line,"browser",browser,sizeof browser))browser[0]=0;
        has_deb=th_json_string(line,"deb",deb,sizeof deb);
        th_copy(th.install_id,sizeof th.install_id,id);
        if(!zell_theater_install_action_ok(browser,action,has_deb?deb:NULL)){
            th_set_error("Theater install command was refused.");
            th_send_install_result(false);
            th_clear_pending();
            return 0;
        }
        th_copy(th.install_browser,sizeof th.install_browser,browser);
        th_copy(th.install_action,sizeof th.install_action,action);
        if(has_deb)th_copy(th.install_deb,sizeof th.install_deb,deb);
        else th.install_deb[0]=0;
        th.install_stored=true;
        if(s)s->theater_install_request=true;
        th_set_notice("Install requested.");
        return 1;
    }
    return 0;
}
static void th_read(zell_scene *s){
    if(th.out_fd<0)return;
    for(int batch=0;batch<20;batch++){
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
    *image=(zell_rect){frame->x+8,frame->y+TH_BAR,w-16,h};
    *dock=(zell_rect){frame->x+w-108,frame->y+4,48,20};
    *close_r=(zell_rect){dock->x-52,dock->y,48,20};
}
void zell_theater_fullscreen_layout(int width,int height,zell_rect *picture,zell_rect *caption,zell_rect *help,zell_rect *drop){
    int w=width>0?width:1,h=height>0?height:1,ph=h-TH_BAR-TH_CAP_H;
    if(ph<1)ph=1;
    if(picture)*picture=(zell_rect){0,TH_BAR,w,ph};
    if(caption)*caption=(zell_rect){0,TH_BAR+ph,w,TH_CAP_H};
    if(help)*help=(zell_rect){w-76,4,68,20};
    if(drop)*drop=(zell_rect){w-280,TH_BAR,268,44};
}
static void th_resize_target(const zell_scene *s,int *w,int *h){
    if(th.fullscreen){
        zell_rect picture,caption,help,drop;
        int width=s->width>0?s->width:1408,height=s->height>0?s->height:585;
        zell_theater_fullscreen_layout(width,height,&picture,&caption,&help,&drop);
        if(picture.w>16384)picture.w=16384;
        if(picture.h>16384)picture.h=16384;
        if(picture.w<1)picture.w=1;
        if(picture.h<1)picture.h=1;
        *w=picture.w;*h=picture.h;return;
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
        if(code==0)th_send_install_result(true);
        else th_send_install_cancel();
    }
    th_read(s);
    if(th.map)zell_theater_present(th.map,TH_MAP_BYTES);
    th_maybe_resize(s);
    th_flush();
    th_reap();
}
static void th_remember_open(const char *json){
    char id[40],kind[16],path[4096],url[2048];
    if(!th_json_string(json,"id",id,sizeof id)||!th_id_ok(id))return;
    th_copy(th.pending_id,sizeof th.pending_id,id);
    th.pending_label[0]=0;
    if(th_json_string(json,"path",path,sizeof path)&&path[0])th_copy(th.pending_label,sizeof th.pending_label,th_base(path));
    else if(th_json_string(json,"url",url,sizeof url)&&url[0])th_copy(th.pending_label,sizeof th.pending_label,url);
    (void)kind;
}
bool zell_theater_submit(zell_scene *s,const char *json){
    bool open_cmd,use_cmd,close_cmd;
    if(!json||!json[0]||strlen(json)>=TH_CMD-2)return false;
    th_copy(th.last,sizeof th.last,json);
    open_cmd=th_cmd_is(json,"open");
    use_cmd=th_cmd_is(json,"use");
    close_cmd=th_cmd_is(json,"close");
    if(open_cmd)th_remember_open(json);
    if(!th_start(s)){
        if(use_cmd||close_cmd)th_retire();
        return false;
    }
    if(open_cmd||use_cmd||close_cmd)th_retire();
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
    char qid[80],qpath[TH_CMD],qurl[TH_CMD];
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
    char id[16],line[TH_CMD];
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
    char id[16],line[TH_CMD];
    if(!th_url_ok(th.url)){th_set_error("Type an http or https URL first.");return false;}
    if(th.items>=TH_ITEMS){th_set_error("The queue is full.");return false;}
    snprintf(id,sizeof id,"q%d",th.next_serial);
    if(th_format_open(line,sizeof line,id,NULL,NULL,th.url,browser)<0){th_set_error("Could not build the theater command.");return false;}
    if(!zell_theater_submit(s,line))return false;
    th.next_serial++;
    return true;
}
static void th_drop_item(const char *id){
    int at=-1;
    if(!id||!id[0])return;
    for(int i=0;i<th.items;i++)if(strcmp(th.item_id[i],id)==0)at=i;
    if(at<0)return;
    for(int i=at;i+1<th.items;i++){
        th_copy(th.item_id[i],sizeof th.item_id[0],th.item_id[i+1]);
        th_copy(th.item_label[i],sizeof th.item_label[0],th.item_label[i+1]);
    }
    th.items--;
    if(th.queue_page>0&&th.queue_page*4>=th.items)th.queue_page--;
}
static void th_use(zell_scene *s,const char *id){
    char line[80];
    if(!th_id_ok(id))return;
    th_copy(th.selected,sizeof th.selected,id);
    snprintf(line,sizeof line,"{\"cmd\":\"use\",\"id\":\"%s\"}",id);
    zell_theater_submit(s,line);
}
static void th_close_active(zell_scene *s){
    char line[80],closing[33];
    if(!th.active[0]&&!th.picture[0]&&!th.selected[0]){th_set_error("Nothing is playing.");return;}
    th_copy(closing,sizeof closing,th.picture[0]?th.picture:th.active[0]?th.active:th.selected);
    snprintf(line,sizeof line,"{\"cmd\":\"close\",\"id\":\"%s\"}",closing);
    zell_theater_submit(s,line);
    th_drop_item(closing);
    if(strcmp(th.picture,closing)==0){th.picture[0]=0;th.playing=false;th.paused=true;th.helper_active=false;th.browser[0]=0;}
    if(strcmp(th.active,closing)==0)th.active[0]=0;
    if(strcmp(th.selected,closing)==0){
        if(th.items>0)th_copy(th.selected,sizeof th.selected,th.item_id[0]);
        else th.selected[0]=0;
    }
    if(th.items==0){th.popout=false;th.fullscreen=false;th.page_keys=false;}
}
static void th_toggle_playback(zell_scene *s){
    const char *id=th.selected[0]?th.selected:th.items>0?th.item_id[0]:"";
    if(!th.playing){
        if(!id[0]){th_set_error("Nothing is queued.");return;}
        if(!th.helper_active){th_use(s,id);return;}
        zell_theater_submit(s,"{\"cmd\":\"resume\"}");
        return;
    }
    zell_theater_submit(s,"{\"cmd\":\"pause\"}");
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
    zell_ui_text(c,x,y,text,color,width);
}
static void th_paint(sr_canvas *c,int dx,int dy,int dw,int dh){
    if(th.frame_w<1||th.frame_h<1||!th.shown||dw<1||dh<1)return;
    zell_ui_blit(c,dx,dy,dw,dh,th.shown,th.frame_w,th.frame_h);
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
    case 7:return th.playing?"Pause":"Play";
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
typedef struct th_menu_hit {
    zell_rect panel;
    zell_rect action[12];
    int action_n;
    zell_rect file[16];
    int file_i[16];
    int file_n;
    zell_rect next;
    int has_next;
} th_menu_hit;
static void th_menu_hit_build(const zell_scene *s,th_menu_hit *hit){
    float sx,sy;
    int width,height,pw=300,row_h=20,x,y,max_ph,rows_fit,action_vis,file_budget,ph;
    memset(hit,0,sizeof *hit);
    if(!s||!th.menu||th.fullscreen)return;
    width=s->width>0?s->width:1;height=s->height>0?s->height:1;
    zell_world_to_screen(s,840.f+92.f,790.f,&sx,&sy);
    x=(int)sx-pw-8;y=(int)sy;
    if(x+pw>width-8)x=width-pw-8;
    if(x<8)x=8;
    max_ph=height-40-80;
    if(max_ph<160)max_ph=height-48;
    if(max_ph<160)max_ph=160;
    rows_fit=(max_ph-16)/row_h;
    if(rows_fit<8)rows_fit=8;
    if(rows_fit>28)rows_fit=28;
    file_budget=th.listing_n>0?1:0;
    action_vis=rows_fit-file_budget;
    if(action_vis>12)action_vis=12;
    if(action_vis<1)action_vis=1;
    file_budget=th.listing_n>0?rows_fit-action_vis:0;
    if(file_budget>16)file_budget=16;
    ph=16+(action_vis+file_budget)*row_h;
    if(y+ph>height-40)y=height-40-ph;
    if(y<8)y=8;
    hit->panel=(zell_rect){x,y,pw,ph};
    for(int i=0;i<action_vis&&i<12;i++)hit->action[hit->action_n++]=(zell_rect){x+8,y+8+i*row_h,pw-16,row_h-2};
    if(th.listing_n>0&&file_budget>0){
        int reserve=0,shown,remain;
        if(th.list_scroll<0||th.list_scroll>=th.listing_n)th.list_scroll=0;
        remain=th.listing_n-th.list_scroll;
        if(remain>file_budget)reserve=1;
        shown=file_budget-reserve;
        if(shown<1){shown=1;reserve=0;}
        for(int i=0;i<shown&&th.list_scroll+i<th.listing_n&&hit->file_n<16;i++){
            hit->file[hit->file_n]=(zell_rect){x+8,y+8+(action_vis+i)*row_h,pw-16,row_h-2};
            hit->file_i[hit->file_n]=th.list_scroll+i;
            hit->file_n++;
        }
        if(reserve){
            if(hit->file_n>0&&shown>=file_budget){
                zell_rect *last=&hit->file[hit->file_n-1];
                int keep=last->w-72;
                if(keep<80)keep=80;
                hit->next=(zell_rect){last->x+keep,last->y,last->w-keep,last->h};
                last->w=keep;
            }else hit->next=(zell_rect){x+8,y+8+(action_vis+shown)*row_h,pw-16,row_h-2};
            hit->has_next=1;
        }
    }
}
void zell_theater_menu_layout(const zell_scene *s,zell_rect *panel,zell_rect *rows,int row_cap,int *row_count){
    th_menu_hit hit;
    if(row_count)*row_count=0;
    if(panel)*panel=(zell_rect){0,0,0,0};
    th_menu_hit_build(s,&hit);
    if(panel)*panel=hit.panel;
    if(!rows||row_cap<=0||!row_count)return;
    for(int i=0;i<hit.action_n&&i<row_cap;i++)rows[i]=hit.action[i];
    *row_count=hit.action_n<row_cap?hit.action_n:row_cap;
}
static bool th_menu_click(zell_scene *s){
    th_menu_hit hit;
    if(!th.menu)return false;
    th_menu_hit_build(s,&hit);
    for(int i=0;i<hit.action_n;i++)if(rect_holds(&hit.action[i],s->mouse_x,s->mouse_y)){th_row_action(s,i);return true;}
    if(hit.has_next&&rect_holds(&hit.next,s->mouse_x,s->mouse_y)){
        th.list_scroll+=hit.file_n>0?hit.file_n:1;
        if(th.list_scroll>=th.listing_n)th.list_scroll=0;
        return true;
    }
    for(int i=0;i<hit.file_n;i++){
        if(!rect_holds(&hit.file[i],s->mouse_x,s->mouse_y))continue;
        th_open_path(s,"file",th.listing_path[hit.file_i[i]]);
        return true;
    }
    th.menu=false;th.focus=0;
    return true;
}
bool zell_theater_click(zell_scene *s){
    zell_rect remote,frame,image,dock,close_r;
    if(!s||!s->desktop||!s->mouse_valid||!s->mouse_click||s->mode!=ZELL_MODE_WALK)return false;
    if(th.fullscreen){
        zell_rect picture,caption,help,drop;
        zell_theater_fullscreen_layout(s->width,s->height,&picture,&caption,&help,&drop);
        if(rect_holds(&help,s->mouse_x,s->mouse_y)){th.help=!th.help;s->mouse_click=false;return true;}
        if(th.help&&rect_holds(&drop,s->mouse_x,s->mouse_y)){s->mouse_click=false;return true;}
        if(rect_holds(&picture,s->mouse_x,s->mouse_y)){
            if(th.browser[0]){th.page_keys=true;th_pointer(s,s->mouse_x-picture.x,s->mouse_y-picture.y);}
        }
        s->mouse_click=false;return true;
    }
    if(th.popout){
        th_window(s,&frame,&image,&dock,&close_r);
        if(rect_holds(&close_r,s->mouse_x,s->mouse_y)){th_close_active(s);s->mouse_click=false;return true;}
        if(rect_holds(&dock,s->mouse_x,s->mouse_y)){th_dock(s);s->mouse_click=false;return true;}
        if(rect_holds(&image,s->mouse_x,s->mouse_y)){
            if(th.browser[0]){th.page_keys=true;th_pointer(s,s->mouse_x-image.x,s->mouse_y-image.y);}
            s->mouse_click=false;return true;
        }
    }
    {
        zell_rect nav;
        if(zell_theater_queue_nav(s->width,s->height,-1,&nav)&&rect_holds(&nav,s->mouse_x,s->mouse_y)){th.queue_page--;s->mouse_click=false;return true;}
        if(zell_theater_queue_nav(s->width,s->height,1,&nav)&&rect_holds(&nav,s->mouse_x,s->mouse_y)){th.queue_page++;s->mouse_click=false;return true;}
    }
    for(int i=0;i<th.items;i++){
        zell_rect button;
        zell_theater_queue_rect(i,s->width,s->height,&button);
        if(button.w<1)continue;
        if(!rect_holds(&button,s->mouse_x,s->mouse_y))continue;
        th_use(s,th.item_id[i]);
        s->mouse_click=false;return true;
    }
    if(th.menu&&th_menu_click(s)){s->mouse_click=false;return true;}
    if(s->room==6&&!th.popout){
        float wx,wy;
        zell_screen_to_world(s,(float)s->mouse_x,(float)s->mouse_y,&wx,&wy);
        zell_theater_remote_rect(&remote);
        if(rect_holds(&remote,(int)floorf(wx),(int)floorf(wy))){th.menu=!th.menu;th.focus=0;th.page_keys=false;s->mouse_click=false;return true;}
        if(th.browser[0]){
            int lx=(int)floorf(wx)-64,ly=(int)floorf(wy)-54;
            if(lx>=0&&ly>=0&&lx<1408&&ly<585){th.page_keys=true;th_pointer(s,lx,ly);s->mouse_click=false;return true;}
        }
    }
    return false;
}
static const char *th_key_name(unsigned key){
    switch(key){
    case 0xe000u:return "Escape";
    case 0xe001u:return "Enter";
    case 0xe002u:return "Tab";
    case 0xe003u:return "Backspace";
    case 0xe004u:return "Insert";
    case 0xe005u:return "Delete";
    case 0xe006u:return "ArrowLeft";
    case 0xe007u:return "ArrowRight";
    case 0xe008u:return "ArrowUp";
    case 0xe009u:return "ArrowDown";
    case 0xe00au:return "PageUp";
    case 0xe00bu:return "PageDown";
    case 0xe00cu:return "Home";
    case 0xe00du:return "End";
    default:break;
    }
    if(key>=0xe014u&&key<=0xe01fu){
        static const char *fn[]={"F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12"};
        return fn[key-0xe014u];
    }
    return NULL;
}
static void th_send_key(zell_scene *s,const zell_controls *c){
    char line[TH_CMD],qtext[256],text[16];
    const char *name;
    unsigned mods,code;
    int n,quoted;
    if(!c)return;
    mods=c->key_modifiers&(1u|2u|4u|8u|64u|128u);
    code=c->key_text;
    name=th_key_name(c->key_code);
    text[0]=0;
    if(name&&code<32){
        quoted=th_quote(qtext,sizeof qtext,"");
        if(quoted<0)return;
        n=snprintf(line,sizeof line,"{\"cmd\":\"key\",\"action\":\"down\",\"key\":\"%s\",\"modifiers\":%u,\"text\":%s}",name,mods,qtext);
    }else if(code>=32){
        int wrote=th_utf8_put(text,sizeof text,0,code);
        if(wrote<0)return;
        text[wrote]=0;
        if(th_quote(qtext,sizeof qtext,text)<0)return;
        n=snprintf(line,sizeof line,"{\"cmd\":\"key\",\"action\":\"down\",\"key\":\"\",\"modifiers\":%u,\"text\":%s}",mods,qtext);
    }else return;
    if(n>0&&(size_t)n<sizeof line)zell_theater_submit(s,line);
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
    if(th.page_keys&&th.browser[0]&&!th.focus&&c->key_press){th_send_key(s,c);return 1;}
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
    int n;
    if(!dst||cap<32||!th.install_stored||!th.install_action[0]||!th.install_browser[0])return false;
    if(th.install_deb[0])n=snprintf(dst,cap,"{\"title\":\"Theater install\",\"browser\":\"%s\",\"action\":\"%s\",\"deb\":\"%s\"}",th.install_browser,th.install_action,th.install_deb);
    else n=snprintf(dst,cap,"{\"title\":\"Theater install\",\"browser\":\"%s\",\"action\":\"%s\"}",th.install_browser,th.install_action);
    return n>0&&(size_t)n<cap&&!strstr(dst,"argv")&&!strstr(dst,"sudo");
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
    if(th_u32(p+16)!=seq||th_u32(p+40)!=generation)return 0;
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
static void th_queue_layout(int width,int height,int *left,int *top,int *per,int *first,int *arrows){
    int right=width>8?width-4:width;
    int start=260,inner,n,pages,reserve=0;
    if(right<start+72)start=8;
    if(th.items>1&&(right-start)<th.items*78)reserve=44;
    inner=right-start-reserve;
    n=inner/78;
    if(n<1)n=1;
    if(n>TH_ITEMS)n=TH_ITEMS;
    pages=th.items>0?(th.items+n-1)/n:1;
    if(th.queue_page>=pages)th.queue_page=pages-1;
    if(th.queue_page<0)th.queue_page=0;
    *left=start;
    *top=height-ZELL_ROOM_STATUS_STRIP-28;
    *per=n;
    *first=th.queue_page*n;
    *arrows=reserve?1:0;
}
void zell_theater_queue_rect(int index,int width,int height,zell_rect *r){
    int left,top,per,first,arrows,slot;
    if(!r)return;
    *r=(zell_rect){0,0,0,0};
    if(index<0||index>=TH_ITEMS)return;
    th_queue_layout(width,height,&left,&top,&per,&first,&arrows);
    if(th.items>1){
        if(index<first||index>=first+per)return;
        slot=index-first;
        *r=(zell_rect){left+(arrows?22:0)+slot*78,top,72,24};
        return;
    }
    *r=(zell_rect){260+index*78,height-ZELL_ROOM_STATUS_STRIP-28,72,24};
}
bool zell_theater_queue_nav(int width,int height,int dir,zell_rect *out){
    int left,top,per,first,arrows;
    if(out)*out=(zell_rect){0,0,0,0};
    th_queue_layout(width,height,&left,&top,&per,&first,&arrows);
    if(!arrows||!out)return false;
    if(dir<0){
        if(first<=0)return false;
        *out=(zell_rect){left,top,20,24};
        return true;
    }
    if(first+per>=th.items)return false;
    *out=(zell_rect){left+22+per*78,top,20,24};
    return true;
}
static void th_send_install_result(bool ok){th_send_install_result_body(ok);}
bool zell_theater_install_action_ok(const char *browser,const char *action,const char *deb){
    if(!browser||!action)return false;
    if(strcmp(browser,"chrome")==0){
        if(strcmp(action,"install-chrome-deb")!=0)return false;
        return deb&&strcmp(deb,TH_CHROME_DEB)==0;
    }
    if(deb&&deb[0])return false;
    if(strcmp(browser,"firefox-esr")==0)return strcmp(action,"install-firefox-esr")==0;
    if(strcmp(browser,"chromium")==0)return strcmp(action,"install-chromium")==0;
    return false;
}
bool zell_theater_wants_keys(const zell_scene *s){
    (void)s;
    return th.page_keys&&th.browser[0]&&th.focus==0&&!th.menu;
}
int zell_theater_listing_count(void){return th.listing_n;}
const char *zell_theater_listing_path(int index){
    if(index<0||index>=th.listing_n)return "";
    return th.listing_path[index];
}
const char *zell_theater_picture(void){return th.picture;}
const char *zell_theater_queue_label(int index){
    if(index<0||index>=th.items)return "";
    return th.item_label[index];
}
bool zell_theater_playing(void){return th.playing;}
bool zell_theater_help_open(void){return th.help;}
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
    zell_rect frame,image,dock,close_r,button,nav;
    int dx,dy,dw,dh;
    if(!s||!c||!s->desktop)return;
    if(th.fullscreen){
        zell_rect picture,caption,help,drop;
        zell_theater_fullscreen_layout(c->w,c->h,&picture,&caption,&help,&drop);
        zell_ui_fill(c,0,0,c->w,c->h,0x05080cu,1.f);
        th_paint(c,picture.x,picture.y,picture.w,picture.h);
        zell_ui_fill(c,caption.x,caption.y,caption.w,caption.h,0x101820u,1.f);
        th_label(c,12,caption.y+14,th.caption[0]?th.caption:"",0xe7e2d4u,c->w-24);
        zell_ui_stroke(c,help.x,help.y,help.w,help.h,1.f,0xd9ba79u,1.f);
        th_label(c,help.x+8,help.y+2,"Help",0xf0d69bu,help.w-8);
        if(th.help){
            zell_ui_fill(c,drop.x,drop.y,drop.w,drop.h,0x10232cu,1.f);
            zell_ui_stroke(c,drop.x,drop.y,drop.w,drop.h,1.f,0xd9ba79u,1.f);
            th_label(c,drop.x+8,drop.y+4,"F11 toggles fullscreen",0xf0d69bu,drop.w-16);
            th_label(c,drop.x+8,drop.y+22,"Escape leaves fullscreen",0xf0d69bu,drop.w-16);
        }
    }else if(s->room==6&&!th.popout){
        th_screen_span(s,64.f,54.f,64.f+1408.f,54.f+585.f,&dx,&dy,&dw,&dh);
        th_paint(c,dx,dy,dw,dh);
        th_screen_span(s,82.f,677.f,82.f+1370.f,677.f+44.f,&dx,&dy,&dw,&dh);
        if(th.caption[0]){
            zell_ui_fill(c,dx,dy,dw,dh,0x101820u,.72f);
            th_label(c,dx+8,dy+12,th.caption,0xe7e2d4u,dw-16);
        }
    }
    if(th.popout&&!th.fullscreen){
        th_window(s,&frame,&image,&dock,&close_r);
        zell_ui_fill(c,frame.x,frame.y,frame.w,frame.h,0x10232cu,.98f);
        zell_ui_stroke(c,frame.x,frame.y,frame.w,frame.h,1.f,0x5b9989u,1.f);
        th_label(c,frame.x+8,frame.y+6,"Theater",0xf0d69bu,frame.w-120);
        th_paint(c,image.x,image.y,image.w,image.h);
        zell_ui_stroke(c,dock.x,dock.y,dock.w,dock.h,1.f,0xd9ba79u,1.f);
        th_label(c,dock.x+4,dock.y+2,"Dock",0xf0d69bu,dock.w-4);
        zell_ui_stroke(c,close_r.x,close_r.y,close_r.w,close_r.h,1.f,0xd9ba79u,1.f);
        th_label(c,close_r.x+4,close_r.y+2,"Close",0xf0d69bu,close_r.w-4);
    }
    if(zell_theater_queue_nav(c->w,c->h,-1,&nav))th_label(c,nav.x+2,nav.y+4,"<",0xf0d69bu,nav.w);
    if(zell_theater_queue_nav(c->w,c->h,1,&nav))th_label(c,nav.x+2,nav.y+4,">",0xf0d69bu,nav.w);
    for(int i=0;i<th.items;i++){
        int on=th.picture[0]&&strcmp(th.item_id[i],th.picture)==0;
        zell_theater_queue_rect(i,c->w,c->h,&button);
        if(button.w<1||button.x+button.w>c->w-4)continue;
        zell_ui_fill(c,button.x,button.y,button.w,button.h,on?0x254b50u:0x10232cu,.95f);
        th_label(c,button.x+4,button.y+4,th.item_label[i][0]?th.item_label[i]:th.item_id[i],0xe8ffe9u,button.w-8);
    }
    if(th.menu&&!th.fullscreen){
        th_menu_hit hit;
        th_menu_hit_build(s,&hit);
        if(hit.panel.w>0){
            zell_ui_fill(c,hit.panel.x,hit.panel.y,hit.panel.w,hit.panel.h,0x10232cu,.98f);
            zell_ui_stroke(c,hit.panel.x,hit.panel.y,hit.panel.w,hit.panel.h,1.f,0xd9ba79u,1.f);
            for(int i=0;i<hit.action_n;i++)th_label(c,hit.action[i].x,hit.action[i].y+2,th_row_label(i),i==0||i==3?0xf5dda7u:0xd5e4e6u,hit.action[i].w);
            for(int i=0;i<hit.file_n;i++)th_label(c,hit.file[i].x,hit.file[i].y+2,th.listing_name[hit.file_i[i]],0xc7d8dau,hit.file[i].w);
            if(hit.has_next)th_label(c,hit.next.x+4,hit.next.y+2,"Next",0xf0d69bu,hit.next.w-4);
        }
    }
    if(th.error[0])th_label(c,12,c->h-ZELL_ROOM_STATUS_STRIP-18,th.error,0xffb4a8u,c->w-24);
    else if(th.notice[0])th_label(c,12,c->h-ZELL_ROOM_STATUS_STRIP-18,th.notice,0xf0d69bu,c->w-24);
}
