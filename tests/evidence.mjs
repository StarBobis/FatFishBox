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
 const capture={sessionId:'main',candidateSha:sha,configuration:'Release',files:[{path:executable,kind:'executable'},{path:imagePath,kind:'screenshot'},
  {path:testLog,kind:'test_log'},{path:join(repo,'missing.png'),kind:'screenshot'}],tests:[{command:'ctest -C Release',exitCode:0,logPath:testLog}]};
 const first=await tool('cluster_capture_evidence',capture);
 assert.equal(first.sourceSha,sha);assert.equal(first.matchesExpectedCandidate,true);assert.equal(first.worktreeClean,true);
 assert.equal(first.sourceChangedDuringCapture,false);assert.equal(first.testExecutionVerified,false);
 assert.equal(first.unavailableFiles,1);assert.equal(first.sourceBinding,'capture_time_only');
 const read=(manifestId,args={})=>tool('cluster_get_evidence_manifest',{manifestId,...args});
 let offset=0,files=[];
 for(;;){const page=await read(first.evidenceManifest,{offset,limit:1});files.push(...page.files);if(page.nextOffset===null)break;offset=page.nextOffset;}
 const exe=files.find(entry=>entry.kind==='executable'),image=files.find(entry=>entry.kind==='screenshot'&&entry.state==='captured');
 assert.equal(exe.sha256,hash(await readFile(executable)));assert.equal(exe.blobVerified,true);
 assert.equal(image.sha256,hash(screenshot));assert.deepEqual(await readFile(image.blobPath),screenshot);
 assert.equal(files.find(entry=>entry.path.endsWith('missing.png')).state,'unavailable');
 const checks=await read(first.checksManifest);
 assert.equal(checks.build.CMAKE_BUILD_TYPE,'Release');assert.equal(checks.build.CMAKE_GENERATOR,'Ninja');
 assert.equal(checks.tests[0].source,'caller_reported');assert.equal(checks.testExecutionVerified,false);
 assert.ok(checks.tests[0].logEvidenceRef.startsWith('sha256:'));
 const work=await tool('cluster_get_work_item',{workItemId:first.workItemId});
 const review={workItemId:first.workItemId,requestId:'review-evidence',expectedRevision:work.revision,action:'record_review',details:{
  sessionId:'main',resultVersion:'1:8',candidateSha:sha,verdict:'passed',notes:'Fixture verified review',checksManifest:first.checksManifest,evidenceManifest:first.evidenceManifest}};
 await tool('cluster_update_work_item',review);
 const beforeAccept=await tool('cluster_get_work_item',{workItemId:first.workItemId});
 const acceptance={workItemId:first.workItemId,requestId:'accept-evidence',expectedRevision:beforeAccept.revision,action:'accept',details:{
  candidateSha:sha,notes:'Fixture acceptance refers to exact manifests',checksManifest:first.checksManifest,evidenceManifest:first.evidenceManifest}};
 await tool('cluster_update_work_item',acceptance);
 assert.equal((await tool('cluster_get_work_item',{workItemId:first.workItemId})).acceptance.evidenceManifest,first.evidenceManifest);
 const second=await tool('cluster_capture_evidence',capture);
 assert.equal(second.checksManifest,first.checksManifest);assert.equal(second.evidenceManifest,first.evidenceManifest);
 assert.equal(second.reusedBlobs,files.filter(entry=>entry.state==='captured').length,'unchanged evidence reuses immutable content');
 await writeFile(exe.blobPath,'corrupted local blob');
 const damaged=await read(first.evidenceManifest);
 assert.equal(damaged.files.find(entry=>entry.kind==='executable').blobVerified,false);
 const repaired=await tool('cluster_capture_evidence',capture);
 assert.equal(repaired.evidenceManifest,first.evidenceManifest);
 assert.equal((await read(first.evidenceManifest)).files.find(entry=>entry.kind==='executable').blobVerified,true);
 await writeFile(imagePath,Buffer.concat([screenshot,Buffer.from('changed bytes')]));
 const changed=await tool('cluster_capture_evidence',capture);
 assert.notEqual(changed.evidenceManifest,first.evidenceManifest);assert.equal(changed.checksManifest,first.checksManifest);
 assert.deepEqual(await readFile(image.blobPath),screenshot,'source edits do not rewrite a captured historical blob');
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
 assert.ok(archived.delivery.evidenceManifest.startsWith('fatfish://evidence/'));
 const automatic=await read(archived.delivery.evidenceManifest);
 const blocked=automatic.files.find(entry=>entry.path===outside);
 assert.equal(blocked.state,'unavailable');assert.equal(blocked.reason,'outside_workspace_select_explicitly');assert.equal(blocked.sha256,undefined);
 await writeFile(join(repo,'source.txt'),'dirty current worktree\n');
 const dirty=await tool('cluster_capture_evidence',capture);assert.equal(dirty.worktreeClean,false);
 const other=(await tool('cluster_create_queue',{name:'Other',requestId:'other'})).queueId;
 await tool('cluster_get_evidence_manifest',{queueId:other,manifestId:first.evidenceManifest},true);
 const storedManifest=join(data,'evidence','manifests',queue,first.evidenceManifest.split('/').at(-1)+'.json');
 const originalManifest=await readFile(storedManifest,'utf8');const manipulated=JSON.parse(originalManifest);manipulated.source.head='f'.repeat(40);
 await writeFile(storedManifest,JSON.stringify(manipulated));await tool('cluster_get_evidence_manifest',{manifestId:first.evidenceManifest},true);
 assert.equal((await tool('cluster_update_work_item',acceptance)).replayed,true,'lost-response replay does not re-execute a committed acceptance');
 await writeFile(storedManifest,originalManifest);
 await close();await connect();assert.equal((await read(first.checksManifest)).source.head,sha);
 console.log('Evidence passed: observed Git SHA/config, real executable and screenshot hashes, immutable/reused blobs, corruption detection/repair, provenance, auto capture boundaries, paging, isolation and restart.');
}finally{if(child)await close();await rm(folder,{recursive:true,force:true});}
