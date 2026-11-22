import assert from 'node:assert/strict';
import {spawn,execFile} from 'node:child_process';
import {once} from 'node:events';
import {promisify} from 'node:util';
import {mkdtemp,mkdir,writeFile,readFile,rm} from 'node:fs/promises';
import {createServer} from 'node:http';
import {createInterface} from 'node:readline';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';

const run=promisify(execFile),executable=resolve(process.argv[2]||'build/Release/FatFishBox.exe');
const root=await mkdtemp(join(tmpdir(),'fatfish-teams-')),data=join(root,'data'),repo=join(root,'repository with spaces');
await Promise.all([mkdir(data),mkdir(repo)]);
const states=new Map(),prompts=new Map(),selected=new Map(),unavailableSessions=new Set(),failedCancellations=new Set();let number=0,child,candidate;
const server=createServer(async(req,res)=>{
    try {
        let body='';for await(const part of req) body+=part;
        const {method,payload}=JSON.parse(body),a=payload.args.request;let value;
        switch(method) {
        case 'permissionPresets/catalog':value={defaultOptions:[{value:'local',name:'Local'}]};break;
        case 'agentPresets/list':value={presets:[]};break;
        case 'session/modelCatalog':value={groups:[{id:'fixture',name:'Fixture',models:['lead','dev','review','integrate'].map(id=>({id,name:id}))}]};break;
        case 'session/create':value={sessionId:`team-session-${++number}`};states.set(value.sessionId,{cursor:1,running:false,queue:[],pendingInput:[]});break;
        case 'commands/execute':value={result:{kind:'success'}};break;
        case 'session/projections':value={values:{permissions:{currentValue:'local'}}};break;
        case 'session/selectModel':selected.set(a.sessionId,a.model);value={selected:a};break;
        case 'session/state':if(unavailableSessions.has(a.sessionId)){res.writeHead(503).end('fixture temporary failure');return;}value=states.get(a.sessionId);break;
        case 'session/prompt':if(!prompts.has(a.sessionId)){prompts.set(a.sessionId,a.content[0].text);states.set(a.sessionId,{cursor:2,running:true,queue:[],pendingInput:[]});}value={accepted:true};break;
        case 'session/cancel':if(failedCancellations.has(a.sessionId)){res.writeHead(503).end('fixture cancellation failure');return;}states.set(a.sessionId,{cursor:7,running:false,queue:[],pendingInput:[]});value={};break;
        case 'session/page':value={records:[{type:'event',event:{seq:6,type:'assistant/message',data:{message:{content:[{text:'Fixture complete\n```fatfish-delivery\n'+JSON.stringify({candidateSha:candidate,status:'passed',worktreeClean:true,tests:[],evidencePaths:[]})+'\n```'}]}}}},
            {type:'event',event:{seq:7,type:'turn/end',time:Date.now(),data:{reason:{kind:'completed'}}}}]};break;
        default:res.writeHead(404).end();return;
        }
        res.setHeader('content-type','application/json');res.end(JSON.stringify({result:{ok:true,value}}));
    } catch(e) {res.writeHead(500).end(String(e));}
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const env={...process.env,FATFISHBOX_DATA_DIR:data,FATFISHBOX_LEGACY_STATE_DIR:join(root,'legacy')};delete env.DSH_WORKERS_CONFIG;
async function connect(mode='--mcp') {
    child=spawn(executable,[mode],{env,windowsHide:true});
    const stream=createInterface({input:child.stdout})[Symbol.asyncIterator]();let sequence=0;
    child.stderr.on('data',()=>{});
    const rpc=async(method,params={})=>{
        const id=++sequence;child.stdin.write(JSON.stringify({jsonrpc:'2.0',id,method,params})+'\n');
        const line=await stream.next();assert.equal(line.done,false,`MCP exited during ${method}`);const result=JSON.parse(line.value);assert.equal(result.error,undefined,JSON.stringify(result));return result.result;
    };
    await rpc('initialize');
    return {rpc,tool:async(name,args={},fail=false)=>{
        const r=await rpc('tools/call',{name,arguments:args});assert.equal(r.isError===true,fail,JSON.stringify(r));return r.structuredContent;
    }};
}
async function close() {if(!child)return;const ended=once(child,'exit');child.stdin.end();await ended;child=undefined;}
async function until(fn,label) {
    const deadline=Date.now()+45000;for(;;) {const value=await fn();if(value)return value;if(Date.now()>deadline)throw Error(`Timed out: ${label}`);await new Promise(r=>setTimeout(r,200));}
}
const disk=async()=>JSON.parse(await readFile(join(data,'teams.json'),'utf8'));
try {
    await run('git',['init',repo],{windowsHide:true});
    await writeFile(join(repo,'feature.txt'),'baseline\n');
    await run('git',['-C',repo,'add','feature.txt'],{windowsHide:true});
    await run('git',['-C',repo,'-c','user.name=Fixture','-c','user.email=fixture@example.test','commit','-m','baseline'],{windowsHide:true});
    const base=(await run('git',['-C',repo,'rev-parse','HEAD'],{windowsHide:true})).stdout.trim();
    candidate=base;
    let {tool,rpc}=await connect();
    const auth=join(root,'auth.json');await writeFile(auth,JSON.stringify({origin:`http://127.0.0.1:${server.address().port}`,cookie:'fixture=1',connectionId:'fixture'}));
    let imported=await tool('cluster_import_models',{authPath:auth,configPath:''}),settings=imported.settings;
    settings=await tool('cluster_configure',{revision:settings.revision,authPath:auth,permission:'local'});
    const pools=Object.fromEntries(Object.values(settings.pools).map(p=>[p.model,p.id]));
    for(const pool of Object.values(settings.pools))settings=await tool('cluster_update_pool',{revision:settings.revision,id:pool.id,enabled:true,concurrency:8});
    const initial=await tool('team_get_settings');
    const config={revision:initial.revision,leadPool:pools.lead,developerPool:pools.dev,reviewerPool:pools.review,integratorPool:pools.integrate,totalConcurrency:1,developerConcurrency:1,reviewerConcurrency:1};
    const saved=await tool('team_update_settings',config);assert.equal(saved.totalConcurrency,1);
    await tool('team_update_settings',config,true);
    const charter={objective:'Implement fixture',acceptance:'A verified candidate',constraints:'Preserve baseline',architecture:'One isolated feature',authority:'Local worktrees and commits',escalation:'Scope conflicts'};
    const input={requestId:'fixture-project',title:'Team fixture',workspace:repo,charter,domains:[{name:'Feature',mission:'Implement feature',acceptance:'Independent review',dependencies:[]}]};
    await tool('team_start_project',{...input,requestId:'bad-dependency',domains:[{...input.domains[0],dependencies:['Feature']}]},true);
    const started=await tool('team_start_project',input),pid=started.projectId;
    assert.equal((await tool('team_start_project',input)).projectId,pid);
    await tool('team_start_project',{...input,title:'Conflicting retry'},true);
    const active=await until(async()=>{const p=(await disk()).projects[pid];return Object.values(p.jobs).find(j=>j.role==='lead'&&j.sessionId)&&p;},'lead startup');
    const d=Object.values(active.domains)[0],lead=Object.values(active.jobs).find(j=>j.role==='lead');
    const credentials={projectId:pid,domainId:d.domainId,token:d.token,charterVersion:1};
    assert.equal(selected.get((JSON.parse(await readFile(join(data,'state.json'),'utf8'))).sessions[lead.sessionId].dshId),'lead');
    assert.ok(prompts.values().next().value.includes('team_domain_context'));
    assert.ok((await readFile(join(lead.workspace,'team-call.ps1'),'utf8')).includes('--team-call'));
    // Force termination, then reproduce the durable boundary where cluster
    // reserved/launched the session but teams has not linked its sessionId yet.
    const killed=once(child,'exit');child.kill('SIGKILL');await killed;child=undefined;
    const recovery=await disk(),recovering=recovery.projects[pid].jobs[lead.jobId];
    delete recovering.sessionId;delete recovering.workItemId;recovering.state='starting';
    await writeFile(join(data,'teams.json'),JSON.stringify(recovery));
    const beforeReplay=number;
    ({tool,rpc}=await connect());
    const relinked=await until(async()=>{const j=(await disk()).projects[pid].jobs[lead.jobId];return j.sessionId&&j;},'interrupted cross-file launch relink');
    assert.equal(relinked.sessionId,lead.sessionId,'recovery must bind the original session');
    assert.equal(number,beforeReplay,'recovery must not create a second remote session');
    assert.equal(prompts.size,1,'recovery must not submit a second prompt');
    const dshId=JSON.parse(await readFile(join(data,'state.json'),'utf8')).sessions[lead.sessionId].dshId;
    unavailableSessions.add(dshId);
    await until(async()=>Object.values((await tool('team_get_project',{projectId:pid})).runtimeErrors).some(e=>e.operation==='observe_session'&&e.scope===lead.sessionId),'visible background failure');
    unavailableSessions.delete(dshId);
    await until(async()=>!Object.values((await tool('team_get_project',{projectId:pid})).runtimeErrors).some(e=>e.operation==='observe_session'),'successful observation clears its own error');
    console.log('Interrupted launch replay and visible/recoverable runtime failures passed.');
    const request=join(root,'bridge-request.json'),response=join(root,'bridge-response.json');
    await writeFile(request,JSON.stringify({tool:'team_domain_context',arguments:credentials}));
    await run(executable,['--team-call',request,response],{env,windowsHide:true});
    assert.equal(JSON.parse(await readFile(response,'utf8')).result.charter.objective,charter.objective);
    await tool('team_domain_context',{...credentials,token:'wrong'},true);
    const call=(name,args={},fail=false)=>tool(name,{...credentials,...args},fail);
    const contract=await call('team_get_contract');assert.equal(contract.version,7);
    assert.ok(contract.tools.some(t=>t.name==='team_next_action'));
    assert.equal(JSON.parse(await readFile(join(lead.workspace,'team-tools.json'),'utf8')).domainAction.example.arguments.operation,'cluster_update_work_item');
    await writeFile(request,JSON.stringify({tool:'team_domain_action',arguments:{...credentials,operation:'finish',arguments:{}}}));
    await assert.rejects(run(executable,['--team-call',request,response],{env,windowsHide:true}));
    const diagnostic=JSON.parse(await readFile(response,'utf8'));
    assert.equal(diagnostic.details.reason,'invalid_domain_operation');
    assert.equal(diagnostic.details.contract.example.arguments.arguments.action,'record_review','CLI errors preserve executable correction templates');

    assert.equal((await tool('team_wait_decisions',{projectId:pid,timeoutMs:0})).ready,false,'normal startup does not wake principal');
    const baselineCursor=(await tool('team_wait_decisions',{projectId:pid,timeoutMs:0})).cursor;
    const task={requestId:'dev-1',title:'Feature implementation',role:'developer',task:'Implement fixture and return candidate'};
    const dev=await call('team_delegate',task);
    assert.equal((await call('team_delegate',task)).jobId,dev.jobId);
    await call('team_delegate',{...task,title:'Bad retry'},true);
    const cancelled=await call('team_delegate',{...task,requestId:'cancel-queued'});
    assert.equal((await call('team_cancel_job',{requestId:'cancel-request',jobId:cancelled.jobId,reason:'Redundant fixture task'})).state,'cancelled');
    await call('team_delegate',{requestId:'bad-review',title:'Bad review',role:'reviewer',task:'Review'},true);
    await call('team_finish_domain',{requestId:'premature',summary:'Done',evidence:[],acceptanceResults:[{}]},true);
    await new Promise(r=>setTimeout(r,2500));
    assert.equal((await disk()).projects[pid].jobs[dev.jobId].sessionId,undefined,'global cap includes running lead');
    assert.equal((await disk()).scheduler.lastProjects['3']??'','','capacity probes must not consume a project turn');
    await call('team_checkpoint',{requestId:'wait-1',state:'waiting',checkpoint:{next:'Review the developer candidate',jobs:[dev.jobId]}});
    const native=JSON.parse(await readFile(join(data,'state.json'),'utf8'));states.set(native.sessions[lead.sessionId].dshId,{cursor:7,running:false,queue:[],pendingInput:[]});
    const runningDev=await until(async()=>{const j=(await disk()).projects[pid].jobs[dev.jobId];return j.sessionId&&j;},'developer after lead yields');
    assert.notEqual(runningDev.workspace,repo);assert.ok(runningDev.workspace.includes('team-workspaces'));
    assert.equal((await run('git',['-C',runningDev.workspace,'rev-parse','HEAD'],{windowsHide:true})).stdout.trim(),base);
    assert.equal((await tool('team_wait_decisions',{projectId:pid,afterCursor:baselineCursor,timeoutMs:0})).ready,false,'ordinary child status stays out of principal context');
    const queue=(await disk()).projects[pid].queueId;
    await tool('cluster_start_session',{queueId:queue,requestId:'bypass',title:'Bypass',batch:'bypass',workspace:repo,task:'Bypass'},true);
    const endJob=async(job)=>{
        const native=JSON.parse(await readFile(join(data,'state.json'),'utf8'));
        states.set(native.sessions[job.sessionId].dshId,{cursor:7,running:false,queue:[],pendingInput:[]});
    };
    await endJob(runningDev);
    await until(async()=>{const j=(await disk()).projects[pid].jobs[dev.jobId];return j.state==='succeeded'&&j.resultVersion;},'developer completion');
    assert.equal((await call('team_priority_results')).items[0].reason,'candidate_needs_review');
    await call('team_next_handoff'); // New results do not prohibit consuming the existing FIFO.
    const successor=await until(async()=>Object.values((await disk()).projects[pid].jobs).find(j=>j.role==='lead'&&j.sessionId&&j.jobId!==lead.jobId),'lead continuation');
    const reviewInput={requestId:'review-1',title:'Independent review',role:'reviewer',sourceJobId:dev.jobId,baseSha:base,task:'Independently verify the exact candidate'};
    await call('team_delegate',{...reviewInput,requestId:'wrong-candidate',baseSha:'f'.repeat(40)},true);
    const review=await call('team_delegate',reviewInput);
    await call('team_checkpoint',{requestId:'wait-review',state:'waiting',checkpoint:{next:'Read independent review',candidate:base}});
    // A failed receipt must not consume the terminal observation permanently.
    // Replace only this isolated fixture's lock file while its process is down.
    await close();
    const receiptLock=join(data,`handoff-${queue}.lock`);
    await rm(receiptLock,{force:true});await mkdir(receiptLock);
    ({tool,rpc}=await connect());
    await endJob(successor);
    await until(async()=>Object.values((await tool('team_get_project',{projectId:pid})).runtimeErrors).some(e=>e.operation==='mark_receipt'&&e.scope===successor.sessionId),'receipt write failure is visible');
    assert.equal((await disk()).projects[pid].jobs[successor.jobId].observedTerminal??false,false,'failed receipt must leave observation retryable');
    await close();await rm(receiptLock,{recursive:true});
    ({tool,rpc}=await connect());
    await until(async()=>(await disk()).projects[pid].jobs[successor.jobId].observedTerminal,'receipt retry after restart');
    assert.equal(JSON.parse(await readFile(join(data,'state.json'),'utf8')).sessions[successor.sessionId].processed,true,'recovered receipt must be persisted');
    const runningReview=await until(async()=>{const j=(await disk()).projects[pid].jobs[review.jobId];return j.sessionId&&j;},'reviewer startup');
    const reviewNative=JSON.parse(await readFile(join(data,'state.json'),'utf8'));
    assert.equal(selected.get(reviewNative.sessions[runningReview.sessionId].dshId),'review');
    assert.notEqual(runningReview.workspace,runningDev.workspace);
    await endJob(runningReview);
    const completedReview=await until(async()=>{const j=(await disk()).projects[pid].jobs[review.jobId];return j.state==='succeeded'&&j.resultVersion&&j;},'review completion');
    assert.equal((await call('team_priority_results')).items[0].reason,'review_result_unhandled');
    await call('team_checkpoint',{requestId:'cannot-park-completed-review',state:'waiting',checkpoint:{next:'Read it after historical backlog'}},true);
    const getWork=()=>call('team_domain_read',{operation:'cluster_get_work_item',arguments:{workItemId:runningDev.workItemId}});
    const action=async(name,details,requestId,fail=false)=>call('team_domain_action',{operation:'cluster_update_work_item',arguments:{workItemId:runningDev.workItemId,expectedRevision:(await getWork()).revision,requestId,action:name,details}},fail);
    const verified={sessionId:completedReview.sessionId,resultVersion:completedReview.resultVersion,candidateSha:base,verdict:'passed',notes:'Independent fixture checks'};
    await action('record_review',{...verified,candidateSha:'f'.repeat(40)},'bad-review-sha',true);
    await action('record_review',verified,'record-review');
    await action('accept',{candidateSha:base,notes:'All fixture conditions verified'},'accept');
    assert.equal((await getWork()).state,'accepted');
    assert.equal((await call('team_priority_results')).items[0].reason,'ready_for_integration','acceptance cannot hide the missing integration stage');
    await call('team_delegate',{requestId:'integrator-without-sources',title:'Wrong integration',role:'integrator',task:'Integrate'},true);
    await action('integrate',{integrationSha:base,targetBranch:'main',notes:'Fixture candidate already equals the repository main commit'},'integrate');
    assert.equal((await call('team_priority_results')).count,0);
    assert.equal((await call('team_domain_jobs')).jobs.length,1,'only the cancelled unaccepted task remains visible');
    assert.equal((await call('team_domain_jobs',{includeAccepted:true})).jobs.length,3,'accepted history remains available on demand');
    const decisionInput={requestId:'decision-1',question:'Choose compatibility',reason:'Existing format conflict',recommendation:'Keep compatibility',alternatives:'Drop support',impact:'One additional test',blockedScope:'Format only'};
    const decision=await call('team_escalate',decisionInput);
    assert.equal((await call('team_escalate',{...decisionInput,requestId:'decision-duplicate'})).decisionId,decision.decisionId);
    const notification=await tool('team_wait_decisions',{projectId:pid,afterCursor:baselineCursor,timeoutMs:0});
    assert.equal(notification.ready,true);assert.equal(notification.events.length,1);assert.equal(notification.events[0].kind,'decision');
    const resolution={projectId:pid,decisionId:decision.decisionId,expectedRevision:0,requestId:'resolve-1',resolution:'Keep compatibility and add regression test'};
    await tool('team_resolve_decision',resolution);await tool('team_resolve_decision',resolution);
    await tool('team_resolve_decision',{...resolution,requestId:'stale-resolution'},true);
    const amended={...charter,constraints:'Preserve baseline and old format'};
    await tool('team_update_charter',{projectId:pid,requestId:'charter-2',expectedVersion:1,charter:amended,changeSummary:'Clarified compatibility'});
    credentials.charterVersion=2;
    assert.equal((await call('team_domain_context')).charterVersion,2);
    await call('team_delegate',{...task,requestId:'stale-charter',charterVersion:1},true);
    await call('team_checkpoint',{requestId:'finish-checkpoint',state:'working',checkpoint:{charterVersion:2,verification:'All conditions covered by independent review'}});
    const final={requestId:'finish',summary:'Feature verified',evidence:[`commit:${base}`],acceptanceResults:[{criterion:'Independent review',outcome:'passed',evidence:`review:${completedReview.sessionId}:${completedReview.resultVersion}`}]};
    assert.equal((await call('team_finish_domain',final)).completed,true);
    assert.equal((await tool('team_get_project',{projectId:pid})).status,'completed');
    assert.equal((await call('team_finish_domain',final)).completed,true,'lost response replay remains valid after completion');
    await tool('team_update_charter',{projectId:pid,requestId:'charter-3',expectedVersion:2,charter:amended,changeSummary:'Reopen for final confirmation'});
    await tool('team_wait_decisions',{projectId:pid,afterCursor:'foreign:1',timeoutMs:0},true);
    await tool('team_control_project',{projectId:pid,requestId:'pause',action:'pause'});
    await call('team_delegate',{...task,requestId:'while-paused'},true);
    assert.equal((await tool('team_get_queue',{queueId:queue})).project.projectId,pid);
    const emptyQueue=(await tool('cluster_create_queue',{name:'Selected team queue',requestId:'selected-queue'})).queueId;
    assert.equal((await tool('team_get_queue',{queueId:emptyQueue})).mode,'direct');
    const bound=await tool('team_start_project',{...input,requestId:'bound-project',queueId:emptyQueue});
    assert.equal(bound.queueId,emptyQueue);
    assert.equal((await tool('team_get_queue',{queueId:emptyQueue})).project.projectId,bound.projectId);
    await tool('team_start_project',{...input,requestId:'duplicate-queue',queueId:emptyQueue},true);
    await tool('team_start_project',{...input,requestId:'occupied-queue',queueId:queue},true);
    await tool('team_control_project',{projectId:bound.projectId,requestId:'pause-bound',action:'pause'});
    const historyQueue=(await tool('cluster_create_queue',{name:'Historical work',requestId:'historical-queue'})).queueId;
    const original=await tool('cluster_start_session',{queueId:historyQueue,requestId:'original-history',title:'Original implementation',batch:'history',role:'developer',workspace:repo,task:'Historical candidate',baseSha:base,poolId:pools.dev});
    await endJob({sessionId:original.id});
    await until(async()=>{const r=await tool('cluster_get_session_status',{queueId:historyQueue,sessionId:original.id});return r.terminal&&r.resultVersion;},'historical candidate');
    const originalItem=await tool('cluster_get_work_item',{queueId:historyQueue,workItemId:original.workItemId});
    const defect=await tool('cluster_update_work_item',{queueId:historyQueue,workItemId:original.workItemId,requestId:'original-defect',expectedRevision:originalItem.revision,action:'add_issue',details:{message:'Historical regression',acceptance:'Regression must pass',candidateSha:base}});
    const unrelated=await tool('cluster_create_work_item',{queueId:historyQueue,requestId:'unrelated',title:'Outside current scope'});
    const cancelledHistory=await tool('cluster_create_work_item',{queueId:historyQueue,requestId:'cancelled-history',title:'Cancelled historical feature'});
    await tool('cluster_update_work_item',{queueId:historyQueue,workItemId:cancelledHistory.workItemId,requestId:'cancel-history',expectedRevision:cancelledHistory.revision,action:'cancel',details:{notes:'Already cancelled before team takeover'}});
    for(let i=0;i<3;i++) {
        const old=await tool('cluster_start_session',{queueId:historyQueue,requestId:`backlog-${i}`,title:`Archived backlog ${i}`,batch:'backlog',role:'developer',workspace:repo,task:'Old cancelled task',baseSha:base,poolId:pools.dev});
        await endJob({sessionId:old.id});
        await until(async()=>{const r=await tool('cluster_get_session_status',{queueId:historyQueue,sessionId:old.id});return r.terminal;},'archived backlog completion');
        const item=await tool('cluster_get_work_item',{queueId:historyQueue,workItemId:old.workItemId});
        await tool('cluster_update_work_item',{queueId:historyQueue,workItemId:item.workItemId,requestId:`archive-cancel-${i}`,expectedRevision:item.revision,action:'cancel',details:{notes:'Historical cancellation requires handoff reconciliation, not redevelopment'}});
    }
    const takeover=await tool('team_start_project',{...input,queueId:historyQueue,requestId:'history-takeover',domains:[{name:'History',mission:'Resume original regression',acceptance:'Original issue verified',dependencies:[]},{name:'Later',mission:'Dependent follow-up',acceptance:'Follow-up verified',dependencies:['History']}]});
    const historicalState=(await disk()).projects[takeover.projectId],historyDomain=Object.values(historicalState.domains).find(d=>d.name==='History'),later=Object.values(historicalState.domains).find(d=>d.name==='Later');
    const historyAuth={projectId:takeover.projectId,domainId:historyDomain.domainId,token:historyDomain.token,charterVersion:1};
    const historyCall=(name,args={},fail=false)=>tool(name,{...historyAuth,...args},fail);
    const inventoryHistory=await historyCall('team_domain_history');assert.equal(inventoryHistory.items.length,2);assert.equal(inventoryHistory.counts.total,6);
    const claimed=await historyCall('team_claim_work_item',{workItemId:original.workItemId,requestId:'claim-original'});assert.equal(claimed.importedSessionCount,1);
    assert.equal((await historyCall('team_claim_work_item',{workItemId:original.workItemId,requestId:'claim-original'})).importedSessionCount,1);
    await tool('team_claim_work_item',{projectId:takeover.projectId,domainId:later.domainId,token:later.token,workItemId:original.workItemId,requestId:'wrong-owner'},true);
    await historyCall('team_claim_work_item',{workItemId:cancelledHistory.workItemId,requestId:'do-not-restart-cancelled'},true);
    await historyCall('team_claim_work_item',{workItemId:original.workItemId,requestId:'cannot-skip-owned',disposition:'defer',reason:'Must not skip assigned implementation'},true);
    await historyCall('team_claim_work_item',{workItemId:unrelated.workItemId,requestId:'defer-unrelated',disposition:'defer',reason:'Outside the one-shot task scope'});
    const preserved=await historyCall('team_domain_read',{operation:'cluster_get_work_item',arguments:{workItemId:original.workItemId}});
    assert.equal(preserved.issues[defect.issueId].state,'open','takeover must preserve unresolved original defects');
    assert.equal((await historyCall('team_domain_context')).unclaimedHistoryCount,0);
    const historicalLead=await until(async()=>Object.values((await disk()).projects[takeover.projectId].jobs).find(j=>j.role==='lead'&&j.domainId===historyDomain.domainId&&j.sessionId),'history lead startup');
    assert.equal((await tool('team_get_project',{projectId:takeover.projectId})).domains.find(d=>d.name==='History').leadSessionId,historicalLead.sessionId,'principal can verify the actual lead session');
    const repair=await historyCall('team_delegate',{requestId:'repair-history',title:'Repair original regression',role:'repair',workItemId:original.workItemId,baseSha:base,task:'Fix original defect and return new candidate'});
    assert.equal(repair.workItemId,original.workItemId);
    assert.equal((await historyCall('team_backlog_status')).pending,4);
    await historyCall('team_checkpoint',{requestId:'cannot-abandon-backlog',state:'waiting',checkpoint:{next:'Only wait for one repair'}},true);
    let handoff=await historyCall('team_next_handoff');
    await tool('team_next_handoff',{projectId:takeover.projectId,domainId:later.domainId,token:later.token},true);
    const disposition=(entry,index)=>entry.workItemId===original.workItemId?{claimToken:entry.claimToken,requestId:`handoff-${index}`,outcome:'linked',jobId:repair.jobId,notes:'Existing repair owns the unresolved regression; continue other backlog'}:
        {claimToken:entry.claimToken,requestId:`handoff-${index}`,outcome:'confirm',notes:'Checked the recorded historical cancellation; no redevelopment',evidence:[`cancel-record:${entry.workItemId}`]};
    const firstDisposition=disposition(handoff,0);
    await historyCall('team_next_handoff',{...firstDisposition,requestId:'wrong-claim-token',claimToken:'wrong-token'},true);
    if(handoff.workItemId===original.workItemId) await historyCall('team_next_handoff',{requestId:'cannot-blind-confirm',claimToken:handoff.claimToken,outcome:'confirm',notes:'Must not pretend an open defect is done',evidence:['unverified']},true);
    handoff=await historyCall('team_next_handoff',firstDisposition);
    await historyCall('team_next_handoff',firstDisposition);
    assert.equal((await historyCall('team_backlog_status')).processed,1,'lost-response replay does not count or confirm twice');
    const earlyFinish=await rpc('tools/call',{name:'team_finish_domain',arguments:{...historyAuth,requestId:'cannot-finish-one',summary:'Only one handoff processed',evidence:['fixture'],acceptanceResults:[{criterion:'One item',outcome:'passed',evidence:'fixture'}]}});
    assert.equal(earlyFinish.isError,true);assert.match(earlyFinish.content[0].text,/未处置交接/);
    await historyCall('team_checkpoint',{requestId:'batch-continue',state:'continue',checkpoint:{next:'Continue remaining handoffs before waiting on the repair',processed:1}});
    await endJob(historicalLead);
    await close();({tool,rpc}=await connect());
    const continuation=await until(async()=>Object.values((await disk()).projects[takeover.projectId].jobs).find(j=>j.role==='lead'&&j.domainId===historyDomain.domainId&&j.jobId!==historicalLead.jobId&&j.sessionId),'backlog lead auto continuation');
    handoff=await historyCall('team_next_handoff');
    for(let index=1;!handoff.empty;index++) {
        assert.ok(index<8,'backlog processing must make progress');
        handoff=await historyCall('team_next_handoff',disposition(handoff,index));
    }
    assert.equal((await historyCall('team_backlog_status')).pending,0);
    assert.equal((await historyCall('team_backlog_status')).processed,4);
    await historyCall('team_checkpoint',{requestId:'wait-repair-history',state:'waiting',checkpoint:{next:'Review the repaired original work item'}});
    await endJob(continuation);
    const resumedRepair=await until(async()=>{const j=(await disk()).projects[takeover.projectId].jobs[repair.jobId];return j.sessionId&&j;},'lead delegates repair of historical item');
    assert.equal(resumedRepair.workItemId,original.workItemId);
    const unchanged=await tool('cluster_get_work_item',{queueId:historyQueue,workItemId:cancelledHistory.workItemId});assert.equal(unchanged.state,'cancelled');
    assert.equal((await tool('cluster_get_work_item',{queueId:historyQueue,workItemId:unrelated.workItemId})).state,'open');
    await tool('team_control_project',{projectId:takeover.projectId,requestId:'pause-history',action:'pause'});
    // One lead dispatches eleven independent tasks without yielding between
    // them; completion of the first starts review while the other ten run.
    settings=await tool('cluster_get_settings');
    for(const pool of Object.values(settings.pools)) settings=await tool('cluster_update_pool',{revision:settings.revision,id:pool.id,enabled:true,concurrency:16});
    const currentSettings=await tool('team_get_settings');
    await tool('team_update_settings',{...config,revision:currentSettings.revision,totalConcurrency:16,developerConcurrency:12,reviewerConcurrency:12});
    const parallelQueue=(await tool('cluster_create_queue',{name:'Parallel coordinator fixture',requestId:'parallel-queue'})).queueId;
    const isolated=await tool('cluster_create_work_item',{queueId:parallelQueue,requestId:'idle-owned-item',title:'Next independent repair'});
    const parallel=await tool('team_start_project',{...input,queueId:parallelQueue,requestId:'parallel-project'});
    const parallelState=(await disk()).projects[parallel.projectId],parallelDomain=Object.values(parallelState.domains)[0];
    const parallelAuth={projectId:parallel.projectId,domainId:parallelDomain.domainId,token:parallelDomain.token,charterVersion:1};
    const parallelCall=(name,args={},fail=false)=>tool(name,{...parallelAuth,...args},fail);
    const parallelLead=await until(async()=>Object.values((await disk()).projects[parallel.projectId].jobs).find(j=>j.role==='lead'&&j.sessionId),'parallel lead');
    const independent=[];
    for(let i=0;i<11;i++) independent.push(await parallelCall('team_delegate',{requestId:`independent-${i}`,title:`Independent ${i}`,role:'developer',task:`Single independent task ${i}; return its verified candidate`}));
    const eleven=await until(async()=>{const p=(await disk()).projects[parallel.projectId];const jobs=independent.map(j=>p.jobs[j.jobId]);return jobs.every(j=>j.sessionId&&j.state==='running')&&jobs;},'eleven simultaneous workers under one running lead');
    assert.equal((await disk()).projects[parallel.projectId].jobs[parallelLead.jobId].state,'running');
    const priorHead=(await parallelCall('team_next_action')).head;
    assert.equal(priorHead.kind,'work');
    await endJob(eleven[0]);
    await until(async()=>{const p=(await disk()).projects[parallel.projectId];return p.jobs[eleven[0].jobId].observedTerminal;},'first completion without batch barrier');
    const fifo=await until(async()=>{const q=await parallelCall('team_next_action');return q.items.some(i=>i.jobId===eleven[0].jobId)&&q;},'completed result enqueued');
    assert.equal(fifo.head.actionId,priorHead.actionId,'new result stays behind the existing queue head');
    assert.ok(fifo.items.findIndex(i=>i.jobId===eleven[0].jobId)>0);
    assert.equal(Object.keys((await disk()).projects[parallel.projectId].jobs[parallelLead.jobId].resultNotifications||{}).length,0,'results queue without steering the active coordinator');
    await close();({tool,rpc}=await connect());
    assert.equal((await parallelCall('team_next_action')).head.actionId,priorHead.actionId,'FIFO head survives process restart');
    const quickReview=await parallelCall('team_delegate',{requestId:'review-first-while-ten-running',title:'Review first candidate now',role:'reviewer',sourceJobId:eleven[0].jobId,baseSha:base,task:'Independently test first candidate; do not wait for the other ten'});
    await until(async()=>{const p=(await disk()).projects[parallel.projectId];return p.jobs[quickReview.jobId].sessionId;},'incremental review starts');
    const overlapped=(await disk()).projects[parallel.projectId];
    assert.equal(eleven.slice(1).filter(j=>overlapped.jobs[j.jobId].state==='running').length,10);
    await parallelCall('team_claim_work_item',{requestId:'own-idle',workItemId:isolated.workItemId});
    assert.ok((await parallelCall('team_domain_context')).readyWorkItems.some(i=>i.workItemId===isolated.workItemId));
    await parallelCall('team_checkpoint',{requestId:'cannot-wait-one-chain',state:'waiting',checkpoint:{next:'Wait for the first review'}},true);
    const smallBlock=await parallelCall('team_escalate',{requestId:'one-chain-block',jobId:eleven[1].jobId,question:'Fixture scope choice',reason:'Only this chain needs a choice',recommendation:'Preserve scope',alternatives:'Defer it',impact:'One chain',blockedScope:eleven[1].workItemId});
    assert.equal((await disk()).projects[parallel.projectId].domains[parallelDomain.domainId].state,'working','one decision does not park the domain');
    const nextRepair=await parallelCall('team_delegate',{requestId:'dispatch-next-despite-one-decision',title:'Repair next independent item',role:'repair',workItemId:isolated.workItemId,baseSha:base,task:'Repair only the next independent item'});
    await until(async()=>{const j=(await disk()).projects[parallel.projectId].jobs[nextRepair.jobId];return j.sessionId&&j.state==='running';},'next independent repair');
    await tool('team_update_charter',{projectId:parallel.projectId,requestId:'parallel-charter-ack',expectedVersion:1,charter,changeSummary:'Running coordinator consumes the updated charter'});
    parallelAuth.charterVersion=2;
    await parallelCall('team_checkpoint',{requestId:'ack-update-and-wait',state:'waiting',checkpoint:{charterVersion:2,next:'All independent work has an in-flight successor'}});
    await endJob(parallelLead);
    await until(async()=>{const p=(await disk()).projects[parallel.projectId];return p.jobs[parallelLead.jobId].observedTerminal;},'coordinator finishes validated waiting checkpoint');
    assert.equal(Object.values((await disk()).projects[parallel.projectId].jobs).filter(j=>j.role==='lead').length,1,'consumed charter wake must not create an empty coordinator successor');

    await tool('team_control_project',{projectId:parallel.projectId,requestId:'cancel-parallel-fixture',action:'cancel'});
    console.log('Coordinator concurrency verified: 11 independent workers plus lead, then 10 developers plus first reviewer; FIFO append without preemption and isolated waiting/decision gates passed.');
    // Compete for one developer slot across two running projects. Repeated
    // full-capacity probes and a process restart must not reset the rotation.
    await until(async()=>Object.values(JSON.parse(await readFile(join(data,'state.json'),'utf8')).sessions).every(j=>['succeeded','failed','cancelled','interrupted'].includes(j.status)),'previous fixture sessions cancelled');
    const fairSettings=await tool('team_get_settings');
    await tool('team_update_settings',{...config,revision:fairSettings.revision,totalConcurrency:3,developerConcurrency:1,reviewerConcurrency:1});
    const fairA=await tool('team_start_project',{...input,requestId:'fair-project-a'});
    const fairB=await tool('team_start_project',{...input,requestId:'fair-project-b'});
    await until(async()=>{const p=(await disk()).projects;return [fairA,fairB].every(f=>Object.values(p[f.projectId].jobs).some(j=>j.role==='lead'&&j.sessionId));},'two project leads');
    const fairCall=(projectId,name,args={})=>disk().then(state=>{
        const domain=Object.values(state.projects[projectId].domains)[0];
        return tool(name,{projectId,domainId:domain.domainId,token:domain.token,charterVersion:1,...args});
    });
    const fairTask=id=>({requestId:id,title:id,role:'developer',task:'One isolated fairness fixture task'});
    const a1=await fairCall(fairA.projectId,'team_delegate',fairTask('fair-a1'));
    const a1Running=await until(async()=>{const j=(await disk()).projects[fairA.projectId].jobs[a1.jobId];return j.sessionId&&j;},'first project developer');
    const a2=await fairCall(fairA.projectId,'team_delegate',fairTask('fair-a2'));
    await fairCall(fairA.projectId,'team_delegate',fairTask('fair-a3'));
    const b1=await fairCall(fairB.projectId,'team_delegate',fairTask('fair-b1'));
    await until(async()=>{const p=(await disk()).projects;return p[fairA.projectId].jobs[a2.jobId].error&&p[fairB.projectId].jobs[b1.jobId].error;},'both projects probe full developer capacity');
    assert.equal((await disk()).scheduler.lastProjects['3'],fairA.projectId,'failed capacity probes preserve the last served project');
    const fairKilled=once(child,'exit');child.kill('SIGKILL');await fairKilled;child=undefined;
    ({tool,rpc}=await connect());
    await endJob(a1Running);
    const b1Running=await until(async()=>{const j=(await disk()).projects[fairB.projectId].jobs[b1.jobId];return j.sessionId&&j;},'other project gets next developer slot after restart');
    assert.equal((await disk()).projects[fairA.projectId].jobs[a2.jobId].sessionId,undefined,'busy project must yield its next slot');
    await endJob(b1Running);
    const a2Running=await until(async()=>{const j=(await disk()).projects[fairA.projectId].jobs[a2.jobId];return j.sessionId&&j;},'rotation returns to first project');
    const a2Dsh=JSON.parse(await readFile(join(data,'state.json'),'utf8')).sessions[a2Running.sessionId].dshId;
    failedCancellations.add(a2Dsh);
    for(const f of [fairA,fairB])await tool('team_control_project',{projectId:f.projectId,requestId:'cancel-fair-'+f.projectId,action:'cancel'});
    await until(async()=>Object.values((await tool('team_get_project',{projectId:fairA.projectId})).runtimeErrors).some(e=>e.operation==='cancel_session'&&e.scope===a2Running.sessionId),'cancellation failure is visible');
    await endJob(a2Running);
    await until(async()=>!Object.values((await tool('team_get_project',{projectId:fairA.projectId})).runtimeErrors).some(e=>e.operation==='cancel_session'&&e.scope===a2Running.sessionId),'observed terminal state resolves prior cancellation failure');
    failedCancellations.delete(a2Dsh);
    console.log('Cross-project fairness passed with a full role cap, pending backlog and forced process restart.');
    await close();({tool,rpc}=await connect('--team-mcp'));
    const inventory=(await rpc('tools/list')).tools.map(t=>t.name);
    assert.ok(inventory.includes('team_start_project'));assert.ok(!inventory.some(n=>n.startsWith('cluster_')));assert.ok(!inventory.includes('team_delegate'));
    assert.ok(!inventory.includes('team_next_handoff'),'principal cannot consume the domain backlog directly');
    await tool('team_delegate',{...credentials,...task},true);
    assert.equal((await tool('team_get_project',{projectId:pid})).status,'paused');
    assert.equal((await tool('team_get_queue',{queueId:emptyQueue})).project.projectId,bound.projectId,'queue identity survives restart');
    const prompt=await tool('team_get_prompt',{role:'principal'});assert.ok(prompt.prompt.includes('不能'));assert.ok(prompt.prompt.length>1500);
    assert.equal(prompt.version,7);assert.ok(prompt.prompt.includes('用户只发一次任务'));
    await tool('team_control_project',{projectId:pid,requestId:'resume',action:'resume'});
    assert.equal((await tool('team_get_project',{projectId:pid})).status,'running');
    await tool('team_control_project',{projectId:pid,requestId:'cancel',action:'cancel'});
    await tool('team_control_project',{projectId:pid,requestId:'bad-resume',action:'resume'},true);
    console.log('Teams passed: durable backlog consumption across lead sessions and process restart, rejection of early waiting, linked in-flight repair without blocking other handoffs, receipt replay safety, historical ownership, role routing, review gates and principal-only tools.');
} catch(error) {
    try {console.error('TEAM_DIAGNOSTIC',JSON.stringify(Object.values((await disk()).projects).map(p=>({status:p.status,domains:Object.values(p.domains).map(d=>({state:d.state,wake:d.wake})),jobs:Object.values(p.jobs).map(j=>({role:j.role,state:j.state,error:j.error,sessionId:j.sessionId,attempts:j.attempts}))}))));} catch {}
    throw error;
} finally {
    await close();await new Promise(r=>server.close(r));
    await rm(root,{recursive:true,force:true,maxRetries:10,retryDelay:200});
}
