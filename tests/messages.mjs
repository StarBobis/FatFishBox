import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {createServer} from 'node:http';
import {once} from 'node:events';
import {createInterface} from 'node:readline';
import {mkdtemp,readFile,writeFile,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
import {setTimeout as delay} from 'node:timers/promises';
const folder=await mkdtemp(join(tmpdir(),'fatfish-messages-'));
const executable=resolve(process.argv[2]||'build/staged/FatFishBox.exe');
const running=new Map(),requests=[],receipts=new Set(),failures=new Map(),stateFailures=new Map();
const server=createServer(async(req,res)=>{
 let raw='';for await(const chunk of req)raw+=chunk;
 const {method,payload}=JSON.parse(raw),arg=payload.args.request;
 let value;
 if(method==='permissionPresets/catalog')value={defaultOptions:[]};
 else if(method==='session/state') {
  const fail=stateFailures.get(arg.sessionId);
  if(fail){res.writeHead(fail).end();return;}
  value={running:running.get(arg.sessionId)??true,cursor:2,queue:[],pendingInput:[]};
 } else if(method==='session/prompt') {
  requests.push(arg);
  const failure=failures.get(arg.sessionId);
  if(failure==='502'){failures.delete(arg.sessionId);res.writeHead(502).end();return;}
  if(failure==='403'){res.writeHead(403).end();return;}
  receipts.add(arg.requestId);
  if(failure==='drop'||failure==='finish-drop') {
   failures.delete(arg.sessionId);
   if(failure==='finish-drop')running.set(arg.sessionId,false);
   res.destroy();return;
  }
  value={accepted:true};
 } else if(method==='session/projections')value={values:{tokenUsage:{uncachedInputTokens:0,outputTokens:0,cacheReadTokens:0,cacheWriteTokens:0}}};
 else {res.writeHead(404).end();return;}
 res.end(JSON.stringify({result:{ok:true,value}}));
});
server.listen(0,'127.0.0.1');await once(server,'listening');
const origin=`http://127.0.0.1:${server.address().port}`,auth=join(folder,'auth.json'),stateFile=join(folder,'state.json');
await writeFile(auth,JSON.stringify({origin,cookie:'fixture=1',connectionId:'messages'}));
let child,tool,queue,other;
async function connect(){
 child=spawn(executable,['--mcp'],{env:{...process.env,FATFISHBOX_DATA_DIR:folder,FATFISHBOX_LEGACY_STATE_DIR:join(folder,'legacy')},windowsHide:true});
 const lines=createInterface({input:child.stdout})[Symbol.asyncIterator]();let id=0;
 const rpc=async(method,params={})=>{child.stdin.write(JSON.stringify({jsonrpc:'2.0',id:++id,method,params})+'\n');const line=await lines.next();assert.equal(line.done,false);const value=JSON.parse(line.value);assert.equal(value.error,undefined);return value.result;};
 await rpc('initialize');
 tool=async(name,args={},fail=false)=>{
  const response=await rpc('tools/call',{name,arguments:{...(name.startsWith('cluster_create_queue')?{}:{queueId:queue}),...args}});
  assert.equal(response.isError===true,fail,JSON.stringify(response));return response.structuredContent;
 };
}
async function close(){const ended=once(child,'exit');child.stdin.end();await ended;child=undefined;}
async function until(check){const deadline=Date.now()+15000;for(;;){const value=await check();if(value)return value;if(Date.now()>deadline)throw Error('Timed out waiting for message recovery');await delay(100);}}
try{
 await connect();queue=(await tool('cluster_create_queue',{name:'Messages',requestId:'messages'})).queueId;
 other=(await tool('cluster_create_queue',{name:'Other',requestId:'other'})).queueId;
 const state=JSON.parse(await readFile(stateFile,'utf8'));
 state.settings.authPath=auth;
 for(const id of ['normal','retry','lost','idle','terminal','ending','denied','tampered'])state.sessions[id]={id,queueId:queue,title:id,
  workspace:folder,role:'support',dshId:id,owner:origin+'#messages',dshAuthPath:auth,startCursor:1,cursor:2,status:id==='terminal'?'succeeded':'running',
  resultVersion:id==='terminal'?'1:8':'',dispatchState:'confirmed',created:Date.now(),updated:Date.now(),processed:false,baseSha:'1'.repeat(40),
  dispatchOperation:{input:{task:'Original task scope'}}};
 await writeFile(stateFile,JSON.stringify(state));
 await tool('cluster_list_work_items');
 const supplement={sessionId:'normal',requestId:'new-baseline',message:'Retain existing scope and verify the new baseline',baseSha:'2'.repeat(40)};
 const admitted=await tool('cluster_send_message',supplement);
 assert.equal(admitted.state,'accepted');assert.equal(admitted.executionConfirmed,false);assert.equal(admitted.mode,'steer');
 assert.ok(admitted.messageId.startsWith('ff-message-'));
 assert.equal(requests.at(-1).mode,'steer');assert.ok(requests.at(-1).content[0].text.includes('2'.repeat(40)));
 const before=requests.length;
 assert.equal((await tool('cluster_send_message',supplement)).messageId,admitted.messageId);
 assert.equal(requests.length,before,'accepted retries do not send another prompt');
 await tool('cluster_send_message',{...supplement,message:'Different body'},true);
 await tool('cluster_send_message',{...supplement,mode:'queue'},true);
 await tool('cluster_send_message',{...supplement,queueId:other},true);
 await tool('cluster_send_message',{sessionId:'normal',requestId:'whitespace',message:'\t\r\n '},true);
 const stored=JSON.parse(await readFile(stateFile,'utf8')).sessions.normal;
 assert.equal(stored.baseSha,'1'.repeat(40),'supplement does not silently rewrite the original baseline');
 assert.equal(stored.workspace,folder);
 const detail=await tool('cluster_get_message',{sessionId:'normal',requestId:'new-baseline',includeMessage:true});
 assert.deepEqual(detail.input,supplement);
 const work=await tool('cluster_get_work_item',{workItemId:admitted.workItemId});
 const issue=await tool('cluster_update_work_item',{workItemId:admitted.workItemId,requestId:'message-defect',expectedRevision:work.revision,
  action:'add_issue',details:{message:'A new defect in the existing scope',acceptance:'Verify the defect'}});
 const defect=await tool('cluster_send_message',{sessionId:'normal',requestId:'defect-only',issueIds:[issue.issueId]});
 assert.equal(defect.accepted,true);assert.ok(requests.at(-1).content[0].text.includes(issue.issueId));
 await tool('cluster_send_message',{sessionId:'normal',requestId:'foreign-defect',issueIds:['I-not-in-this-item']},true);
 assert.equal((await tool('cluster_send_message',{sessionId:'normal',requestId:'baseline-only',baseSha:'3'.repeat(40)})).accepted,true);
 const queued=await tool('cluster_send_message',{sessionId:'normal',requestId:'after-turn',message:'Follow-up after this turn',mode:'queue'});
 assert.equal(queued.mode,'queue');assert.equal(requests.at(-1).mode,'queue');
 running.set('idle',false);
 const idle=await tool('cluster_send_message',{sessionId:'idle',requestId:'idle',message:'Must not restart an idle worker'});
 assert.equal(idle.state,'not_sent');assert.equal(requests.some(r=>r.sessionId==='idle'),false);
 await tool('cluster_send_message',{sessionId:'terminal',requestId:'finished',message:'Do not restart'},true);
 failures.set('retry','502');
 const retry=await tool('cluster_send_message',{sessionId:'retry',requestId:'retry-502',message:'Retry same message'});
 assert.equal(retry.state,'unconfirmed');assert.equal(retry.retryable,true);assert.equal(retry.reason,'http_502');
 assert.equal(retry.stage,'session/prompt');
 await until(async()=>{const r=await tool('cluster_get_message',{sessionId:'retry',requestId:'retry-502'});return r.accepted&&r;});
 const retryIds=requests.filter(r=>r.sessionId==='retry').map(r=>r.requestId);
 assert.ok(retryIds.length>=2);assert.equal(new Set(retryIds).size,1);
 failures.set('lost','drop');
 const lost=await tool('cluster_send_message',{sessionId:'lost',requestId:'lost-reply',message:'Persist before delivery'});
 assert.equal(lost.state,'unconfirmed');
 await close();await connect();
 await until(async()=>{const r=await tool('cluster_get_message',{sessionId:'lost',requestId:'lost-reply'});return r.accepted&&r;});
 const lostIds=requests.filter(r=>r.sessionId==='lost').map(r=>r.requestId);
 assert.ok(lostIds.length>=2);assert.equal(new Set(lostIds).size,1,'restart retains the original deduplication ID');
 failures.set('ending','finish-drop');
 await tool('cluster_send_message',{sessionId:'ending',requestId:'ended-uncertain',message:'Do not revive after completion'});
 const ended=await until(async()=>{const r=await tool('cluster_get_message',{sessionId:'ending',requestId:'ended-uncertain'});return r.state==='unconfirmed_terminal'&&r;});
 assert.equal(ended.retryable,false);assert.equal(ended.needsInspection,true);
 assert.equal(requests.filter(r=>r.sessionId==='ending').length,1,'idle workers are not restarted during recovery');
 failures.set('denied','403');
 const denied=await tool('cluster_send_message',{sessionId:'denied',requestId:'denied',message:'Denied fixture'});
 assert.equal(denied.state,'failed');assert.equal(denied.retryable,false);
 stateFailures.set('tampered',503);
 const tamper=await tool('cluster_send_message',{sessionId:'tampered',requestId:'archive-check',message:'Original immutable text'});
 const body=JSON.parse(await readFile(tamper.messagePath,'utf8'));body.request.content[0].text='Modified text';
 await writeFile(tamper.messagePath,JSON.stringify(body));stateFailures.delete('tampered');
 const invalid=await until(async()=>{const r=await tool('cluster_get_message',{sessionId:'tampered',requestId:'archive-check'});return r.state==='failed'&&r;});
 assert.equal(invalid.reason,'message_archive_invalid');assert.equal(requests.some(r=>r.sessionId==='tampered'),false);
 assert.equal((await tool('cluster_get_message',{sessionId:'normal',requestId:'new-baseline'})).messageId,admitted.messageId);
 console.log('Message supplements passed: steer/queue, scope preservation, strict idempotency, 502 recovery, lost-response restart, idle protection, denial and archive integrity.');
}finally{if(child)await close();await new Promise(r=>server.close(r));await rm(folder,{recursive:true,force:true});}
