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
const goals=new Map(),reports=new Map(),goalModes=new Map(),goalReads=new Map(),goalWrites=new Map(),bootstraps=new Map();
let nextGoalMode="delayed";
const states=new Map(),prompts=new Map(),selected=new Map(),unavailableSessions=new Set(),failedCancellations=new Set();let number=0,child,candidate;
const server=createServer(async(req,res)=>{
    try {
        let body='';for await(const part of req) body+=part;
        const {method,payload}=JSON.parse(body),a=payload.args.request??payload.args;let value;
        switch(method) {
        case 'permissionPresets/catalog':value={defaultOptions:[{value:'local',name:'Local'}]};break;
        case 'agentPresets/list':value={presets:[]};break;
        case 'session/modelCatalog':value={groups:[{id:'fixture',name:'Fixture',models:['lead','dev','review','integrate'].map(id=>({id,name:id}))}]};break;
        case 'session/create':value={sessionId:`team-session-${++number}`};states.set(value.sessionId,{cursor:1,running:false,queue:[],pendingInput:[]});break;
        case 'commands/execute':{
            if(a.line.startsWith('/goal ')) {
                if(a.line==='/goal pause') {const goal=goals.get(a.agentId);if(goal)goal.phase='paused';}
                else {
                    goalWrites.set(a.agentId,(goalWrites.get(a.agentId)||0)+1);
                    goalModes.set(a.agentId,nextGoalMode);nextGoalMode='normal';
                    const objective=a.line.slice(6).trim();
                    // Model the real race: /goal starts a round before the RPC returns.
                    // The first round must already know the manager role and full startup file.
                    assert.match(objective,/禁止自己修改产品代码或审计脚本/);
                    const file=objective.match(/启动文档：([^\r\n]+)/u)?.[1];assert.ok(file,'bootstrap path must be part of first goal context');
                    const startup=await readFile(file,'utf8');
                    assert.match(startup,/项目经理/);assert.match(startup,/team_delegate/);assert.match(startup,/凭据/);
                    assert.match(startup,/完整项目授权与任务计划/);assert.match(startup,/"tasks"/);
                    bootstraps.set(a.agentId,{file,content:startup});
                    goals.set(a.agentId,{id:'goal-'+a.agentId,revision:1,phase:'active',objective});
                    states.set(a.agentId,{cursor:2,running:true,queue:[],pendingInput:[]});
                }
            }
            value={result:{kind:'success'}};break;
        }
        case 'session/projections':{
            let goal=goals.get(a.sessionId)||null;
            if(goal) {
                const reads=(goalReads.get(a.sessionId)||0)+1;goalReads.set(a.sessionId,reads);
                const mode=goalModes.get(a.sessionId);
                if(mode==='delayed' && reads===1) {res.writeHead(503).end('projection not ready');return;}
                if((mode==='delayed' && reads<4)||mode==='missing') goal=null;
                else if(mode==='mismatch') goal={...goal,objective:'A different task'};
                else if(mode==='paused') goal={...goal,phase:'paused'};
            }
            // Real DSH 0.2.2-rc.5 wire shape, confirmed by a read-only live probe.
            value={kind:'session/projections',asOfSeq:7,values:{permissions:{currentValue:'local'},goal:{goal,roundsStarted:0,createdAt:1,updatedAt:1}}};break;
        }
        case 'session/selectModel':selected.set(a.sessionId,a.model);value={selected:a};break;
        case 'session/state':if(unavailableSessions.has(a.sessionId)){res.writeHead(503).end('fixture temporary failure');return;}value=states.get(a.sessionId);break;
        case 'session/prompt':assert.equal(goals.has(a.sessionId),false,'manager must never queue a second startup prompt');if(!prompts.has(a.sessionId)){prompts.set(a.sessionId,a.content[0].text);states.set(a.sessionId,{cursor:2,running:true,queue:[],pendingInput:[]});}value={accepted:true};break;
        case 'session/cancel':if(failedCancellations.has(a.sessionId)){res.writeHead(503).end('fixture cancellation failure');return;}states.set(a.sessionId,{cursor:7,running:false,queue:[],pendingInput:[]});value={};break;
        case 'session/page':value={records:[{type:'event',event:{seq:6,type:'assistant/message',data:{message:{content:[{text:'Fixture complete\n```fatfish-delivery\n'+JSON.stringify(reports.get(a.sessionId)||{candidateSha:candidate,status:'passed',worktreeClean:true,tests:[],evidencePaths:[]})+'\n```'}]}}}},
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
    await run('git',['init','-b','main',repo],{windowsHide:true});
    await writeFile(join(repo,'feature.txt'),'baseline\n');
    await run('git',['-C',repo,'add','.'],{windowsHide:true});
    const commit=async(work,msg)=>run('git',['-C',work,'-c','user.name=Fixture','-c','user.email=fixture@example.test','commit','-am',msg],{windowsHide:true});
    await commit(repo,'baseline');
    const base=(await run('git',['-C',repo,'rev-parse','HEAD'],{windowsHide:true})).stdout.trim();candidate=base;
    let {tool,rpc}=await connect();
    const auth=join(root,'auth.json');await writeFile(auth,JSON.stringify({origin:`http://127.0.0.1:${server.address().port}`,cookie:'fixture=1',connectionId:'fixture'}));
    let settings=(await tool('cluster_import_models',{authPath:auth,configPath:''})).settings;
    settings=await tool('cluster_configure',{revision:settings.revision,authPath:auth,permission:'local'});
    const pools=Object.fromEntries(Object.values(settings.pools).map(p=>[p.model,p.id]));
    for(const pool of Object.values(settings.pools))settings=await tool('cluster_update_pool',{revision:settings.revision,id:pool.id,enabled:true,concurrency:8});
    const initial=await tool('team_get_settings');
    const config={revision:initial.revision,leadPool:pools.lead,developerPool:pools.dev,reviewerPool:pools.review,totalConcurrency:4,developerConcurrency:2,reviewerConcurrency:2};
    await tool('team_update_settings',{...config,totalConcurrency:1},true);
    await tool('team_update_settings',config); // No integrator pool required.
    const charter={objective:'\u3000 Implement and independently verify the fixture \r\n',acceptance:'Review and actual branch tests',constraints:'Preserve data',architecture:'One manager',authority:'Local worktrees',escalation:'Real blocking failures'};
    const input={requestId:'loop-project',title:'Two-loop fixture',workspace:repo,charter,tasks:[{name:'Feature',steps:'Change file, review, run branch tests',acceptance:'Tests pass'}],domains:[{name:'Manager',mission:'Run the plan',acceptance:'Every task accepted',dependencies:[]}]};
    await tool('team_start_project',{...input,requestId:'multi',domains:[...input.domains,{...input.domains[0],name:'Other'}]},true);
    await tool('team_start_project',{...input,requestId:'over-plan',tasks:Array.from({length:4},()=>input.tasks[0])},true);
    const defaultQueue=(await tool('cluster_create_queue',{name:'Default team queue',requestId:'default-team'})).queueId;
    assert.equal((await tool('team_get_queue',{queueId:defaultQueue})).mode,'team','unbound queues default to team collaboration');
    const started=await tool('team_start_project',input),pid=started.projectId;
    assert.equal((await tool('team_start_project',input)).projectId,pid);
    const leader=await until(async()=>Object.values((await disk()).projects[pid].jobs).find(j=>j.role==='lead'&&j.sessionId&&j.state==='running'),'single manager');
    const native=async()=>JSON.parse(await readFile(join(data,'state.json'),'utf8'));
    const remote=async job=>(await native()).sessions[job.sessionId].dshId;
    assert.equal(goals.get(await remote(leader)).phase,'active','goal must be created through actual DSH command protocol');
    await close();
    const withHistory=await disk();
    withHistory.projects[pid].jobs['previous-failed-manager']={jobId:'previous-failed-manager',domainId:leader.domainId,role:'lead',state:'failed',created:0,observedTerminal:true,charterVersion:0,title:'Previous failed manager'};
    // Reproduce interruption after native activation but before team/session linkage.
    delete withHistory.projects[pid].jobs[leader.jobId].sessionId;
    delete withHistory.projects[pid].jobs[leader.jobId].workItemId;
    withHistory.projects[pid].jobs[leader.jobId].state='starting';
    await writeFile(join(data,'teams.json'),JSON.stringify(withHistory));
    ({tool,rpc}=await connect());
    await until(async()=>(await disk()).projects[pid].jobs[leader.jobId].sessionId===leader.sessionId,'recover original manager activation without replay');
    assert.equal(goalWrites.get(await remote(leader)),1,'cross-file recovery must not activate goal twice');
    assert.equal(states.get(await remote(leader)).queue.length,0,'recovery must not queue a manager prompt');
    const p=(await disk()).projects[pid],domain=Object.values(p.domains)[0];
    const authz={projectId:pid,domainId:domain.domainId,token:domain.token,charterVersion:1};
    const call=(name,args={},fail=false)=>tool(name,{...authz,...args},fail);
    assert.equal((await call('team_get_contract')).version,8);
    const listed=(await rpc('tools/list')).tools;
    const taskSchema=listed.find(t=>t.name==='team_start_project').inputSchema.properties.tasks.items;
    assert.equal(taskSchema.properties.steps.type,'string');
    assert.deepEqual(taskSchema.required,['name','steps','acceptance']);
    assert.equal(goalWrites.get(await remote(leader)),1,'projection retries never replay /goal creation');
    assert.ok(goalReads.get(await remote(leader))>=4,'delayed nested projection was polled');
    assert.ok(p.jobs[leader.jobId].goal.objective.endsWith(charter.objective.trim()),'user objective retained after bootstrap contract');
    assert.equal(prompts.has(await remote(leader)),false,'no unread manager startup prompt');
    assert.equal(states.get(await remote(leader)).queue.length,0);
    assert.equal((await native()).sessions[leader.sessionId].startCursor,1,'cursor captured before automatic goal round');
    assert.ok(!p.jobs[leader.jobId].goal.objective.includes(authz.token),'credentials stay out of goal projection');
    assert.ok(bootstraps.get(await remote(leader)).content.includes(authz.token),'first turn can read manager credentials');
    const waitSchema=listed.find(t=>t.name==='team_wait_decisions').inputSchema;
    assert.equal(waitSchema.properties.timeoutMs.maximum,60000);
    assert.equal(waitSchema.properties.timeoutMs.default,60000);
    const before=Date.now();assert.equal((await tool('team_wait_decisions',{projectId:pid,timeoutMs:50})).timedOut,true);assert.ok(Date.now()-before>=50);
    const leaderRemote=await remote(leader);
    states.set(leaderRemote,{cursor:2,running:false,queue:[],pendingInput:[]});
    assert.equal((await call('team_domain_read',{operation:'cluster_get_session_status',arguments:{sessionId:leader.sessionId}})).status,'running','active goal idle gap stays running');
    states.set(leaderRemote,{cursor:2,running:true,queue:[],pendingInput:[]});
    const first=await call('team_delegate',{requestId:'dev',taskName:'Feature',title:'Implement',role:'developer',task:'Return a candidate'});
    const running=await until(async()=>{const j=(await disk()).projects[pid].jobs[first.jobId];return j.sessionId&&j;},'developer');
    assert.ok(running.workspace!==repo);
    assert.match((await run('git',['-C',running.workspace,'branch','--show-current'])).stdout,/codex\/task-/);
    const finish={requestId:'finish',summary:'All tasks verified',evidence:['fixture'],acceptanceResults:[{criterion:'Independent review and actual test',outcome:'passed',evidence:'fixture'}]};
    await call('team_finish_domain',finish,true); // Empty result list but active developer.
    const end=async(job,report)=>{
        const sid=await remote(job);reports.set(sid,{candidateSha:candidate,status:'passed',worktreeClean:true,tests:[],evidencePaths:[],...report});
        states.set(sid,{cursor:7,running:false,queue:[],pendingInput:[]});
        return until(async()=>{const j=(await disk()).projects[pid].jobs[job.jobId];return j.observedTerminal&&j;},'terminal child');
    };
    const completed=await end(running);
    assert.equal(Object.values((await disk()).projects[pid].jobs).filter(j=>j.role==='lead').length,2,'child completion must not rebuild manager; historical failed round retained');
    assert.equal((await call('team_next_action')).head.reason,'candidate_needs_review');
    await call('team_delegate',{requestId:'integrator',role:'integrator',title:'Wrong',task:'Wrong'},true);
    const reviewArgs={title:'Adversarial review',role:'reviewer',sourceJobId:completed.jobId,baseSha:base,task:'Review exact candidate'};
    await call('team_delegate',{...reviewArgs,requestId:'wrong-sha',baseSha:'f'.repeat(40)},true);
    const review=await call('team_delegate',{...reviewArgs,requestId:'review1'});
    const reviewLive=await until(async()=>{const j=(await disk()).projects[pid].jobs[review.jobId];return j.sessionId&&j;},'first review');
    const failedReview=await end(reviewLive,{status:'failed'});
    const item=()=>call('team_domain_read',{operation:'cluster_get_work_item',arguments:{workItemId:running.workItemId}});
    const action=async(name,details,id,fail=false)=>call('team_domain_action',{operation:'cluster_update_work_item',arguments:{workItemId:running.workItemId,expectedRevision:(await item()).revision,requestId:id,action:name,details}},fail);
    await action('record_review',{sessionId:failedReview.sessionId,resultVersion:failedReview.resultVersion,candidateSha:base,verdict:'failed',notes:'Adversarial fixture failure'},'review-failed');
    await call('team_verify_candidate',{requestId:'before-review',workItemId:running.workItemId,candidateSha:base,executable:process.execPath,arguments:['-e','process.exit(0)']},true);
    const repair=await call('team_delegate',{requestId:'repair',title:'Repair review failure',role:'repair',sourceJobId:completed.jobId,baseSha:base,task:'Fix the exact issue'});
    const repairLive=await until(async()=>{const j=(await disk()).projects[pid].jobs[repair.jobId];return j.sessionId&&j;},'repair');
    await writeFile(join(repairLive.workspace,'feature.txt'),'repaired\n');await commit(repairLive.workspace,'repair');
    candidate=(await run('git',['-C',repairLive.workspace,'rev-parse','HEAD'])).stdout.trim();
    assert.notEqual(candidate,base);
    const repaired=await end(repairLive);
    const second=await call('team_delegate',{...reviewArgs,requestId:'review2',sourceJobId:repaired.jobId,baseSha:candidate});
    const secondLive=await until(async()=>{const j=(await disk()).projects[pid].jobs[second.jobId];return j.sessionId&&j;},'new independent review');
    assert.notEqual(secondLive.workspace,repairLive.workspace);
    const passedReview=await end(secondLive);
    await action('record_review',{sessionId:passedReview.sessionId,resultVersion:passedReview.resultVersion,candidateSha:candidate,verdict:'passed',notes:'Repaired candidate independently checked'},'review-passed');
    const verify={workItemId:running.workItemId,candidateSha:candidate,executable:process.execPath,arguments:['-e','process.exit(1)']};
    const failedTest=await call('team_verify_candidate',{...verify,requestId:'test-fail'});
    assert.equal(failedTest.passed,false);assert.equal(failedTest.exitCode,1);
    await action('accept',{candidateSha:candidate,verificationId:failedTest.verificationId,notes:'Cannot accept failed test'},'reject-failed-test',true);
    const goodInput={...verify,arguments:['-e',"require('node:assert/strict').equal(require('node:fs').readFileSync('feature.txt','utf8'),'repaired\\n');console.log('real branch test passed')"],requestId:'test-pass'};
    const verified=await call('team_verify_candidate',goodInput);
    assert.equal(verified.passed,true,JSON.stringify(verified));
    assert.match(await readFile(verified.logPath,'utf8'),/real branch test passed/);
    assert.equal((await call('team_verify_candidate',goodInput)).verificationId,verified.verificationId,'verification replay');
    await action('accept',{candidateSha:candidate,verificationId:verified.verificationId,notes:'Manager actual branch test passed'},'accept');
    assert.equal((await item()).state,'accepted');
    assert.equal((await call('team_priority_results')).count,0,'accepted requires no integration task');
    await call('team_checkpoint',{requestId:'checkpoint',state:'working',checkpoint:{verified:verified.verificationId}});
    assert.equal((await call('team_finish_domain',finish)).completed,true);
    assert.equal((await call('team_finish_domain',finish)).completed,true,'finish idempotent after completion');
    assert.equal((await tool('team_get_project',{projectId:pid})).status,'finishing','wait for manager to end goal');
    goals.get(await remote(leader)).phase='complete';states.set(await remote(leader),{cursor:7,running:false,queue:[],pendingInput:[]});
    await until(async()=>(await tool('team_get_project',{projectId:pid})).status==='completed','manager ends naturally');
    await tool('team_record_merge',{projectId:pid,requestId:'unmerged',workItemId:running.workItemId,integrationSha:base,targetBranch:'main',notes:'Not merged'},true);
    await run('git',['-C',repo,'merge','--ff-only',candidate]);
    await tool('team_record_merge',{projectId:pid,requestId:'merged',workItemId:running.workItemId,integrationSha:candidate,targetBranch:'main',notes:'Principal independently accepted and merged'});
    assert.equal((await item()).state,'integrated');
    await tool('team_record_merge',{projectId:pid,requestId:'merged',workItemId:running.workItemId,integrationSha:candidate,targetBranch:'main',notes:'Principal independently accepted and merged'});
    // A new full plan starts a new manager round after completion, not a new domain.
    goals.get(await remote(leader)).phase='complete';states.set(await remote(leader),{cursor:7,running:false,queue:[],pendingInput:[]});
    await until(async()=>(await disk()).projects[pid].jobs[leader.jobId].observedTerminal,'manager ends naturally');
    await tool('team_update_charter',{projectId:pid,requestId:'rework-plan',expectedVersion:1,charter:{...charter,objective:'Complete a new explicitly documented rework plan'},changeSummary:'Principal final acceptance requests rework'});
    authz.charterVersion=2;
    const nextManager=await until(async()=>Object.values((await disk()).projects[pid].jobs).find(j=>j.role==='lead'&&j.jobId!==leader.jobId&&j.sessionId&&j.state==='running'),'next full-plan manager round');
    assert.ok(goals.get(await remote(nextManager)).objective.endsWith('Complete a new explicitly documented rework plan'));
    // Restart transport: state and accepted source remain durable.
    await close();({tool,rpc}=await connect());
    assert.equal((await tool('team_get_project',{projectId:pid})).charterVersion,2);
    await tool('team_control_project',{projectId:pid,requestId:'cancel',action:'cancel'});
    await until(async()=>Object.values((await disk()).projects[pid].jobs).every(j=>['succeeded','failed','cancelled','interrupted'].includes(j.state)),'cancel all sessions');
    // A missing, wrong or paused goal cannot start a manager prompt.
    for(const mode of ['missing','mismatch','paused']) {
        nextGoalMode=mode;
        const failedProject=await tool('team_start_project',{...input,requestId:`goal-${mode}`});
        const failedManager=await until(async()=>Object.values((await disk()).projects[failedProject.projectId].jobs).find(j=>j.role==='lead'&&j.state==='failed'&&j.sessionId),`goal ${mode} bounded failure`);
        const failedNative=(await native()).sessions[failedManager.sessionId];
        assert.equal(failedNative.failureStage,'set_goal');
        assert.equal(failedNative.dispatch.goalCleanup.pauseConfirmed,true);
        assert.equal(failedNative.dispatch.goalCleanup.cancelConfirmed,true);
        assert.equal(goals.get(failedNative.dshId).phase,'paused','failed setup must not orphan an armed goal');
        assert.equal(prompts.has(failedNative.dshId),false,'unverified goal never receives task prompt');
        assert.equal(goalWrites.get(failedNative.dshId),1,'never duplicate goal command');
        assert.ok(goalReads.get(failedNative.dshId)>=20 && goalReads.get(failedNative.dshId)<40,'verification stops after bounded reads including background observation');
        await tool('team_control_project',{projectId:failedProject.projectId,requestId:`cancel-${mode}`,action:'cancel'});
    }
    console.log('Two-loop behavior passed: one real goal, isolated branches, failed review/repair/new review, actual failed/passed tests, acceptance without integrator, principal merge, durable rework and cancellation.');
} finally {
    await close();await new Promise(resolve=>server.close(resolve));await rm(root,{recursive:true,force:true});
}
