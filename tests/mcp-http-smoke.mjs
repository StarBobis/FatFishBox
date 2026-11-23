import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { access, mkdtemp, rm } from 'node:fs/promises';
import { createServer } from 'node:net';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { setTimeout as delay } from 'node:timers/promises';

const executable=resolve(process.argv[2] || 'build/Release/FatFishBox.exe');
const folder=await mkdtemp(join(tmpdir(),'fatfish-http-'));
const probe=createServer();
probe.listen(0,'127.0.0.1');
await once(probe,'listening');
const port=probe.address().port;
probe.close();
await once(probe,'close');
const env={...process.env,FATFISHBOX_DATA_DIR:folder,FATFISHBOX_LEGACY_STATE_DIR:join(folder,'legacy'),
    FATFISHBOX_MCP_PORT:String(port)};
const headless=spawn(executable,['--mcp-http'],{env,stdio:['ignore','ignore','inherit'],windowsHide:true});
const stopped=once(headless,'close');
const url=`http://127.0.0.1:${port}/mcp`;
const base={'Content-Type':'application/json','Accept':'application/json, text/event-stream'};
const call=(method,params={},id=1,headers={})=>fetch(url,{method:'POST',headers:{...base,...headers},
    body:JSON.stringify({jsonrpc:'2.0',method,params,...(id!==null?{id}:{})})});
try {
    let ready;
    for (let i=0;i<40 && !ready;i++) {
        ready=await call('ping').catch(()=>null);
        if (!ready) await delay(50);
    }
    assert.equal(ready?.status,200);
    assert.deepEqual((await ready.json()).result,{});
    await assert.rejects(access(join(folder,'mcp-http.token')),{code:'ENOENT'});
    assert.equal((await call('ping',{},1,{Origin:'https://example.test'})).status,403);
    assert.equal((await call('ping',{},1,{'MCP-Protocol-Version':'bad'})).status,400);
    assert.equal((await fetch(url)).status,405);
    const teamUrl=`http://127.0.0.1:${port}/team`;
    const teamCall=(method,params={})=>fetch(teamUrl,{method:'POST',headers:base,body:JSON.stringify({jsonrpc:'2.0',id:1,method,params})}).then(r=>r.json());
    const initParams={protocolVersion:'2025-11-25',capabilities:{},clientInfo:{name:'test',version:'1'}};
    const teamInitialization=(await teamCall('initialize',initParams)).result;
    assert.match(teamInitialization.instructions,/before calling any FatFishBox tool/);
    const principalCatalog=(await teamCall('tools/list')).result.tools;
    const principalTools=principalCatalog.map(t=>t.name);
    for (const name of ['team_start_project','team_activate_project','team_wait_decisions','team_control_project']) {
        const schema=principalCatalog.find(t=>t.name===name);
        assert.equal(schema.inputSchema.type,'object');
        assert.ok(schema.inputSchema.properties.requestId || schema.inputSchema.properties.projectId);
        assert.equal(schema.outputSchema.type,'object');
    }
    const promptResult=(await teamCall('tools/call',{name:'team_get_prompt',arguments:{role:'principal'}})).result;
    assert.equal(promptResult.structuredContent.version,16);
    assert.match(promptResult.structuredContent.prompt,/verify it yourself without calling any FatFishBox tools/);
    assert.deepEqual(JSON.parse(promptResult.content[0].text),promptResult.structuredContent);
    assert.deepEqual(promptResult.structuredContent.tools.team_start_project,principalCatalog.find(t=>t.name==='team_start_project').inputSchema);
    assert.ok(principalTools.includes('team_start_project'));
    assert.ok(!principalTools.some(n=>n.startsWith('cluster_')));
    assert.ok(!principalTools.includes('team_delegate'));
    assert.equal((await teamCall('tools/call',{name:'cluster_find_queues',arguments:{}})).result.isError,true);
    assert.equal((await fetch(teamUrl,{method:'POST',headers:{...base,Origin:'https://example.test'},body:'{}'})).status,403);
    assert.equal((await fetch(teamUrl)).status,405);
    const initialized=await call('initialize',initParams);
    const initialization=(await initialized.json()).result;
    assert.equal(initialization.protocolVersion,'2025-11-25');
    assert.match(initialization.instructions,/before calling any FatFishBox tool/);
    assert.match(initialization.instructions,/only pastes connection instructions, wait for a task/);
    assert.equal((await call('notifications/initialized',{},null)).status,202);
    const missingQueue=await call('tools/call',{name:'cluster_overview'},7).then(response=>response.json());
    assert.equal(missingQueue.result.structuredContent.reason,'queue_id_required');
    const discovery=await call('tools/call',{name:'cluster_find_queues'},8).then(response=>response.json());
    assert.equal(discovery.result.content[0]?.type,'text');
    assert.deepEqual(JSON.parse(discovery.result.content[0].text),discovery.result.structuredContent);
    const queueId=discovery.result.structuredContent.queues.find(queue=>queue.legacy).queueId;
    assert.equal(discovery.result.structuredContent.queues.length,1);
    const createQueue={name:'cluster_create_queue',arguments:{name:'首次创建',requestId:'first-queue'}};
    const created=await call('tools/call',createQueue).then(response=>response.json());
    assert.equal(created.result.isError,undefined);
    const newQueue=created.result.structuredContent;
    assert.equal(newQueue.name,'首次创建');
    assert.equal(newQueue.legacy,false);
    assert.notEqual(newQueue.queueId,queueId);
    const retried=await call('tools/call',createQueue).then(response=>response.json());
    assert.equal(retried.result.structuredContent.queueId,newQueue.queueId);
    const newOverview=await call('tools/call',{name:'cluster_overview',arguments:{queueId:newQueue.queueId}})
        .then(response=>response.json());
    assert.equal(newOverview.result.structuredContent.unprocessed,0);
    const [inventory,overview]=await Promise.all([
        call('tools/list',{},2).then(response=>response.json()),
        call('tools/call',{name:'cluster_overview',arguments:{queueId}},3).then(response=>response.json()),
    ]);
    const schema=name=>inventory.result.tools.find(tool=>tool.name===name).inputSchema;
    for (const name of ['cluster_next_handoff','cluster_start_session','cluster_repair_handoff','cluster_get_session_status']) {
        assert.equal(schema(name).properties.queueId.type,'string');
        assert(schema(name).required.includes('queueId'));
    }
    assert.equal(schema('cluster_start_session').properties.requestId.type,'string');
    const handoffSchema=schema('cluster_next_handoff');
    assert.deepEqual(schema('cluster_wait_handoff').required,['queueId']);
    assert.equal(inventory.result.tools.some(tool=>tool.name==='cluster_retry_review'),false);
    assert.deepEqual(handoffSchema.required,['queueId']);
    assert.deepEqual(handoffSchema.properties.claimNext.type,['boolean','null']);
    for (const key of ['claimToken','outcome']) assert.deepEqual(handoffSchema.properties[key].type,['string','null']);
    for (const options of [{}, {claimToken:null,outcome:null,claimNext:null}, {claimToken:'',outcome:'',claimNext:false}]) {
        const first=await call('tools/call',{name:'cluster_next_handoff',arguments:{queueId:newQueue.queueId,...options}})
            .then(response=>response.json());
        assert.equal(first.result.isError,undefined);
        assert.equal(first.result.structuredContent.empty,true);
    }
    const eventCall=arguments_=>call('tools/call',{name:'cluster_wait_events',arguments:arguments_}).then(r=>r.json());
    const baseline=(await eventCall({targets:[{queueId:newQueue.queueId}],timeoutMs:0})).result.structuredContent;
    assert.equal(baseline.targets[0].initial,true);
    const batch=(await eventCall({targets:[{queueId:newQueue.queueId,afterCursor:baseline.targets[0].cursor},{queueId}],timeoutMs:0})).result.structuredContent;
    assert.equal(batch.targets.length,2);
    assert.deepEqual(batch.targets[0].events,[]);
    const duplicate=await eventCall({targets:[{queueId},{queueId}],timeoutMs:0});
    assert.equal(duplicate.result.isError,true);
    const wrongQueue=(await eventCall({targets:[{queueId,afterCursor:baseline.targets[0].cursor}],timeoutMs:0})).result.structuredContent;
    assert.equal(wrongQueue.targets[0].resetRequired,true);
    const waits=Array.from({length:3},(_,index)=>call('tools/call',{name:'cluster_wait_handoff',
        arguments:{queueId:newQueue.queueId,timeoutMs:750}},10+index).then(response=>response.json()));
    assert.equal((await call('ping',{},13)).status,200);
    const waitResults=(await Promise.all(waits)).map(response=>response.result);
    for (const result of waitResults)
        assert.deepEqual(JSON.parse(result.content.at(-1).text),result.structuredContent);
    assert.equal(waitResults.filter(result=>result.structuredContent.timedOut===true).length,2);
    assert.equal(waitResults.filter(result=>result.structuredContent.reason==='wait_capacity').length,1);
    assert(inventory.result.tools.some(tool=>tool.name==='cluster_create_queue'));
    assert(inventory.result.tools.every(tool=>!tool.name.includes('delete_queue')));
    const deleteAttempt=await call('tools/call',{name:'cluster_delete_queue',arguments:{queueId}},9).then(response=>response.json());
    assert.equal(deleteAttempt.result.isError,true);
    assert.equal(overview.result.structuredContent.unprocessed,0);
    const denied=await call('tools/call',{name:'cluster_list_sessions'},4).then(response=>response.json());
    assert.equal(denied.result.isError,true);
    console.log('Streamable HTTP passed without credentials: initialize, concurrent clients, notifications and origin validation.');
} finally {
    headless.kill();
    await stopped;
    await rm(folder,{recursive:true,force:true});
}
