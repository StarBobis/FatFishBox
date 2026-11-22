import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createInterface } from 'node:readline';

const root = await mkdtemp(join(tmpdir(), 'fatfish-monitor-'));
const legacy = join(root, 'codex');
const state = join(root, 'fatfish');
const registry = join(legacy, 'registry');
const workers = join(registry, 'workers.json');
const managed = join(legacy, 'services', 'svc-fixture');
const auth = join(root, 'attached.json');
const configs = join(root, 'profiles');
const config = join(configs, 'workers.json');
const home=join(root,'home'), discoveredConfigs=join(home,'.deepseek-workers');
const localAppData=join(root,'localappdata');
const put = (path, value) => writeFile(path, JSON.stringify(value));
let active = true;
let resumed = false;
let offline = false;
let paged = 0;
const day = new Date().setHours(0,0,0,0);
const usageSession = id => id === 'native-usage' || id === 'legacy-usage';
const fixture = async (isManaged) => {
    const server = createServer(async (request, response) => {
        let raw = '';
        for await (const chunk of request) raw += chunk;
        const { method, payload } = JSON.parse(raw);
        const args = payload.args.request;
        let value;
        if (method === 'permissionPresets/catalog') {
            if (request.headers.cookie==='fixture=expired') { response.writeHead(401).end(); return; }
            value = {defaultOptions:[]};
        } else if (method === 'session/modelCatalog') {
            value={groups:[{id:'provider',models:isManaged?[{id:'managed'},{id:'other'}]:[{id:'desktop'}]}]};
        } else if (method === 'session/state') {
            if ((offline && isManaged) || args.sessionId === 'failed-native') {
                response.writeHead(503).end();
                return;
            }
            const running = isManaged ? resumed : active;
            value = {running, cursor:isManaged ? (resumed ? 19 : 10) : (active ? 3 : 12),
                queue:args.sessionId==='successful-native'?[{placement:'queued'}]:[], pendingInput:[]};
            if (usageSession(args.sessionId) || args.sessionId==='native-turn-failed' || args.sessionId==='native-http-failed')
                value={running:false,cursor:8,queue:[],pendingInput:[]};
            if (args.sessionId==='old-dispatch' || args.sessionId==='retrying-dispatch')
                value={running:false,cursor:1,queue:[{placement:'context'}],pendingInput:[]};
        } else if (method === 'session/page') {
            const event = (seq,type,data={},time=1690000000123) => ({type:'event',event:{seq,type,data,time}});
            if (args.address.sessionId==='native-http-failed') {
                value={records:[event(8,'turn/end',{reason:{kind:'error',error:{code:'http_502',message:'Gateway failure'}}})]};
            } else if (args.address.sessionId==='native-turn-failed') {
                value={records:[event(8,'turn/end',{reason:{kind:'error',
                    error:{code:'FIXTURE_FAILURE',message:'Execution failed'}}})]};
            } else if (usageSession(args.address.sessionId)) {
                const sample=(seq,values,time)=>event(seq,'assistant/message',{turn:1,step:0,
                    usage:{inputTokens:values[0],outputTokens:values[1],cacheReadTokens:values[2],cacheWriteTokens:values[3]}},time);
                if (args.maxMessages>4 && args.beforeSeq===undefined) {
                    response.end(JSON.stringify({result:{ok:false,error:{code:'response_too_large'}}}));
                    return;
                }
                value={records:[sample(7,[10,20,30,40],day),event(8,'turn/end',{reason:{kind:'completed'}},day+1),
                    sample(4,[1000,2000,3000,4000],day-1),sample(6,[5,10,20,30],day)]};
            } else if (!isManaged && args.beforeSeq === undefined) {
                value = {records:[event(11,'system/note'),event(12,'system/note')],hasMore:true};
            } else {
                if (!isManaged) paged++;
                value = {records:[event(isManaged ? 9 : 8,'turn/end',{reason:{kind:'completed'}})]};
            }
        } else if (method === 'session/projections') {
            value = {values:{tokenUsage:usageSession(args.sessionId)?
                {uncachedInputTokens:1010,outputTokens:2020,cacheReadTokens:3030,cacheWriteTokens:4040}:
                {uncachedInputTokens:1,outputTokens:2,cacheReadTokens:3,cacheWriteTokens:4}}};
        } else {
            response.writeHead(404).end();
            return;
        }
        response.end(JSON.stringify({result:{ok:true,value}}));
    });
    server.listen(0,'127.0.0.1');
    await once(server,'listening');
    return {server, origin:`http://127.0.0.1:${server.address().port}`};
};
const attached = await fixture(false);
const secondary = await fixture(true);
let child;
try {
    await Promise.all([registry, state, managed, configs, discoveredConfigs].map(path => mkdir(path, {recursive:true})));
    await put(auth,{origin:attached.origin,cookie:'fixture=1',connectionId:'test'});
    await put(join(managed,'auth-private.json'),{origin:secondary.origin,cookie:'fixture=2',runtimeVersion:'0.2.2-rc.5'});
    await put(config,{stateDir:'../codex',service:{attachAuthFile:'../attached.json'}});
    const legacyRows = {
        a:{sessionId:'same-id',serviceId:'attached',serviceOrigin:attached.origin,startSeq:1,cursor:3,status:'running',updatedAt:12,
            provider:'account-a',reasoningEffort:'low',actualReasoningEfforts:['high','max']},
        b:{sessionId:'same-id',serviceId:'svc-fixture',serviceOrigin:secondary.origin,startSeq:1,cursor:10,status:'succeeded',updatedAt:11},
    };
    await put(workers,{sessions:legacyRows});
    const processed = JSON.stringify([secondary.origin,'same-id']).replace(',', ', ');
    await put(join(registry,'monitor-review.json'),{[processed]:{version:'1:9',read:true,processed:true}});
    const file = join(state,'state.json');
    const native = (id,dshId) => ({id,dshId,startCursor:1,cursor:3,status:'running',updated:1,created:Date.now(),
        owner:attached.origin+'#test',resultVersion:'',read:false,processed:false});
    await put(file,{settings:{revision:0,configPath:config,authPath:auth,accounts:{},pools:{}},
        sessions:{first:native('first','failed-native'),second:native('second','successful-native')}});

    child = spawn(resolve(process.argv[2]),[],{env:{...process.env,FATFISHBOX_DATA_DIR:state,
        FATFISHBOX_LEGACY_STATE_DIR:'',DSH_WORKERS_STATE:join(root,'unused'),DSH_WORKERS_CONFIG:config,
        FATFISHBOX_DSH_SESSIONS_DIR:join(root,'history'),USERPROFILE:home,
        LOCALAPPDATA:localAppData,DSH_HOME:join(home,'.dsh')},
        windowsHide:true,stdio:['pipe','pipe','inherit']});
    const lines = createInterface({input:child.stdout})[Symbol.asyncIterator]();
    const snapshot = async (command='', shouldFail=false) => {
        child.stdin.write(command+'\n');
        const {value,done} = await lines.next();
        assert.equal(done,false);
        const result = JSON.parse(value);
        assert.equal(typeof result.error==='string',shouldFail,result.error);
        return result;
    };
    const initial = await snapshot();
    assert.equal(initial.overview.codexSessions,2);
    assert.equal(initial.running.filter(row => row.source === 'codex').length,1);
    const observed=initial.running.find(row => row.source === 'codex');
    assert.equal(observed.provider,'account-a');
    assert.equal(observed.effort,'max');
    assert.equal(initial.pending.length,0,'the turn-end receipt must match even when the registry cursor is newer');
    assert.equal(initial.liveStale,1,'a failing native session must be reported independently');

    const unchangedRegistry = await readFile(workers,'utf8');
    active = false;
    resumed = true;
    const changed = await snapshot();
    assert.equal((await readFile(workers,'utf8')),unchangedRegistry,'changes are reported before any registry write');
    const finished = changed.pending.find(row => row.source === 'codex');
    assert.equal(finished.resultVersion,'1:8');
    assert.equal(finished.completed,1690000000123);
    const reopened = changed.running.find(row => row.source === 'codex');
    assert.equal(reopened.status,'running');
    assert.equal(reopened.processed,false);
    assert.notEqual(finished.id,reopened.id);
    assert.equal(changed.pending.find(row => row.id==='second').status,'succeeded');
    assert.ok(paged>=2,'native and imported sessions must scan turn boundaries past the first page');

    offline = true;
    const stale = await snapshot();
    assert.equal(stale.running.find(row => row.id===reopened.id).stale,true);
    assert.equal(stale.liveStale,2);
    offline = false;
    const recovered = await snapshot();
    assert.equal(recovered.running.find(row => row.id===reopened.id).stale,false);
    assert.equal(recovered.liveStale,1);
    const stored=JSON.parse(await readFile(file,'utf8'));
    const link={sessionId:finished.id,resultVersion:finished.resultVersion};
    stored.sessions.second.repairOf=link;
    await put(file,stored);
    const queue=join(state,'handoff.json');
    const reference={parentId:finished.id,parentVersion:finished.resultVersion};
    const claim={...link,claimToken:'old-claim',expiresAt:Date.now()+60000};
    await put(queue,{claim});
    const claimed=(await snapshot()).pending.find(row=>row.id===finished.id);
    assert.equal(claimed.handoffClaimed,true);
    assert.equal(claimed.claimToken,undefined);
    await put(queue,{claim:{...claim,expiresAt:Date.now()-1}});
    assert.equal((await snapshot()).pending.find(row=>row.id===finished.id).handoffClaimed,undefined);
    await put(queue,{repair:{...reference,sessionId:'second'},claim:{...link,claimToken:'old-claim',expiresAt:Date.now()+60000}});
    const migrated=await snapshot('migrate');
    assert.equal(migrated.pending[0].id,'second');
    assert.deepEqual(migrated.pending[0].repairOf,link);
    assert.equal(migrated.pending.some(row=>row.id===finished.id),false);
    const inbox=JSON.parse(await readFile(queue,'utf8'));
    assert.equal(inbox.claim,undefined);
    assert.equal(inbox.repair,undefined);
    const oldDispatch=JSON.parse(await readFile(file,'utf8'));
    oldDispatch.sessions.second={...native('second','old-dispatch'),cursor:1,status:'unknown',dispatchState:'unconfirmed',
        dispatch:{stage:'submit_prompt',mayHaveDispatched:true},repairOf:link,repairQueued:true,precheck:{}};
    oldDispatch.sessions.retry={...oldDispatch.sessions.second,id:'retry',dshId:'retrying-dispatch',repairQueued:false,
        pendingPrompt:{retryAt:Date.now()+60000},messageOutbox:{
            neverSent:{state:'pending',retryAt:Date.now()+60000},sent:{state:'unconfirmed',sentAt:1,retryAt:Date.now()+60000}}};
    await put(file,oldDispatch);
    const archived=await snapshot();
    assert.equal(archived.running.some(row=>row.id==='second'),false);
    const failure=archived.pending.find(row=>row.id==='second');
    assert.equal(failure.status,'failed');
    assert.equal(failure.resultVersion,'dispatch:failed');
    assert.equal(failure.failure.code,'unconfirmed_submission');
    assert.equal(archived.pending.some(row=>row.id===finished.id),true,'the failed dispatch no longer replaces its parent');
    assert.equal(archived.running.find(row=>row.id==='retry').dispatchState,'unconfirmed');
    const archivedAgain=(await snapshot()).pending.find(row=>row.id==='second');
    assert.equal(archivedAgain.completed,failure.completed);
    const startupState=JSON.parse(await readFile(file,'utf8'));
    const placeholder={...native('orphan','unused'),status:'starting',dispatchState:'starting',
        resultVersion:'',created:Date.now()-180000,updated:Date.now()-180000};
    delete placeholder.startCursor;
    delete placeholder.dshId;
    startupState.sessions.orphan=placeholder;
    startupState.sessions.fresh={...placeholder,id:'fresh',updated:Date.now()};
    await put(file,startupState);
    const startup=await snapshot();
    assert.equal(startup.pending.find(row=>row.id==='orphan').failure.code,'startup_interrupted');
    assert.equal(startup.running.find(row=>row.id==='fresh').status,'starting');
    const manual=(await snapshot('fail:fresh')).session;
    assert.equal(manual.status,'failed');
    assert.equal(manual.failure.code,'manual_failure');
    assert.equal((await snapshot()).pending.find(row=>row.id==='fresh').resultVersion,'dispatch:failed');
    const cancelled=(await snapshot('fail:retry')).session;
    assert.equal(cancelled.status,'failed');
    const afterManual=JSON.parse(await readFile(file,'utf8')).sessions.retry;
    assert.equal(afterManual.pendingPrompt,undefined,'manual failure stops submission retries');
    assert.equal(afterManual.messageOutbox.neverSent.state,'not_sent');
    assert.equal(afterManual.messageOutbox.sent.state,'unconfirmed_terminal');
    assert.equal(afterManual.messageOutbox.sent.retryable,false);
    assert.equal(afterManual.messageOutbox.sent.retryAt,undefined);
    const manualAgain=(await snapshot()).pending.find(row=>row.id==='retry');
    assert.equal(manualAgain.failure.code,'manual_failure','refresh must not revive a manually failed session');
    assert.equal(manualAgain.completed,cancelled.completed);
    const dailyFile=JSON.parse(await readFile(file,'utf8'));
    dailyFile.sessions={
        today:{...native('today','native-usage'),created:day-60000,cursor:8,status:'succeeded',completed:day+1,resultVersion:'1:8'},
        yesterday:{...native('yesterday','yesterday'),created:day-60000,cursor:8,status:'succeeded',completed:day-1,resultVersion:'1:8',
            tokenDay:day-1,todayUsage:{uncachedInputTokens:55,outputTokens:55,cacheReadTokens:55,cacheWriteTokens:55}},
    };
    await put(file,dailyFile);
    await put(workers,{sessions:{today:{sessionId:'legacy-usage',serviceId:'attached',serviceOrigin:attached.origin,
        startSeq:1,cursor:8,status:'succeeded',updatedAt:(day+1)/1000,endedAt:(day+1)/1000,submittedAt:(day-60000)/1000}}});
    const daily=(await snapshot('usage')).overview.usage;
    assert.deepEqual(daily.today,{uncachedInputTokens:20,outputTokens:40,cacheReadTokens:60,cacheWriteTokens:80});
    assert.equal(daily.todayTokens,200);
    assert.equal(daily.total.cacheReadTokens,6060);
    assert.equal(daily.todaySampled,2);
    assert.equal(daily.missing,0);
    const beforeDelete=JSON.parse(await readFile(file,'utf8'));
    const target='DE0E3185-3B6A-4A9B-AB35-37C5F40016FF';
    beforeDelete.queues[target]={queueId:target,name:'Deletion fixture',legacy:false,workspaces:[],updated:1};
    beforeDelete.desktopQueueId=target;
    beforeDelete.sessions.today.queueId=target;
    beforeDelete.legacyQueues={'codex:legacy-usage':target};
    await put(file,beforeDelete);
    const targetHandoff=join(state,`handoff-${target}.json`);
    const targetClaim={claim:{sessionId:'today',resultVersion:'1:8',claimToken:'fixture',expiresAt:Date.now()+60000}};
    await put(targetHandoff,targetClaim);
    assert.equal((await snapshot(`show:${target}`)).pending.length,2);
    assert.match((await snapshot(`delete:${beforeDelete.legacyQueueId}`,true)).error,/原有队列/);
    const afterDelete=await snapshot(`delete:${target}`);
    assert.equal(afterDelete.queues.some(queue=>queue.queueId===target),false);
    assert.equal(afterDelete.pending.some(row=>row.id==='today' || row.id==='codex:legacy-usage'),false);
    assert.match((await snapshot(`show:${target}`,true)).error,/找不到该队列/);
    const preserved=JSON.parse(await readFile(file,'utf8'));
    assert.equal(preserved.sessions.today.queueId,target);
    assert.equal(preserved.legacyQueues['codex:legacy-usage'],target);
    assert.equal(preserved.desktopQueueId,beforeDelete.legacyQueueId);
    assert.deepEqual(JSON.parse(await readFile(targetHandoff,'utf8')),targetClaim);

    const invoke=(tool,args={})=>snapshot('invoke:'+JSON.stringify({tool,args}));
    const managedConfig=join(discoveredConfigs,'managed-workers.json');
    const offlineConfig=join(discoveredConfigs,'offline-workers.json');
    const offlineAuth=join(root,'offline-auth.json');
    const managedAuth=join(managed,'auth-private.json');
    const marker=join(legacy,'services','current.json');
    await put(managedConfig,{stateDir:'../../codex'});
    await put(offlineConfig,{service:{attachAuthFile:offlineAuth}});
    await put(offlineAuth,{origin:attached.origin,cookie:'fixture=expired'});
    await put(marker,{serviceId:'svc-fixture'});
    const candidates=await invoke('cluster_detect_dsh_connection');
    assert.equal(candidates.filter(row=>row.configPath===config).length,1);
    assert.equal(candidates.find(row=>row.configPath===offlineConfig).reachable,false);
    const managedCandidate=candidates.find(row=>row.configPath===managedConfig);
    assert.equal(managedCandidate.reachable,true);
    assert.equal(managedCandidate.authPath,managedAuth);
    const desktopProfileDir=join(home,'.dsh','profiles','desktop');
    await mkdir(desktopProfileDir,{recursive:true});
    await put(join(desktopProfileDir,'package.json'),{name:'dsh-profile-desktop'});
    const desktopAuth=join(localAppData,'FatFishBox','dsh-desktop','auth-private.json');
    const pendingDesktop=(await invoke('cluster_detect_dsh_connection')).find(row=>row.desktopPlugin);
    assert.equal(pendingDesktop.authPath,desktopAuth);
    assert.equal(pendingDesktop.reachable,false);
    assert.equal(pendingDesktop.setupRequired,true);
    const extracted=await snapshot('extract-desktop-plugin');
    for (const file of ['package.json','index.cjs','cordis.patch.yml'])
        assert.deepEqual(await readFile(join(extracted.path,file)),await readFile(new URL('../dsh-desktop-plugin/'+file,import.meta.url)));
    await mkdir(join(desktopAuth,'..'),{recursive:true});
    await put(desktopAuth,{origin:attached.origin,cookie:'fixture=expired'});
    const expiredDesktop=(await invoke('cluster_detect_dsh_connection')).find(row=>row.desktopPlugin);
    assert.equal(expiredDesktop.reachable,false);
    assert.equal(expiredDesktop.setupRequired,true);
    await put(desktopAuth,{origin:attached.origin,cookie:'fixture=1',desktopHostPid:process.pid});
    const connectedDesktop=(await invoke('cluster_detect_dsh_connection')).find(row=>row.desktopPlugin);
    assert.equal(connectedDesktop.reachable,true);
    assert.equal(connectedDesktop.desktop,true);
    assert.equal(connectedDesktop.configPath,'');
    assert.equal((await invoke('cluster_test_dsh_connection',{configPath:'',authPath:desktopAuth})).models,1);
    const manage={profileId:'new',dshName:'Managed',configPath:managedConfig,authPath:managedAuth,
        automaticAuth:true,permission:'',preset:'',revision:preserved.settings.revision};
    const primary=await invoke('cluster_import_models',{authPath:auth,configPath:config});
    const managedProfile=await invoke('cluster_save_dsh_profile',{...manage,revision:primary.settings.revision});
    assert.deepEqual(managedProfile.pools,{});
    const list=await invoke('cluster_list_dsh_profiles');
    assert.equal(list.current,managedProfile.dshProfile);
    assert.equal(list.profiles[list.current].name,'Managed');
    assert.equal(list.profiles.default.authPath,auth);
    assert.equal((await invoke('cluster_test_dsh_connection')).models,2);
    assert.deepEqual(await invoke('cluster_check_dsh_version'),{version:'0.2.2-rc.5',supported:true});
    const rotated=join(legacy,'services','svc-rotated');
    await mkdir(rotated);
    await put(join(rotated,'auth-private.json'),{origin:attached.origin,cookie:'fixture=1',connectionId:'rotated',
        runtimeVersion:'0.2.0-rc.2'});
    await put(marker,{serviceId:'svc-rotated'});
    assert.equal((await invoke('cluster_test_dsh_connection')).models,1,'managed profile follows the current marker');
    assert.deepEqual(await invoke('cluster_check_dsh_version'),{version:'0.2.0-rc.2',supported:false});
    const withOriginal=JSON.parse(await readFile(file,'utf8'));
    withOriginal.sessions.original={...native('original','native-usage'),status:'succeeded',resultVersion:'1:8'};
    await put(file,withOriginal);
    assert.equal((await invoke('cluster_refresh_session',{sessionId:'original'})).resultVersion,'1:8',
        'old sessions still use the original authorization after switching');
    const restored=await invoke('cluster_save_dsh_profile',{...manage,profileId:'default',revision:managedProfile.revision,
        dshName:'Desktop',configPath:config,authPath:auth,automaticAuth:false});
    assert.deepEqual(restored.pools,primary.settings.pools);

    const httpQueue=await invoke('cluster_create_queue',{name:'Runtime HTTP errors',requestId:'runtime-http'});
    const runtimeState=JSON.parse(await readFile(file,'utf8'));
    runtimeState.sessions.httpFailure={...native('httpFailure','native-http-failed'),queueId:httpQueue.queueId};
    await put(file,runtimeState);
    const runtimeStatus=await invoke('cluster_get_session_status',{queueId:httpQueue.queueId,sessionId:'httpFailure'});
    assert.equal(runtimeStatus.status,'failed');
    assert.equal(runtimeStatus.retryable,true);
    assert.equal(runtimeStatus.retryAfterMs,2000);
    assert.equal(runtimeStatus.retryMode,'new_request');
    assert.equal(runtimeStatus.failureStage,'execution');
    const runtimeClaim=await invoke('cluster_next_handoff',{queueId:httpQueue.queueId});
    assert.equal(runtimeClaim.failure.httpStatus,502);
    assert.equal(runtimeClaim.failure.retryable,true);
    assert.equal(runtimeClaim.failure.stage,'execution');
    const failures=await invoke('cluster_create_queue',{name:'Failed handoff',requestId:'failed-handoff-test'});
    const failureState=JSON.parse(await readFile(file,'utf8'));
    failureState.sessions.archivedFailure={...native('archivedFailure','unavailable'),queueId:failures.queueId,
        status:'failed',resultVersion:'dispatch:failed',completed:1,owner:'offline',
        precheck:{},dispatch:{reason:'unconfirmed_submission'},error:'Submission was never confirmed'};
    failureState.sessions.turnFailure={...native('turnFailure','native-turn-failed'),queueId:failures.queueId,
        status:'failed',resultVersion:'1:8',completed:2,
        precheck:{},failure:{kind:'error',code:'FIXTURE_FAILURE',message:'Execution failed'}};
    await put(file,failureState);
    const queueInvoke=(tool,args={})=>invoke(tool,{queueId:failures.queueId,...args});
    const failedClaim=await queueInvoke('cluster_next_handoff');
    assert.equal(failedClaim.session.id,'archivedFailure');
    assert.equal(failedClaim.failure.message,'Submission was never confirmed');
    assert.equal((await queueInvoke('cluster_next_handoff',{claimToken:failedClaim.claimToken})).session.id,'archivedFailure');
    const acknowledged=await queueInvoke('cluster_next_handoff',{claimToken:failedClaim.claimToken,
        outcome:'Failure investigated',claimNext:false});
    assert.equal(acknowledged.claimReleased,true);
    const failedTurn=await queueInvoke('cluster_next_handoff');
    assert.equal(failedTurn.session.id,'turnFailure');
    assert.equal(failedTurn.failure.code,'FIXTURE_FAILURE');
    assert.deepEqual(failedTurn.result,[]);

    // A registry cursor may include events after the failed turn. The receipt
    // belongs to the turn-end sequence, including when its original Host is gone.
    const offlineId='session-legacy-terminal-failure';
    const historyFolder=join(root,'history','_no-cwd',offlineId);
    await mkdir(historyFolder,{recursive:true});
    const offlineRecord={sessionId:offlineId,serviceId:'svc-retired',serviceOrigin:secondary.origin,startSeq:0,
        cursor:3,status:'failed',updatedAt:23,failureKind:'error',error:{code:'QUOTA'}};
    const offlineLog=[{type:'session',version:4,id:offlineId},{seq:0,type:'session/created'},
        {seq:1,type:'turn/start'},{seq:2,type:'turn/end',time:20000,data:{reason:{kind:'error',error:{code:'QUOTA'}}}},
        {seq:3,type:'system/note'}];
    const historyPath=join(historyFolder,'session.v4.jsonl');
    await writeFile(historyPath,offlineLog.map(row=>JSON.stringify(row)).join('\n')+'\n');
    await put(workers,{sessions:{offlineRecord}});
    const offlineKey='codex:'+offlineId;
    const pendingFailure=(await snapshot()).pending.find(row=>row.id===offlineKey);
    assert.equal(pendingFailure.resultVersion,'0:2');
    assert.equal(pendingFailure.stale,false);
    await put(join(registry,'monitor-review.json'),{
        [JSON.stringify([secondary.origin,offlineId])]:{version:'0:2',read:true,processed:true},
    });
    const confirmedFailure=await snapshot();
    assert.equal(confirmedFailure.pending.some(row=>row.id===offlineKey),false);
    assert.equal(confirmedFailure.processed.find(row=>row.id===offlineKey).resultVersion,'0:2');
    offlineLog.push({seq:4,type:'turn/start'},
        {seq:5,type:'turn/end',time:24000,data:{reason:{kind:'error',error:{code:'NEW_FAILURE'}}}});
    await writeFile(historyPath,offlineLog.map(row=>JSON.stringify(row)).join('\n')+'\n');
    const newestFailure=(await snapshot()).pending.find(row=>row.id===offlineKey);
    assert.equal(newestFailure.resultVersion,'0:5');
    assert.equal(newestFailure.failure.code,'NEW_FAILURE');
    child.stdin.end();
    assert.equal((await once(child,'exit'))[0],0);
} finally {
    if (child && child.exitCode===null) child.kill();
    await Promise.all([attached, secondary].map(({server}) => new Promise(done => server.close(done))));
    await rm(root,{recursive:true,force:true,maxRetries:4,retryDelay:100});
}
