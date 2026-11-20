import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { createServer } from 'node:http';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createInterface } from 'node:readline';

const executable = resolve(process.argv[2] || 'build/Release/FatFishBox.exe');
const root = await mkdtemp(join(tmpdir(), 'fatfish-smoke-'));
let running = false;
let selected;
let prompts = 0;
let terminal = false;
let offerMax = true;
let rejectProvider = '';
let hiddenProvider = '';
let deliveryReport = false;
let submittedPrompt = '';
let forcedState;
let uniqueSessionIds=false;
let sessionNumber=0;
const fixedStates=new Map();
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
        value = {groups:[
            {id:'account-a', name:'Account A', models:[
                {id:'fast',name:'Fast',reasoning:{defaultEffort:'high',efforts:[{id:'high'},...(offerMax?[{id:'max'}]:[])]}},
                {id:'light',name:'Light'},
            ]},
            {id:'account-b', name:'Account B', models:[{id:'fast',name:'Fast'}]},
        ].filter(group=>group.id!==hiddenProvider)};
        break;
    case 'session/create': value = {sessionId:uniqueSessionIds?`repair-fixture-${++sessionNumber}`:'external-session'}; break;
    case 'commands/execute': value = {result:{kind:'success'}}; break;
    case 'session/projections': value = {values:{permissions:{currentValue:'prompt-on-risk'},
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
    case 'session/prompt': running = true; prompts++; submittedPrompt=args.request.content[0].text; value = {accepted:true}; break;
    case 'session/page': value = {records:[
        {type:'event',event:{seq:6,type:'assistant/message',data:{message:{content:[
            {text:'交接证据'+(deliveryReport?delivery(join(root,'repair.png')):'')}]}}}},
        {type:'event',event:{seq:7,type:'turn/end',data:{reason:{kind:'completed'}}}},
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
async function connect(directory) {
    const child = spawn(executable,['--mcp'],{env:{...process.env,FATFISHBOX_DATA_DIR:directory,
        FATFISHBOX_LEGACY_STATE_DIR:join(directory,'codex-state')},stdio:['pipe','pipe','inherit'],windowsHide:true});
    live.push(child);
    const stream = createInterface({input:child.stdout})[Symbol.asyncIterator]();
    let id=0;
    const rpc = async (method,params={}) => {
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
    return {
        rpc,
        async close() {
            const exited=once(child,'exit');
            child.stdin.end();
            await exited;
        },
        async tool(name,arguments_={},shouldFail=false) {
            const response = await rpc('tools/call',{name,arguments:arguments_});
            assert.equal(response.isError===true,shouldFail,JSON.stringify(response.content));
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
    const registry=join(root,'codex-state','registry');
    await mkdir(registry,{recursive:true});
    await writeFile(join(registry,'workers.json'),JSON.stringify({sessions:{slot:{
        sessionId:'stale-slot',serviceId:'attached',serviceOrigin:`http://127.0.0.1:${server.address().port}`,
        startSeq:1,cursor:2,status:'running',updatedAt:1,provider:'account-a',model:'fast',
    }}}));
    const args = {title:'Smoke dispatch',batch:'smoke',role:'developer',task:'Return one report',workspace:a};
    const session = await one.tool('cluster_start_session',args);
    assert.equal(session.status,'queued');
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
    assert.equal(combined.usage.missing,1);
    assert.equal(combined.usage.sampled,1);
    await one.tool('cluster_read_result',{sessionId:session.id},true);
    await one.tool('cluster_mark_receipt',{sessionId:session.id,resultVersion:'1:7',processed:true},true);
    await one.tool('cluster_next_handoff',{sessionId:session.id},true);
    const [first, second] = await Promise.all([one.tool('cluster_next_handoff'),two.tool('cluster_next_handoff')]);
    const oldest=first.busy?second:first;
    assert.equal((first.busy?first:second).busy,true);
    assert.equal(oldest.session.id,'codex:old-session');
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
    const latest={sessionId:'last-codex-session',serviceId:'attached',serviceOrigin:`http://127.0.0.1:${server.address().port}`,
        status:'running',startSeq:1,cursor:4,updatedAt:20,assistantText:'旧登记内容'};
    await writeFile(join(registry,'workers.json'),JSON.stringify({sessions:{[old.sessionId]:old,[latest.sessionId]:latest}}));
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
            ...args,role:index===0?'reviewer':'repair',workspace,...(index===0?{poolId:firstPool}:{}),
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
    const unrelated={...old,sessionId:'unrelated',updatedAt:40};
    await writeFile(join(repairRegistry,'workers.json'),JSON.stringify({sessions:{parent,following,unrelated}}));
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
    const launched=await repair.tool('cluster_repair_handoff',request);
    assert.equal(launched.session.status,'queued');
    assert.equal(launched.claimReleased,true);
    assert.equal(launched.session.batch,'repair-batch');
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
    assert.equal((await repair.tool('cluster_attach_handoff_repair',attach)).replayed,true);
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
        ['queued',{cursor:2,queue:[{}]}],['running',{cursor:2,running:true}],['waiting_input',{cursor:3,pendingInput:[{}]}],
        ['succeeded',{cursor:7}],
    ]) {
        forcedState={running:false,queue:[],pendingInput:[],...flags};
        const observed=await repair.tool('cluster_get_session_status',{sessionId:existing.id});
        assert.deepEqual(Object.keys(observed).sort(),['sessionId','stale','status','terminal','updated']);
        assert.equal(observed.status,status);
        assert.equal(observed.stale,false);
        assert.equal(observed.terminal,status==='succeeded');
    }
    deliveryReport=true;
    const linked=await repair.tool('cluster_next_handoff');
    assert.equal(linked.session.id,existing.id,'the later-dispatched repair finished first');
    assert.equal(linked.session.repairOf.sessionId,followingClaim.session.id);
    assert.notEqual(linked.claimToken,attach.claimToken);
    assert.deepEqual(linked.delivery.evidencePaths,[join(root,'repair.png')]);
    fixedStates.set(launched.session.dshId,{cursor:7,running:false,queue:[],pendingInput:[]});
    const firstRepair=await repair.tool('cluster_next_handoff',{claimToken:linked.claimToken,outcome:'Verified.'});
    assert.equal(firstRepair.session.id,launched.session.id);
    assert.equal(firstRepair.session.repairOf.sessionId,repairClaim.session.id);
    assert.equal((await repair.tool('cluster_next_handoff',{claimToken:firstRepair.claimToken,outcome:'Verified.'})).empty,true);
    forcedState=undefined;
    const workflow=await repair.rpc('resources/read',{uri:'fatfish://workflow'});
    for (const marker of ['git worktree add','allowedPoolIds','cluster_repair_handoff','实窗验收'])
        assert.ok(workflow.contents[0].text.includes(marker));
    console.log('Dispatch diagnostics, batch fallback and linked repair handoff passed.');
    console.log('stdio MCP passed: DSH presets, scheduling, FIFO claims, shared receipts and denied bypasses.');
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
