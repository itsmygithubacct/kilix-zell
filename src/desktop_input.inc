/* The outer terminal's input, split by zell_input_feed (desktop.c): keys go
 * to the keyboard decoder; SGR pixel-mouse reports move Zell's pointer and,
 * in a pane room, go to the panes; a bracketed paste goes to the pane with
 * the keys, or is typed as before anywhere else. */
static zell_input pointer_input;
/* Bytes for the key decoder, in order: keys, and pastes outside the pane
 * rooms, which can be longer than its queue. They go in as it has room;
 * more input is read only once they have all gone. */
static char *typed_bytes;
static size_t typed_n,typed_cap;
static void typed_add(const char *bytes,size_t count){
 if(!count||count>ZELL_INPUT_PASTE_MAX+4096u-typed_n)return;
 if(typed_n+count>typed_cap){
  size_t cap=typed_cap?typed_cap:4096;
  while(cap<typed_n+count)cap*=2;
  char *grown=realloc(typed_bytes,cap);
  if(!grown)return;
  typed_bytes=grown;typed_cap=cap;
 }
 memcpy(typed_bytes+typed_n,bytes,count);typed_n+=count;
}
static void typed_feed(void){
 size_t room=KITTYKB_EVENT_QUEUE_CAPACITY-keyboard.input.queue_count;
 /* A byte is at most one event. */
 if(!typed_n||room<2)return;
 size_t take=typed_n<room-1?typed_n:room-1;
 kittykb_input_feed(&keyboard.input,typed_bytes,take);
 memmove(typed_bytes,typed_bytes+take,typed_n-take);typed_n-=take;
}
static void desktop_keys(void *user,const char *bytes,size_t count){
 (void)user;typed_add(bytes,count);typed_feed();
}
/* A paste with a line break in it would run its lines where it goes: it
 * waits, said, for Enter (pasted) or any other key, a click, another room
 * or ZELL_PASTE_ASK_MS (not pasted), as kitty asked before Zell took the
 * pastes itself. */
static char *ask_bytes;
static size_t ask_n;
static int ask_room;
static int64_t ask_until;
/* Answers are keys made after the question: keys before the paste, even
 * read with it, go where they were going first. */
static bool ask_armed;
/* The Enter that let a paste through: its repeats and its release are no
 * one's keys. */
static uint32_t answered_with;
static void ask_say(zell_scene *s,const char *text){
 if(ZELL_ROOM_IS_PANES(s->room)){zell_panes_tell(s,text,ZELL_PASTE_ASK_MS);return;}
 /* Said again even when the last notice, long gone, said the same. */
 zell_notice_apply(s,"");
 zell_notice_apply(s,text);
}
static bool next_key(kittykb_event *event){
 while(kittykb_input_next(&keyboard.input,event)){
  if(answered_with&&event->key==answered_with){
   if(event->action==KITTYKB_ACTION_REPEAT)continue;
   if(event->action==KITTYKB_ACTION_RELEASE){answered_with=0;continue;}
   answered_with=0;
  }
  return true;
 }
 return false;
}
static void ask_drop(zell_scene *s,bool say){
 free(ask_bytes);ask_bytes=NULL;ask_n=0;
 if(say)ask_say(s,"The paste was not sent.");
}
static void desktop_pointer(void *user,unsigned buttons,int x,int y,bool release){
 zell_scene *s=user;
 /* A click, not motion or the wheel, answers no. */
 if(ask_n&&!release&&!(buttons&(32u|64u)))ask_drop(s,true);
 s->mouse_x=x-1-kittyfb_origin_x(&framebuffer);s->mouse_y=y-1-kittyfb_origin_y(&framebuffer);s->mouse_valid=true;
 /* Left press only: not motion (32), wheel (64) or extra buttons (128). */
 if(!release&&!(buttons&(3u|32u|64u|128u)))s->mouse_click=true;
 if(ZELL_ROOM_IS_PANES(s->room))(void)zell_panes_pointer(s,buttons,s->mouse_x,s->mouse_y,release);
}
static void paste_deliver(zell_scene *s,const char *bytes,size_t count){
 if(ZELL_ROOM_IS_PANES(s->room)){(void)zell_panes_paste(s,bytes,count);return;}
 /* Elsewhere a paste is typed, as before 2004 was asked for. */
 if(bytes){typed_add(bytes,count);typed_feed();}
}
static void desktop_paste(void *user,const char *bytes,size_t count){
 zell_scene *s=user;
 size_t lines=bytes&&count!=ZELL_INPUT_PASTE_UNENDED?zell_paste_lines(bytes,count):0;
 /* One line, or a pane room where Kilix has the keys (it goes nowhere). */
 if(!lines||(ZELL_ROOM_IS_PANES(s->room)&&!zell_panes_keys_to_pane())){paste_deliver(s,bytes,count);return;}
 /* A newer paste takes the place of one still waiting. */
 ask_drop(s,false);
 char *held=malloc(count);
 if(!held){ask_say(s,"The paste was not sent.");return;}
 memcpy(held,bytes,count);
 ask_bytes=held;ask_n=count;ask_room=s->room;ask_until=zell_notice_clock_ms()+ZELL_PASTE_ASK_MS;
 ask_armed=!typed_n&&!keyboard.input.queue_count;
 /* Short enough for a laptop pane's title bar beside its buttons. */
 char text[64];
 snprintf(text,sizeof text,"Paste %zu line%s? Enter=yes Esc=no",lines,lines==1?"":"s");
 ask_say(s,text);
}
/* No key (an event that is not one), or a modifier alone. */
static bool modifier_key(uint32_t key){return key==KITTYKB_KEY_NONE||(key>=KITTYKB_KEY_LEFT_SHIFT&&key<=KITTYKB_KEY_ISO_LEVEL5_SHIFT);}
/* While a paste waits, the keys answer it: true when they were taken. */
static bool paste_answer(zell_scene *s){
 if(!ask_n)return false;
 /* Unanswered in time it is said; in another room, it is just gone. */
 if(s->room!=ask_room||zell_notice_clock_ms()>=ask_until){ask_drop(s,s->room==ask_room);return false;}
 if(!ask_armed){
  if(typed_n||keyboard.input.queue_count)return false;
  ask_armed=true;
 }
 kittykb_event event;
 while(kittykb_input_next(&keyboard.input,&event)){
  if(event.action!=KITTYKB_ACTION_PRESS||modifier_key(event.key))continue;
  if(event.key==KITTYKB_KEY_ENTER||event.key==KITTYKB_KEY_KP_ENTER){
   char *bytes=ask_bytes;size_t n=ask_n;
   ask_bytes=NULL;ask_n=0;answered_with=event.key;
   paste_deliver(s,bytes,n);
   free(bytes);
  }else ask_drop(s,true);
  break;
 }
 return true;
}
static int desktop_input_read(zell_scene *s){
 zell_input_sinks sinks={desktop_keys,desktop_pointer,desktop_paste,s};
 char bytes[1024];
 typed_feed();
 if(typed_n)return 0;
 ssize_t n=read(STDIN_FILENO,bytes,sizeof bytes);
 if(n>0)zell_input_feed(&pointer_input,&sinks,bytes,(size_t)n,now_ms());
 else if(n<0&&errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)return -1;
 zell_input_idle(&pointer_input,&sinks,now_ms());
 if(!typed_n&&kittykb_input_has_pending_escape(&keyboard.input)&&now_ms()-pointer_input.changed>ZELL_INPUT_ESCAPE_MS)kittykb_input_flush_escape(&keyboard.input);
 return 0;
}
/* Pixel mouse reports, and pastes framed so they are not taken for keys.
 * Between two of the presenter's frames while it runs. */
static void desktop_mouse_mode(bool enabled){
 const char *sequence=enabled?"\033[?1003h\033[?1006h\033[?1016h\033[?2004h":"\033[?2004l\033[?1016l\033[?1006l\033[?1003l";
 if(!kittyfb_write_control(&framebuffer,sequence,strlen(sequence)))(void)write(STDOUT_FILENO,sequence,strlen(sequence));
}
/* Text selected in a pane goes on the outer terminal's clipboard, between
 * two of the presenter's frames. One that cannot be written is dropped, not
 * tried again: a stalled terminal would stall Zell each time. */
static void desktop_copy(void){
 size_t length=0;
 const char *sequence=zell_panes_copy_sequence(&length);
 if(!sequence)return;
 (void)kittyfb_write_control(&framebuffer,sequence,length);
 zell_panes_copy_written();
}
