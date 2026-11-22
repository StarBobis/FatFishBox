import assert from 'node:assert/strict';
import {spawn} from 'node:child_process';
import {once} from 'node:events';
import {mkdtemp,readFile,writeFile,rm} from 'node:fs/promises';
import {createInterface} from 'node:readline';
import {tmpdir} from 'node:os';
import {join,resolve} from 'node:path';
const folder=await mkdtemp(join(tmpdir(),'fatfish-work-items-'));
const executable=resolve(process.argv[2]||'build/staged/FatFishBox.exe');
const sha='1'.repeat(40),integration='2'.repeat(40);
let child;
async function connect() {
    child=spawn(executable,['--mcp'],{env:{...process.env,FATFISHBOX_DATA_DIR:folder,FATFISHBOX_LEGACY_STATE_DIR:join(folder,'legacy')},windowsHide:true});
    const lines=createInterface({input:child.stdout})[Symbol.asyncIterator]();
    let sequence=0;
    const rpc=async(method,params={})=>{
        child.stdin.write(JSON.stringify({jsonrpc:'2.0',id:++sequence,method,params})+'\n');
        const line=await lines.next();assert.equal(line.done,false);
        const result=JSON.parse(line.value);assert.equal(result.error,undefined,JSON.stringify(result));return result.result;
    };
    await rpc('initialize');
    return async(name,args={},fail=false)=>{
        const response=await rpc('tools/call',{name,arguments:args});
        assert.equal(response.isError===true,fail,JSON.stringify(response));
        return response.structuredContent;
    };
}
async function close() {const ended=once(child,'exit');child.stdin.end();await ended;child=undefined;}
try {
    let tool=await connect();
    const queue=(await tool('cluster_create_queue',{name:'Ledger work',requestId:'ledger'})).queueId;
    const other=(await tool('cluster_create_queue',{name:'Other queue',requestId:'other'})).queueId;
    const invoke=(name,args={},fail=false)=>tool(name,{queueId:queue,...args},fail);
    const create={title:'One tracked feature',requestId:'feature',task:'Original immutable scope',baseSha:sha};
    const created=await invoke('cluster_create_work_item',create);
    assert.equal((await invoke('cluster_create_work_item',create)).workItemId,created.workItemId);
    await invoke('cluster_create_work_item',{...create,title:'Conflicting retry'},true);
    const get=()=>invoke('cluster_get_work_item',{workItemId:created.workItemId});
    let item=await get();assert.equal(item.task,undefined);
    assert.equal((await invoke('cluster_get_work_item',{workItemId:item.workItemId,includeTask:true})).task,create.task);
    assert.equal(JSON.parse(await readFile(created.taskRef,'utf8')).task,create.task);
    const update=async(action,details,requestId=action,fail=false,revision)=>{
        const current=await get();
        return invoke('cluster_update_work_item',{workItemId:item.workItemId,requestId,expectedRevision:revision??current.revision,action,details},fail);
    };
    const file=join(folder,'state.json');
    const seed=JSON.parse(await readFile(file,'utf8'));
    const session=(id,role,workItemId,extra={})=>({id,queueId:queue,workItemId,title:id,role,status:'succeeded',resultVersion:'1:8',
        created:1,updated:1,completed:2,processed:true,workspace:folder,...extra});
    seed.sessions.dev=session('dev','developer',item.workItemId);
    seed.sessions.review=session('review','reviewer',item.workItemId);
    seed.sessions.repair=session('repair','repair',undefined,{repairOf:{sessionId:'dev',resultVersion:'1:8'}});
    delete seed.sessions.repair.workItemId;
    await writeFile(file,JSON.stringify(seed));
    item=await get();
    assert.equal(item.sessions.repair.id,'repair','repair is grouped under its original task');
    let totals=(await invoke('cluster_list_work_items')).counts;
    assert.equal(totals.total,1);assert.equal(totals.remaining,1,'processed session does not mean integrated work');
    await update('accept',{candidateSha:sha,notes:'No review yet'},'premature');
    await update('record_review',{sessionId:'dev',resultVersion:'1:8',candidateSha:sha,verdict:'passed',notes:'Wrong role'},'wrong-review');
    const issue=await update('add_issue',{message:'Regression',acceptance:'Run the regression',candidateSha:sha});
    const replayRequest={workItemId:item.workItemId,requestId:'add_issue',expectedRevision:item.revision,action:'add_issue',
        details:{message:'Regression',acceptance:'Run the regression',candidateSha:sha}};
    // Reconstruct the exact original revision to exercise lost-response replay despite later revisions.
    replayRequest.expectedRevision=issue.revision-1;
    const replay=await invoke('cluster_update_work_item',replayRequest);
    assert.equal(JSON.parse(await readFile(issue.taskRef,'utf8')).issues[issue.issueId].message,'Regression');
    assert.equal(replay.issueId,issue.issueId);assert.equal(replay.replayed,true);
    await update('record_review',{sessionId:'review',resultVersion:'old',candidateSha:sha,verdict:'passed',notes:'Wrong version'},'wrong-version');
    await update('record_review',{sessionId:'review',resultVersion:'1:8',candidateSha:sha,verdict:'passed',notes:'Independent checks read'},'review-pass');
    await update('accept',{candidateSha:sha,notes:'Issue still open'},'open-issue');
    await update('resolve_issue',{issueId:issue.issueId,notes:'Regression fixed and checked'});
    await update('accept',{candidateSha:sha,notes:'Final acceptance complete'});
    totals=(await invoke('cluster_list_work_items')).counts;
    assert.equal(totals.remaining,1);assert.equal(totals.pendingIntegration,1);
    const superseded=JSON.parse(await readFile(file,'utf8'));
    superseded.sessions.review.resultVersion='1:9';
    await writeFile(file,JSON.stringify(superseded));
    assert.equal((await get()).state,'accepted','metadata changes do not revoke manager judgement');
    await update('record_review',{sessionId:'review',resultVersion:'1:9',candidateSha:sha,verdict:'passed',notes:'Updated independent review read'},'review-new');
    await update('accept',{candidateSha:sha,notes:'Updated result accepted'},'accept-new');
    await update('integrate',{integrationSha:integration,targetBranch:'main',notes:'Integration commit recorded'});
    totals=(await invoke('cluster_list_work_items')).counts;
    assert.equal(totals.remaining,0);assert.equal(totals.integrated,1);
    assert.equal((await get()).integration.integrationSha,integration);
    await tool('cluster_get_work_item',{queueId:other,workItemId:item.workItemId},true);
    await update('block',{notes:'Cannot mutate integrated work without reopening'},'integrated-mutation',true);
    await update('reopen',{notes:'New requirement in the same tracked scope'});
    item=await get();
    assert.equal(item.integration,undefined);assert.equal(item.history.at(-1).integration.integrationSha,integration);
    await update('block',{notes:'Stale write'},'stale',true,item.revision-1);
    const second=await invoke('cluster_create_work_item',{title:'Second item',requestId:'second'});
    const more=JSON.parse(await readFile(file,'utf8'));
    more.sessions.other=session('other','developer',second.workItemId);
    more.sessions.shared=session('shared','repair',item.workItemId,{repairLinks:[{sessionId:'dev',resultVersion:'1:8'},{sessionId:'other',resultVersion:'1:8'}]});
    await writeFile(file,JSON.stringify(more));
    const shared=await invoke('cluster_get_work_item',{workItemId:second.workItemId});
    assert.equal(shared.sessions.shared.id,'shared','one repair may cover multiple tracked items');
    assert.equal((await invoke('cluster_list_work_items',{limit:1})).nextOffset,1);
    const imported=(await tool('cluster_create_queue',{name:'Migration order',requestId:'migration-order'})).queueId;
    const ordered=JSON.parse(await readFile(file,'utf8'));
    ordered.sessions['a-old-repair']={...session('a-old-repair','repair',undefined),queueId:imported,
        repairOf:{sessionId:'z-old-root',resultVersion:'1:8'},dispatchOperation:{input:{task:'Repair delta only'}}};
    ordered.sessions['z-old-root']={...session('z-old-root','developer',undefined),queueId:imported,
        dispatchOperation:{input:{task:'Original scope from parent'}}};
    delete ordered.sessions['a-old-repair'].workItemId;delete ordered.sessions['z-old-root'].workItemId;
    await writeFile(file,JSON.stringify(ordered));
    await tool('cluster_list_work_items',{queueId:imported});
    const migratedState=JSON.parse(await readFile(file,'utf8'));
    const migratedId=migratedState.sessions['a-old-repair'].workItemId;
    assert.equal(migratedId,migratedState.sessions['z-old-root'].workItemId);
    const specification=await tool('cluster_get_work_item',{queueId:imported,workItemId:migratedId,includeTask:true});
    assert.equal(specification.task,'Original scope from parent','migration order must not replace original scope with repair delta');
    const savedId=item.workItemId;
    await close();tool=await connect();
    assert.equal((await invoke('cluster_get_work_item',{workItemId:savedId})).workItemId,savedId);
    assert.equal((await invoke('cluster_list_work_items')).counts.total,2);
    console.log('Work ledger passed: stable repair identity, manager judgement without proof gates, issue lifecycle, revision/replay safety, integration counts, shared repairs and restart.');
} finally {
    if (child) {const ended=once(child,'exit');child.stdin.end();await ended;}
    await rm(folder,{recursive:true,force:true});
}
