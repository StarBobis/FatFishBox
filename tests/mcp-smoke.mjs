import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createInterface } from 'node:readline';
import { zstdCompressSync } from 'node:zlib';

const executable = resolve(process.argv[2] || 'build/Release/FatFishBox.exe');
const root = await mkdtemp(join(tmpdir(), 'fatfish-smoke-'));
const historyRoot=join(root,'dsh-sessions');
let running = false;
let selected;
let prompts = 0;
let withholdPromptReply = false;
let terminal = false;
let offerMax = true;
let rejectProvider = '';
let hiddenProvider = '';
let catalogHttpFailure=0;
let configuredModels = false;
let deliveryReport = false;
let submittedPrompt = '';
let forcedState;
let uniqueSessionIds=false;
let reviewFixture=false;
let sessionNumber=0;
const fixedStates=new Map();
const reports=new Map();
const promptsBySession=new Map();
const promptRequests=[];
const turnReasons=new Map();
const completedTimes=new Map();
const sha='0123456789abcdef0123456789abcdef01234567';
const delivery=(path)=>'\n```fatfish-delivery\n'+JSON.stringify({candidateSha:sha,worktreeClean:true,
    tests:[{command:'ctest -R mask',exitCode:0}],evidencePaths:[path]})+'\n```';
const server = createServer(async (req,res) => {
    let raw = '';
    for await (const buffer of req) raw += buffer;
    const {method, payload} = JSON.parse(raw);
    const args = payload.args;
    let value;
    switch (method) {
    case 'permissionPresets/catalog':
        value = {defaultOptions:[{value:'prompt-on-risk',name:'Prompt on risk'}]};
        break;
    case 'agentPresets/list':
        value = {presets:[{id:'ptc',name:'PTC'}, {id:'broken',broken:'Unavailable'}]};
        break;
    case 'session/modelCatalog':
        if (catalogHttpFailure) {res.writeHead(catalogHttpFailure).end();return;}
        value = {groups:[
            {id:'account-a', name:'Account A', models:[
                {id:'fast',name:'Fast',reasoning:{defaultEffort:'high',efforts:[{id:'high'},...(offerMax?[{id:'max'}]:[])]}},
                {id:'light',name:'Light'},
            ]},
            {id:'account-b', name:'Account B', models:[{id:'fast',name:'Fast'}]},
        ].filter(group=>group.id!==hiddenProvider)};
        if (configuredModels)
            value.failures=[{id:'my-gateway',name:'My Gateway',message:'fixture catalog failure'}];
        break;
    case 'llm/listConfigurableProviders':
        if (!configuredModels) { res.writeHead(404).end(); return; }
        value=[{provider:'my-gateway',displayName:'My Gateway',settingsNs:'adapter',settingsPath:['providers','my-gateway']},
            {provider:'account-a',displayName:'Alternate name',settingsNs:'adapter',settingsPath:['providers','account-a']},
            {provider:'built-in',settingsNs:'adapter',settingsPath:['providers','missing']}];
        break;
    case 'settings/describe':
        value={namespaces:[{ns:'adapter',value:{providers:{
            'my-gateway':{models:[{id:'my-model',reasoning:true},{id:'my-model'}]},
            'account-a':{models:[{id:'fast',name:'Configured name'},{id:'other-model',name:'Other Model'}]},
        }}}]};
        break;
    case 'session/create':
        value = {sessionId:uniqueSessionIds?`repair-fixture-${++sessionNumber}`:'external-session'};
        if (reviewFixture) fixedStates.set(value.sessionId,{cursor:1,running:false,queue:[],pendingInput:[]});
        break;
    case 'commands/execute': value = {result:{kind:'success'}}; break;
    case 'session/projections': value = {values:{permissions:{currentValue:'danger-full-access'},
        tokenUsage:{uncachedInputTokens:100,outputTokens:20,cacheReadTokens:40,cacheWriteTokens:5}}}; break;
    case 'session/selectModel':
        if (args.request.provider===rejectProvider) {
            res.end(JSON.stringify({result:{ok:false,error:{code:'rate_limited'}}}));
            return;
        }
        selected = args.request; value = {selected}; break;
    case 'session/state':
        value=args.request.sessionId==='stale-slot' ? {cursor:7,running:false,queue:[],pendingInput:[]} :
            fixedStates.get(args.request.sessionId) ?? forcedState ?? {cursor:terminal?7:running?2:1,running,queue:[],pendingInput:[]};
        break;
    case 'session/prompt':
        prompts++;
        promptRequests.push(args.request);
        submittedPrompt=args.request.content[0].text;
        promptsBySession.set(args.request.sessionId,submittedPrompt);
        if (withholdPromptReply) return;
        running = true;
        if (reviewFixture || fixedStates.has(args.request.sessionId))
            fixedStates.set(args.request.sessionId,{cursor:2,running:true,queue:[],pendingInput:[]});
        value = {accepted:true};
        break;
    case 'session/page': value = {records:[
        ...(reviewFixture?[{type:'event',event:{seq:4,type:'user/message',data:{content:'Private input before final'}}}]:[]),
        {type:'event',event:{seq:6,type:'assistant/message',data:{message:{content:[
            {text:reports.get(args.request.address?.sessionId) ?? ('交接证据'+(deliveryReport?delivery(join(root,'repair.png')):''))}]}}}},
        {type:'event',event:{seq:7,type:'turn/end',time:completedTimes.get(args.request.address?.sessionId),
            data:{reason:turnReasons.get(args.request.address?.sessionId) ?? {kind:'completed'}}}},
    ]}; break;
    default: res.writeHead(404).end(); return;
    }
    res.setHeader('content-type','application/json');
    res.end(JSON.stringify({result:{ok:true,value}}));
});
await new Promise(resolve => server.listen(0,'127.0.0.1',resolve));
const auth = join(root,'auth.json');
const a = join(root,'workspace-a'), b = join(root,'workspace-b');
await Promise.all([mkdir(a),mkdir(b),writeFile(auth,JSON.stringify({
    origin:`http://127.0.0.1:${server.address().port}`, cookie:'test=1', connectionId:'private-fixture',
}))]);

const live = [];
const queueTools=new Set(['cluster_overview','cluster_get_session_status','cluster_get_session_result','cluster_start_session','cluster_next_handoff','cluster_resume_handoff','cluster_wait_handoff','cluster_create_work_item','cluster_get_work_item','cluster_list_work_items','cluster_update_work_item',
    'cluster_review_issues','cluster_retry_review','cluster_repair_handoff','cluster_attach_handoff_repair',
    'cluster_detach_handoff_repair','cluster_refresh_sessions','cluster_cancel_session']);
async function connect(directory) {
    const child = spawn(executable,['--mcp'],{env:{...process.env,FATFISHBOX_DATA_DIR:directory,
        FATFISHBOX_LEGACY_STATE_DIR:join(directory,'codex-state'),FATFISHBOX_DSH_SESSIONS_DIR:historyRoot},
        stdio:['pipe','pipe','inherit'],windowsHide:true});
    live.push(child);
    const stream = createInterface({input:child.stdout})[Symbol.asyncIterator]();
    let id=0;
    let queueId;
    const rpc = async (method,params={},raw=false) => {
        if (!raw && method==='tools/call' && queueTools.has(params.name))
            params={...params,arguments:{queueId,...params.arguments}};
        const request = ++id;
        child.stdin.write(JSON.stringify({jsonrpc:'2.0',id:request,method,params})+'\n');
        const line = await stream.next();
        assert.equal(line.done,false,`MCP exited during ${method}`);
        const response = JSON.parse(line.value);
        assert.equal(response.id,request);
        assert.equal(response.error,undefined,JSON.stringify(response.error));
        return response.result;
    };
    assert.equal((await rpc('initialize',{protocolVersion:'2025-11-25',capabilities:{},clientInfo:{name:'smoke',version:'1'}}))
        .serverInfo.title,'肥鱼重工');
    child.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n');
    const discovery=await rpc('tools/call',{name:'cluster_find_queues'});
    queueId=discovery.structuredContent.queues.find(queue=>queue.legacy).queueId;
    return {
        rpc,
        queueId,
        async close() {
            const exited=once(child,'exit');
            child.stdin.end();
            await exited;
        },
        async tool(name,arguments_={},shouldFail=false) {
            const response = await rpc('tools/call',{name,arguments:arguments_});
            assert.equal(response.isError===true,shouldFail,JSON.stringify(response.content));
            if (response.structuredContent!==undefined) {
                const payload=response.content[shouldFail ? 1 : 0];
                assert.equal(payload?.type,'text',name);
                assert.deepEqual(JSON.parse(payload.text),response.structuredContent,name);
            }
            return response.structuredContent;
        },
    };
}

try {
    const one = await connect(root);
    const two = await connect(root);
    const inventory = (await one.rpc('tools/list')).tools;
    assert(inventory.some(tool => tool.name==='cluster_refresh_sessions'));
    assert(!inventory.some(tool => ['cluster_list_sessions','cluster_get_session','cluster_read_result','cluster_mark_receipt','cluster_update_account'].includes(tool.name)));
    const presets=await one.tool('cluster_get_presets',{authPath:auth,configPath:''});
    assert.deepEqual(presets.permissions,[{id:'prompt-on-risk',label:'Prompt on risk'}]);
    assert.deepEqual(presets.agents,[{id:'ptc',label:'PTC'}]);
    catalogHttpFailure=502;
    const gateway=await one.tool('cluster_get_presets',{authPath:auth,configPath:''});
    // Model import makes a direct catalog request and must retain actionable transport diagnostics.
    const gatewayError=await one.tool('cluster_import_models',{authPath:auth,configPath:''},true);
    assert.equal(gatewayError.retryable,true);
    assert.equal(gatewayError.retryAfterMs,2000);
    assert.equal(gatewayError.stage,'session/modelCatalog');
    catalogHttpFailure=0;
    const initial = await one.tool('cluster_import_models',{authPath:auth,configPath:''});
    assert.equal(initial.accounts,2);
    assert.equal(initial.models,3);
    let settings = await two.tool('cluster_configure',{revision:initial.settings.revision,permission:'prompt-on-risk'});
    await one.tool('cluster_configure',{revision:initial.settings.revision,preset:'ptc'},true);
    const pool = Object.values(settings.pools).find(row => row.provider==='account-a' && row.model==='fast');
    await two.tool('cluster_update_pool',{revision:settings.revision,id:pool.id,concurrency:2},true);
    const editor=await connect(root);
    settings = await editor.tool('cluster_update_pool',{revision:settings.revision,id:pool.id,enabled:true,concurrency:1,effort:'max'});
    await editor.close();
    const reopened=await connect(root);
    assert.deepEqual((await reopened.tool('cluster_get_settings')).pools[pool.id],settings.pools[pool.id]);
    await reopened.close();
    offerMax=false;
    const replay = await one.tool('cluster_import_models',{authPath:auth,configPath:''});
    assert.equal(replay.settings.pools[pool.id].enabled,true);
    assert.equal(replay.settings.pools[pool.id].concurrency,1);
    assert.equal(replay.settings.pools[pool.id].effort,'max');
    assert.deepEqual(replay.settings.pools[pool.id].efforts,['high']);
    const configuration=join(root,'worker-config.json');
    await writeFile(configuration,JSON.stringify({service:{attachAuthFile:'auth.json'}}));
    configuredModels=true;
    const imported=await one.tool('cluster_import_models',{configPath:configuration});
    assert.equal(imported.accounts,3);
    assert.equal(imported.models,5);
    assert.equal(imported.failedProviders,1);
    assert.equal(imported.configuredModels,2);
    assert.equal(imported.settings.authPath,auth);
    assert.deepEqual(imported.settings.pools[pool.id],replay.settings.pools[pool.id]);
    const custom=Object.values(imported.settings.pools).find(row=>row.provider==='my-gateway');
    assert.equal(custom.model,'my-model');
    assert.equal(custom.enabled,false);
    assert.deepEqual(custom.efforts,[]);
    assert.equal(Object.values(imported.settings.pools).find(row=>row.model==='other-model').name,'Other Model');
    configuredModels=false;
    const registry=join(root,'codex-state','registry');
    await mkdir(registry,{recursive:true});
    await writeFile(join(registry,'workers.json'),JSON.stringify({sessions:{slot:{
        sessionId:'stale-slot',serviceId:'attached',serviceOrigin:`http://127.0.0.1:${server.address().port}`,
        startSeq:1,cursor:2,status:'running',updatedAt:1,provider:'account-a',model:'fast',
    }}}));
    const args = {title:'Smoke dispatch',batch:'smoke',role:'support',task:'Return one report',workspace:a};
    const session = await one.tool('cluster_start_session',args);
    assert.equal(session.status,'queued');
    assert.equal(session.effort,'high');
    const byDshId=await one.tool('cluster_get_session_status',{sessionId:session.dshId});
    assert.equal(byDshId.sessionId,session.id);
    assert.equal(byDshId.status,'running');
    assert.deepEqual(await one.tool('cluster_get_session_status',{sessionId:session.id}),byDshId);
    assert.equal(selected.reasoningEffort,'high');
    assert.equal(session.owner,undefined);
    const unavailable=await two.tool('cluster_start_session',{...args,workspace:b,poolId:pool.id},true);
    assert.deepEqual(unavailable.pools.map(({poolId,reason,occupied,concurrency})=>({poolId,reason,occupied,concurrency})),
        [{poolId:pool.id,reason:'slots_full',occupied:1,concurrency:1}]);
    assert.equal(prompts,1);
    running=false;terminal=true;
    const page=await two.tool('cluster_refresh_sessions');
    assert.equal(page.synced,1);
    assert.equal((await two.tool('cluster_overview')).usage.todayTokens,165);
    const origin='http://127.0.0.1:42';
    const old={sessionId:'old-session',serviceOrigin:origin,status:'succeeded',startSeq:1,cursor:7,
        updatedAt:10,batchId:'legacy',title:'历史任务',assistantText:'原插件证据'};
    await writeFile(join(registry,'workers.json'), JSON.stringify({sessions:{[old.sessionId]:old}}));
    const oldKey='["'+origin+'", "'+old.sessionId+'"]';
    const receipts=join(registry,'monitor-review.json');
    await writeFile(receipts,JSON.stringify({[oldKey]:{version:'1:6',read:true,processed:true}}));
    const combined=await two.tool('cluster_overview');
    assert.equal(combined.codexSessions,1);
    assert.equal(combined.unprocessed,2);
    assert.equal(combined.usage.missing,0,'an old completed session does not make today’s statistics incomplete');
    assert.equal(combined.usage.sampled,1);
    await one.tool('cluster_read_result',{sessionId:session.id},true);
    await one.tool('cluster_mark_receipt',{sessionId:session.id,resultVersion:'1:7',processed:true},true);
    await one.tool('cluster_next_handoff',{sessionId:session.id},true);
    assert.equal((await one.tool('cluster_wait_handoff',{timeoutMs:0})).ready,true);
    const [first, second] = await Promise.all([one.tool('cluster_next_handoff'),two.tool('cluster_next_handoff')]);
    const oldest=first.busy?second:first;
    assert.equal((first.busy?first:second).busy,true);
    assert.equal(oldest.session.id,'codex:old-session');
    const resumedClaim=await two.tool('cluster_resume_handoff');
    assert.equal(resumedClaim.resumed,true);
    assert.equal(resumedClaim.claimToken,oldest.claimToken);
    assert.equal(resumedClaim.expiresAt,oldest.expiresAt);
    assert.deepEqual(resumedClaim.result,oldest.result);
    assert.equal(resumedClaim.session.resultVersion,oldest.session.resultVersion);
    const claimedWait=await two.tool('cluster_wait_handoff',{timeoutMs:0});
    assert.equal(claimedWait.ready,false);
    assert.equal(claimedWait.busy,true);
    assert.equal((await two.tool('cluster_get_session_status',{sessionId:old.sessionId})).sessionId,oldest.session.id);
    assert.equal(oldest.result[0].text,'原插件证据');
    assert.equal((await one.tool('cluster_overview')).unprocessed,2);
    assert.equal((await two.tool('cluster_next_handoff',{claimToken:oldest.claimToken})).claimToken,oldest.claimToken);
    const ack={claimToken:oldest.claimToken,outcome:'Checked the legacy worktree and recorded its result.'};
    const next=await two.tool('cluster_next_handoff',ack);
    assert.equal(next.session.id,session.id);
    assert.equal(next.result[0].text,'交接证据');
    assert.equal((await one.tool('cluster_next_handoff',ack)).claimToken,next.claimToken);
    const updated=JSON.parse(await readFile(receipts,'utf8'))[oldKey];
    assert.equal(updated.version,'1:7');
    assert.equal(updated.processed,true);
    assert.equal(updated.outcome,ack.outcome);
    assert.equal((await two.tool('cluster_next_handoff',{claimToken:next.claimToken,outcome:'Verified report.'})).empty,true);
    assert.equal((await one.tool('cluster_overview')).unprocessed,0);
    assert.equal((await one.tool('cluster_resume_handoff')).reason,'no_active_claim');
    const latest={sessionId:'last-codex-session',serviceId:'attached',serviceOrigin:`http://127.0.0.1:${server.address().port}`,
        status:'running',startSeq:1,cursor:4,updatedAt:20,assistantText:'旧登记内容'};
    await writeFile(join(registry,'workers.json'),JSON.stringify({sessions:{[old.sessionId]:old,[latest.sessionId]:latest}}));
    running=true;
    assert.equal((await two.tool('cluster_wait_handoff',{timeoutMs:20})).timedOut,true);
    const waiting=two.tool('cluster_wait_handoff',{timeoutMs:5000});
    setTimeout(()=>{running=false;},150);
    assert.equal((await waiting).ready,true);
    assert.equal((await two.tool('cluster_overview')).unprocessed,1);
    const liveClaim=await two.tool('cluster_next_handoff');
    assert.equal(liveClaim.session.id,'codex:last-codex-session');
    assert.equal(liveClaim.session.resultVersion,'1:7');
    assert.equal(liveClaim.result[0].text,'交接证据');
    running=true;
    assert.equal((await two.tool('cluster_next_handoff',{claimToken:liveClaim.claimToken,outcome:'Previous result verified.'})).changed,true);
    assert.equal(JSON.parse(await readFile(receipts,'utf8'))[JSON.stringify([latest.serviceOrigin,latest.sessionId]).replace(',',', ')],undefined);
    running=false;
    const confirmed=await two.tool('cluster_next_handoff');
    assert.equal((await two.tool('cluster_next_handoff',{claimToken:confirmed.claimToken,outcome:'Latest result verified.'})).empty,true);
    assert.equal(JSON.parse(await readFile(receipts,'utf8'))[JSON.stringify([latest.serviceOrigin,latest.sessionId]).replace(',',', ')].version,'1:7');
    const balanced=await connect(join(root,'balanced'));
    const balancedImport=await balanced.tool('cluster_import_models',{authPath:auth,configPath:''});
    let balancedSettings=await balanced.tool('cluster_configure',{revision:balancedImport.settings.revision,permission:'prompt-on-risk'});
    let firstPool;
    for (const row of Object.values(balancedSettings.pools)) {
        if (row.provider==='account-a' && row.model==='fast') firstPool=row.id;
        balancedSettings=await balanced.tool('cluster_update_pool',{revision:balancedSettings.revision,id:row.id,enabled:true,concurrency:4});
    }
    const c=join(root,'workspace-c'),d=join(root,'workspace-d');
    await Promise.all([mkdir(c),mkdir(d)]);
    terminal=false;
    running=false;
    const spread=[];
    for (const [index,workspace] of [a,b,c,d].entries()) {
        const row=await balanced.tool('cluster_start_session',{
            ...args,role:index===0?'reviewer':'support',workspace,...(index===0?{poolId:firstPool}:{}),
        });
        spread.push(row.provider);
    }
    assert.equal(spread[0],'account-a');
    assert.equal(spread[1],'account-b');
    assert.deepEqual(spread.sort(),['account-a','account-a','account-b','account-b']);
    const ambiguous=await balanced.rpc('tools/call',{name:'cluster_get_session_status',arguments:{sessionId:'external-session'}});
    assert.equal(ambiguous.isError,true);
    assert.match(ambiguous.content[0].text,/多条登记.*FatFishBox id/);
    const fallback=await connect(join(root,'fallback'));
    let fallbackSettings=(await fallback.tool('cluster_import_models',{authPath:auth,configPath:''})).settings;
    fallbackSettings=await fallback.tool('cluster_configure',{revision:fallbackSettings.revision,permission:'prompt-on-risk'});
    const fastPools=Object.values(fallbackSettings.pools).filter(row=>row.model==='fast').sort((a,b)=>a.id.localeCompare(b.id,'en'));
    for (const row of fastPools)
        fallbackSettings=await fallback.tool('cluster_update_pool',{revision:fallbackSettings.revision,id:row.id,enabled:true,concurrency:3});
    const allowed=fastPools.map(row=>row.id);
    rejectProvider=fastPools[0].provider;
    running=false;terminal=false;
    const promptBefore=prompts;
    const switched=await fallback.tool('cluster_start_session',{...args,allowedPoolIds:allowed});
    assert.equal(switched.provider,fastPools[1].provider);
    assert.equal(switched.poolFallbacks[fastPools[0].id],'account_cooldown');
    assert.equal(prompts,promptBefore+1);
    const blocked=await fallback.tool('cluster_start_session',{...args,workspace:b,poolId:fastPools[0].id},true);
    assert.equal(blocked.pools.find(p=>p.poolId===fastPools[0].id).reason,'account_cooldown');
    assert.ok(blocked.pools.find(p=>p.poolId===fastPools[0].id).retryAt>Date.now());
    assert.ok(blocked.availablePools.some(p=>p.poolId===fastPools[1].id));
    const restricted=await fallback.tool('cluster_start_session',{...args,workspace:b,poolId:
        Object.values(fallbackSettings.pools).find(row=>row.model==='light').id},true);
    assert.ok(restricted.pools.some(p=>p.reason==='batch_restricted'));
    assert.equal((await fallback.tool('cluster_start_session',{...args,workspace:b,allowedPoolIds:[fastPools[0].id]},true)).reason,
        'batch_allowlist_conflict');
    hiddenProvider=fastPools[1].provider;
    const missing=await fallback.tool('cluster_start_session',{...args,workspace:b,poolId:fastPools[1].id},true);
    assert.equal(missing.pools.find(p=>p.poolId===fastPools[1].id).reason,'model_unavailable');
    hiddenProvider='';
    rejectProvider='';

    const repairRoot=join(root,'repair');
    const repairRegistry=join(repairRoot,'codex-state','registry');
    uniqueSessionIds=true;
    await mkdir(repairRegistry,{recursive:true});
    const parent={...old,sessionId:'repair-original',batchId:'repair-batch',assistantText:'原交付'+delivery(join(root,'original.png'))};
    const following={...old,sessionId:'follows-repair',updatedAt:30};
    const peerReview={...old,sessionId:'shared-review',updatedAt:35};
    const unrelated={...old,sessionId:'unrelated',updatedAt:40};
    await writeFile(join(repairRegistry,'workers.json'),JSON.stringify({sessions:{parent,following,peerReview,unrelated}}));
    const repair=await connect(repairRoot);
    let repairSettings=(await repair.tool('cluster_import_models',{authPath:auth,configPath:''})).settings;
    repairSettings=await repair.tool('cluster_configure',{revision:repairSettings.revision,permission:'prompt-on-risk'});
    const repairPool=Object.values(repairSettings.pools).find(row=>row.provider==='account-b');
    await repair.tool('cluster_update_pool',{revision:repairSettings.revision,id:repairPool.id,enabled:true,concurrency:2});
    const repairClaim=await repair.tool('cluster_next_handoff');
    assert.equal(repairClaim.session.id,'codex:repair-original');
    assert.equal(repairClaim.delivery.candidateSha,sha);
    assert.equal(repairClaim.delivery.tests[0].exitCode,0);
    assert.equal(repairClaim.delivery.source,'agent_reported');
    assert.ok(!repairClaim.result[0].text.includes('fatfish-delivery'));
    const request={claimToken:repairClaim.claimToken,originalVersion:repairClaim.session.resultVersion,requestId:'repair-once',
        candidateSha:sha,screenshotPaths:[join(root,'original.png')],acceptance:'Check the real window',
        title:'Repair transparency',workspace:c,task:'Fix the mask.'};
    running=false;terminal=false;
    await repair.tool('cluster_repair_handoff',{...request,originalVersion:'wrong'},true);
    await repair.tool('cluster_repair_handoff',{...request,workspace:join(root,'missing-worktree')},true);
    assert.equal((await repair.tool('cluster_next_handoff',{claimToken:request.claimToken})).session.id,repairClaim.session.id);
    const promptsBeforeTimeout=prompts;
    withholdPromptReply=true;
    const uncertain=await repair.tool('cluster_repair_handoff',request);
    assert.equal(uncertain.dispatchState,'unconfirmed');
    assert.equal(uncertain.claimReleased,false);
    assert.equal(uncertain.session.pendingPrompt,undefined);
    assert.ok(uncertain.session.dispatchRetryAt>0);
    fixedStates.set(uncertain.session.dshId,{cursor:1,running:false,queue:[{placement:'context'}],pendingInput:[]});
    const contextOnly=await repair.tool('cluster_repair_handoff',request);
    assert.equal(contextOnly.session.id,uncertain.session.id);
    assert.equal(contextOnly.dispatchState,'unconfirmed');
    assert.equal(contextOnly.claimReleased,false);
    assert.equal((await repair.tool('cluster_next_handoff',{claimToken:request.claimToken})).session.id,repairClaim.session.id);
    await repair.tool('cluster_next_handoff',{claimToken:request.claimToken,outcome:'Premature receipt'},true);
    withholdPromptReply=false;
    const retryDeadline=Date.now()+15000;
    let recovered;
    do {
        await new Promise(resolve=>setTimeout(resolve,100));
        recovered=await repair.tool('cluster_get_session_status',{sessionId:uncertain.session.id});
    } while (recovered.dispatchState==='unconfirmed' && Date.now()<retryDeadline);
    assert.equal(recovered.dispatchState,'confirmed','background worker retries without a new tool invocation to dispatch');
    assert.equal(recovered.retryable,undefined,'confirmed dispatch no longer advertises an old retry failure');
    assert.equal(recovered.failureStage,undefined);
    assert.equal(recovered.dispatchRetryAt,undefined);
    const launched=await repair.tool('cluster_repair_handoff',request);
    assert.equal(launched.session.status,'running');
    assert.equal(launched.dispatchState,'confirmed');
    assert.equal(launched.session.id,uncertain.session.id);
    assert.equal(prompts,promptsBeforeTimeout+2);
    assert.deepEqual(promptRequests.slice(promptsBeforeTimeout),Array(2).fill(promptRequests[promptsBeforeTimeout]));
    assert.equal(launched.claimReleased,true);
    assert.equal(launched.session.batch,'repair-batch');
    assert.equal(launched.session.workItemId,repairClaim.workItemId,'repair retains stable task identity');
    assert.equal(launched.session.repairOf.candidateSha,sha);
    assert.ok(submittedPrompt.includes(request.acceptance));
    assert.equal((await repair.tool('cluster_repair_handoff',request)).session.id,launched.session.id);
    await repair.tool('cluster_repair_handoff',{...request,task:'Different request payload'},true);
    await repair.tool('cluster_next_handoff',{claimToken:request.claimToken,outcome:'Old claim'},true);
    fixedStates.set(launched.session.dshId,{cursor:2,running:true,queue:[],pendingInput:[]});
    const followingClaim=await repair.tool('cluster_next_handoff');
    assert.equal(followingClaim.session.id,'codex:follows-repair');
    running=false;terminal=false;
    const existing=await repair.tool('cluster_start_session',{...args,workspace:d,batch:following.batchId});
    const attach={claimToken:followingClaim.claimToken,originalVersion:followingClaim.session.resultVersion,sessionId:existing.id};
    const previousPromptCount=prompts;
    await repair.tool('cluster_attach_handoff_repair',{...attach,claimToken:'wrong'},true);
    await repair.tool('cluster_attach_handoff_repair',{...attach,originalVersion:'wrong'},true);
    await repair.tool('cluster_attach_handoff_repair',{...attach,sessionId:launched.session.id},true);
    const attachedRepair=await repair.tool('cluster_attach_handoff_repair',attach);
    assert.equal(attachedRepair.handoffState,'repairing');
    assert.equal(attachedRepair.replayed,false);
    assert.equal(attachedRepair.claimReleased,true);
    assert.equal(attachedRepair.linkedHandoffs,1);
    assert.equal((await repair.tool('cluster_attach_handoff_repair',attach)).replayed,true);
    const review=await repair.tool('cluster_next_handoff');
    assert.equal(review.session.id,'codex:shared-review');
    const attachReview={...attach,claimToken:review.claimToken,originalVersion:review.session.resultVersion};
    const shared=await repair.tool('cluster_attach_handoff_repair',attachReview);
    assert.equal(shared.claimReleased,true);
    assert.equal(shared.linkedHandoffs,2);
    assert.equal((await repair.tool('cluster_attach_handoff_repair',attach)).replayed,true);
    assert.equal((await repair.tool('cluster_attach_handoff_repair',attachReview)).replayed,true);
    assert.equal(prompts,previousPromptCount,'binding must not dispatch another prompt');
    const other=await repair.tool('cluster_next_handoff');
    assert.equal(other.session.id,'codex:unrelated');
    assert.equal((await repair.tool('cluster_next_handoff',{claimToken:other.claimToken,outcome:'Verified.'})).empty,true);
    for (const [sessionId,expected] of [['unknown',/找不到该会话/],[42,/请提供 sessionId/]]) {
        const refused=await repair.rpc('tools/call',{name:'cluster_get_session_status',arguments:{sessionId}});
        assert.equal(refused.isError,true);
        assert.match(refused.content[0].text,expected);
    }
    for (const [status,flags] of [
        ['queued',{cursor:2,queue:[{placement:'queued',rpcId:existing.id}]}],
        ['running',{cursor:2,running:true}],['waiting_input',{cursor:3,pendingInput:[{}]}],
        ['succeeded',{cursor:7,queue:[{placement:'queued',rpcId:existing.id}]}],
    ]) {
        forcedState={running:false,queue:[],pendingInput:[],...flags};
        const observed=await repair.tool('cluster_get_session_status',{sessionId:existing.id});
        assert.equal(observed.title,args.title);
        assert.deepEqual(observed.repairLinks.map(item=>item.sessionId),[followingClaim.session.id,review.session.id]);
        assert.equal(observed.status,status);
        assert.equal(observed.stale,false);
        assert.equal(observed.terminal,status==='succeeded');
    }
    deliveryReport=true;
    const linked=await repair.tool('cluster_next_handoff');
    assert.equal(linked.session.id,existing.id,'the later-dispatched repair finished first');
    assert.equal(linked.session.repairOf.sessionId,followingClaim.session.id);
    assert.deepEqual(linked.session.repairLinks.map(item=>item.sessionId),[followingClaim.session.id,review.session.id]);
    assert.equal(linked.session.repairRequests,undefined);
    assert.notEqual(linked.claimToken,attach.claimToken);
    assert.deepEqual(linked.delivery.evidencePaths,[join(root,'repair.png')]);
    const detach={sessionId:existing.id,originalSessionId:followingClaim.session.id,originalVersion:attach.originalVersion};
    await repair.tool('cluster_detach_handoff_repair',detach,true);
    await repair.tool('cluster_detach_handoff_repair',{...detach,claimToken:'wrong'},true);
    const authorizedDetach={...detach,claimToken:linked.claimToken};
    assert.equal((await repair.tool('cluster_detach_handoff_repair',{...authorizedDetach,originalVersion:'wrong'})).detached,false);
    const detached=await repair.tool('cluster_detach_handoff_repair',authorizedDetach);
    assert.equal(detached.detached,true);
    assert.deepEqual(detached.repairLinks.map(item=>item.sessionId),[review.session.id]);
    assert.equal((await repair.tool('cluster_detach_handoff_repair',authorizedDetach)).detached,false);
    await repair.tool('cluster_attach_handoff_repair',attach,true);
    assert.equal((await repair.tool('cluster_attach_handoff_repair',attachReview)).replayed,true);
    const refreshedRepair=await repair.tool('cluster_next_handoff',{claimToken:linked.claimToken});
    assert.equal(refreshedRepair.session.repairOf.sessionId,review.session.id);
    fixedStates.set(launched.session.dshId,{cursor:7,running:false,queue:[],pendingInput:[]});
    const restored=await repair.tool('cluster_next_handoff',{claimToken:linked.claimToken,outcome:'Verified.'});
    assert.equal(restored.session.id,followingClaim.session.id);
    assert.notEqual(restored.claimToken,followingClaim.claimToken);
    await repair.tool('cluster_detach_handoff_repair',detach,true);
    const firstRepair=await repair.tool('cluster_next_handoff',{claimToken:restored.claimToken,outcome:'Verified independently.'});
    assert.equal(firstRepair.session.id,launched.session.id);
    assert.equal(firstRepair.session.repairOf.sessionId,repairClaim.session.id);
    assert.equal((await repair.tool('cluster_next_handoff',{claimToken:firstRepair.claimToken,outcome:'Verified.'})).empty,true);
    forcedState=undefined;
    const workflow=await repair.rpc('resources/read',{uri:'fatfish://workflow'});
    for (const marker of ['team_next_action','team_verify_candidate','team_record_merge','team_wait_decisions','team_wait_actions','300000'])
        assert.ok(workflow.contents[0].text.includes(marker));

    const fifoRoot=join(root,'fifo');
    const fifoRegistry=join(fifoRoot,'codex-state','registry');
    await mkdir(fifoRegistry,{recursive:true});
    running=false;terminal=true;
    const observedSession={...old,serviceId:'attached',serviceOrigin:`http://127.0.0.1:${server.address().port}`};
    const oldDesktopOrigin='http://127.0.0.1:42';
    const desktopAuth=join(fifoRoot,'auth-private.json');
    await writeFile(desktopAuth,JSON.stringify({...JSON.parse(await readFile(auth,'utf8')),desktopHostPid:process.pid}));
    await writeFile(join(fifoRoot,'fatfishbox-desktop-recovery.json'),JSON.stringify({origins:[oldDesktopOrigin]}));
    const fifoSessions={oldest:{...observedSession,sessionId:'oldest',updatedAt:100,serviceOrigin:oldDesktopOrigin}};
    completedTimes.set('oldest',1000);
    for (let i=0;i<80;i++) {
        const sessionId=`later-${i}`;
        fifoSessions[sessionId]={...observedSession,sessionId,updatedAt:200+i};
        completedTimes.set(sessionId,20000+i);
    }
    await writeFile(join(fifoRegistry,'workers.json'),JSON.stringify({sessions:fifoSessions}));
    const fifo=await connect(fifoRoot);
    await fifo.tool('cluster_import_models',{authPath:desktopAuth,configPath:''});
    const oldestClaim=await fifo.tool('cluster_next_handoff');
    assert.equal(oldestClaim.session.id,'codex:oldest','recover the oldest handoff from the previous desktop listener');
    assert.equal(oldestClaim.session.completed,1000);
    const later=await fifo.tool('cluster_next_handoff',{claimToken:oldestClaim.claimToken,outcome:'Verified.'});
    assert.equal(later.session.id,'codex:later-0');

    const archivedRoot=join(root,'archived');
    const archivedRegistry=join(archivedRoot,'codex-state','registry');
    await mkdir(archivedRegistry,{recursive:true});
    const archiveId='session-history-offline', failedId='session-history-failed';
    const archiveRecord={...old,sessionId:archiveId,serviceId:'attached',startSeq:0,cursor:3,
        updatedAt:200,assistantText:'registry placeholder'};
    const failedRecord={...archiveRecord,sessionId:failedId,status:'failed',updatedAt:100};
    await writeFile(join(archivedRegistry,'workers.json'),JSON.stringify({sessions:{archiveRecord,failedRecord}}));
    const historyHeader=id=>({type:'session',version:4,id});
    const event=(type,seq,time,data={})=>({type,seq,time,data});
    const lines=records=>Buffer.from(records.map(v=>JSON.stringify(v)).join('\n')+'\n');
    const frames=(...blocks)=>Buffer.concat(blocks.map(block=>zstdCompressSync(lines(block))));
    const group=join(historyRoot,'_no-cwd');
    await Promise.all([archiveId,failedId].map(id=>mkdir(join(group,id),{recursive:true})));
    const archiveLog=join(group,archiveId,'session.v4.jsonl.zstd');
    const archiveBytes=frames([historyHeader(archiveId)],
        [event('session/created',0,10),event('turn/start',1,1000)],
        [event('assistant/message',2,1500,{message:{content:[{type:'text',text:'Original durable result'}]}}),
            event('turn/end',3,2000,{reason:{kind:'completed'}})]);
    await writeFile(archiveLog,archiveBytes);
    await writeFile(join(group,failedId,'session.v4.jsonl'),lines([historyHeader(failedId),
        event('session/created',0,11),event('turn/start',1,2000),event('step/start',2,2500),
        event('turn/end',3,3000,{reason:{kind:'error',error:{code:'HISTORY_ERR',message:'Stored failure'}}})]));
    const archived=await connect(archivedRoot);
    const peek=await archived.tool('cluster_get_session_result',{sessionId:'codex:'+archiveId});
    assert.equal(peek.resultVersion,'0:3');
    assert.equal(peek.resultSource,'dsh_history');
    assert.equal(peek.messages[0].text,'Original durable result');
    const historyClaim=await archived.tool('cluster_next_handoff');
    assert.equal(historyClaim.session.id,'codex:'+archiveId,'durable completion time determines FIFO');
    assert.equal(historyClaim.resultSource,'dsh_history');
    await writeFile(archiveLog,Buffer.concat([archiveBytes,frames([event('turn/start',4,3500),
        event('assistant/message',5,4000,{message:{content:'A newer turn'}}),
        event('turn/end',6,4500,{reason:{kind:'completed'}})])]));
    assert.equal((await archived.tool('cluster_next_handoff',{claimToken:historyClaim.claimToken,outcome:'Old turn only'})).changed,true);
    const failedHistory=await archived.tool('cluster_next_handoff');
    assert.equal(failedHistory.session.id,'codex:'+failedId);
    assert.deepEqual(failedHistory.failure,{kind:'error',code:'HISTORY_ERR',message:'Stored failure'});
    assert.equal((await archived.tool('cluster_next_handoff',{claimToken:failedHistory.claimToken,
        outcome:'Offline failure verified.',claimNext:false})).claimReleased,true);
    const newerHistory=await archived.tool('cluster_next_handoff');
    assert.equal(newerHistory.session.resultVersion,'0:6');
    assert.equal(newerHistory.result[0].text,'A newer turn');

    const scopedRoot=join(root,'queue-isolation');
    const scopedRegistry=join(scopedRoot,'codex-state','registry');
    await mkdir(scopedRegistry,{recursive:true});
    const migrated={...old,sessionId:'migration-active'};
    const assignable={...old,sessionId:'external-assignable',updatedAt:99,status:'failed',failureKind:'error',
        error:{code:'FIXTURE_LEGACY',message:'Imported failure.'}};
    await writeFile(join(scopedRegistry,'workers.json'),JSON.stringify({sessions:{migrated,assignable}}));
    const migrationClaim={claimToken:'pre-upgrade-token',sessionId:'codex:migration-active',resultVersion:'1:7',expiresAt:Date.now()+60000};
    await writeFile(join(scopedRoot,'handoff.json'),JSON.stringify({claim:migrationClaim}));
    const director=await connect(scopedRoot);
    const collaborator=await connect(scopedRoot);
    assert.equal(collaborator.queueId,director.queueId);
    const pendingLegacy=await director.rpc('tools/call',{name:'cluster_next_handoff',arguments:{claimToken:migrationClaim.claimToken}},true);
    assert.equal(pendingLegacy.structuredContent.claimToken,migrationClaim.claimToken);
    assert.equal(pendingLegacy.structuredContent.queueId,director.queueId);
    const missingQueue=await director.rpc('tools/call',{name:'cluster_next_handoff',arguments:{}},true);
    assert.equal(missingQueue.structuredContent.reason,'queue_id_required');

    const createA={name:'Queue A',requestId:'create-a',workKey:'work-a',workspace:a};
    const queueA=(await director.tool('cluster_create_queue',createA)).queueId;
    const queueB=(await collaborator.tool('cluster_create_queue',{name:'Queue B',requestId:'create-b',workspace:a})).queueId;
    assert.notEqual(queueA,queueB);
    assert.notEqual(queueA,director.queueId);
    await director.tool('cluster_rename_queue',{queueId:queueA,name:'Renamed A'});
    assert.equal((await director.tool('cluster_create_queue',createA)).queueId,queueA);
    await director.tool('cluster_bind_queue',{queueId:queueA,clientKey:'chat-a'});
    assert.equal((await collaborator.tool('cluster_find_queues',{clientKey:'chat-a'})).lastQueueId,queueA);
    assert.equal((await collaborator.tool('cluster_find_queues',{clientKey:'unknown'})).queues.length,0);
    assert.equal((await director.tool('cluster_find_queues',{workKey:'work-a'})).queues[0].queueId,queueA);
    assert.deepEqual(new Set((await director.tool('cluster_find_queues',{workspace:a})).queues.map(q=>q.queueId)),new Set([queueA,queueB]));
    assert.equal((await director.tool('cluster_next_handoff',{queueId:queueA})).empty,true);
    assert.equal((await director.tool('cluster_next_handoff',{queueId:queueB})).empty,true);
    await director.tool('cluster_assign_legacy_session',{queueId:queueA,sessionId:migrationClaim.sessionId},true);
    await director.tool('cluster_assign_legacy_session',{queueId:queueA,sessionId:'codex:external-assignable'});
    await collaborator.tool('cluster_assign_legacy_session',{queueId:queueB,sessionId:'codex:external-assignable'},true);
    await director.tool('cluster_get_session_status',{sessionId:'codex:external-assignable'},true);
    const reassigned=await director.tool('cluster_next_handoff',{queueId:queueA});
    assert.equal(reassigned.session.id,'codex:external-assignable');
    assert.deepEqual(reassigned.failure,{kind:'error',code:'FIXTURE_LEGACY',message:'Imported failure.'});
    await director.tool('cluster_next_handoff',{queueId:queueA,claimToken:reassigned.claimToken,outcome:'Verified external.'});

    let isolated=(await director.tool('cluster_import_models',{authPath:auth,configPath:''})).settings;
    isolated=await director.tool('cluster_configure',{revision:isolated.revision,permission:'prompt-on-risk'});
    const independentPools=Object.values(isolated.pools).filter(row=>row.model==='fast');
    for (const pool of independentPools)
        isolated=await director.tool('cluster_update_pool',{revision:isolated.revision,id:pool.id,enabled:true});
    running=false;terminal=false;
    const taskA=await director.tool('cluster_start_session',{...args,queueId:queueA,allowedPoolIds:[independentPools[0].id]});
    const taskBRequest={...args,queueId:queueB,workspace:b,allowedPoolIds:[independentPools[1].id],requestId:'start-once'};
    const promptsBeforeStart=prompts;
    const taskB=await collaborator.tool('cluster_start_session',taskBRequest);
    assert.equal((await collaborator.tool('cluster_start_session',taskBRequest)).id,taskB.id);
    await collaborator.tool('cluster_start_session',{...taskBRequest,task:'Different request payload'},true);
    assert.equal(prompts,promptsBeforeStart+1);
    assert.equal(taskA.queueId,queueA);
    assert.equal(taskB.queueId,queueB);
    assert.notEqual(taskA.poolId,taskB.poolId);
    fixedStates.set(taskA.dshId,{cursor:7,running:false,queue:[],pendingInput:[]});
    fixedStates.set(taskB.dshId,{cursor:7,running:false,queue:[],pendingInput:[]});
    turnReasons.set(taskA.dshId,{kind:'error',error:{code:'FIXTURE_DSH',message:'DSH execution failed.'}});
    const [claimA,claimB]=await Promise.all([director.tool('cluster_next_handoff',{queueId:queueA}),
        collaborator.tool('cluster_next_handoff',{queueId:queueB,claimToken:'',outcome:'',claimNext:false})]);
    assert.equal(claimA.session.id,taskA.id);
    assert.equal(claimA.session.status,'failed');
    assert.deepEqual(claimA.failure,{kind:'error',code:'FIXTURE_DSH',message:'DSH execution failed.'});
    assert.equal(claimB.session.id,taskB.id);
    const overviewA=await director.tool('cluster_overview',{queueId:queueA});
    const overviewB=await collaborator.tool('cluster_overview',{queueId:queueB});
    assert.deepEqual([overviewA.unprocessed,overviewA.claimed,overviewA.processed],[1,1,1]);
    assert.deepEqual([overviewB.unprocessed,overviewB.claimed,overviewB.processed],[1,1,0]);
    await director.tool('cluster_get_session_status',{queueId:queueA,sessionId:taskB.id},true);
    await director.tool('cluster_cancel_session',{queueId:queueA,sessionId:taskB.id},true);
    await director.tool('cluster_attach_handoff_repair',{queueId:queueA,claimToken:claimA.claimToken,
        originalVersion:claimA.session.resultVersion,sessionId:taskB.id},true);
    assert.equal((await collaborator.tool('cluster_next_handoff',{queueId:queueB,claimToken:claimA.claimToken,outcome:'Wrong queue'})).busy,true);
    await director.tool('cluster_next_handoff',{queueId:queueA,claimToken:'',outcome:'Missing token',claimNext:null},true);
    await director.tool('cluster_next_handoff',{queueId:queueA,claimToken:claimA.claimToken,outcome:'',claimNext:false},true);
    assert.equal((await director.tool('cluster_next_handoff',{queueId:queueA,claimToken:claimA.claimToken,outcome:'Verified A.',claimNext:null})).empty,true);
    assert.equal((await collaborator.tool('cluster_next_handoff',{queueId:queueB,claimToken:claimB.claimToken,outcome:null,claimNext:null})).session.id,taskB.id);
    const receipt={queueId:queueB,claimToken:claimB.claimToken,outcome:'Verified B.',claimNext:false};
    for (let i=0;i<2;i++) {
        const result=await collaborator.tool('cluster_next_handoff',receipt);
        assert.equal(result.completed,true);
        assert.equal(result.claimReleased,true);
    }
    assert.equal((await director.tool('cluster_next_handoff',{claimToken:migrationClaim.claimToken})).session.id,migrationClaim.sessionId);
    const legacyAck=await director.rpc('tools/call',{name:'cluster_next_handoff',
        arguments:{claimToken:migrationClaim.claimToken,outcome:'Continued after upgrade.'}},true);
    assert.equal(legacyAck.structuredContent.completed,true);
    assert.equal(legacyAck.structuredContent.queueId,director.queueId);
    reviewFixture=true;
    const gated=await connect(join(root,'review-gate'));
    let gatedSettings=(await gated.tool('cluster_import_models',{authPath:auth,configPath:''})).settings;
    gatedSettings=await gated.tool('cluster_configure',{revision:gatedSettings.revision,permission:'prompt-on-risk'});
    const gatedPool=Object.values(gatedSettings.pools).find(pool=>pool.provider==='account-b');
    await gated.tool('cluster_update_pool',{revision:gatedSettings.revision,id:gatedPool.id,enabled:true});
    const developer=await gated.tool('cluster_start_session',{...args,role:'developer',batch:'gated-batch'});
    const finish=(sid,report)=>{
        reports.set(sid,report);
        fixedStates.set(sid,{cursor:7,running:false,queue:[],pendingInput:[]});
    };
    finish(developer.dshId,'Developer delivery'+delivery(join(root,'private.png')));
    const savedStatePath=join(root,'review-gate','state.json');
    const savedState=JSON.parse(await readFile(savedStatePath,'utf8'));
    savedState.sessions[developer.id].precheck={state:'blocked',version:'1:7',code:'review_inconclusive',findings:[{issue:'Existing review blocked'}]};
    await writeFile(savedStatePath,JSON.stringify(savedState));
    const devClaim=await gated.tool('cluster_next_handoff');
    assert.equal(devClaim.review.state,'blocked');
    assert.equal(devClaim.session.reviewState,'blocked');
    assert.equal(devClaim.session.id,developer.id);
    assert.match(JSON.stringify(devClaim.result),/Developer delivery/);
    assert.equal([...promptsBySession.values()].filter(p=>p.startsWith('Independently adversarially review')).length,0);
    const reviewer=await gated.tool('cluster_start_session',{...args,role:'reviewer',workItemId:developer.workItemId,batch:'gated-batch',requestId:'manual-review',task:'Independently review the candidate and report defects'});
    finish(reviewer.dshId,'Review failed: regression found');
    const reviewReport=await gated.tool('cluster_get_session_result',{sessionId:reviewer.id});
    assert.match(JSON.stringify(reviewReport),/Review failed/);
    await gated.tool('cluster_next_handoff',{claimToken:devClaim.claimToken,outcome:'Read delivery and dispatched independent review',claimNext:false});
    const reviewClaim=await gated.tool('cluster_next_handoff');
    assert.equal(reviewClaim.session.id,reviewer.id);
    assert.match(JSON.stringify(reviewClaim.result),/regression found/);
    await gated.tool('cluster_next_handoff',{claimToken:reviewClaim.claimToken,outcome:'Recorded findings and arranged follow-up',claimNext:false});
    const watch=await gated.tool('cluster_wait_events',{targets:[{queueId:gated.queueId}],timeoutMs:0});
    assert.equal(watch.targets[0].initial,true);
    const eventStart=watch.targets[0].cursor;
    const structured=await gated.tool('cluster_start_session',{...args,role:'support',batch:'artifact-test',requestId:'artifact-long'});
    const longText='Long report header'+delivery(join(root,'durable.png'))+'z'.repeat(14000)+'中文结束';
    finish(structured.dshId,longText);
    const structuredClaim=await gated.tool('cluster_next_handoff');
    assert.equal(structuredClaim.session.id,structured.id);
    assert.equal(structuredClaim.delivery.candidateSha,sha,'delivery survives a long trailing log');
    for(const key of ['workItemId','candidateSha','baseSha','status','issues','checksManifest','evidenceManifest','nextAction'])
        assert.notEqual(structuredClaim.delivery[key],undefined,'normalized delivery has '+key);
    assert.equal(structuredClaim.delivery.checksManifest,null,'reads never create proof manifests');
    assert.equal(structuredClaim.delivery.evidenceManifest,null,'reads never collect evidence');
    assert.ok(Buffer.byteLength(JSON.stringify(structuredClaim))<4000,'default handoff remains compact');
    assert.equal(structuredClaim.resultTruncated,true);
    const durable=JSON.parse(await readFile(structuredClaim.logPath,'utf8'));
    assert.equal(durable.delivery.candidateSha,sha);
    assert.ok(durable.messages.some(message=>message.text.endsWith('中文结束')));
    let offset=0,joined='';
    for (;;) {
        const page=await gated.tool('cluster_get_session_result',{sessionId:structured.id,resultVersion:structuredClaim.session.resultVersion,offset,limit:6000});
        assert.equal(page.delivery.candidateSha,sha);
        assert.ok(Buffer.byteLength(page.messages[0].text)<=6000);
        joined+=page.messages[0].text;
        if (!page.hasMore) break;
        assert.ok(page.nextOffset>offset);
        offset=page.nextOffset;
    }
    assert.ok(joined.endsWith('中文结束'));
    assert.ok(joined.includes('z'.repeat(14000)));
    const otherQueue=await gated.tool('cluster_create_queue',{name:'Isolated event queue',requestId:'event-isolation'});
    let cursor=eventStart,seenEvents=new Set();
    for (let page=0;page<20;page++) {
        const changed=await gated.tool('cluster_wait_events',{targets:[{queueId:gated.queueId,afterCursor:cursor},
            {queueId:otherQueue.queueId}],timeoutMs:0,limit:1});
        const target=changed.targets[0];
        for (const event of target.events) {
            assert.equal(seenEvents.has(event.sequence),false,'pagination must not repeat events');
            seenEvents.add(event.sequence);
            if (event.type.startsWith('session.')) assert.equal(event.data.id,structured.id);
            else assert.equal(event.data.workItemId,structured.workItemId);
            assert.equal(event.data.repairLinks,undefined);
        }
        assert.equal(changed.targets[1].events.length,0,'events never leak across queues');
        cursor=target.cursor;
        if (!target.hasMore) break;
    }
    assert.ok(seenEvents.size>=2);
    const unchanged=await gated.tool('cluster_wait_events',{targets:[{queueId:gated.queueId,afterCursor:cursor}],timeoutMs:5});
    assert.equal(unchanged.timedOut,true);
    assert.deepEqual(unchanged.targets[0].events,[]);
    const reset=await gated.tool('cluster_wait_events',{targets:[{queueId:gated.queueId,afterCursor:'another-store:4'}],timeoutMs:0});
    assert.equal(reset.targets[0].resetRequired,true);
    await gated.close();
    reports.set(structured.dshId,'Remote changed after archival');
    const restarted=await connect(join(root,'review-gate'));
    const persisted=await restarted.tool('cluster_get_session_result',{sessionId:structured.id,resultVersion:structuredClaim.session.resultVersion});
    assert.equal(persisted.delivery.candidateSha,sha,'archived delivery survives process restart and remote changes');
    const continued=await restarted.tool('cluster_wait_events',{targets:[{queueId:restarted.queueId,afterCursor:cursor}],timeoutMs:0});
    assert.equal(continued.targets[0].resetRequired,false,'event cursor survives restart');
    assert.deepEqual(continued.targets[0].events,[]);
    console.log('Compact durable delivery, UTF-8 log paging, isolated incremental event pagination and restart continuity passed.');
    const tracked=await restarted.tool('cluster_get_work_item',{workItemId:structured.workItemId});
    const recordedIssue=await restarted.tool('cluster_update_work_item',{workItemId:structured.workItemId,requestId:'delta-defect',
        expectedRevision:tracked.revision,action:'add_issue',details:{message:'Fix a newly found edge case',acceptance:'Retest only that edge case',candidateSha:sha}});
    const deltaRepair=await restarted.tool('cluster_repair_handoff',{claimToken:structuredClaim.claimToken,
        originalVersion:structuredClaim.session.resultVersion,requestId:'delta-only-repair',candidateSha:sha,
        acceptance:'Retest the edge case',title:'Delta repair',workspace:b,issueIds:[recordedIssue.issueId]});
    assert.equal(deltaRepair.claimReleased,true);
    assert.equal(deltaRepair.session.workItemId,structured.workItemId);
    const deltaPrompt=promptsBySession.get(deltaRepair.session.dshId);
    assert.ok(deltaPrompt.includes(recordedIssue.issueId));
    assert.equal(deltaPrompt.includes(args.task),false,'repair references original scope instead of repeating its full text');
    const taskSpec=JSON.parse(await readFile(deltaRepair.session.taskRef,'utf8'));
    assert.equal(taskSpec.task,args.task);
    assert.equal(taskSpec.issues[recordedIssue.issueId].state,'open');
    console.log('Stable task identity and issue-only repair references passed.');

    reviewFixture=false;

    const restartingRoot=join(root,'restarting');
    const restarting=await connect(restartingRoot);
    const restartingConfig=join(restartingRoot,'config.json');
    const restartingAuth=join(restartingRoot,'auth.json');
    const desktopStub=join(restartingRoot,'DeepSeek Harness.exe');
    await Promise.all([
        writeFile(desktopStub,''),
        writeFile(restartingConfig,JSON.stringify({service:{attachAuthFile:restartingAuth},runtime:{command:[desktopStub]}})),
    ]);
    const restartingSettings=await restarting.tool('cluster_get_settings');
    await restarting.tool('cluster_configure',{revision:restartingSettings.revision,configPath:restartingConfig,authPath:restartingAuth});
    await writeFile(join(restartingRoot,'fatfishbox-desktop-recovery.json'),JSON.stringify({attemptAt:Date.now()}));
    const recoveryStarted=performance.now();
    const recovering=await restarting.tool('cluster_next_handoff',{},true);
    const recoveryElapsed=Math.round(performance.now()-recoveryStarted);
    assert.equal(recovering.reason,'dsh_recovering');
    assert.equal(recovering.retryAfterMs,2000);
    assert(recoveryElapsed<5000,`Pending DSH restart blocked MCP for ${recoveryElapsed} ms`);
    console.log(`Pending DSH restart reported in ${recoveryElapsed} ms.`);
    console.log('Dispatch diagnostics, batch fallback and linked repair handoff passed.');
    console.log('stdio MCP passed: DSH presets, scheduling, FIFO claims, queue isolation and legacy migration.');
    if (process.env.FATFISH_TEST_LIVE_DSH === '1') {
        const directory = join(root,'real');
        const real = await connect(directory);
        const result = await real.tool('cluster_import_models');
        assert(result.accounts > 0 && result.models > 0);
        console.log(`Live DSH catalog: ${result.accounts} accounts, ${result.models} models.`);
        const livePresets=await real.tool('cluster_get_presets');
        assert(livePresets.permissions.length>0 && livePresets.agents.length>0);
        console.log(`Live DSH presets: ${livePresets.permissions.length} permission, ${livePresets.agents.length} agent.`);
        await real.tool('cluster_configure',{revision:result.settings.revision,permission:livePresets.permissions[0].id});
        const preflight=await real.tool('cluster_start_session',{...args,allowedPoolIds:['nonexistent-smoke-pool']},true);
        assert.equal(preflight.reason,'unknown_pool','read the real catalog without creating a DSH session');
    }
} finally {
    for (const child of live) child.stdin.end();
    await Promise.all(live.map(child => new Promise(resolve => child.exitCode===null?child.once('close',resolve):resolve())));
    await new Promise(resolve => server.close(resolve));
    await rm(root,{recursive:true,force:true});
}
