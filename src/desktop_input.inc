/* The outer terminal's input, split by zell_input_feed (desktop.c): keys go
 * to the keyboard decoder; SGR pixel-mouse reports move Zell's pointer and,
 * in a pane room, go to the panes; a bracketed paste goes to the pane with
 * the keys, or is typed as before anywhere else. */
static zell_input pointer_input;

static void desktop_keys(void *user,const char *bytes,size_t count){
 (void)user;kittykb_input_feed(&keyboard.input,bytes,count);
}
static void desktop_pointer(void *user,unsigned buttons,int x,int y,bool release){
 zell_scene *s=user;
 s->mouse_x=x-1-kittyfb_origin_x(&framebuffer);s->mouse_y=y-1-kittyfb_origin_y(&framebuffer);s->mouse_valid=true;
 /* Left press only: not motion (32), wheel (64) or extra buttons (128). */
 if(!release&&!(buttons&(3u|32u|64u|128u)))s->mouse_click=true;
 if(ZELL_ROOM_IS_PANES(s->room))(void)zell_panes_pointer(s,buttons,s->mouse_x,s->mouse_y,release);
}
static void desktop_paste(void *user,const char *bytes,size_t count){
 zell_scene *s=user;
 if(ZELL_ROOM_IS_PANES(s->room)){(void)zell_panes_paste(s,bytes,count);return;}
 kittykb_input_feed(&keyboard.input,bytes,count);
}
static int desktop_input_read(zell_scene *s){
 zell_input_sinks sinks={desktop_keys,desktop_pointer,desktop_paste,s};
 char bytes[1024];size_t room=KITTYKB_EVENT_QUEUE_CAPACITY-keyboard.input.queue_count;
 /* A paste is taken whole however full the key queue is. */
 if(zell_input_pasting(&pointer_input))room=sizeof bytes+1;
 if(room<2)return 0;
 if(room>sizeof bytes)room=sizeof bytes+1;
 ssize_t n=read(STDIN_FILENO,bytes,room-1);
 if(n>0)zell_input_feed(&pointer_input,&sinks,bytes,(size_t)n,now_ms());
 else if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)return -1;
 zell_input_idle(&pointer_input,&sinks,now_ms());
 if(kittykb_input_has_pending_escape(&keyboard.input)&&now_ms()-pointer_input.changed>ZELL_INPUT_ESCAPE_MS)kittykb_input_flush_escape(&keyboard.input);
 return 0;
}
/* Pixel mouse reports, and pastes framed so they are not taken for keys. */
static void desktop_mouse_mode(bool enabled){
 const char *sequence=enabled?"\033[?1003h\033[?1006h\033[?1016h\033[?2004h":"\033[?2004l\033[?1016l\033[?1006l\033[?1003l";
 (void)write(STDOUT_FILENO,sequence,strlen(sequence));
}
/* Text selected in a pane goes on the outer terminal's clipboard, between
 * two of the presenter's frames. */
static void desktop_copy(void){
 size_t length=0;
 const char *sequence=zell_panes_copy_sequence(&length);
 if(!sequence)return;
 if(kittyfb_write_control(&framebuffer,sequence,length))zell_panes_copy_written();
}
