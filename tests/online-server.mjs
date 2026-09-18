// Loopback integration fixture; no network service is left running after tests.
import net from 'node:net';
import {spawn} from 'node:child_process';
import {writeFileSync} from 'node:fs';
const sockets = new Set();
let requests=0;
const xmlEscape=s=>s.replaceAll('&','&amp;').replaceAll('<','&lt;');
function prop(href, directory=false, size=10, prefix='D') {
  const tag=prefix ? `${prefix}:` : '';
  return `<${tag}response><${tag}href>${xmlEscape(href)}</${tag}href><${tag}propstat><${tag}prop><${tag}resourcetype>${directory?`<${tag}collection/>`:''}</${tag}resourcetype><${tag}getcontentlength>${size}</${tag}getcontentlength></${tag}prop><${tag}status>HTTP/1.1 200 OK</${tag}status></${tag}propstat></${tag}response>`;
}
const chunks=body=>{
  const output=[];
  for(let i=0;i<body.length;i+=7) {
    const chunk=body.subarray(i,i+7);
    output.push(Buffer.from(`${chunk.length.toString(16)};fixture=1\r\n`),chunk,Buffer.from('\r\n'));
  }
  output.push(Buffer.from('0\r\nX-Test: done\r\n\r\n'));
  return Buffer.concat(output);
};
const server=net.createServer(socket=>{
  sockets.add(socket); socket.on('close',()=>sockets.delete(socket)); socket.on('error',()=>{});
  let input=Buffer.alloc(0),handled=false;
  socket.on('data',data=>{
    if(handled)return;
    input=Buffer.concat([input,data]);
    const end=input.indexOf('\r\n\r\n'); if(end<0)return;
    const header=input.subarray(0,end).toString();
    const contentLength=Number(header.match(/content-length: (\d+)/i)?.[1]||0);
    if(input.length<end+4+contentLength)return;
    handled=true; ++requests;
    const [method,target]=header.split('\r\n')[0].split(' ');
    const path=decodeURIComponent(new URL(target,'http://localhost').pathname);
    const send=(status,body='',extra={})=>{
      body=Buffer.isBuffer(body)?body:Buffer.from(body);
      const fields={'Content-Length':body.length,...extra};
      if(fields['Transfer-Encoding'])delete fields['Content-Length'];
      const head=Buffer.from(`HTTP/1.1 ${status} Test\r\n${Object.entries(fields).map(([k,v])=>`${k}: ${v}\r\n`).join('')}\r\n`);
      socket.end(Buffer.concat([head,fields['Transfer-Encoding']?chunks(body):body]));
    };
    const raw=s=>socket.end(s);
    if(path==='/silent')return;
    if(path==='/loop')return send(302,'',{Location:'/loop'});
    if(path==='/redirect')return send(308,'',{Location:'/chunked'});
    if(path==='/relative/start')return send(302,'',{Location:'../plain'});
    if(path==='/cross-origin')return send(302,'',{Location:'http://localhost:1/never'});
    if(path==='/negative-length')return raw('HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n');
    if(path==='/duplicate-length')return raw('HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx');
    if(path==='/ambiguous')return raw('HTTP/1.1 200 OK\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n');
    if(path==='/oversize-header')return raw('HTTP/1.1 200 OK\r\nX-Large: '+'a'.repeat(5000)+'\r\n\r\n');
    if(path==='/bad-range')return send(206,'123',{ 'Content-Range':'bytes 8-3/10'});
    if(path==='/error')return send(403,'no');
    if(path==='/encoding')return send(200,'compressed',{'Content-Encoding':'gzip'});
    if(path==='/truncated')return raw('HTTP/1.1 200 OK\r\nContent-Length: 99\r\n\r\nshort');
    if(path==='/bad-chunk')return raw('HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZ\r\n');
    if(path==='/short-chunk')return raw('HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nA\r\nx');
    if(path==='/icy-status')return raw('ICY 200 OK\r\nContent-Length: 10\r\n\r\n0123456789');
    if(path.startsWith('/radio')) {
      const title=path==='/radio-long-title'?'T'.repeat(500):'Artist - Track';
      const meta=Buffer.from(`StreamTitle='${title}';`);
      const padded=Buffer.alloc(Math.ceil(meta.length/16)*16);meta.copy(padded);
      const body=Buffer.concat([Buffer.from('ABCDE'),Buffer.from([padded.length/16]),padded,Buffer.from('FGHIJ'),Buffer.from([0])]);
      return send(200,body,{'icy-metaint':'5','icy-name':'Fixture Radio',...(path==='/radio-chunked'?{'Transfer-Encoding':'chunked'}:{})});
    }
    if(path.startsWith('/dav/')) {
      if(!header.includes('Authorization: Basic dXNlcjpwYXNz'))return send(401);
      const exists=['/dav/','/dav/Album/','/dav/Large/','/dav/ete & 100%.mp3','/dav/empty.mp3'].includes(path);
      if(!exists)return send(404);
      const directory=path.endsWith('/');
      if(method==='PROPFIND') {
        const prefix=path==='/dav/'?'D':'';
        let body=prop(target,directory,path.endsWith('empty.mp3')?0:10,prefix);
        if(path==='/dav/'&&header.includes('Depth: 1')) {
          body+=prop('/dav/Album/',true)+prop('/dav/ete%20%26%20100%25.mp3')+prop('/dav/empty.mp3',false,0);
          body+=prop('/outside.mp3')+prop('/dav/Album/nested.mp3')+prop('/dav/../outside.mp3');
          body+=prop('/dav/denied.mp3').replace('200 OK','403 Forbidden');
          body+=prop('/dav/ete%20%26%20100%25.mp3');
        }
        if(path==='/dav/Large/'&&header.includes('Depth: 1')) {
          for(let i=0;i<1000;i++)body+=prop(`/dav/Large/Folder-${String(i).padStart(4,'0')}/`,true,10,'');
        }
        const tag=prefix?`${prefix}:`:'';
        body=`<?xml version="1.0"?><${tag}multistatus ${prefix?'xmlns:D':'xmlns'}="DAV:">${body}</${tag}multistatus>`;
        return send(207,body,{'Content-Type':'application/xml',...(path==='/dav/Large/'?{}:{'Transfer-Encoding':'chunked'})});
      }
      if(method==='GET') {
        const full=Buffer.from(path.endsWith('empty.mp3')?'':'0123456789');
        const range=header.match(/Range: bytes=(\d+)-/i);
        if(range&&full.length) {
          const start=Number(range[1]); if(start>=full.length)return send(416);
          return send(206,full.subarray(start),{'Content-Range':`bytes ${start}-${full.length-1}/${full.length}`});
        }
        return send(200,full);
      }
      return send(405);
    }
    if(path==='/plain'||path==='/chunked')return send(200,'0123456789',path==='/chunked'?{'Transfer-Encoding':'chunked'}:{});
    send(404);
  });
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const port=server.address().port;
writeFileSync('tests/out/webdav.conf',`# fixture\nurl=http://127.0.0.1:${port}/dav/\nname=Test Library\nusername=user\npassword=pass\n`);
let result;
try {
  result=await new Promise((resolve,reject)=>{
    const child=spawn('tests/out/test_online.exe',[String(port)],{stdio:'inherit',windowsHide:true});
    const timer=setTimeout(()=>{child.kill();reject(new Error('Integration tests exceeded 90 seconds'));},90000);
    child.on('error',reject);child.on('exit',code=>{clearTimeout(timer);resolve(code);});
  });
} finally {
  for(const socket of sockets)socket.destroy();
  server.close();
}
console.log(`HTTP requests exercised: ${requests}`);
process.exitCode=result||0;
