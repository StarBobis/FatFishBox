import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
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
                {id:'fast',name:'Fast',reasoning:{defaultEffort:'high',efforts:[{id:'high'},{id:'max'}]}},
                {id:'light',name:'Light'},
            ]},
            {id:'account-b', name:'Account B', models:[{id:'fast',name:'Fast'}]},
        ]};
        break;
    case 'session/create': value = {sessionId:'external-session'}; break;
    case 'commands/execute': value = {result:{kind:'success'}}; break;
    case 'session/projections': value = {values:{permissions:{currentValue:'prompt-on-risk'},
        tokenUsage:{uncachedInputTokens:100,outputTokens:20,cacheReadTokens:40,cacheWriteTokens:5}}}; break;
    case 'session/selectModel': selected = args.request; value = {selected}; break;
    case 'session/state': value = {cursor:terminal?7:running?2:1,running,queue:[],pendingInput:[]}; break;
    case 'session/prompt': running = true; prompts++; value = {accepted:true}; break;
    case 'session/page': value = {records:[
        {type:'event',event:{seq:6,type:'assistant/message',data:{message:{content:[{text:'交接证据'}]}}}},
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
    assert(!inventory.some(tool => ['cluster_list_sessions','cluster_get_session','cluster_read_result','cluster_mark_receipt'].includes(tool.name)));
    const presets=await one.tool('cluster_get_presets',{authPath:auth,configPath:''});
    assert.deepEqual(presets.permissions,[{id:'prompt-on-risk',label:'Prompt on risk'}]);
    assert.deepEqual(presets.agents,[{id:'ptc',label:'PTC'}]);
    const initial = await one.tool('cluster_import_models',{authPath:auth,configPath:''});
    assert.equal(initial.accounts,2);
    assert.equal(initial.models,3);
    let settings = await two.tool('cluster_configure',{revision:initial.settings.revision,permission:'prompt-on-risk'});
    await one.tool('cluster_configure',{revision:initial.settings.revision,preset:'ptc'},true);
    settings = await two.tool('cluster_update_account',{revision:settings.revision,id:'account-a',concurrency:2});
    const pool = Object.values(settings.pools).find(row => row.provider==='account-a' && row.model==='fast');
    settings = await two.tool('cluster_update_pool',{revision:settings.revision,id:pool.id,enabled:true,concurrency:1});
    const replay = await one.tool('cluster_import_models',{authPath:auth,configPath:''});
    assert.equal(replay.settings.pools[pool.id].enabled,true);
    const args = {title:'Smoke dispatch',batch:'smoke',role:'developer',task:'Return one report',workspace:a};
    const session = await one.tool('cluster_start_session',args);
    assert.equal(session.status,'queued');
    assert.equal(selected.reasoningEffort,'high');
    assert.equal(session.owner,undefined);
    await two.tool('cluster_start_session',{...args,workspace:b},true);
    assert.equal(prompts,1);
    running=false;terminal=true;
    const page=await two.tool('cluster_refresh_sessions');
    assert.equal(page.synced,1);
    assert.equal((await two.tool('cluster_overview')).usage.todayTokens,165);
    const registry=join(root,'codex-state','registry');
    await mkdir(registry,{recursive:true});
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
    const balanced=await connect(join(root,'balanced'));
    const balancedImport=await balanced.tool('cluster_import_models',{authPath:auth,configPath:''});
    let balancedSettings=await balanced.tool('cluster_configure',{revision:balancedImport.settings.revision,permission:'prompt-on-risk'});
    for (const id of ['account-a','account-b'])
        balancedSettings=await balanced.tool('cluster_update_account',{revision:balancedSettings.revision,id,concurrency:4});
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
    }
} finally {
    for (const child of live) child.stdin.end();
    await Promise.all(live.map(child => new Promise(resolve => child.exitCode===null?child.once('close',resolve):resolve())));
    await new Promise(resolve => server.close(resolve));
    await rm(root,{recursive:true,force:true});
}
