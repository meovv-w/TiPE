// Test-only DOM observations. Input uses the real keyboard and IME protocol.
import fs from 'node:fs';
const [profile, expression] = process.argv.slice(2);
const port = fs.readFileSync(profile+'/DevToolsActivePort','utf8').split('\n')[0];
const targets=await (await fetch('http://127.0.0.1:'+port+'/json/list')).json();
const target=process.env.TIPE_CDP_TARGET || 'input.html';
const page=targets.find(x=>x.type==='page' && x.url.includes(target));
if(!page) throw Error('Owned input fixture target missing');
const ws=new WebSocket(page.webSocketDebuggerUrl);
const timer=setTimeout(()=>{process.stderr.write('CDP timeout\n');process.exit(1);},5000);
await new Promise((resolve,reject)=>{ws.onopen=resolve;ws.onerror=reject;});
const response=new Promise((resolve,reject)=>{
  ws.onmessage=event=>{
    const message=JSON.parse(event.data);
    if(message.id!==1)return;
    if(message.error)reject(Error(JSON.stringify(message.error)));
    else resolve(message.result);
  };
});
ws.send(JSON.stringify({id:1,method:'Runtime.evaluate',params:{expression,returnByValue:true}}));
const result=await response;
if(result.exceptionDetails) throw Error(JSON.stringify(result.exceptionDetails));
process.stdout.write(JSON.stringify(result.result.value)+'\n');
clearTimeout(timer);ws.close();
