#define _GNU_SOURCE
#include "command_term.h"
#include "runtime.h"
#include "desktop.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    zell_command_screen screen;
    pid_t pid;
    int fd;
    bool started;
    char *input;
    size_t offset,length,capacity;
} command_pty;

static command_pty shells[ZELL_ALL_TERMS];

const zell_command_screen *zell_command_term_screen(int index){
    return index>=0&&index<ZELL_ALL_TERMS?&shells[index].screen:NULL;
}

/* Installed games keep tools beside the binary. Test programs live in build/. */
static char *tool_path(const char *name){
    const char *exe=zell_runtime_directory();
    if(!exe)return NULL;
    const char *roots[]={"tools","../tools"};
    for(size_t i=0;i<sizeof roots/sizeof roots[0];i++){
        char *path=NULL;
        if(asprintf(&path,"%s/%s/%s",exe,roots[i],name)<0)return NULL;
        if(access(path,R_OK)==0)return path;
        free(path);
    }
    return NULL;
}
static void enter_shell_directory(int index){
    const char *home=getenv("HOME");
    if(home)(void)chdir(home);
    if(index!=ZELL_COMMAND_SHELL)return;
    char *mana=tool_path("mana");
    if(!mana||chdir(mana)<0){
        dprintf(STDERR_FILENO,"Could not open the mana scripts directory.\r\n");
        free(mana);return;
    }
    free(mana);
    char *path=NULL,*dir=getcwd(NULL,0);
    if(dir&&asprintf(&path,"%s:%s",dir,getenv("PATH")?getenv("PATH"):"/usr/bin:/bin")>=0){
        setenv("PATH",path,1);
        free(path);
    }
    free(dir);
}

static void start_shell(command_pty *p,int index,int cols,int rows){
    struct winsize size={.ws_col=(unsigned short)cols,.ws_row=(unsigned short)rows};
    p->started=true;
    p->fd=-1;
    p->screen.revision++;
    p->screen.message="Shell unavailable - select to retry";
    neuroterm_free(&p->screen.terminal);
    if(!neuroterm_init(&p->screen.terminal,cols,rows))return;
    pid_t pid=forkpty(&p->fd,NULL,NULL,&size);
    if(pid<0){p->fd=-1;return;}
    if(pid==0){
        close_range(3,UINT_MAX,0);
        sigset_t empty;sigemptyset(&empty);sigprocmask(SIG_SETMASK,&empty,NULL);
        const int signals[]={SIGINT,SIGTERM,SIGQUIT,SIGHUP,SIGPIPE,SIGCHLD,SIGTSTP,SIGTTIN,SIGTTOU};
        for(size_t i=0;i<sizeof signals/sizeof signals[0];i++)signal(signals[i],SIG_DFL);
        setenv("TERM","xterm-256color",1);
        setenv("COLORTERM","truecolor",1);
        const char *const outer[]={"KITTY_WINDOW_ID","WINDOWID","KITTY_PID","KITTY_LISTEN_ON",
                                   "KITTY_PUBLIC_KEY","KITTY_PTY_BROKER_SESSION"};
        for(size_t i=0;i<sizeof outer/sizeof outer[0];i++)unsetenv(outer[i]);
        enter_shell_directory(index);
        if(index>=ZELL_COMMAND_TERMS){
            static const char *const actions[]={"upgrade","clean","autoclean","rotate","cache"};
            char *script=tool_path("zell_maintenance.py");
            if(script)execlp("python3","python3",script,actions[index-ZELL_COMMAND_TERMS],(char *)NULL);
            dprintf(STDERR_FILENO,"Could not open the maintenance tool.\r\n");
            free(script);
        }
        const char *shell=getenv("SHELL");
        if(!shell||*shell!='/'||access(shell,X_OK)<0)shell="/bin/sh";
        const char *base=strrchr(shell,'/');
        execl(shell,base?base+1:shell,"-i",(char *)NULL);
        dprintf(STDERR_FILENO,"Could not start %s: %s\r\n",shell,strerror(errno));
        _exit(127);
    }
    p->pid=pid;
    fcntl(p->fd,F_SETFD,FD_CLOEXEC);
    if(fcntl(p->fd,F_SETFL,O_NONBLOCK)<0){close(p->fd);p->fd=-1;return;}
    p->screen.message=NULL;
}

static void flush_input(command_pty *p){
    while(p->length&&p->fd>=0){
        ssize_t n=write(p->fd,p->input+p->offset,p->length);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)break;
        p->offset+=(size_t)n;p->length-=(size_t)n;
    }
    if(!p->length)p->offset=0;
}

bool zell_command_term_write(int index,const void *bytes,size_t length){
    if(index<0||index>=ZELL_ALL_TERMS||!bytes||!length)return false;
    command_pty *p=&shells[index];
    if(!p->pid||p->fd<0||length>ZELL_INPUT_PASTE_MAX+4096u-p->length)return false;
    if(p->offset&&p->offset+p->length+length>p->capacity){
        memmove(p->input,p->input+p->offset,p->length);
        p->offset=0;
    }
    size_t need=p->offset+p->length+length;
    if(need>p->capacity){
        size_t capacity=need<4096?4096:need;
        char *next=realloc(p->input,capacity);
        if(!next)return false;
        p->input=next;p->capacity=capacity;
    }
    memcpy(p->input+p->offset+p->length,bytes,length);
    p->length+=length;
    flush_input(p);
    return true;
}

static void terminal_replies(int index){
    char buf[512];size_t n;
    while((n=neuroterm_read(&shells[index].screen.terminal,buf,sizeof buf))!=0)
        (void)zell_command_term_write(index,buf,n);
}

void zell_command_term_key(int index,VTermKey key,VTermModifier modifiers){
    if(index<0||index>=ZELL_ALL_TERMS||!shells[index].pid)return;
    vterm_keyboard_key(shells[index].screen.terminal.term,key,modifiers);
    terminal_replies(index);
}

bool zell_command_term_paste(int index,const char *bytes,size_t length){
    if(index<0||index>=ZELL_ALL_TERMS||!bytes||!length||length>ZELL_INPUT_PASTE_MAX)return false;
    command_pty *p=&shells[index];
    if(!p->pid||length+12>ZELL_INPUT_PASTE_MAX+4096u-p->length)return false;
    vterm_keyboard_start_paste(p->screen.terminal.term);terminal_replies(index);
    bool ok=zell_command_term_write(index,bytes,length);
    vterm_keyboard_end_paste(p->screen.terminal.term);terminal_replies(index);
    return ok;
}

void zell_command_term_retry(int index){
    if(index<0||index>=ZELL_ALL_TERMS)return;
    command_pty *p=&shells[index];
    if(p->started&&!p->pid&&p->fd<0)p->started=false;
}

void zell_command_term_pump(const zell_scene *s){
    for(int i=0;i<ZELL_ALL_TERMS;i++){
        command_pty *p=&shells[i];
        neuroterm *t=&p->screen.terminal;
        bool wall=i<ZELL_COMMAND_TERMS&&s->desktop&&s->room==1;
        if(wall||(i>=ZELL_COMMAND_TERMS&&zell_input_terminal(s)==i)){
            zell_rect frame,body;
            float scale=1;
            if(wall){zell_command_term_layout(i,&frame,&body);scale=s->room_view.scale;}
            else{zell_utility_layout layout;zell_utility_rects(s,&layout);body=layout.body;}
            int cols=(int)((float)body.w*scale/ZELL_TERM_CELL_W);
            int rows=(int)((float)body.h*scale/ZELL_TERM_CELL_H);
            if(cols<1)cols=1;
            if(rows<1)rows=1;
            if(!p->started)start_shell(p,i,cols,rows);
            else if(p->pid&&p->fd>=0&&(t->cols!=cols||t->rows!=rows)&&neuroterm_resize(t,cols,rows)){
                struct winsize size={.ws_col=(unsigned short)cols,.ws_row=(unsigned short)rows};
                ioctl(p->fd,TIOCSWINSZ,&size);
                p->screen.revision++;
            }
        }
        if(!p->started)continue;
        char buf[4096];
        for(int chunk=0;p->fd>=0&&chunk<8;chunk++){
            ssize_t n=read(p->fd,buf,sizeof buf);
            if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK))break;
            if(n<0&&errno==EINTR)continue;
            if(n<=0){close(p->fd);p->fd=-1;break;}
            neuroterm_feed(t,buf,(size_t)n);
            terminal_replies(i);
            p->screen.revision++;
        }
        flush_input(p);
        if(p->pid&&waitpid(p->pid,NULL,WNOHANG)==p->pid){
            p->pid=0;p->length=p->offset=0;
        }
        if(!p->pid&&p->fd<0&&!p->screen.message)
            p->screen.message="Shell exited - select to restart";
    }
}

void zell_command_term_shutdown(void){
    for(int i=0;i<ZELL_ALL_TERMS;i++){
        command_pty *p=&shells[i];
        if(p->started&&p->fd>=0){
            pid_t fg=tcgetpgrp(p->fd);
            if(fg>0)kill(-fg,SIGHUP);
            close(p->fd);
        }
        if(p->pid)kill(p->pid,SIGHUP);
    }
    for(int i=0;i<ZELL_ALL_TERMS;i++){
        command_pty *p=&shells[i];
        if(p->pid){
            if(waitpid(p->pid,NULL,WNOHANG)==0){
                kill(p->pid,SIGKILL);
                while(waitpid(p->pid,NULL,0)<0&&errno==EINTR){}
            }
        }
        free(p->input);
        neuroterm_free(&p->screen.terminal);
        *p=(command_pty){0};
    }
}
