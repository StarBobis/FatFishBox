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
        if (method === 'session/state') {
            if ((offline && isManaged) || args.sessionId === 'failed-native') {
                response.writeHead(503).end();
                return;
            }
            const running = isManaged ? resumed : active;
            value = {running, cursor:isManaged ? (resumed ? 19 : 10) : (active ? 3 : 12), queue:[], pendingInput:[]};
            if (usageSession(args.sessionId)) value={running:false,cursor:8,queue:[],pendingInput:[]};
        } else if (method === 'session/page') {
            const event = (seq,type,data={},time=1690000000123) => ({type:'event',event:{seq,type,data,time}});
            if (usageSession(args.address.sessionId)) {
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
    await Promise.all([registry, state, managed, configs].map(path => mkdir(path, {recursive:true})));
    await put(auth,{origin:attached.origin,cookie:'fixture=1',connectionId:'test'});
    await put(join(managed,'auth-private.json'),{origin:secondary.origin,cookie:'fixture=2'});
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
        FATFISHBOX_LEGACY_STATE_DIR:'',DSH_WORKERS_STATE:join(root,'unused'),DSH_WORKERS_CONFIG:config},
        windowsHide:true,stdio:['pipe','pipe','inherit']});
    const lines = createInterface({input:child.stdout})[Symbol.asyncIterator]();
    const snapshot = async (command='') => {
        child.stdin.write(command+'\n');
        const {value,done} = await lines.next();
        assert.equal(done,false);
        const result = JSON.parse(value);
        assert.equal(result.error,undefined);
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
    await put(queue,{repair:{...reference,sessionId:'second'},claim:{...link,claimToken:'old-claim',expiresAt:Date.now()+60000}});
    const migrated=await snapshot('migrate');
    assert.equal(migrated.pending[0].id,'second');
    assert.deepEqual(migrated.pending[0].repairOf,link);
    assert.equal(migrated.pending.some(row=>row.id===finished.id),false);
    const inbox=JSON.parse(await readFile(queue,'utf8'));
    assert.equal(inbox.claim,undefined);
    assert.equal(inbox.repair,undefined);
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
    child.stdin.end();
    assert.equal((await once(child,'exit'))[0],0);
} finally {
    if (child && child.exitCode===null) child.kill();
    await Promise.all([attached, secondary].map(({server}) => new Promise(done => server.close(done))));
    await rm(root,{recursive:true,force:true,maxRetries:4,retryDelay:100});
}
