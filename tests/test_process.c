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
    puts("process test ok");
    return 0;
}
