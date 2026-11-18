/* Exercise the actual question process and pump inside a controlling terminal. */
int zell_application_main(int argc, char **argv);
#define main zell_application_main
#include "../src/main.c"
#undef main
#include <assert.h>

static int graceful_fd=-1;
static void graceful_exit(int signum){
    (void)signum;
    const char done='G';(void)write(graceful_fd,&done,1);_exit(0);
}

static void test_terminal_transport(zell_scene *scene){
    char restart[]="J31",notice[]="N32204a6f622066696e6973686564",audio[]="A312031";
    service_frame(scene,restart,SERVICE_ORIGIN_MONITOR,0,0);service_frame(scene,notice,SERVICE_ORIGIN_MONITOR,0,0);service_frame(scene,audio,SERVICE_ORIGIN_MONITOR,0,0);
    assert(scene->restart_needed&&scene->notice_count==2&&!strcmp(scene->notice_text,"Job finished"));
    assert(scene->music_muted&&scene->dj_active);
    int dictpipe[2];assert(pipe(dictpipe)==0);
    pid_t dictchild=fork();assert(dictchild>=0);
    if(!dictchild){close(dictpipe[0]);assert(write(dictpipe[1],"Review these words\n",19)==19);close(dictpipe[1]);_exit(0);}
    close(dictpipe[1]);fcntl(dictpipe[0],F_SETFL,O_NONBLOCK);
    desktop_dictation dict={.pid=dictchild,.fd=dictpipe[0],.started=now_ms()};
    scene->mode=ZELL_MODE_WALK;scene->request_ready=false;
    for(int attempt=0;attempt<100&&dict.pid>0;attempt++){dictation_sync(&dict,scene);usleep(1000);}
    assert(dict.pid==0&&scene->mode==ZELL_MODE_QUESTION&&!scene->request_ready);
    assert(!strcmp(scene->question,"Review these words"));dictation_stop(&dict);
    char weather[]="W31203320343720460a4578616d706c650a5261696e";
    service_frame(scene,weather,SERVICE_ORIGIN_MONITOR,0,0);
    assert(scene->weather_state==1&&scene->weather_icon==3);
    assert(!strcmp(scene->weather_temperature,"47 F")&&!strcmp(scene->weather_place,"Example"));
    char removed[]="W302037200a0a";service_frame(scene,removed,SERVICE_ORIGIN_MONITOR,0,0);
    assert(scene->weather_state==0&&!scene->weather_place[0]);
    unsigned char bytes[TERMINAL_KEY_BYTES];
    kittykb_event event={.action=KITTYKB_ACTION_PRESS,.key='e',.text_length=3,.text={0xe9,0x1f642,'x'}};
    const unsigned char expected[]={0xc3,0xa9,0xf0,0x9f,0x99,0x82,'x'};
    assert(terminal_key_bytes(&event,bytes)==sizeof expected&&!memcmp(bytes,expected,sizeof expected));
    event=(kittykb_event){.action=KITTYKB_ACTION_PRESS,.key=32,.modifiers=KITTYKB_MOD_CTRL};
    assert(terminal_key_bytes(&event,bytes)==1&&bytes[0]==0);
    event.key='a';event.modifiers=KITTYKB_MOD_ALT|KITTYKB_MOD_CTRL;
    assert(terminal_key_bytes(&event,bytes)==2&&bytes[0]==27&&bytes[1]==1);
    event.key=KITTYKB_KEY_ESCAPE;event.modifiers=0;
    assert(terminal_key_bytes(&event,bytes)==1&&bytes[0]==27);
    /* Drain a burst larger than the wire buffer without dropping queued keys. */
    kittykb_input_init(&keyboard.input);
    const char burst[]="\033[120;1;120:120:120:120:120:120:120:120u";
    for(int i=0;i<100;i++)kittykb_input_feed(&keyboard.input,burst,sizeof burst-1);
    size_t total=keyboard.input.queue_count*8,delivered=0;
    assert(total>0);
    scene->mode=ZELL_MODE_SERVICE;scene->service_phase=5;scene->service_send=false;
    while(keyboard.input.queue_count){
        zell_controls controls;read_controls(scene,&controls);
        assert(scene->service_send&&scene->service_reply[0]=='K');
        delivered+=(strlen(scene->service_reply)-2)/2;
        scene->service_send=false;
    }
    assert(delivered==total);
    /* A full pending wire frame leaves events queued for the next iteration. */
    kittykb_input_feed(&keyboard.input,"z",1);
    scene->service_reply[0]='K';memset(scene->service_reply+1,'1',1000);
    scene->service_reply[1001]='\n';scene->service_reply[1002]=0;scene->service_send=true;
    size_t queued=keyboard.input.queue_count;zell_controls controls;read_controls(scene,&controls);
    assert(keyboard.input.queue_count==queued);
    scene->service_send=false;read_controls(scene,&controls);
    assert(!strcmp(scene->service_reply,"K7a\n"));scene->service_send=false;

    desktop_service service={.in_fd=-1,.out_fd=-1,.frame_fd=-1};
    service.frame_fd=(int)syscall(SYS_memfd_create,"zell-transport-test",0);assert(service.frame_fd>=0);
    assert(ftruncate(service.frame_fd,ZELL_TERMINAL_BYTES)==0);
    service.frame_map=mmap(NULL,ZELL_TERMINAL_BYTES,PROT_READ|PROT_WRITE,MAP_SHARED,service.frame_fd,0);
    assert(service.frame_map!=MAP_FAILED);
    service.frame_scratch=malloc(1200u*640u*4u);scene->terminal_pixels=calloc(1200u*640u,4);
    assert(service.frame_scratch&&scene->terminal_pixels);
    uint32_t *header=service.frame_map;uint32_t *pixels=(uint32_t *)((char *)service.frame_map+32);
    header[2]=2;header[3]=1;header[4]=2;pixels[0]=0xff123456;pixels[1]=0xffabcdef;
    terminal_frame(&service,scene);
    assert(scene->terminal_width==2&&scene->terminal_height==1&&service.frame_seq==2);
    assert(scene->terminal_pixels[0]==pixels[0]&&scene->terminal_pixels[1]==pixels[1]);
    header[4]=3;pixels[0]=0;
    terminal_frame(&service,scene);assert(scene->terminal_pixels[0]==0xff123456);
    header[4]=4;header[2]=1201;
    terminal_frame(&service,scene);assert(scene->terminal_pixels[0]==0xff123456&&service.frame_seq==2);
    service_stop(&service);free(scene->terminal_pixels);scene->terminal_pixels=NULL;

    int ready[2];assert(pipe(ready)==0);
    pid_t child=fork();assert(child>=0);
    if(!child){
        close(ready[0]);assert(setsid()>=0);graceful_fd=ready[1];signal(SIGTERM,graceful_exit);
        assert(write(ready[1],"R",1)==1);for(;;)pause();
    }
    close(ready[1]);char mark=0;assert(read(ready[0],&mark,1)==1&&mark=='R');
    service=(desktop_service){.pid=child,.in_fd=-1,.out_fd=-1,.frame_fd=-1};
    service_stop(&service);
    assert(read(ready[0],&mark,1)==1&&mark=='G');close(ready[0]);
    assert(waitpid(child,NULL,WNOHANG)==-1&&errno==ECHILD);

    /* Navigating through a hatch while minimized must not lose the private
     * terminal's cleanup policy when the original station ID changes. */
    int output[2];assert(pipe(output)==0);
    child=fork();assert(child>=0);
    if(!child){close(output[0]);close(output[1]);_exit(0);}
    close(output[1]);fcntl(output[0],F_SETFL,O_NONBLOCK);
    service=(desktop_service){.pid=child,.in_fd=-1,.out_fd=output[0],.frame_fd=-1,.sensitive=true};
    scene->station=13;scene->terminal_private=true;scene->terminal_minimized=true;
    scene->terminal_pixels=calloc(1200u*640u,4);assert(scene->terminal_pixels);
    strcpy(scene->service_reply,"K6162\n");strcpy(scene->service_input,"synthetic");
    for(int attempt=0;attempt<100&&service.pid>0;attempt++){service_pump(&service,scene);usleep(10000);}
    assert(service.pid==0&&!scene->terminal_pixels&&!scene->terminal_minimized);
    assert(!scene->service_reply[0]&&!scene->service_input[0]);
}

typedef struct {pid_t pid;int rd,wr,ctl;} frame_peer;

static frame_peer frame_peer_open(void){
    int data[2],ctl[2];
    assert(pipe(data)==0&&pipe(ctl)==0);
    pid_t pid=fork();
    assert(pid>=0);
    if(!pid){
        assert(setsid()>=0);
        close(data[0]);close(data[1]);close(ctl[1]);
        char b=0;
        while(read(ctl[0],&b,1)<0&&errno==EINTR){}
        _exit(0);
    }
    close(ctl[0]);
    assert(fcntl(data[0],F_SETFL,O_NONBLOCK)>=0);
    return (frame_peer){.pid=pid,.rd=data[0],.wr=data[1],.ctl=ctl[1]};
}
static desktop_service peer_service(const frame_peer *peer,bool monitor){
    return (desktop_service){.pid=peer->pid,.in_fd=-1,.out_fd=peer->rd,.frame_fd=-1,.monitor=monitor};
}
static void peer_write(const frame_peer *peer,const char *bytes,size_t n){
    size_t off=0;
    while(off<n){
        ssize_t wrote=write(peer->wr,bytes+off,n-off);
        if(wrote<0&&errno==EINTR)continue;
        assert(wrote>0);
        off+=(size_t)wrote;
    }
}
static void peer_pump(const frame_peer *peer,desktop_service *svc,zell_scene *scene,const char *frame){
    peer_write(peer,frame,strlen(frame));
    service_pump(svc,scene);
    assert(svc->pid==peer->pid);
}
static void frame_line(char *dst,size_t cap,char kind,const char *text){
    static const char hex[]="0123456789abcdef";
    size_t len=strlen(text);
    assert(cap>=2+len*2+1);
    size_t n=0;
    dst[n++]=kind;
    for(size_t i=0;i<len;i++){
        unsigned char c=(unsigned char)text[i];
        dst[n++]=hex[c>>4];
        dst[n++]=hex[c&15];
    }
    dst[n++]='\n';
    dst[n]=0;
}
static void expect_mana(const zell_scene *s,int a,int b,int c){
    assert(s->provider_mana_ready);
    assert(s->provider_mana[0]==a&&s->provider_mana[1]==b&&s->provider_mana[2]==c);
}
/* After the numeric job menu, monitor frames must leave this dialogue alone. */
static void expect_numeric_menu(const zell_scene *s){
    assert(strcmp(s->service_title,"1 2 3")==0);
    assert(s->service_count==0&&s->service_index==0&&s->service_scroll==0);
    assert(!s->service_text[0]);
    assert(s->service_phase==2&&strcmp(s->service_input,"kept")==0);
    assert(strcmp(s->terminal_title,"Kept")==0&&s->mode==ZELL_MODE_SERVICE);
}
static void peer_reap(frame_peer *peer,desktop_service *svc){
    char go=1;ssize_t wrote;
    do wrote=write(peer->ctl,&go,1); while(wrote<0&&errno==EINTR);
    assert(wrote==1);
    close(peer->ctl);peer->ctl=-1;
    int status=0;pid_t ended;
    do ended=waitpid(peer->pid,&status,0); while(ended<0&&errno==EINTR);
    assert(ended==peer->pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    close(peer->wr);peer->wr=-1;
    close(peer->rd);peer->rd=-1;
    svc->pid=0;svc->out_fd=-1;
}
/* Drive service_pump from pipes whose children this test owns. The second menu
 * fails on the old M early-return: it would still start with "old one" and
 * have five choices. */
static void test_menu_and_mana(zell_scene *scene){
    frame_peer job=frame_peer_open();
    frame_peer mon=frame_peer_open();
    desktop_service job_svc=peer_service(&job,false);
    desktop_service mon_svc=peer_service(&mon,true);
    char line[256];
    scene->service_send=scene->service_cancel=scene->coding_yolo_refresh=false;
    scene->terminal_minimized=false;
    scene->mode=ZELL_MODE_SERVICE;
    scene->service_phase=3;
    scene->service_scroll=5;
    strcpy(scene->service_text,"stale");
    strcpy(scene->service_title,"");
    strcpy(scene->service_input,"typed");
    strcpy(scene->terminal_title,"Station");
    scene->service_count=0;scene->service_index=0;
    scene->provider_mana_ready=true;
    scene->provider_mana[0]=scene->provider_mana[1]=scene->provider_mana[2]=1;
    const char first[]=
        "M4669727374206d656e75202f203220726573756c7473\n"
        "L6f6c64206f6e65\n"
        "L6f6c642074776f\n"
        "L46696c7465722028616c6c29\n"
        "Q\n";
    peer_pump(&job,&job_svc,scene,first);
    assert(strcmp(scene->service_title,"First menu / 2 results")==0);
    assert(scene->service_count==3&&scene->service_index==0&&scene->service_scroll==0);
    assert(!scene->service_text[0]);
    assert(strcmp(scene->service_choices[0],"old one")==0);
    assert(strcmp(scene->service_choices[1],"old two")==0);
    assert(strcmp(scene->service_choices[2],"Filter (all)")==0);
    expect_mana(scene,1,1,1);
    scene->service_index=2;
    frame_line(line,sizeof line,'G',"mana 10 20 30");
    peer_pump(&mon,&mon_svc,scene,line);
    expect_mana(scene,10,20,30);
    assert(scene->service_count==3&&scene->service_index==2);
    assert(strcmp(scene->service_choices[0],"old one")==0);
    assert(strcmp(scene->service_title,"First menu / 2 results")==0);
    const char second[]=
        "M5365636f6e64206d656e75202f203120726573756c7473\n"
        "L6e657720616374696f6e\n"
        "L46696c7465722028616c6c29\n"
        "Q\n";
    peer_pump(&job,&job_svc,scene,second);
    assert(strcmp(scene->service_title,"Second menu / 1 results")==0);
    assert(scene->service_count==2&&scene->service_index==0);
    assert(strcmp(scene->service_choices[0],"new action")==0);
    assert(strcmp(scene->service_choices[1],"Filter (all)")==0);
    expect_mana(scene,10,20,30);
    strcpy(scene->service_text,"gone");scene->service_scroll=4;scene->service_index=1;
    frame_line(line,sizeof line,'M',"1 2 3");
    peer_pump(&job,&job_svc,scene,line);
    assert(strcmp(scene->service_title,"1 2 3")==0);
    assert(scene->service_count==0&&scene->service_index==0&&scene->service_scroll==0);
    assert(!scene->service_text[0]);
    expect_mana(scene,10,20,30);
    scene->service_phase=2;
    strcpy(scene->service_input,"kept");
    strcpy(scene->terminal_title,"Kept");
    scene->mode=ZELL_MODE_SERVICE;
    frame_line(line,sizeof line,'M',"4 5 6");
    peer_pump(&mon,&mon_svc,scene,line);
    expect_mana(scene,4,5,6);
    expect_numeric_menu(scene);
    frame_line(line,sizeof line,'M',"First menu / 2 results");
    peer_pump(&mon,&mon_svc,scene,line);
    frame_line(line,sizeof line,'M',"1 2 3 extra");
    peer_pump(&mon,&mon_svc,scene,line);
    expect_mana(scene,4,5,6);
    expect_numeric_menu(scene);
    frame_line(line,sizeof line,'H',"1 2 3");
    peer_pump(&mon,&mon_svc,scene,line);
    frame_line(line,sizeof line,'F',"First menu / 2 results");
    peer_pump(&mon,&mon_svc,scene,line);
    frame_line(line,sizeof line,'U',"9 9 9");
    peer_pump(&mon,&mon_svc,scene,line);
    expect_mana(scene,4,5,6);
    expect_numeric_menu(scene);
    assert(mon_svc.length==0&&job_svc.length==0);
    peer_pump(&mon,&mon_svc,scene,"Gzz\n");
    peer_pump(&job,&job_svc,scene,"Mzz\n");
    frame_line(line,sizeof line,'G',"mana 10 20 no");
    peer_pump(&mon,&mon_svc,scene,line);
    frame_line(line,sizeof line,'G',"mana 8 8 8");
    peer_pump(&job,&job_svc,scene,line);
    scene->hp=42;scene->restart_needed=true;
    frame_line(line,sizeof line,'V',"9 9 9 9 9");
    peer_pump(&job,&job_svc,scene,line);
    frame_line(line,sizeof line,'J',"0");
    peer_pump(&job,&job_svc,scene,line);
    assert(scene->hp==42&&scene->restart_needed);
    expect_mana(scene,4,5,6);
    expect_numeric_menu(scene);
    {
        const char *prefix="G6d616e61203720382039"; /* "mana 7 8 9", which must not apply */
        size_t prefix_len=strlen(prefix),total=9000;
        char *big=malloc(total+2);
        assert(big);
        memcpy(big,prefix,prefix_len);
        memset(big+prefix_len,'0',total-prefix_len);
        big[total]='\n';big[total+1]=0;
        peer_write(&mon,big,total+1);
        free(big);
        service_pump(&mon_svc,scene);
        assert(mon_svc.pid==mon.pid);
    }
    expect_mana(scene,4,5,6);
    expect_numeric_menu(scene);
    frame_line(line,sizeof line,'G',"mana -2 -3 101");
    peer_pump(&mon,&mon_svc,scene,line);
    expect_mana(scene,-2,-1,-1);
    expect_numeric_menu(scene);
    frame_line(line,sizeof line,'G',"mana 3 3 3");
    line[strlen(line)-1]=0; /* no newline: the frame is still partial */
    peer_pump(&mon,&mon_svc,scene,line);
    assert(mon_svc.length>0);
    expect_mana(scene,-2,-1,-1);
    expect_numeric_menu(scene);
    peer_reap(&job,&job_svc);
    peer_reap(&mon,&mon_svc);
}
static void expect_provider_snapshot(const zell_scene *s){
    assert(s->provider_generation==11&&s->provider_count==3);
    assert(!strcmp(s->provider_rows[0].id,"codex")&&s->provider_rows[0].percent==10);
    assert(!strcmp(s->provider_rows[0].state,"verified")&&!strcmp(s->provider_rows[0].source,"api"));
    assert(!strcmp(s->provider_rows[0].plan,"unknown"));
    assert(!strcmp(s->provider_rows[1].id,"claude")&&s->provider_rows[1].percent==20);
    assert(!strcmp(s->provider_rows[2].id,"grok")&&s->provider_rows[2].percent==30);
    expect_mana(s,10,20,30);
}
static void expect_fleet_page(const zell_scene *s){
    assert(s->fleet_revision==4&&s->fleet_total==1&&s->fleet_offset==0&&s->fleet_count==1);
    assert(!strcmp(s->fleet_rows[0].fleet_id,"alpha"));
    assert(!strcmp(s->fleet_rows[0].display_name,"North desk"));
    assert(!strcmp(s->fleet_rows[0].project_label,"Demo"));
    assert(s->fleet_rows[0].worker_count==2&&s->fleet_rows[0].live_count==1);
}
static void send_text(const frame_peer *peer,desktop_service *svc,zell_scene *scene,char kind,const char *text){
    char line[4096];
    frame_line(line,sizeof line,kind,text);
    peer_pump(peer,svc,scene,line);
}
static void expect_first_menu(const zell_scene *s){
    assert(strcmp(s->service_title,"First menu / 2 results")==0);
    assert(s->service_count==3&&s->service_index==0&&s->service_scroll==0);
    assert(!s->service_text[0]);
    assert(strcmp(s->service_choices[0],"old one")==0);
    assert(strcmp(s->service_choices[1],"old two")==0);
    assert(strcmp(s->service_choices[2],"Filter (all)")==0);
}
/* Slot 0's generation is part of the frame identity. A late frame and another
 * job's frame both go through service_pump and must leave the menu in place. */
static void test_window_ownership(zell_scene *scene){
    frame_peer owner=frame_peer_open();
    frame_peer other=frame_peer_open();
    desktop_service owner_svc=peer_service(&owner,false);
    desktop_service other_svc=peer_service(&other,false);
    memset(scene->windows,0,sizeof scene->windows);
    int generation=0;
    assert(window_prepare_slot0(scene,&generation));
    assert(generation>0&&scene->windows[0].used&&scene->windows[0].generation==generation);
    assert(!scene->windows[0].close_requested&&scene->windows[0].owner_service==0);
    owner_svc.window_slot=0;
    owner_svc.window_generation=generation;
    other_svc.window_slot=1;
    other_svc.window_generation=generation+30;
    scene->windows[1].used=true;
    scene->windows[1].generation=generation+3;
    scene->mode=ZELL_MODE_SERVICE;
    scene->service_phase=0;
    scene->service_text[0]=scene->service_title[0]=scene->service_input[0]=0;
    scene->service_count=scene->service_index=scene->service_scroll=0;
    const char first[]=
        "M4669727374206d656e75202f203220726573756c7473\n"
        "L6f6c64206f6e65\n"
        "L6f6c642074776f\n"
        "L46696c7465722028616c6c29\n"
        "Q\n";
    peer_pump(&owner,&owner_svc,scene,first);
    expect_first_menu(scene);
    assert(scene->service_phase==2);
    const char foreign[]=
        "T\n"
        "P4e616d653f\n"
        "O6869\n"
        "C\n"
        "L6e6577\n"
        "R\n"
        "Q\n"
        "M5365636f6e64206d656e75202f203120726573756c7473\n"
        "L6e657720616374696f6e\n"
        "L46696c7465722028616c6c29\n"
        "Q\n";
    peer_pump(&other,&other_svc,scene,foreign);
    expect_first_menu(scene);
    assert(scene->service_phase==2);
    int previous=generation;
    assert(window_prepare_slot0(scene,&generation));
    assert(generation!=previous&&scene->windows[0].generation==generation&&scene->windows[0].used);
    peer_pump(&owner,&owner_svc,scene,foreign);
    expect_first_menu(scene);
    assert(scene->service_phase==2);
    owner_svc.window_generation=generation;
    const char second[]=
        "M5365636f6e64206d656e75202f203120726573756c7473\n"
        "L6e657720616374696f6e\n"
        "L46696c7465722028616c6c29\n"
        "Q\n";
    peer_pump(&owner,&owner_svc,scene,second);
    assert(strcmp(scene->service_title,"Second menu / 1 results")==0);
    assert(scene->service_count==2&&scene->service_index==0);
    assert(strcmp(scene->service_choices[0],"new action")==0);
    assert(strcmp(scene->service_choices[1],"Filter (all)")==0);
    peer_reap(&owner,&owner_svc);
    peer_reap(&other,&other_svc);

    int ready[2];assert(pipe(ready)==0);
    pid_t child=fork();assert(child>=0);
    if(!child){
        assert(setsid()>=0);
        close(ready[0]);
        assert(write(ready[1],"R",1)==1);
        for(;;)pause();
    }
    close(ready[1]);
    char mark=0;assert(read(ready[0],&mark,1)==1&&mark=='R');
    close(ready[0]);
    desktop_service live_svc={.pid=child,.in_fd=-1,.out_fd=-1,.frame_fd=-1,.window_slot=0,.window_generation=40};
    memset(scene->windows,0,sizeof scene->windows);
    for(int i=0;i<ZELL_WINDOWS;i++){
        scene->windows[i].used=true;
        scene->windows[i].generation=40+i;
    }
    strcpy(scene->service_title,"Kept menu");
    scene->service_count=2;
    strcpy(scene->service_choices[0],"stay");
    scene->desktop_launch=true;
    service_start(&live_svc,scene);
    assert(live_svc.pid==child&&!scene->desktop_launch);
    assert(scene->windows[0].generation==40&&scene->windows[0].used);
    assert(strcmp(scene->service_title,"Kept menu")==0);
    assert(scene->service_count==2&&strcmp(scene->service_choices[0],"stay")==0);
    scene->windows[0].cancel_blocked=true;
    scene->windows[0].close_requested=true;
    service_pump(&live_svc,scene);
    assert(live_svc.pid==child&&scene->windows[0].used&&scene->windows[0].close_requested);
    scene->windows[0].cancel_blocked=false;
    service_pump(&live_svc,scene);
    assert(live_svc.pid==0);
    assert(scene->windows[0].close_requested&&!scene->windows[0].used&&scene->windows[0].generation==0);
    assert(waitpid(child,NULL,WNOHANG)==-1&&errno==ECHILD);

    scene->desktop=true;scene->mode=ZELL_MODE_SERVICE;scene->service_phase=5;
    scene->terminal_minimized=false;scene->terminal_width=160;scene->terminal_height=64;
    scene->mouse_valid=true;
    scene->windows[0].used=true;scene->windows[0].generation=8;scene->windows[0].close_requested=false;
    zell_terminal_layout layout;zell_terminal_layout_for(scene,scene->width,scene->height,&layout);
    scene->mouse_x=layout.close.x+3;scene->mouse_y=layout.close.y+3;scene->mouse_click=true;
    assert(zell_terminal_click(scene)&&scene->windows[0].close_requested);
    assert(scene->mode==ZELL_MODE_SERVICE&&!scene->terminal_minimized);
    kittykb_event escape={.action=KITTYKB_ACTION_PRESS,.key=KITTYKB_KEY_ESCAPE};
    unsigned char key[8];
    assert(terminal_key_bytes(&escape,key)==1&&key[0]==27);
}
static void test_notice_timer(void){
    zell_scene scene;
    frame_peer mon=frame_peer_open();
    frame_peer job=frame_peer_open();
    desktop_service mon_svc=peer_service(&mon,true);
    desktop_service job_svc=peer_service(&job,false);
    char key[64],payload[128],longer[80],changed[80];
    zell_scene_init(&scene,640,400);
    zell_notice_set_clock(5000);
    send_text(&mon,&mon_svc,&scene,'N',"1 Job finished");
    assert(scene.notice_count==1&&!strcmp(scene.notice_text,"Job finished"));
    assert(scene.notice_until_ms==13000&&scene.notice_key[0]);
    memcpy(key,scene.notice_key,sizeof key);
    /* The monitor repeats the same words with a new count. That must not rearm. */
    zell_notice_set_clock(10000);
    send_text(&mon,&mon_svc,&scene,'N',"2 Job finished");
    assert(scene.notice_count==2&&scene.notice_until_ms==13000);
    assert(!memcmp(scene.notice_key,key,sizeof key));
    zell_notice_set_clock(11000);
    send_text(&mon,&mon_svc,&scene,'N',"1 A different notice");
    assert(!strcmp(scene.notice_text,"A different notice"));
    assert(scene.notice_until_ms==19000&&memcmp(scene.notice_key,key,sizeof key)!=0);
    memcpy(key,scene.notice_key,sizeof key);
    zell_notice_set_clock(12000);
    send_text(&job,&job_svc,&scene,'N',"1 Job finished");
    send_text(&mon,&mon_svc,&scene,'N',"201 ignored");
    assert(scene.notice_until_ms==19000&&!strcmp(scene.notice_text,"A different notice"));
    assert(!memcmp(scene.notice_key,key,sizeof key));
    /* Past the deadline, the same sample still must not move it. */
    zell_notice_set_clock(19000);
    send_text(&mon,&mon_svc,&scene,'N',"1 A different notice");
    assert(scene.notice_until_ms==19000);
    zell_notice_set_clock(20000);
    send_text(&mon,&mon_svc,&scene,'N',"0 ");
    assert(scene.notice_count==0&&!scene.notice_text[0]);
    assert(scene.notice_until_ms==0&&!scene.notice_key[0]);
    zell_notice_set_clock(21000);
    send_text(&mon,&mon_svc,&scene,'N',"1 Job finished");
    assert(!strcmp(scene.notice_text,"Job finished")&&scene.notice_until_ms==29000);
    assert(scene.notice_key[0]);
    memset(longer,'Q',70);longer[70]=0;
    memcpy(changed,longer,71);changed[63]='Z';
    zell_notice_set_clock(30000);
    snprintf(payload,sizeof payload,"1 %s",longer);
    send_text(&mon,&mon_svc,&scene,'N',payload);
    assert(strlen(scene.notice_text)==70&&scene.notice_until_ms==38000);
    memcpy(key,scene.notice_key,sizeof key);
    zell_notice_set_clock(31000);
    snprintf(payload,sizeof payload,"1 %s",changed);
    send_text(&mon,&mon_svc,&scene,'N',payload);
    assert(scene.notice_text[63]=='Z'&&scene.notice_until_ms==39000);
    assert(memcmp(scene.notice_key,key,sizeof key)!=0);
    peer_reap(&job,&job_svc);
    peer_reap(&mon,&mon_svc);
}
static void test_contract_frames(zell_scene *scene){
    frame_peer job=frame_peer_open();
    frame_peer mon=frame_peer_open();
    desktop_service job_svc=peer_service(&job,false);
    desktop_service mon_svc=peer_service(&mon,true);
    const char *versioned=
        "1 11 3\n"
        "codex 10 verified api unknown\n"
        "claude 20 configured cli unknown\n"
        "grok 30 unknown vault unknown";
    char ninth[1024];
    size_t used=0;
    expect_numeric_menu(scene);
    assert(scene->provider_count==0&&scene->provider_generation==0);
    assert(scene->provider_check_generation==0&&scene->provider_check_running==0);
    assert(scene->fleet_count==0&&!scene->update_job_active);
    send_text(&mon,&mon_svc,scene,'G',"mana 10 20 30");
    expect_mana(scene,10,20,30);
    assert(scene->provider_count==0&&scene->provider_generation==0);
    /* Three bare integers are never mana. They are a provider snapshot's
     * header, and a snapshot with no rows is nothing but that header. */
    send_text(&mon,&mon_svc,scene,'G',"19 62 5");
    expect_mana(scene,10,20,30);
    assert(scene->provider_count==0&&scene->provider_generation==0);
    send_text(&mon,&mon_svc,scene,'G',"1 4 0");
    assert(scene->provider_count==0&&scene->provider_generation==4);
    expect_mana(scene,-1,-1,-1);
    /* The mana line follows the snapshot and has the last word. */
    send_text(&mon,&mon_svc,scene,'G',"mana 19 62 5");
    expect_mana(scene,19,62,5);
    assert(scene->provider_count==0&&scene->provider_generation==4);
    send_text(&mon,&mon_svc,scene,'G',"mana7 8 9");
    send_text(&mon,&mon_svc,scene,'G',"mana 1 2");
    send_text(&mon,&mon_svc,scene,'G',"mana 1 2 3 4");
    expect_mana(scene,19,62,5);
    send_text(&mon,&mon_svc,scene,'G',"mana 10 20 30");
    scene->provider_generation=0;
    expect_numeric_menu(scene);
    send_text(&mon,&mon_svc,scene,'G',versioned);
    expect_provider_snapshot(scene);
    send_text(&mon,&mon_svc,scene,'G',"1 12 2\ncodex 1 verified api unknown\ncodex 2 verified api unknown");
    expect_provider_snapshot(scene);
    send_text(&mon,&mon_svc,scene,'G',"1 13 1\ncodex 101 verified api unknown");
    expect_provider_snapshot(scene);
    send_text(&job,&job_svc,scene,'G',versioned);
    expect_provider_snapshot(scene);
    expect_numeric_menu(scene);
    send_text(&mon,&mon_svc,scene,'H',"1 7 1");
    assert(scene->provider_check_generation==7&&scene->provider_check_running==1);
    send_text(&mon,&mon_svc,scene,'H',"1 7 2");
    assert(scene->provider_check_generation==7&&scene->provider_check_running==1);
    send_text(&job,&job_svc,scene,'H',"1 8 0");
    assert(scene->provider_check_generation==7&&scene->provider_check_running==1);
    send_text(&mon,&mon_svc,scene,'H',"1 6 0");
    assert(scene->provider_check_generation==7&&scene->provider_check_running==1);
    send_text(&mon,&mon_svc,scene,'H',"1 8 1");
    assert(scene->provider_check_generation==8&&scene->provider_check_running==1);
    send_text(&mon,&mon_svc,scene,'H',"1 7 0");
    assert(scene->provider_check_generation==8&&scene->provider_check_running==1);
    send_text(&mon,&mon_svc,scene,'H',"1 8 0");
    assert(scene->provider_check_generation==8&&scene->provider_check_running==0);
    send_text(&mon,&mon_svc,scene,'F',"1 4 1 0 1\nalpha\tNorth desk\tDemo\t2\t1");
    expect_fleet_page(scene);
    send_text(&mon,&mon_svc,scene,'F',"1 97 1 0 1\nalpha\tBad\nname\tDemo\t2\t1");
    expect_fleet_page(scene);
    used=(size_t)snprintf(ninth,sizeof ninth,"1 99 8 0 8\n");
    for(int row=0;row<9;row++){
        int wrote=snprintf(ninth+used,sizeof ninth-used,"id%d\tName %d\tProject\t1\t0\n",row,row);
        assert(wrote>0&&used+(size_t)wrote<sizeof ninth);
        used+=(size_t)wrote;
    }
    send_text(&mon,&mon_svc,scene,'F',ninth);
    expect_fleet_page(scene);
    send_text(&mon,&mon_svc,scene,'F',"1 98 2 0 2\nalpha\tOnly\tDemo\t1\t0");
    expect_fleet_page(scene);
    send_text(&job,&job_svc,scene,'F',"1 4 1 0 1\nalpha\tNorth desk\tDemo\t2\t1");
    expect_fleet_page(scene);
    send_text(&mon,&mon_svc,scene,'U',"1 4 1 3 running");
    assert(scene->update_job_active&&scene->update_job_generation==4);
    assert(scene->update_done==1&&scene->update_total==3&&!strcmp(scene->update_state,"running"));
    send_text(&mon,&mon_svc,scene,'U',"1 4 4 3 running");
    assert(scene->update_done==1&&scene->update_total==3&&!strcmp(scene->update_state,"running"));
    send_text(&mon,&mon_svc,scene,'U',"1 9 0 1 running");
    assert(scene->update_job_generation==4&&scene->update_done==1&&scene->update_total==3);
    send_text(&mon,&mon_svc,scene,'U',"1 4 2 3 done");
    assert(scene->update_job_generation==4&&scene->update_done==2&&scene->update_total==3);
    assert(!strcmp(scene->update_state,"done"));
    send_text(&job,&job_svc,scene,'U',"1 4 0 3 idle");
    assert(scene->update_done==2&&!strcmp(scene->update_state,"done"));
    expect_provider_snapshot(scene);
    {
        const char *seven=
            "1 70 7\n"
            "codex 5 verified api unknown\n"
            "claude 6 verified cli unknown\n"
            "grok 7 verified vault unknown\n"
            "qwen 8 configured api unknown\n"
            "minimax 9 unknown api unknown\n"
            "gemini -1 stale api unknown\n"
            "elevenlabs 0 verified api unknown";
        char many[2048];
        size_t filled=0;
        int wrote;
        send_text(&mon,&mon_svc,scene,'G',seven);
        assert(scene->provider_count==7&&scene->provider_generation==70);
        assert(!strcmp(scene->provider_rows[3].id,"qwen"));
        assert(!strcmp(scene->provider_rows[4].id,"minimax"));
        assert(!strcmp(scene->provider_rows[5].id,"gemini")&&scene->provider_rows[5].percent==-1);
        assert(!strcmp(scene->provider_rows[6].id,"elevenlabs")&&scene->provider_rows[6].percent==0);
        expect_mana(scene,5,6,7);
        /* More rows than the header allows: refused whole. */
        send_text(&mon,&mon_svc,scene,'G',"1 71 33\ncodex 1 verified api unknown");
        assert(scene->provider_count==7&&scene->provider_generation==70);
        expect_mana(scene,5,6,7);
        wrote=snprintf(many,sizeof many,"1 72 32\n");
        assert(wrote>0);
        filled=(size_t)wrote;
        for(int row=0;row<32;row++){
            wrote=snprintf(many+filled,sizeof many-filled,"codex 1 verified api unknown\n");
            assert(wrote>0&&filled+(size_t)wrote<sizeof many);
            filled+=(size_t)wrote;
        }
        send_text(&mon,&mon_svc,scene,'G',many);
        assert(scene->provider_count==7&&scene->provider_generation==70);
        expect_mana(scene,5,6,7);
        send_text(&mon,&mon_svc,scene,'G',versioned);
    }
    expect_provider_snapshot(scene);
    expect_fleet_page(scene);
    expect_numeric_menu(scene);
    peer_reap(&job,&job_svc);
    peer_reap(&mon,&mon_svc);
}

/* The outer terminal's bytes as desktop_input_read gives them to the input
 * scanner, with Zell's own sinks. */
static void desktop_input_feed(zell_scene *s,const char *bytes,size_t count){
    zell_input_sinks sinks={desktop_keys,desktop_pointer,desktop_paste,s};
    zell_input_feed(&pointer_input,&sinks,bytes,count,now_ms());
}
/* A paste of more than one line waits, said, for Enter; any other key, a
 * click or eight seconds drop it; Shift alone does not answer. */
/* The Command Center's screens show Kilix panes in the floating terminal:
 * their service is given its shared surface, as the Workshop computer's is;
 * a station that never opens it is not. */
static void test_service_surfaces(void){
    /* The DJ's browser (station 4, action 9) too: any station's helper. */
    int stations[]={ZELL_COMMAND_SCREEN_FIRST,ZELL_COMMAND_SCREEN_LAST,ZELL_WORKSHOP_COMPUTER,ZELL_COMMAND_COMPUTER,4,10};
    bool surface[]={true,true,true,true,true,true};
    for(int i=0;i<6;i++){
        static zell_scene scene;
        memset(&scene,0,sizeof scene);
        scene.desktop=true;scene.room=1;scene.station=stations[i];scene.menu_index=stations[i]==4?9:0;
        desktop_service svc={.in_fd=-1,.out_fd=-1,.frame_fd=-1};
        service_start(&svc,&scene);
        assert(svc.pid>0);
        assert((svc.frame_map!=NULL)==surface[i]&&(svc.frame_fd>=0)==surface[i]);
        assert((scene.terminal_pixels!=NULL)==surface[i]);
        service_stop(&svc);
        free(scene.terminal_pixels);scene.terminal_pixels=NULL;
    }
    /* The vitals monitor has none. */
    static zell_scene scene;
    memset(&scene,0,sizeof scene);
    scene.desktop=true;scene.room=1;
    desktop_service monitor={.in_fd=-1,.out_fd=-1,.frame_fd=-1};
    service_start_mode(&monitor,&scene,true);
    assert(monitor.pid>0&&!monitor.frame_map&&monitor.frame_fd<0&&!scene.terminal_pixels);
    service_stop(&monitor);
}
/* Kilix hands a pane room its window keys while Zell says so with the
 * user variable kilix_app_keys: set on entering one, unset on leaving it
 * and when Zell stops, each once. */
static void test_app_keys(void){
    static zell_scene scene;
    memset(&scene,0,sizeof scene);
    scene.desktop=true;scene.room=1;
    assert(!app_keys_change(&scene,false));
    scene.room=ZELL_ROOM_PANES_LEFT;
    assert(!strcmp(app_keys_change(&scene,false),"\033]1337;SetUserVar=kilix_app_keys=MQ==\007"));
    assert(!app_keys_change(&scene,false));
    scene.room=ZELL_ROOM_PANES_RIGHT;assert(!app_keys_change(&scene,false));
    scene.room=1;
    assert(!strcmp(app_keys_change(&scene,false),"\033]1337;SetUserVar=kilix_app_keys\007"));
    assert(!app_keys_change(&scene,true));
    scene.room=ZELL_ROOM_PANES_LEFT;
    assert(app_keys_change(&scene,false)&&!strcmp(app_keys_change(&scene,true),"\033]1337;SetUserVar=kilix_app_keys\007"));
    assert(!app_keys_change(&scene,true));
    /* Not the desktop: never. */
    scene.desktop=false;assert(!app_keys_change(&scene,false));
}
static void test_paste_question(void){
    zell_scene scene;
    kittykb_event event;
    char shift[32];
    int shift_n=snprintf(shift,sizeof shift,"\033[%d;2:1u",KITTYKB_KEY_LEFT_SHIFT);
    zell_scene_init(&scene,960,600);zell_desktop_init(&scene);
    scene.room=1;scene.mode=ZELL_MODE_WALK;
    assert(!ZELL_ROOM_IS_PANES(scene.room));
    kittykb_input_init(&keyboard.input);zell_input_free(&pointer_input);
    zell_notice_set_clock(1000);
    /* One line with no break is typed straight away. */
    desktop_input_feed(&scene,"\033[200~ok\033[201~",14);
    assert(kittykb_input_next(&keyboard.input,&event)&&event.key=='o');
    assert(kittykb_input_next(&keyboard.input,&event)&&event.key=='k'&&!kittykb_input_next(&keyboard.input,&event));
    /* Two lines wait; Shift does not answer; Enter types them. */
    desktop_input_feed(&scene,"\033[200~ls\nrm x\n\033[201~",20);
    assert(keyboard.input.queue_count==0&&ask_n==8);
    assert(!strcmp(scene.notice_text,"Paste 2 lines? Enter=yes Esc=no"));
    zell_controls controls;
    desktop_input_feed(&scene,shift,(size_t)shift_n);
    read_controls(&scene,&controls);
    assert(ask_n==8&&keyboard.input.queue_count==0);
    /* Nor does a key let go, nor the pointer moving. */
    desktop_input_feed(&scene,"\033[97;1:3u",10);
    read_controls(&scene,&controls);
    assert(ask_n==8);
    desktop_input_feed(&scene,"\033[<35;20;50M",13);
    assert(ask_n==8&&scene.mouse_valid);
    assert(!strcmp(scene.notice_text,"Paste 2 lines? Enter=yes Esc=no"));
    desktop_input_feed(&scene,"\r",1);
    read_controls(&scene,&controls);
    assert(ask_n==0);
    const uint32_t want[]={'l','s',KITTYKB_KEY_ENTER,'r','m',' ','x',KITTYKB_KEY_ENTER};
    for(size_t i=0;i<sizeof want/sizeof want[0];i++)assert(kittykb_input_next(&keyboard.input,&event)&&event.key==want[i]);
    assert(!kittykb_input_next(&keyboard.input,&event));
    /* Any other key drops it, and says so. */
    desktop_input_feed(&scene,"\033[200~a\r\nb\033[201~",16);
    assert(ask_n==4&&!strcmp(scene.notice_text,"Paste 2 lines? Enter=yes Esc=no"));
    desktop_input_feed(&scene,"q",1);
    read_controls(&scene,&controls);
    assert(ask_n==0&&!kittykb_input_next(&keyboard.input,&event)&&typed_n==0);
    assert(!strcmp(scene.notice_text,"The paste was not sent."));
    /* One line and its Enter is asked about too; a newer paste takes its
     * place. */
    desktop_input_feed(&scene,"\033[200~ls\n\033[201~",15);
    assert(ask_n==3&&!strcmp(scene.notice_text,"Paste 1 line? Enter=yes Esc=no"));
    desktop_input_feed(&scene,"\033[200~1\n2\n3\033[201~",17);
    assert(ask_n==5&&!strcmp(scene.notice_text,"Paste 3 lines? Enter=yes Esc=no"));
    /* Keys typed before a paste that the key queue had no room for yet go
     * first too: until they have, nothing answers. */
    {
        static char many[KITTYKB_EVENT_QUEUE_CAPACITY+64+15];
        size_t k=KITTYKB_EVENT_QUEUE_CAPACITY+64;
        memset(many,'z',k);memcpy(many+k,"\033[200~1\n2\033[201~",15);
        desktop_input_feed(&scene,many,k+15);
        assert(ask_n==3&&typed_n>0);
        read_controls(&scene,&controls);
        assert(keyboard.input.queue_count==0&&typed_n>0);
        assert(!paste_answer(&scene)&&ask_n==3);
        while(typed_n){typed_feed();read_controls(&scene,&controls);}
        assert(paste_answer(&scene)&&ask_n==3);
        ask_drop(&scene,false);
    }
    /* Asked again long after, the same question is said again. */
    zell_notice_set_clock(200000);
    desktop_input_feed(&scene,"\033[200~1\n2\n3\033[201~",17);
    assert(ask_n==5&&scene.notice_until_ms==200000+ZELL_PASTE_ASK_MS&&!strcmp(scene.notice_text,"Paste 3 lines? Enter=yes Esc=no"));
    zell_notice_set_clock(1000);ask_until=1000+ZELL_PASTE_ASK_MS;
    /* Unanswered for eight seconds it is dropped, and said. */
    zell_notice_set_clock(1000+ZELL_PASTE_ASK_MS-1);
    assert(paste_answer(&scene)&&ask_n==5);
    zell_notice_set_clock(1000+ZELL_PASTE_ASK_MS);
    assert(!paste_answer(&scene)&&ask_n==0&&!strcmp(scene.notice_text,"The paste was not sent."));
    /* In a pane room it is said on the pane with the keys, and sent there
     * on Enter; a click drops it. */
    zell_notice_set_clock(20000);
    zell_panes_test_reset();
    zell_panes_enter(&scene,ZELL_ROOM_PANES_RIGHT);
    scene.mouse_valid=true;scene.mouse_click=true;scene.mouse_x=10;scene.mouse_y=300;
    assert(zell_desktop_click(&scene)&&zell_panes_keys_to_pane());
    desktop_input_feed(&scene,"\033[200~a\nb\033[201~",15);
    assert(ask_n==3&&!strcmp(zell_panes_notice(ZELL_ROOM_PANES_RIGHT,0),"Paste 2 lines? Enter=yes Esc=no"));
    assert(!strcmp(zell_panes_pending(),""));
    desktop_input_feed(&scene,"\r",1);
    read_controls(&scene,&controls);
    zell_panes_test_drain();
    assert(ask_n==0&&!strcmp(zell_panes_pending(),"P 10 0 610a62\nP 10 0 .\n"));
    /* Held, the Enter that pasted repeats and is let go: neither goes to
     * the pane; the next key does. */
    desktop_input_feed(&scene,"\033[200~1\n2\033[201~",15);
    assert(ask_n==3);
    desktop_input_feed(&scene,"\r\033[13;1:2u\033[13;1:2u\033[13;1:3ux",29);
    read_controls(&scene,&controls);
    read_controls(&scene,&controls);
    zell_panes_test_drain();
    assert(!strcmp(zell_panes_pending(),"P 10 0 610a62\nP 10 0 .\nP 10 0 310a32\nP 10 0 .\nK78\n"));
    /* An Enter typed before the paste, read with it, goes to the pane and
     * does not answer: the question waits for a key after it. */
    desktop_input_feed(&scene,"\r\033[200~rm a\nrm b\n\033[201~",23);
    assert(ask_n==10);
    read_controls(&scene,&controls);
    zell_panes_test_drain();
    assert(ask_n==10&&!strcmp(zell_panes_pending(),"P 10 0 610a62\nP 10 0 .\nP 10 0 310a32\nP 10 0 .\nK78\nK0d\n"));
    assert(!strcmp(zell_panes_notice(ZELL_ROOM_PANES_RIGHT,0),"Paste 2 lines? Enter=yes Esc=no"));
    /* With the keys before it gone, the next key answers. */
    read_controls(&scene,&controls);
    desktop_input_feed(&scene,"q",1);
    read_controls(&scene,&controls);
    assert(ask_n==0&&!strcmp(zell_panes_notice(ZELL_ROOM_PANES_RIGHT,0),"The paste was not sent."));
    desktop_input_feed(&scene,"\033[200~c\nd\033[201~",15);
    assert(ask_n==3);
    desktop_input_feed(&scene,"\033[<0;20;300M",13);
    assert(ask_n==0&&!strcmp(zell_panes_notice(ZELL_ROOM_PANES_RIGHT,0),"The paste was not sent."));
    assert(!strstr(zell_panes_pending(),"630a64"));
    /* Where Kilix has the keys a paste goes nowhere: it is not asked about. */
    scene.mouse_valid=true;scene.mouse_click=true;scene.mouse_x=950;scene.mouse_y=10;
    assert(zell_desktop_click(&scene)&&!zell_panes_keys_to_pane());
    desktop_input_feed(&scene,"\033[200~g\nh\033[201~",15);
    assert(ask_n==0);
    scene.mouse_valid=true;scene.mouse_click=true;scene.mouse_x=10;scene.mouse_y=300;
    assert(zell_desktop_click(&scene)&&zell_panes_keys_to_pane());
    /* Alt and an arrow move the keys; the pane is sent nothing for them. */
    {
        size_t before=strlen(zell_panes_pending());
        desktop_input_feed(&scene,"\033[1;3C",6);
        read_controls(&scene,&controls);
        assert(zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==1&&!strcmp(zell_panes_pending()+before,"F 1\n"));
        desktop_input_feed(&scene,"\033[1;3D\033[1;3A",12);
        read_controls(&scene,&controls);
        assert(!zell_panes_keys_to_pane()&&scene.x==120.f&&!strcmp(zell_panes_pending()+before,"F 1\nF 0\n"));
        /* Held keys are known only from a terminal that reports releases,
         * as kitty does once asked: so here, until the paste tests. */
        uint8_t support=keyboard.input.support;uint32_t flags=keyboard.input.terminal_flags;
        keyboard.input.support=(uint8_t)KITTYKB_SUPPORT_AVAILABLE;
        keyboard.input.terminal_flags|=KITTYKB_FLAG_REPORT_EVENTS|KITTYKB_FLAG_REPORT_ALL_KEYS;
        /* On the walkway, Alt held, Right takes him over the next pane but
         * he does not walk, though Right is held. */
        char held[64];
        int n=snprintf(held,sizeof held,"\033[%d;3:1u\033[1;3:1C",KITTYKB_KEY_LEFT_ALT);
        desktop_input_feed(&scene,held,(size_t)n);
        read_controls(&scene,&controls);
        assert(scene.x==364.f&&!zell_panes_keys_to_pane());
        read_controls(&scene,&controls);
        assert(kittykb_input_key_down(&keyboard.input,KITTYKB_KEY_RIGHT)&&kittykb_input_key_down(&keyboard.input,KITTYKB_KEY_LEFT_ALT));
        assert(controls.move_x==0&&scene.x==364.f);
        n=snprintf(held,sizeof held,"\033[1;3:3C\033[%d;1:3u",KITTYKB_KEY_LEFT_ALT);
        desktop_input_feed(&scene,held,(size_t)n);
        read_controls(&scene,&controls);
        /* Shift held: he runs. */
        n=snprintf(held,sizeof held,"\033[%d;2:1u\033[1;2:1C",KITTYKB_KEY_LEFT_SHIFT);
        desktop_input_feed(&scene,held,(size_t)n);
        read_controls(&scene,&controls);
        read_controls(&scene,&controls);
        assert(controls.run&&controls.move_x==1);
        n=snprintf(held,sizeof held,"\033[1;2:3C\033[%d;1:3u",KITTYKB_KEY_LEFT_SHIFT);
        desktop_input_feed(&scene,held,(size_t)n);
        read_controls(&scene,&controls);
        read_controls(&scene,&controls);
        assert(!controls.run&&controls.move_x==0);
        assert(!kittykb_input_key_down(&keyboard.input,KITTYKB_KEY_RIGHT)&&!kittykb_input_key_down(&keyboard.input,KITTYKB_KEY_LEFT_SHIFT));
        keyboard.input.support=support;keyboard.input.terminal_flags=flags;
        desktop_input_feed(&scene,"\033[1;3B",6);
        read_controls(&scene,&controls);
        assert(zell_panes_keys_to_pane()&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==1);
        assert(!strstr(zell_panes_pending()+before,"K"));
        /* Ctrl with Alt and an arrow is the pane's, as a key. */
        desktop_input_feed(&scene,"\033[1;7C",6);
        read_controls(&scene,&controls);
        assert(zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==1&&strchr(zell_panes_pending()+before,'K'));
        /* Into pane 0, then Alt+Right with Alt let go first: the arrow's
         * repeat and release do not reach pane 1. */
        {
            keyboard.input.support=(uint8_t)KITTYKB_SUPPORT_AVAILABLE;
            keyboard.input.terminal_flags|=KITTYKB_FLAG_REPORT_EVENTS|KITTYKB_FLAG_REPORT_ALL_KEYS;
            zell_rect r;zell_panes_pane_rect(&scene,0,&r);
            scene.mouse_valid=true;scene.mouse_click=true;scene.mouse_x=r.x+4;scene.mouse_y=300;
            assert(zell_desktop_click(&scene)&&zell_panes_keys_to_pane()&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==0);
            size_t at=strlen(zell_panes_pending());
            n=snprintf(held,sizeof held,"\033[%d;3:1u\033[1;3:1C\033[%d;3:3u\033[1;1:2C\033[1;1:3C",KITTYKB_KEY_LEFT_ALT,KITTYKB_KEY_LEFT_ALT);
            desktop_input_feed(&scene,held,(size_t)n);
            read_controls(&scene,&controls);
            assert(zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==1&&!strcmp(zell_panes_pending()+at,"F 1\n"));
            /* And the next Right is the pane's. */
            desktop_input_feed(&scene,"\033[1;1:1C",8);
            read_controls(&scene,&controls);
            assert(strchr(zell_panes_pending()+at,'K'));
            keyboard.input.support=support;keyboard.input.terminal_flags=flags;
        }
        /* F1 opens the Hotkeys window; while it is open no key goes to the
         * pane or moves anything; Esc closes it. */
        before=strlen(zell_panes_pending());
        desktop_input_feed(&scene,"\033[11~",5);
        read_controls(&scene,&controls);
        assert(zell_panes_hotkeys_open());
        desktop_input_feed(&scene,"ab\033[1;3C\033[9;5u",14);
        read_controls(&scene,&controls);
        assert(zell_panes_hotkeys_open()&&!strcmp(zell_panes_pending()+before,"")&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==1);
        desktop_input_feed(&scene,"\033[27u",5);
        read_controls(&scene,&controls);
        assert(!zell_panes_hotkeys_open()&&!strcmp(zell_panes_pending()+before,""));
        /* Ctrl+Tab, Ctrl+Shift+Tab and Ctrl+Alt+Z are the room's, never the
         * pane's. */
        desktop_input_feed(&scene,"\033[9;5u",6);
        read_controls(&scene,&controls);
        assert(zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==2&&!strcmp(zell_panes_pending()+before,"F 2\n"));
        desktop_input_feed(&scene,"\033[9;6u\033[122;7u",15);
        read_controls(&scene,&controls);
        assert(zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==1&&!strcmp(zell_panes_pending()+before,"F 2\nF 1\nZ 1\n"));
        desktop_input_feed(&scene,"\033[122;7u",9);
        read_controls(&scene,&controls);
        assert(zell_panes_zoom(ZELL_ROOM_PANES_RIGHT)==-1&&!strcmp(zell_panes_pending()+before,"F 2\nF 1\nZ 1\nZ -1\n"));
        /* Back to pane 0, as the paste tests that follow expect. */
        scene.mouse_valid=true;scene.mouse_click=true;scene.mouse_x=10;scene.mouse_y=300;
        assert(zell_desktop_click(&scene)&&zell_panes_focus(ZELL_ROOM_PANES_RIGHT)==0);
    }
    /* Leaving the room drops it, unsaid. */
    desktop_input_feed(&scene,"\033[200~e\nf\033[201~",15);
    assert(ask_n==3);
    scene.room=1;
    assert(!paste_answer(&scene)&&ask_n==0);
    zell_panes_test_reset();
    kittykb_input_init(&keyboard.input);zell_input_free(&pointer_input);
}
int main(void)
{
    zell_proc proc = {.out_fd = -1, .err_fd = -1};
    zell_turn turn = {0};
    zell_scene scene;
    int tty = open("/dev/tty", O_RDWR);
    assert(tty >= 0);
    close(tty);
    zell_scene_init(&scene, 320, 240);
    turn.phase = ZELL_PHASE_LLM;
    turn.pending = true;
    turn.request.route = ZELL_ROUTE_LLM;
    strcpy(turn.request.model, "qwen3.5-0.8b");
    strcpy(turn.request.question, "Hello");
    assert(proc_start(&proc, &turn.request, 5000));
    while (proc.active) {
        struct timespec pause = {.tv_nsec = 10000000};
        pump(&proc, &turn, &scene);
        nanosleep(&pause, NULL);
    }
    assert(strcmp(scene.answer, "Hello from the model.") == 0);
    assert(!turn.pending);
    assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);

    /* Cancellation must reap the one-shot process too. */
    strcpy(turn.request.question, "wait");
    assert(proc_start(&proc, &turn.request, 5000));
    while (!strstr(turn.text, "ready")) {
        struct timespec pause = {.tv_nsec = 10000000};
        assert(elapsed_ms(&proc) < 5000);
        drain(&proc, &turn);
        nanosleep(&pause, NULL);
    }
    proc_stop(&proc);
    assert(!proc.active);
    assert(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);
    /* Exercise the real service pipe pump and keyboard round trip. */
    desktop_service service={.in_fd=-1,.out_fd=-1};
    int request_pipe[2],reply_pipe[2];
    assert(pipe(request_pipe)==0&&pipe(reply_pipe)==0);
    pid_t helper=fork();assert(helper>=0);
    if(helper==0){
        assert(setsid()>=0);
        close(request_pipe[0]);close(reply_pipe[1]);
        const char frame[]="P4e616d653f\n";
        assert(write(request_pipe[1],frame,sizeof frame-1)==sizeof frame-1);
        char reply[10]={0};assert(read(reply_pipe[0],reply,sizeof reply)>0);
        assert(strcmp(reply,"a\n")==0);
        const char output[]="O53617665642e\n";
        assert(write(request_pipe[1],output,sizeof output-1)==sizeof output-1);
        close(request_pipe[1]);close(reply_pipe[0]);_exit(0);
    }
    close(request_pipe[1]);close(reply_pipe[0]);
    service.pid=helper;service.out_fd=request_pipe[0];service.in_fd=reply_pipe[1];
    fcntl(service.out_fd,F_SETFL,O_NONBLOCK);
    zell_desktop_init(&scene);scene.mode=ZELL_MODE_SERVICE;
    int64_t deadline=now_ms()+3000;
    while(service.pid>0){
        assert(now_ms()<deadline);service_pump(&service,&scene);
        if(scene.service_phase==1){
            assert(strcmp(scene.service_title,"Name?")==0);
            zell_controls control={.character='a'};zell_scene_tick(&scene,&control);
            control=(zell_controls){.confirm=true};zell_scene_tick(&scene,&control);
        }
        struct timespec pause={.tv_nsec=1000000};nanosleep(&pause,NULL);
    }
    assert(scene.service_phase==4&&strstr(scene.service_text,"Saved."));
    assert(service.in_fd==-1&&service.out_fd==-1);
    assert(waitpid(-1,NULL,WNOHANG)==-1&&errno==ECHILD);
    /* A stuck station can be cancelled without blocking the render loop. */
    assert(pipe(request_pipe)==0&&pipe(reply_pipe)==0);
    helper=fork();assert(helper>=0);
    if(helper==0){
        assert(setsid()>=0);signal(SIGTERM,SIG_IGN);
        close(request_pipe[0]);close(reply_pipe[1]);
        const char ready[]="O5265616479\n";
        assert(write(request_pipe[1],ready,sizeof ready-1)==sizeof ready-1);
        for(;;)pause();
    }
    close(request_pipe[1]);close(reply_pipe[0]);
    service=(desktop_service){.pid=helper,.out_fd=request_pipe[0],.in_fd=reply_pipe[1]};
    fcntl(service.out_fd,F_SETFL,O_NONBLOCK);
    scene.service_text[0]=0;scene.service_phase=0;deadline=now_ms()+3000;
    while(service.pid>0){
        assert(now_ms()<deadline);service_pump(&service,&scene);
        if(strstr(scene.service_text,"Ready"))scene.service_cancel=true;
        struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);
    }
    assert(strstr(scene.service_text,"Cancelled."));
    assert(waitpid(-1,NULL,WNOHANG)==-1&&errno==ECHILD);
    /* Real Kitty keyboard decoding: both Shift keys, release, text fields. */
    kittykb_input_init(&keyboard.input);
    const char supported[]="\033[?31u";
    kittykb_input_feed(&keyboard.input,supported,sizeof supported-1);
    scene.mode=ZELL_MODE_WALK;
    for(int side=0;side<2;side++){
        char sequence[64];int key=side?KITTYKB_KEY_RIGHT_SHIFT:KITTYKB_KEY_LEFT_SHIFT;
        int bytes=snprintf(sequence,sizeof sequence,"\033[%d;2:1u",key);
        kittykb_input_feed(&keyboard.input,sequence,(size_t)bytes);
        zell_controls control;read_controls(&scene,&control);assert(control.run);
        scene.mode=ZELL_MODE_SERVICE;scene.service_phase=1;
        read_controls(&scene,&control);assert(!control.run);
        scene.mode=ZELL_MODE_WALK;
        bytes=snprintf(sequence,sizeof sequence,"\033[%d;1:3u",key);
        kittykb_input_feed(&keyboard.input,sequence,(size_t)bytes);
        read_controls(&scene,&control);assert(!control.run);
    }
    /* Pixel mouse reports never become typed text, including split reports. */
    kittykb_input_init(&keyboard.input);zell_input_free(&pointer_input);
    const char mouse[]="\033[<35;20;50M";
    desktop_input_feed(&scene,mouse,sizeof mouse-1);
    assert(scene.mouse_valid&&scene.mouse_x==19&&scene.mouse_y==49);
    assert(keyboard.input.queue_count==0);
    const char fragment[]="\033[<35;30;";
    desktop_input_feed(&scene,fragment,sizeof fragment-1);
    assert(keyboard.input.queue_count==0);
    desktop_input_feed(&scene,"70M",3);
    assert(scene.mouse_x==29&&scene.mouse_y==69&&keyboard.input.queue_count==0);
    /* Motion (35) is not a press; a left press latches once; release, wheel
     * and right button do not. Modifier bits do not stop a left press. */
    assert(!scene.mouse_click);
    desktop_input_feed(&scene,"\033[<0;40;80M",sizeof "\033[<0;40;80M"-1);
    assert(scene.mouse_click&&scene.mouse_x==39&&scene.mouse_y==79&&keyboard.input.queue_count==0);
    scene.mouse_click=false;
    desktop_input_feed(&scene,"\033[<0;40;80m",sizeof "\033[<0;40;80m"-1);assert(!scene.mouse_click);
    desktop_input_feed(&scene,"\033[<64;40;80M",sizeof "\033[<64;40;80M"-1);assert(!scene.mouse_click);
    desktop_input_feed(&scene,"\033[<2;40;80M",sizeof "\033[<2;40;80M"-1);assert(!scene.mouse_click);
    desktop_input_feed(&scene,"\033[<32;40;80M",sizeof "\033[<32;40;80M"-1);assert(!scene.mouse_click);
    desktop_input_feed(&scene,"\033[<4;40;80M",sizeof "\033[<4;40;80M"-1);assert(scene.mouse_click);
    scene.mouse_click=false;
    /* The shared surface is asked for the window's size, then the maximized one. */
    {
        desktop_service sized={.in_fd=-1,.out_fd=-1,.frame_fd=-1};uint32_t cells[8]={0};
        zell_scene view=scene;view.width=1000;view.height=700;view.terminal_maximized=false;
        sized.frame_map=cells;terminal_size(&sized,&view);assert(cells[0]==81&&cells[1]==23);
        view.terminal_maximized=true;terminal_size(&sized,&view);assert(cells[0]==117&&cells[1]==33);
    }
    desktop_input_feed(&scene,"a",1);
    kittykb_event typed;assert(kittykb_input_next(&keyboard.input,&typed)&&typed.key=='a');
    desktop_input_feed(&scene,"\033[A",3);
    assert(kittykb_input_next(&keyboard.input,&typed)&&typed.key==KITTYKB_KEY_UP);
    test_service_surfaces();
    test_app_keys();
    test_paste_question();
    test_terminal_transport(&scene);
    test_menu_and_mana(&scene);
    test_contract_frames(&scene);
    test_window_ownership(&scene);
    test_notice_timer();
    puts("process test ok");
    return 0;
}
