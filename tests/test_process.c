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
    service_frame(scene,restart,SERVICE_ORIGIN_MONITOR);service_frame(scene,notice,SERVICE_ORIGIN_MONITOR);service_frame(scene,audio,SERVICE_ORIGIN_MONITOR);
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
    service_frame(scene,weather,SERVICE_ORIGIN_MONITOR);
    assert(scene->weather_state==1&&scene->weather_icon==3);
    assert(!strcmp(scene->weather_temperature,"47 F")&&!strcmp(scene->weather_place,"Example"));
    char removed[]="W302037200a0a";service_frame(scene,removed,SERVICE_ORIGIN_MONITOR);
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
    frame_line(line,sizeof line,'G',"10 20 30");
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
    frame_line(line,sizeof line,'G',"10 20 no");
    peer_pump(&mon,&mon_svc,scene,line);
    frame_line(line,sizeof line,'G',"8 8 8");
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
        const char *prefix="G3720382039"; /* "7 8 9", which must not apply */
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
    frame_line(line,sizeof line,'G',"-2 -3 101");
    peer_pump(&mon,&mon_svc,scene,line);
    expect_mana(scene,-2,-1,-1);
    expect_numeric_menu(scene);
    frame_line(line,sizeof line,'G',"3 3 3");
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
    send_text(&mon,&mon_svc,scene,'G',"10 20 30");
    expect_mana(scene,10,20,30);
    assert(scene->provider_count==0&&scene->provider_generation==0);
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
    expect_fleet_page(scene);
    expect_numeric_menu(scene);
    peer_reap(&job,&job_svc);
    peer_reap(&mon,&mon_svc);
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
    kittykb_input_init(&keyboard.input);pointer_input=(desktop_input){0};
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
    test_terminal_transport(&scene);
    test_menu_and_mana(&scene);
    test_contract_frames(&scene);
    puts("process test ok");
    return 0;
}
