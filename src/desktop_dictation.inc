/* One explicit microphone turn; text stays in memory and needs Enter to send. */
typedef struct {pid_t pid;int fd;size_t used;char text[240];bool ready;int64_t started;} desktop_dictation;
static void dictation_read(desktop_dictation *d){
 char bytes[256];ssize_t count;
 while((count=read(d->fd,bytes,sizeof bytes))>0){for(ssize_t i=0;i<count;i++){
  unsigned char c=(unsigned char)bytes[i];if((c>=32&&c!=127)||c=='\n'||c=='\t'){
   if(d->used+1<sizeof d->text)d->text[d->used++]=(c=='\n'||c=='\t')?' ':(char)c;
  }
 }}
 d->text[d->used]=0;
}
static void dictation_stop(desktop_dictation *d){
 if(d->pid>0){
  kill(-d->pid,SIGTERM);int64_t until=now_ms()+6000;pid_t ended=0;
  while(now_ms()<until){ended=waitpid(d->pid,NULL,WNOHANG);if(ended==d->pid||(ended<0&&errno==ECHILD))break;struct timespec pause={.tv_nsec=10000000};nanosleep(&pause,NULL);}
  if(ended==0){kill(-d->pid,SIGKILL);while(waitpid(d->pid,NULL,0)<0&&errno==EINTR){}}
 }
 if(d->fd>=0)close(d->fd);
 explicit_bzero(d->text,sizeof d->text);*d=(desktop_dictation){.fd=-1};
}
static void dictation_sync(desktop_dictation *d,zell_scene *s){
 if(s->dictation_changed){
  s->dictation_changed=false;
  if(!s->dictation_on&&d->pid>0)kill(-d->pid,SIGINT); /* finish this utterance */
  else if(s->dictation_on&&d->pid<=0){
   int pipefd[2];
   if(pipe(pipefd)!=0){s->dictation_on=false;return;}
   pid_t pid=fork();
   if(pid==0){
    setsid();int nullfd=open("/dev/null",O_RDWR);dup2(nullfd,STDIN_FILENO);dup2(nullfd,STDERR_FILENO);
    dup2(pipefd[1],STDOUT_FILENO);close(pipefd[0]);close(pipefd[1]);if(nullfd>2)close(nullfd);
    syscall(SYS_close_range,3u,~0u,0);
    execlp("kilix","kilix","dictate","--seconds","60",(char *)NULL);_exit(127);
   }
   close(pipefd[1]);
   if(pid<0){close(pipefd[0]);s->dictation_on=false;return;}
   d->pid=pid;d->fd=pipefd[0];d->used=0;d->text[0]=0;d->ready=false;d->started=now_ms();
   fcntl(d->fd,F_SETFL,fcntl(d->fd,F_GETFL)|O_NONBLOCK);
  }
 }
 if(d->pid>0){
  dictation_read(d);int status=0;pid_t done=waitpid(d->pid,&status,WNOHANG);
  if(done==d->pid){
   dictation_read(d);
   close(d->fd);d->fd=-1;d->pid=0;s->dictation_on=false;
   while(d->used&&d->text[d->used-1]==' ')d->text[--d->used]=0;
   d->ready=WIFEXITED(status)&&WEXITSTATUS(status)==0&&d->used>0;
   if(!d->ready){snprintf(s->notice_text,sizeof s->notice_text,"Dictation heard no text. Check microphone and Kilix STT settings.");s->notice_count++;}
  }else if(now_ms()-d->started>75000){dictation_stop(d);s->dictation_on=false;}
 }
 if(d->ready&&(s->mode==ZELL_MODE_WALK||s->mode==ZELL_MODE_QUESTION)){
  snprintf(s->question,sizeof s->question,"%s",d->text);s->question_len=(int)strlen(s->question);
  s->mode=ZELL_MODE_QUESTION;s->station=0;d->ready=false;explicit_bzero(d->text,sizeof d->text);
 }
}
