"""Bounded line protocol between station helpers and the cave's native panels."""
import builtins
import sys
import threading

TITLE_WIDTH=40  # a menu title is one line of about (pane width - 56) / 8 columns

class Dialogue:
    def __init__(self, reader=None, writer=None):
        self.reader=reader or sys.stdin
        self.writer=writer or sys.stdout
        self.pending_output=False
        self.record=None
        self.send_lock=threading.Lock()

    def send(self, kind, text=''):
        # Keep terminal controls out of the renderer and bound every wire frame.
        text=''.join(c for c in str(text) if c.isprintable() or c in '\n\t')
        with self.send_lock:
            self.writer.write(kind+text.encode('utf-8')[:3500].hex()+'\n')
            self.writer.flush()

    def reply(self):
        line=self.reader.readline()
        if not line:raise EOFError
        return line.rstrip('\n')

    def input(self, prompt=''):
        self.send('P',prompt)
        reply=self.reply()
        if reply=='\x1b':raise KeyboardInterrupt
        if self.record:self.record('reply',str(prompt)+'\n'+reply)
        return reply

    def page(self, text):
        if self.record:self.record('report',text)
        self.send('C')
        # Reports use the same bounded history as command output.
        text=str(text)
        if len(text)>5500:text=text[:5400]+'\n[Preview truncated. Open the full file in a tool for more.]'
        self.write(text)
        self.send('R')
        self.reply()
        self.pending_output=False

    def choose(self, title, entries):
        if self.pending_output:
            self.send('R');self.reply();self.pending_output=False
        query='';page=0
        title=str(title)
        if '\n' in title or len(title)>TITLE_WIDTH:
            # The menu draws its title on one line. Show the whole question on a
            # scrollable page first, then ask with its first line as the title.
            self.send('C');self.write(title);self.send('R')
            escaped=self.reply()=='\x1b'
            self.pending_output=False
            if escaped:return None
            title=title.split('\n',1)[0]
            if len(title)>TITLE_WIDTH:title=title[:TITLE_WIDTH-3]+'...'
        while True:
            rows=[row for row in entries if query.casefold() in str(row[0]).casefold()]
            visible=rows[page:page+32]
            options=[(str(label),('item',value)) for label,value in visible]
            if page:options.append(('Previous page',('previous',None)))
            if page+32<len(rows):options.append(('Next page',('next',None)))
            options.append((f'Filter ({query or "all"})',('filter',None)))
            self.send('M',f'{title} / {len(rows)} results')
            for label,_ in options:self.send('L',label[:300])
            self.send('Q')
            reply=self.reply()
            if reply=='\x1b':return None
            try:operation,value=options[int(reply)][1]
            except (ValueError,IndexError):continue
            if operation=='item':
                if self.record:self.record('choice',title+' / '+options[int(reply)][0])
                return value
            if operation=='next':page+=32
            elif operation=='previous':page=max(0,page-32)
            else:
                try:query=self.input('Filter by name (empty shows all): ');page=0
                except KeyboardInterrupt:pass

    def write(self, text):
        if text.strip():self.pending_output=True
        # print() may write large reports: split without dropping later chunks.
        for start in range(0,len(text),800):self.send('O',text[start:start+800])
        return len(text)

    def flush(self):self.writer.flush()

    def install(self):
        builtins.input=self.input
        sys.stdout=self
        sys.stderr=self
