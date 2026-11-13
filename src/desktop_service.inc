/* Native station helper transport. No child has access to the pane's terminal. */
typedef struct {
    pid_t pid;
    int in_fd,out_fd;
    char frame[8192];
    size_t length;
    int64_t cancelled;
    bool monitor;
    bool sensitive;
    int frame_fd;
    void *frame_map;
    unsigned frame_seq;
    uint32_t *frame_scratch;
    /* Slot and generation captured when this job reserved its window.
     * A frame keeps this identity after the slot's generation changes. */
    int window_slot;
    int window_generation;
} desktop_service;

#define ZELL_TERMINAL_BYTES (32u+1200u*640u*4u)
static int service_hex(char c){return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;}
static void service_append(zell_scene *s,const char *text){
    size_t n=strlen(text),used=strlen(s->service_text),cap=sizeof s->service_text-1;
    if(n>cap){text+=n-cap;n=cap;}
    if(used+n>cap){size_t drop=used+n-cap;memmove(s->service_text,s->service_text+drop,used-drop+1);used-=drop;}
    memcpy(s->service_text+used,text,n+1);
}
enum { SERVICE_ORIGIN_JOB=0, SERVICE_ORIGIN_MONITOR=1 };
static const struct { char kind; bool job; bool monitor; } zell_frame_registry[]={
 {'C',true,false},{'M',true,true},{'L',true,false},{'P',true,false},{'Q',true,false},
 {'R',true,false},{'O',true,false},{'T',true,false},
 {'G',false,true},{'H',false,true},{'F',false,true},{'U',false,true},
 {'V',false,true},{'Y',false,true},{'N',false,true},{'S',false,true},{'W',false,true},
 {'A',false,true},{'J',false,true},{'D',false,true},{'B',false,true}
};
static int frame_origin_allowed(char kind,int origin){
 for(size_t i=0;i<sizeof zell_frame_registry/sizeof zell_frame_registry[0];i++){
  if(zell_frame_registry[i].kind!=kind)continue;
  return (origin==SERVICE_ORIGIN_MONITOR?zell_frame_registry[i].monitor:zell_frame_registry[i].job)?1:0;
 }
 return -1;
}
/* Three integers and nothing after them. A successful sscanf with leftover text is not a reading. */
static void service_mana(zell_scene *s,const char *text){
    int v[3],n=0;
    if(sscanf(text,"%d %d %d%n",&v[0],&v[1],&v[2],&n)!=3||text[n])return;
    for(int i=0;i<3;i++)s->provider_mana[i]=v[i]<-2||v[i]>100?-1:v[i];
    s->provider_mana_ready=true;
}
static int window_generation_clock;
static int window_used_count(const zell_scene *s){
    int count=0;
    for(int i=0;i<ZELL_WINDOWS;i++)if(s->windows[i].used)count++;
    return count;
}
static int window_next_generation(const zell_scene *s){
    int floor=window_generation_clock;
    for(int i=0;i<ZELL_WINDOWS;i++)if(s->windows[i].generation>floor)floor=s->windows[i].generation;
    int generation=floor+1;
    if(generation<=0||generation>1000000000)generation=1;
    for(int attempt=0;attempt<ZELL_WINDOWS+1;attempt++){
        bool taken=false;
        for(int i=0;i<ZELL_WINDOWS;i++)if(s->windows[i].generation==generation)taken=true;
        if(!taken&&generation!=0){window_generation_clock=generation;return generation;}
        if(++generation<=0||generation>1000000000)generation=1;
    }
    window_generation_clock=generation;
    return generation;
}
/* Drop a slot when its owning job is gone. A generation of 0 is the legacy
 * terminal and does not own a reserved slot. close_requested stays visible. */
static void window_release_owner(zell_scene *s,int slot,int generation){
    if(!s||generation==0||slot<0||slot>=ZELL_WINDOWS)return;
    zell_window *w=&s->windows[slot];
    if(w->generation!=generation)return;
    bool close_requested=w->close_requested;
    memset(w,0,sizeof *w);
    w->close_requested=close_requested;
}
/* Reserve window 0 for the one station service. At four used slots, leave
 * every running job alone: do not kill one and do not overwrite its slot. */
static bool window_prepare_slot0(zell_scene *s,int *generation){
    if(window_used_count(s)>=ZELL_WINDOWS)return false;
    int open_order=1,z_order=0,gen=window_next_generation(s);
    for(int i=0;i<ZELL_WINDOWS;i++){
        if(s->windows[i].open_order>=open_order)open_order=s->windows[i].open_order+1;
        if(s->windows[i].used&&s->windows[i].z_order>=z_order)z_order=s->windows[i].z_order+1;
    }
    zell_window *w=&s->windows[0];
    *w=(zell_window){0};
    w->generation=gen;
    w->open_order=open_order;
    w->z_order=z_order;
    w->used=true;
    w->owner_service=0;
    *generation=gen;
    return true;
}
static void service_frame(zell_scene *s,char *frame,int origin,int slot,int generation){
    /* A job frame is ignored unless zell_window_accepts_frame says this slot
     * still has the generation captured with the owning pipe. */
    char kind=frame[0],decoded[4096];size_t n=0;
    for(size_t i=1;frame[i]&&frame[i+1]&&n+1<sizeof decoded;i+=2){
        int a=service_hex(frame[i]),b=service_hex(frame[i+1]);if(a<0||b<0)return;
        unsigned char c=(unsigned char)(a*16+b);
        if(c>=32||c=='\n'||c=='\t')decoded[n++]=(char)c;
    }
    decoded[n]=0;
    int allowed=frame_origin_allowed(kind,origin);
    if(allowed==0)return;
    if(origin==SERVICE_ORIGIN_MONITOR){
        /* L5 calls zell_provider_apply. */
        if(kind=='G'){
            int a,b,c,used=0;
            if(sscanf(decoded,"%d %d %d%n",&a,&b,&c,&used)==3&&!decoded[used]){service_mana(s,decoded);return;}
            (void)a;(void)b;(void)c;
            zell_provider_apply(s,decoded);return;
        }
        /* L5 calls zell_provider_check_apply. */
        if(kind=='H'){zell_provider_check_apply(s,decoded);return;}
        /* L6 calls zell_fleet_apply. */
        if(kind=='F'){zell_fleet_apply(s,decoded);return;}
        if(kind=='U'){zell_update_apply(s,decoded);return;}
        /* Legacy M carries the same three integers for one transition, never a menu. */
        if(kind=='M'){service_mana(s,decoded);return;}
        if(kind=='J'){s->restart_needed=decoded[0]=='1';return;}
        if(kind=='Y'){
            s->coding_yolo_ready=n==1&&(decoded[0]=='0'||decoded[0]=='1');
            s->coding_yolo=s->coding_yolo_ready&&decoded[0]=='1';return;
        }
    if(kind=='N'){
        int count=0;
        if(sscanf(decoded,"%d",&count)==1&&count>=0&&count<=200){
            char *text=strchr(decoded,' ');
            const char *body=text?text+1:"";
            s->notice_count=count;
            /* A later sample of the same words does not move the deadline. */
            zell_notice_apply(s,count>0&&body[0]?body:"");
        }
        return;
    }
    if(kind=='A'){int muted,active;if(sscanf(decoded,"%d %d",&muted,&active)==2){s->music_muted=muted==1;s->dj_active=active==1;}return;}
    if(kind=='W'){
        /* Each reading replaces the previous one, including location removal. */
        s->weather_state=0;s->weather_temperature[0]=s->weather_place[0]=s->weather_condition[0]=0;
        int state,icon;
        if(sscanf(decoded,"%d %d",&state,&icon)!=2||state<0||state>2||icon<0||icon>7)return;
        char *place=strchr(decoded,'\n');if(!place)return;*place++=0;
        char *condition=strchr(place,'\n');if(!condition)return;*condition++=0;
        char *temperature=strchr(decoded,' ');if(temperature)temperature=strchr(temperature+1,' ');
        if(!temperature)return;
        s->weather_state=state;s->weather_icon=icon;
        snprintf(s->weather_temperature,sizeof s->weather_temperature,"%.23s",temperature+1);
        snprintf(s->weather_place,sizeof s->weather_place,"%.95s",place);
        snprintf(s->weather_condition,sizeof s->weather_condition,"%.63s",condition);return;
    }
    if(kind=='S'){
        if(n<2||decoded[0]<'0'||decoded[0]>'4'||decoded[1]!=' ')return;
        int i=decoded[0]-'0';char *body=strchr(decoded+2,'\n');
        if(!body)return;
        *body++=0;snprintf(s->screen_status[i],sizeof s->screen_status[i],"%.95s",decoded+2);
        snprintf(s->screen_preview[i],sizeof s->screen_preview[i],"%.1199s",body);s->screens_ready=true;return;
    }
    if(kind=='V'){
        int hp,fatigue,mana,days,age,baseline,dirty;
        int fields=sscanf(decoded,"%d %d %d %d %d %d %d",&hp,&fatigue,&mana,&days,&age,&baseline,&dirty);
        if((fields==5||fields==7)&&hp>=-1&&hp<=100&&fatigue>=-1&&fatigue<=100&&mana>=-1&&mana<=100){
            s->hp=hp;s->fatigue=fatigue;s->mana=mana;s->update_age_days=days;s->mana_age=age;
            if(fields==7&&baseline>=-1&&baseline<=100&&dirty>=-1){s->os_health=baseline;s->dirty_files=dirty;}
            else {s->os_health=hp;s->dirty_files=-1;}
        }
        return;
    }
    if(kind=='D'){s->disk_count=0;s->disks_ready=true;return;}
    if(kind=='B'&&s->disk_count<8){
        int used;char label[96];
        if(sscanf(decoded,"%d %95[^\n]",&used,label)==2&&used>=-1&&used<=100){
            int i=s->disk_count++;s->disk_used[i]=used;snprintf(s->disk_labels[i],96,"%s",label);
        }
        return;
    }
    return;
    }
    /* Telemetry belongs to the monitor. A job M is a menu even when the title is numeric. */
    if(kind&&strchr("GHFUVYNSWAJDB",kind))return;
    if(kind&&strchr("TPQROLCM",kind)&&!zell_window_accepts_frame(s,slot,generation))return;
    if(kind=='T'){s->service_phase=5;s->terminal_maximized=s->terminal_minimized=false;return;}
    /* A helper that leaves the terminal for a prompt or report must be seen. */
    if(kind=='P'||kind=='Q'||kind=='R')zell_terminal_restore(s);
    if(kind=='O')service_append(s,decoded);
    if(kind=='C'||kind=='M'){s->service_text[0]=0;s->service_scroll=0;}
    if(kind=='M'){s->service_count=0;s->service_index=0;}
    if(kind=='P'||kind=='M')snprintf(s->service_title,sizeof s->service_title,"%.511s",decoded);
    if(kind=='L'&&s->service_count<36)snprintf(s->service_choices[s->service_count++],320,"%.319s",decoded);
    if(kind=='P'){s->service_phase=1;s->service_input[0]=0;}
    if(kind=='Q')s->service_phase=2;
    if(kind=='R')s->service_phase=3;
}
static void service_stop(desktop_service *p){
    if(p->pid>0){
        /* Give the Python helper time to close and reap its separate PTY session. */
        if(kill(-p->pid,SIGTERM)<0)kill(p->pid,SIGTERM);
        int64_t deadline=now_ms()+500;pid_t ended=0;
        while(now_ms()<deadline){
            ended=waitpid(p->pid,NULL,WNOHANG);
            if(ended==p->pid||(ended<0&&errno==ECHILD))break;
            struct timespec delay={.tv_nsec=10000000};nanosleep(&delay,NULL);
        }
        if(ended!=p->pid&&!(ended<0&&errno==ECHILD)){
            if(kill(-p->pid,SIGKILL)<0)kill(p->pid,SIGKILL);
            while(waitpid(p->pid,NULL,0)<0&&errno==EINTR){}
        }
    }
    if(p->frame_map){explicit_bzero(p->frame_map,ZELL_TERMINAL_BYTES);munmap(p->frame_map,ZELL_TERMINAL_BYTES);close(p->frame_fd);}
    if(p->frame_scratch){explicit_bzero(p->frame_scratch,1200u*640u*4u);free(p->frame_scratch);}
    if(p->in_fd>=0)close(p->in_fd);
    if(p->out_fd>=0)close(p->out_fd);
    *p=(desktop_service){.in_fd=-1,.out_fd=-1,.frame_fd=-1};
}
static void terminal_size(desktop_service *p,const zell_scene *s){
    if(!p->frame_map)return;
    volatile uint32_t *header=p->frame_map;
    int cols,rows;zell_terminal_cells(s,&cols,&rows);
    header[0]=(uint32_t)cols;header[1]=(uint32_t)rows;
}
/* Candidate pixels never touch the visible buffer until the sequence validates. */
static void terminal_frame(desktop_service *p,zell_scene *s){
    if(!p->frame_map||!p->frame_scratch||!s->terminal_pixels)return;
    volatile uint32_t *h=p->frame_map;
    unsigned seq=h[4];__sync_synchronize();
    if(!seq||(seq&1)||seq==p->frame_seq)return;
    unsigned w=h[2],height=h[3];
    if(!w||!height||w>1200||height>640)return;
    memcpy(p->frame_scratch,(char *)p->frame_map+32,(size_t)w*height*4);
    __sync_synchronize();
    if(h[4]!=seq)return;
    uint32_t *previous=s->terminal_pixels;s->terminal_pixels=p->frame_scratch;p->frame_scratch=previous;
    s->terminal_width=(int)w;s->terminal_height=(int)height;p->frame_seq=seq;
}
static void service_start_mode(desktop_service *p,zell_scene *s,bool monitor){
    int generation=0;
    /* The new generation is on the slot before the previous helper is stopped
     * and before the new helper is forked. Four used slots skip both. */
    if(!monitor&&!window_prepare_slot0(s,&generation)){s->desktop_launch=false;return;}
    service_stop(p);
    p->monitor=monitor;
    p->window_slot=0;
    p->window_generation=monitor?0:generation;
    /* The Workshop computer uses the floating terminal, not a private vault. */
    bool workshop=!monitor&&s->station==ZELL_WORKSHOP_COMPUTER;
    p->sensitive=!monitor&&!workshop&&(s->station==0||s->station==2||s->station==3||s->station==8||s->station==9||s->station>=31);
    if(!monitor){
    if(s->terminal_pixels){explicit_bzero(s->terminal_pixels,1200u*640u*4u);free(s->terminal_pixels);}
    s->terminal_pixels=NULL;s->terminal_width=s->terminal_height=0;
    s->desktop_launch=false;s->mode=ZELL_MODE_SERVICE;s->service_phase=0;
    s->terminal_maximized=s->terminal_minimized=false;
    s->terminal_private=p->sensitive;
    snprintf(s->terminal_title,sizeof s->terminal_title,"%s",s->station==ZELL_DJ_STATION&&s->menu_index==ZELL_DJ_BROWSER_ACTION?"Browser":zell_stations[s->station].name);
    snprintf(s->windows[0].title,sizeof s->windows[0].title,"%s",s->terminal_title);
    s->windows[0].private_surface=p->sensitive;
    s->windows[0].minimized=s->windows[0].maximized=false;
    s->service_text[0]=s->service_title[0]=s->service_input[0]=0;
    s->service_scroll=0;s->service_send=s->service_cancel=false;
    }
    char script[4096],station[16],action[16];int to_child[2],from_child[2];
    ssize_t n=readlink("/proc/self/exe",script,sizeof script-32);
    if(n<0)goto failed;
    script[n]=0;char *slash=strrchr(script,'/');if(!slash)goto failed;
    strcpy(slash+1,"tools/desktop.py");
    snprintf(station,sizeof station,"%d",monitor?-1:s->station);snprintf(action,sizeof action,"%d",s->menu_index);
    if(pipe(to_child)!=0)goto failed;
    if(pipe(from_child)!=0){close(to_child[0]);close(to_child[1]);goto failed;}
    if(!monitor&&(p->sensitive||workshop)){
        p->frame_fd=(int)syscall(SYS_memfd_create,"zell-terminal",0);
        if(p->frame_fd>=0&&ftruncate(p->frame_fd,(off_t)ZELL_TERMINAL_BYTES)==0){
            p->frame_map=mmap(NULL,ZELL_TERMINAL_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,p->frame_fd,0);
            if(p->frame_map==MAP_FAILED)p->frame_map=NULL;
        }
        if(!p->frame_map){if(p->frame_fd>=0)close(p->frame_fd);p->frame_fd=-1;}
        else {
            terminal_size(p,s);s->terminal_pixels=calloc(1200u*640u,4);
            p->frame_scratch=malloc(1200u*640u*4u);
        }
        if(!p->frame_map||!s->terminal_pixels||!p->frame_scratch){
            close(to_child[0]);close(to_child[1]);close(from_child[0]);close(from_child[1]);
            service_stop(p);free(s->terminal_pixels);s->terminal_pixels=NULL;
            goto failed;
        }
    }
    p->pid=fork();
    if(p->pid==0){
        if(setsid()<0)_exit(127);
        dup2(to_child[0],STDIN_FILENO);dup2(from_child[1],STDOUT_FILENO);
        int null=open("/dev/null",O_WRONLY);if(null>=0){dup2(null,STDERR_FILENO);close(null);}
        close(to_child[0]);close(to_child[1]);close(from_child[0]);close(from_child[1]);
        if(p->frame_map){char fd[24];snprintf(fd,sizeof fd,"%d",p->frame_fd);setenv("ZELL_TERMINAL_FD",fd,1);}
        signal(SIGINT,SIG_DFL);signal(SIGTERM,SIG_DFL);signal(SIGPIPE,SIG_DFL);
        execlp("python3","python3",script,station,action,"--dialogue",(char *)NULL);_exit(127);
    }
    close(to_child[0]);close(from_child[1]);p->in_fd=to_child[1];p->out_fd=from_child[0];
    if(p->pid<0){service_stop(p);goto failed;}
    if(p->frame_map)fcntl(p->frame_fd,F_SETFD,FD_CLOEXEC);
    fcntl(p->out_fd,F_SETFL,fcntl(p->out_fd,F_GETFL)|O_NONBLOCK);
    fcntl(p->in_fd,F_SETFL,fcntl(p->in_fd,F_GETFL)|O_NONBLOCK);
    fcntl(p->out_fd,F_SETFD,FD_CLOEXEC);fcntl(p->in_fd,F_SETFD,FD_CLOEXEC);
    return;
failed:
    if(!monitor)window_release_owner(s,0,generation);
    if(monitor)return;
    strcpy(s->service_text,"Could not start this station's helper.");s->service_phase=4;
}
static void service_start(desktop_service *p,zell_scene *s){service_start_mode(p,s,false);}
static bool service_owns_window(const desktop_service *p,const zell_scene *s,int slot){
    if(slot<0||slot>=ZELL_WINDOWS)return false;
    /* Generation 0 is the legacy terminal only while the slot is still unarmed. */
    if(p->window_generation==0)return slot==0&&s->windows[slot].generation==0;
    return s->windows[slot].generation==p->window_generation;
}
static void service_pump(desktop_service *p,zell_scene *s){
    if(p->pid<=0)return;
    if(!p->monitor){
        int slot=p->window_slot;
        if(service_owns_window(p,s,slot)&&s->windows[slot].close_requested&&!s->windows[slot].cancel_blocked){
            int generation=p->window_generation;bool sensitive=p->sensitive;
            service_stop(p);
            window_release_owner(s,slot,generation);
            zell_terminal_restore(s);s->service_phase=4;s->service_scroll=0;
            if(sensitive){
                if(s->terminal_pixels){explicit_bzero(s->terminal_pixels,1200u*640u*4u);free(s->terminal_pixels);s->terminal_pixels=NULL;s->terminal_width=s->terminal_height=0;}
                explicit_bzero(s->service_reply,sizeof s->service_reply);
                explicit_bzero(s->service_input,sizeof s->service_input);
            }
            return;
        }
    }
    if(p->monitor&&s->coding_yolo_refresh){
        if(write(p->in_fd,"Y\n",2)==2)s->coding_yolo_refresh=false;
    }
    if(!p->monitor&&s->service_cancel&&!p->cancelled){kill(-p->pid,SIGTERM);p->cancelled=now_ms();s->service_cancel=false;}
    if(p->cancelled&&now_ms()-p->cancelled>500)kill(-p->pid,SIGKILL);
    if(!p->monitor&&s->service_send){
        size_t length=strlen(s->service_reply);ssize_t sent=write(p->in_fd,s->service_reply,length);
        if(sent==(ssize_t)length){s->service_send=false;memset(s->service_reply,0,sizeof s->service_reply);}
        else if(sent<0&&errno!=EAGAIN&&errno!=EINTR){s->service_send=false;kill(-p->pid,SIGTERM);p->cancelled=now_ms();}
    }
    terminal_size(p,s);
    terminal_frame(p,s);
    char bytes[4096];ssize_t count;
    /* Bound work per frame even if a helper produces continuous output. */
    for(int batch=0;batch<8;batch++){
        count=read(p->out_fd,bytes,sizeof bytes);if(count<=0)break;
        for(ssize_t i=0;i<count;i++){
            if(bytes[i]=='\n'){
                /* A frame that does not fit is discarded whole, so a truncated prefix cannot apply. */
                if(p->length&&p->length<sizeof p->frame){
                    p->frame[p->length]=0;
                    if(!p->monitor||strchr("VDBSWJNAYGHFMU",p->frame[0]))
                        service_frame(s,p->frame,p->monitor?SERVICE_ORIGIN_MONITOR:SERVICE_ORIGIN_JOB,p->window_slot,p->window_generation);
                }
                p->length=0;
            }else if(p->length+1<sizeof p->frame)p->frame[p->length++]=bytes[i];
            else p->length=sizeof p->frame;
        }
    }
    if(count>0)return; /* Drain the remaining frames before reaping. */
    int status=0;pid_t ended=waitpid(p->pid,&status,WNOHANG);
    if(ended==p->pid){
        p->pid=0;
        if(p->monitor){s->fatigue=-1;s->screens_ready=false;s->coding_yolo_ready=false;s->weather_state=0;memset(s->screen_preview,0,sizeof s->screen_preview);service_stop(p);return;}
        if(p->cancelled)service_append(s,"\nCancelled.");
        else if(!WIFEXITED(status)||WEXITSTATUS(status)!=0)service_append(s,"\nThe station helper failed. Please try again.");
        else if(!s->service_text[0])service_append(s,"Done.");
        bool sensitive=p->sensitive;
        int slot=p->window_slot,generation=p->window_generation;
        service_stop(p);zell_terminal_restore(s);s->service_phase=4;s->service_scroll=0;
        window_release_owner(s,slot,generation);
        if((s->station==45||s->station==46)&&WIFEXITED(status)&&WEXITSTATUS(status)==0)s->mode=ZELL_MODE_WALK;
        if(sensitive){
            if(s->terminal_pixels){explicit_bzero(s->terminal_pixels,1200u*640u*4u);free(s->terminal_pixels);s->terminal_pixels=NULL;s->terminal_width=s->terminal_height=0;}
            explicit_bzero(s->service_reply,sizeof s->service_reply);
            explicit_bzero(s->service_input,sizeof s->service_input);
        }
    }
}
