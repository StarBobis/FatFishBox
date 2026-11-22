import assert from 'node:assert/strict';
import {spawn,execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {once} from 'node:events';
import {createInterface} from 'node:readline';
import {mkdtemp,mkdir,readFile,writeFile,readdir,rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import {join,resolve,basename} from 'node:path';
import {createHash} from 'node:crypto';
const folder=await mkdtemp(join(tmpdir(),'fatfish-evidence-')),repo=join(folder,'repo'),data=join(folder,'data');
const executable=resolve(process.argv[2]||'build/staged/FatFishBox.exe'),run=promisify(execFile);
const hash=bytes=>createHash('sha256').update(bytes).digest('hex');
let child,tool,queue;
async function connect(){
 child=spawn(executable,['--mcp'],{env:{...process.env,FATFISHBOX_DATA_DIR:data,FATFISHBOX_LEGACY_STATE_DIR:join(folder,'legacy')},windowsHide:true});
 const lines=createInterface({input:child.stdout})[Symbol.asyncIterator]();let id=0;
 const rpc=async(method,params={})=>{child.stdin.write(JSON.stringify({jsonrpc:'2.0',id:++id,method,params})+'\n');const value=JSON.parse((await lines.next()).value);assert.equal(value.error,undefined);return value.result;};
 await rpc('initialize');
 const catalog=await rpc('tools/list');
 for(const name of ['cluster_capture_evidence','team_capture_evidence','cluster_get_evidence_manifest'])assert.ok(!catalog.tools.some(t=>t.name===name),'retired tool is hidden: '+name);
 tool=async(name,args={},fail=false)=>{const r=await rpc('tools/call',{name,arguments:{...(name==='cluster_create_queue'?{}:{queueId:queue}),...args}});assert.equal(r.isError===true,fail,JSON.stringify(r));return r.structuredContent;};
}
async function close(){const ended=once(child,'exit');child.stdin.end();await ended;child=undefined;}
try{
 await mkdir(repo);await mkdir(join(repo,'build'));await mkdir(data);
 const git=async(...args)=>run('git',['-C',repo,...args],{windowsHide:true});
 await git('init','--quiet');await git('config','user.email','fixture@example.invalid');await git('config','user.name','Fixture');
 await writeFile(join(repo,'.gitignore'),'build/\nshot.png\ntest.log\n');await writeFile(join(repo,'source.txt'),'committed source\n');
 await git('add','.');await git('commit','--quiet','-m','Fixture source');
 const sha=(await git('rev-parse','HEAD')).stdout.trim();
 const screenshot=Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aGRkAAAAASUVORK5CYII=','base64');
 const imagePath=join(repo,'shot.png'),testLog=join(repo,'test.log');
 await writeFile(imagePath,screenshot);await writeFile(testLog,'Fixture report only; service did not execute this test.\n');
 await writeFile(join(repo,'build','CMakeCache.txt'),`CMAKE_BUILD_TYPE:STRING=Release\nCMAKE_GENERATOR:INTERNAL=Ninja\nCMAKE_HOME_DIRECTORY:INTERNAL=${repo}\n`);
 const outside=join(folder,'private-outside.txt');await writeFile(outside,'Outside workspace fixture');
 await connect();queue=(await tool('cluster_create_queue',{name:'Evidence',requestId:'evidence'})).queueId;
 const file=join(data,'state.json');const state=JSON.parse(await readFile(file,'utf8'));
 for(const id of ['main','automatic'])state.sessions[id]={id,queueId:queue,title:id,workspace:repo,role:id==='main'?'reviewer':'support',status:'succeeded',
  resultVersion:'1:8',created:1,updated:1,completed:2,processed:false,baseSha:sha};
 await writeFile(file,JSON.stringify(state));await tool('cluster_list_work_items');
 const currentState=JSON.parse(await readFile(file,'utf8'));
 const auto=currentState.sessions.automatic;
 const resultFolder=join(data,'results',queue);await mkdir(resultFolder,{recursive:true});
 await writeFile(join(resultFolder,hash('automatic\n1:8')+'.json'),JSON.stringify({queueId:queue,sessionId:'automatic',resultVersion:'1:8',
  workItemId:auto.workItemId,messages:[{type:'assistant/message',text:'Completed fixture'}],archiveComplete:true,
  delivery:{candidateSha:sha,baseSha:sha,status:'passed',tests:[],evidencePaths:[outside],source:'agent_reported'}}));
 const cachedFile=join(resultFolder,hash('automatic\n1:8')+'.json');
 const cachedBytes=await readFile(cachedFile);
 const archived=await tool('cluster_get_session_result',{sessionId:'automatic'});
 for(const key of ['workItemId','candidateSha','baseSha','status','issues','checksManifest','evidenceManifest','nextAction'])
  assert.notEqual(archived.delivery[key],undefined,'old archives expose normalized '+key);
 assert.deepEqual(await readFile(cachedFile),cachedBytes,'enrichment preserves the original versioned log bytes');
 assert.equal(archived.delivery.evidenceManifest,null);
 assert.equal(archived.delivery.checksManifest,null);
 for(const name of ['cluster_capture_evidence','team_capture_evidence']) {
  const retired=await tool(name,{sessionId:'automatic',files:[outside,join(repo,'missing.png')]});
  assert.equal(retired.collectionDisabled,true);
 }
 assert.ok(!(await readdir(data)).includes('evidence'),'result reads and stale collection calls create no proof archive');
 assert.deepEqual(await readFile(cachedFile),cachedBytes);
 assert.equal(await readFile(outside,'utf8'),'Outside workspace fixture');
 console.log('No-collection regression passed: result reads preserve original reports; retired collectors are harmless and produce no archives.');
} finally {
 if(child)await close();
 await rm(folder,{recursive:true,force:true});
}
