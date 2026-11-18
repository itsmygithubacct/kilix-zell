/* Pane rooms, through the Command Center's top corners: a Kilix tab of two
 * panes on the left and of four on the right, side by side, filling the
 * whole Zell pane with no border. Kilix walks over them in canvas pixels,
 * arrow keys only; every other key goes to the pane last clicked.
 *
 * tools/zell_panes.py owns the shells and paints their cells, in its own
 * process group with its own pipes and map, as the theater's helper does.
 * The pump starts it the first time a pane room is shown, never the tick,
 * so tests walk in and out without a process. Its shells keep running while
 * Kilix is elsewhere; only the shown room is painted. The map starts with
 * 64 bytes: "ZPNE", version 1, width, height, sequence (odd while a frame
 * is written), room. */
#include <stdatomic.h>
#define PN_MAX_W 1920
#define PN_MAX_H 1920
#define PN_MAP_BYTES (64u+(size_t)PN_MAX_W*PN_MAX_H*4u)
#define PN_QUEUE 8192

typedef struct {
    pid_t pid;
    int in_fd,out_fd,frame_fd;
    void *map;
    uint32_t *shown,*scratch;
    size_t cap;
    uint32_t seq;
    int frame_w,frame_h,frame_room;
    /* Last room and size sent: room 0 is hidden. */
    int sent_room,sent_cols,sent_rows;
    bool failed;
    char error[160];
    char queue[PN_QUEUE];
    size_t queued;
    char line[256];
    size_t line_n;
} pn_state;

static pn_state pn={.in_fd=-1,.out_fd=-1,.frame_fd=-1};

/* A plain arrow walks Kilix; with Shift, Ctrl, Alt, Super, Hyper or Meta
 * it is the pane's (word moves, selections). Lock keys do not count. */
bool zell_panes_walks(bool arrow,unsigned modifiers){return arrow&&!(modifiers&0x3fu);}
int zell_panes_count(int room){return room==ZELL_ROOM_PANES_LEFT?2:room==ZELL_ROOM_PANES_RIGHT?4:0;}
/* The left room is entered from its right and is left the same way. */
int zell_panes_exit_side(int room){return room==ZELL_ROOM_PANES_LEFT?1:room==ZELL_ROOM_PANES_RIGHT?-1:0;}
static void pn_bounds(const zell_scene *s,float *x0,float *x1,float *y0,float *y1){
    int w=s->width>0?s->width:1,h=s->height>0?s->height:1;
    *x0=(float)ZELL_PANES_SIDE;*x1=(float)(w-ZELL_PANES_SIDE);
    *y0=(float)ZELL_PANES_TOP;*y1=(float)(h-ZELL_PANES_BOTTOM);
    if(*x1<*x0)*x1=*x0;
    if(*y1<*y0)*y1=*y0;
}
void zell_panes_exit_rect(const zell_scene *s,zell_rect *r){
    int side=zell_panes_exit_side(s->room),w=s->width>0?s->width:1;
    if(!side){*r=(zell_rect){0};return;}
    *r=(zell_rect){side>0?w-ZELL_PANES_EXIT_W:0,0,ZELL_PANES_EXIT_W,ZELL_PANES_EXIT_H};
}
static void pn_stand(zell_scene *s){
    s->mode=ZELL_MODE_WALK;s->running=false;s->run_charge=0;s->run_bump=0;s->run_blocked=false;
    s->step=0;s->step_ticks=0;s->desktop_launch=false;
}
void zell_panes_enter(zell_scene *s,int room){
    float x0,x1,y0,y1;
    if(!zell_panes_count(room))return;
    s->room=room;pn_stand(s);
    pn_bounds(s,&x0,&x1,&y0,&y1);
    /* Just inside, beside the line he came in by, facing into the room. */
    if(zell_panes_exit_side(room)>0){s->x=x1-48.f;s->direction=2;}
    else {s->x=x0+48.f;s->direction=3;}
    if(s->x<x0)s->x=x0;
    if(s->x>x1)s->x=x1;
    s->y=y0+(float)ZELL_PANES_EXIT_H*.5f;
    if(s->y>y1)s->y=y1;
}
void zell_panes_leave(zell_scene *s){
    int side=zell_panes_exit_side(s->room);
    if(!side)return;
    /* Back in the tunnel of the opening he left by, facing the room. */
    s->room=1;pn_stand(s);
    s->x=side>0?70.f:(float)(ZELL_WORLD_W-70);
    s->y=(float)(ZELL_COMMAND_OPENING_TOP+ZELL_COMMAND_OPENING_BOTTOM)*.5f;
    s->direction=side>0?3:2;
}
int zell_panes_tick(zell_scene *s,const zell_controls *c){
    float x0,x1,y0,y1,dx=(float)c->move_x,dy=(float)c->move_y,d=sqrtf(dx*dx+dy*dy);
    int side=zell_panes_exit_side(s->room);
    s->mode=ZELL_MODE_WALK;s->running=false;s->run_charge=0;s->run_bump=0;s->run_blocked=false;
    pn_bounds(s,&x0,&x1,&y0,&y1);
    if(d>0){
        float speed=ZELL_WALK_SPEED*zell_desktop_speed_factor(s);
        s->direction=dx<0?2:dx>0?3:dy<0?1:0;
        s->x+=dx/d*speed;s->y+=dy/d*speed;
        s->step_ticks++;s->step=(s->step_ticks/7)%2;
    }else{s->step=0;s->step_ticks=0;}
    if(s->x<x0)s->x=x0;
    if(s->x>x1)s->x=x1;
    if(s->y<y0)s->y=y0;
    if(s->y>y1)s->y=y1;
    /* Out by walking into the line: at the side, beside it, going that way. */
    if(s->y<y0+(float)ZELL_PANES_EXIT_H&&((side>0&&dx>0&&s->x>=x1)||(side<0&&dx<0&&s->x<=x0)))zell_panes_leave(s);
    return 1;
}
void zell_panes_cells(int width,int height,int *cols,int *rows){
    int c=width/8,r=height/16;
    if(c<20)c=20;
    if(c>PN_MAX_W/8)c=PN_MAX_W/8;
    if(r<8)r=8;
    if(r>PN_MAX_H/16)r=PN_MAX_H/16;
    *cols=c;*rows=r;
}
/* Cells are drawn 1:1 from the top left. Only a pane bigger than the
 * largest frame stretches it to fit. */
static void pn_frame_rect(const zell_scene *s,zell_rect *r){
    int cols,rows,w=s->width>0?s->width:1,h=s->height>0?s->height:1;
    zell_panes_cells(w,h,&cols,&rows);
    if(w>PN_MAX_W||h>PN_MAX_H)*r=(zell_rect){0,0,w,h};
    else *r=(zell_rect){0,0,cols*8,rows*16};
}
int zell_panes_column(const zell_scene *s,int x){
    int cols,rows;zell_rect r;
    pn_frame_rect(s,&r);
    zell_panes_cells(s->width>0?s->width:1,s->height>0?s->height:1,&cols,&rows);
    if(x<r.x||r.w<1)return -1;
    int column=(int)((int64_t)(x-r.x)*cols/r.w);
    return column<cols?column:cols-1;
}
static void pn_send(const char *text,size_t n){
    if(n==0||pn.queued+n>=sizeof pn.queue)return;
    memcpy(pn.queue+pn.queued,text,n);pn.queued+=n;pn.queue[pn.queued]=0;
}
const char *zell_panes_pending(void){pn.queue[pn.queued<sizeof pn.queue?pn.queued:sizeof pn.queue-1]=0;return pn.queue;}
void zell_panes_key(const unsigned char *bytes,size_t count){
    static const char hex[]="0123456789abcdef";
    char line[1024];
    size_t at=0;
    /* One line per batch; a batch that cannot fit whole is dropped whole. */
    if(!bytes||!count||2u*count+3u>sizeof line)return;
    line[at++]='K';
    for(size_t i=0;i<count;i++){line[at++]=hex[bytes[i]>>4];line[at++]=hex[bytes[i]&15u];}
    line[at++]='\n';
    pn_send(line,at);
}
bool zell_panes_click(zell_scene *s){
    char line[32];
    if(!ZELL_ROOM_IS_PANES(s->room)||!s->mouse_click||!s->mouse_valid)return false;
    s->mouse_click=false;
    int column=zell_panes_column(s,s->mouse_x);
    if(column<0)return true;
    int n=snprintf(line,sizeof line,"C %d\n",column);
    if(n>0&&(size_t)n<sizeof line)pn_send(line,(size_t)n);
    return true;
}
static void pn_close_fds(void){
    if(pn.map&&pn.map!=MAP_FAILED){munmap(pn.map,PN_MAP_BYTES);pn.map=NULL;}
    if(pn.frame_fd>=0){close(pn.frame_fd);pn.frame_fd=-1;}
    if(pn.in_fd>=0){close(pn.in_fd);pn.in_fd=-1;}
    if(pn.out_fd>=0){close(pn.out_fd);pn.out_fd=-1;}
}
static void pn_fail(const char *text){
    size_t n=0;
    while(text[n]&&n+1<sizeof pn.error){pn.error[n]=text[n];n++;}
    pn.error[n]=0;
    pn.failed=true;
}
static bool pn_start(void){
    char script[4096],fd_text[24];
    int to_child[2],from_child[2];
    if(pn.pid>0)return true;
    if(!th_script(script,sizeof script,"zell_panes.py")){pn_fail("The panes need tools/zell_panes.py beside Zell.");return false;}
    if(pipe(to_child)!=0){pn_fail("Could not start the panes.");return false;}
    if(pipe(from_child)!=0){close(to_child[0]);close(to_child[1]);pn_fail("Could not start the panes.");return false;}
    pn.frame_fd=(int)syscall(SYS_memfd_create,"zell-panes",0);
    if(pn.frame_fd<0||ftruncate(pn.frame_fd,(off_t)PN_MAP_BYTES)!=0){
        close(to_child[0]);close(to_child[1]);close(from_child[0]);close(from_child[1]);
        pn_close_fds();pn_fail("Could not make the panes' picture.");return false;
    }
    pn.map=mmap(NULL,PN_MAP_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,pn.frame_fd,0);
    if(pn.map==MAP_FAILED){
        pn.map=NULL;
        close(to_child[0]);close(to_child[1]);close(from_child[0]);close(from_child[1]);
        pn_close_fds();pn_fail("Could not map the panes' picture.");return false;
    }
    memset(pn.map,0,64);
    pn.pid=fork();
    if(pn.pid==0){
        if(setsid()<0)_exit(127);
        if(dup2(to_child[0],STDIN_FILENO)<0||dup2(from_child[1],STDOUT_FILENO)<0)_exit(127);
        int nullfd=open("/dev/null",O_WRONLY);
        if(nullfd>=0){dup2(nullfd,STDERR_FILENO);if(nullfd>2)close(nullfd);}
        /* The map is fd 3 and nothing else of Zell's is open: the helper's
         * shells live long and must not hold Zell's other pipes. */
        if(pn.frame_fd!=3&&dup2(pn.frame_fd,3)<0)_exit(127);
        syscall(SYS_close_range,4u,~0u,0);
        snprintf(fd_text,sizeof fd_text,"%d",3);
        setenv("ZELL_PANES_FD",fd_text,1);
        signal(SIGINT,SIG_DFL);signal(SIGTERM,SIG_DFL);signal(SIGPIPE,SIG_DFL);signal(SIGHUP,SIG_DFL);signal(SIGXFSZ,SIG_DFL);
        execlp("python3","python3",script,(char *)NULL);
        _exit(127);
    }
    close(to_child[0]);close(from_child[1]);
    if(pn.pid<0){
        pn.pid=0;close(to_child[1]);close(from_child[0]);
        pn_close_fds();pn_fail("Could not start the panes.");return false;
    }
    pn.in_fd=to_child[1];pn.out_fd=from_child[0];
    fcntl(pn.frame_fd,F_SETFD,FD_CLOEXEC);
    fcntl(pn.in_fd,F_SETFL,fcntl(pn.in_fd,F_GETFL)|O_NONBLOCK);
    fcntl(pn.out_fd,F_SETFL,fcntl(pn.out_fd,F_GETFL)|O_NONBLOCK);
    fcntl(pn.in_fd,F_SETFD,FD_CLOEXEC);
    fcntl(pn.out_fd,F_SETFD,FD_CLOEXEC);
    pn.seq=0;pn.frame_w=pn.frame_h=pn.frame_room=0;pn.line_n=0;
    /* A new helper has no rooms yet: the next pump sends the shown one. */
    pn.sent_room=-1;pn.sent_cols=pn.sent_rows=0;
    return true;
}
static void pn_flush(void){
    while(pn.queued>0&&pn.in_fd>=0){
        ssize_t wrote=write(pn.in_fd,pn.queue,pn.queued);
        if(wrote<0){
            if(errno==EINTR)continue;
            if(errno!=EAGAIN&&errno!=EWOULDBLOCK)pn_fail("The panes' pipe closed.");
            return;
        }
        memmove(pn.queue,pn.queue+wrote,pn.queued-(size_t)wrote);
        pn.queued-=(size_t)wrote;pn.queue[pn.queued]=0;
    }
}
static void pn_read(void){
    if(pn.out_fd<0)return;
    for(int batch=0;batch<8;batch++){
        char bytes[512];
        ssize_t count=read(pn.out_fd,bytes,sizeof bytes);
        if(count<0&&errno==EINTR)continue;
        if(count<=0)return;
        for(ssize_t i=0;i<count;i++){
            if(bytes[i]!='\n'){if(pn.line_n+1<sizeof pn.line)pn.line[pn.line_n++]=bytes[i];continue;}
            pn.line[pn.line_n]=0;
            if(pn.line[0]=='E'&&pn.line[1]==' ')pn_fail(pn.line+2);
            pn.line_n=0;
        }
    }
}
static bool pn_ensure(size_t pixels){
    if(pn.cap>=pixels&&pn.shown&&pn.scratch)return true;
    uint32_t *a=calloc(pixels,4),*b=calloc(pixels,4);
    if(!a||!b){free(a);free(b);return false;}
    free(pn.shown);free(pn.scratch);
    pn.shown=a;pn.scratch=b;pn.cap=pixels;
    return true;
}
/* Sequence first, then the header and pixels, then the sequence again: a
 * frame is taken only if no write began or ended in between. */
static void pn_copy(void){
    const unsigned char *p=pn.map;
    if(!p)return;
    uint32_t seq=th_u32(p+16);
    atomic_thread_fence(memory_order_acquire);
    if(!seq||(seq&1u)||seq==pn.seq)return;
    if(memcmp(p,"ZPNE",4)!=0||th_u32(p+4)!=1)return;
    uint32_t w=th_u32(p+8),h=th_u32(p+12),room=th_u32(p+20);
    if(w<1||h<1||w>PN_MAX_W||h>PN_MAX_H||!zell_panes_count((int)room))return;
    if(!pn_ensure((size_t)w*h))return;
    memcpy(pn.scratch,p+64,(size_t)w*h*4u);
    atomic_thread_fence(memory_order_acquire);
    if(th_u32(p+16)!=seq)return;
    uint32_t *swap=pn.shown;pn.shown=pn.scratch;pn.scratch=swap;
    pn.seq=seq;pn.frame_w=(int)w;pn.frame_h=(int)h;pn.frame_room=(int)room;
}
static void pn_reap(void){
    int status=0;
    if(pn.pid<=0)return;
    if(waitpid(pn.pid,&status,WNOHANG)!=pn.pid)return;
    /* Its last picture goes too, so the room says what happened. */
    pn.pid=0;pn_close_fds();pn.queued=0;pn.frame_room=0;pn.seq=0;
    if(!pn.failed)pn_fail("The panes stopped. Walk out and in again to start new shells.");
}
void zell_panes_pump(zell_scene *s){
    bool shown=ZELL_ROOM_IS_PANES(s->room);
    /* A failed helper is tried again on the next walk in. */
    if(!shown&&pn.pid<=0)pn.failed=false;
    if(shown&&pn.pid<=0&&!pn.failed){pn.queued=0;pn_start();}
    if(pn.pid<=0)return;
    pn_read();
    int room=shown?s->room:0,cols=0,rows=0;
    if(shown)zell_panes_cells(s->width,s->height,&cols,&rows);
    if(room!=pn.sent_room||cols!=pn.sent_cols||rows!=pn.sent_rows){
        char line[48];int n=room?snprintf(line,sizeof line,"S %d %d %d\n",room,cols,rows):snprintf(line,sizeof line,"H\n");
        /* After the lines already queued, which may be half written and
         * belong to the room he was in; until it fits it is tried again. */
        if(n>0&&(size_t)n<sizeof line&&pn.queued+(size_t)n<sizeof pn.queue){
            pn_send(line,(size_t)n);
            pn.sent_room=room;pn.sent_cols=cols;pn.sent_rows=rows;
        }
    }
    pn_flush();
    pn_copy();
    pn_reap();
}
const uint32_t *zell_panes_frame(int room,int *width,int *height){
    if(!pn.shown||pn.frame_room!=room||pn.frame_w<1||pn.frame_h<1)return NULL;
    *width=pn.frame_w;*height=pn.frame_h;
    return pn.shown;
}
const char *zell_panes_error(void){return pn.failed?pn.error:"";}
void zell_panes_shutdown(void){
    struct timespec delay={.tv_nsec=10000000};
    if(pn.pid>0){
        if(pn.in_fd>=0){ssize_t wrote=write(pn.in_fd,"Q\n",2);(void)wrote;}
        for(int64_t deadline=zell_notice_clock_ms()+700;pn.pid>0&&zell_notice_clock_ms()<deadline;){
            pid_t ended=waitpid(pn.pid,NULL,WNOHANG);
            if(ended==pn.pid||(ended<0&&errno==ECHILD))pn.pid=0;
            else nanosleep(&delay,NULL);
        }
        if(pn.pid>0){
            if(kill(-pn.pid,SIGTERM)<0)kill(pn.pid,SIGTERM);
            for(int64_t deadline=zell_notice_clock_ms()+500;pn.pid>0&&zell_notice_clock_ms()<deadline;){
                pid_t ended=waitpid(pn.pid,NULL,WNOHANG);
                if(ended==pn.pid||(ended<0&&errno==ECHILD))pn.pid=0;
                else nanosleep(&delay,NULL);
            }
        }
        if(pn.pid>0){
            if(kill(-pn.pid,SIGKILL)<0)kill(pn.pid,SIGKILL);
            while(waitpid(pn.pid,NULL,0)<0&&errno==EINTR){}
            pn.pid=0;
        }
    }
    pn_close_fds();
    free(pn.shown);free(pn.scratch);pn.shown=pn.scratch=NULL;pn.cap=0;
}
/* Tests hand the copy a map of their own (from mmap, PN_MAP_BYTES long, so
 * the reset can unmap it) and fail the helper without starting one. */
void zell_panes_test_copy(void *map){if(!pn.map)pn.map=map;pn_copy();}
void zell_panes_test_fail(const char *text){pn_fail(text);pn.frame_room=0;}
size_t zell_panes_map_bytes(void){return PN_MAP_BYTES;}
void zell_panes_test_reset(void){
    zell_panes_shutdown();
    memset(&pn,0,sizeof pn);
    pn.in_fd=pn.out_fd=pn.frame_fd=-1;
}
