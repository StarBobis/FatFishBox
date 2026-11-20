import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
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
    assert.equal(ready?.status,401);
    const key=await readFile(join(folder,'mcp-http.token'),'utf8');
    assert.match(key,/^[a-f0-9]{64}$/);
    const auth={Authorization:`Bearer ${key}`};
    assert.equal((await call('ping',{},1,{...auth,Origin:'https://example.test'})).status,403);
    assert.equal((await call('ping',{},1,{...auth,'MCP-Protocol-Version':'bad'})).status,400);
    assert.equal((await fetch(url,{headers:auth})).status,405);
    const initialized=await call('initialize',{protocolVersion:'2025-11-25',capabilities:{},clientInfo:{name:'test',version:'1'}},1,auth);
    assert.equal((await initialized.json()).result.protocolVersion,'2025-11-25');
    assert.equal((await call('notifications/initialized',{},null,auth)).status,202);
    const [inventory,overview]=await Promise.all([
        call('tools/list',{},2,auth).then(response=>response.json()),
        call('tools/call',{name:'cluster_overview'},3,auth).then(response=>response.json()),
    ]);
    assert(inventory.result.tools.some(tool=>tool.name==='cluster_next_handoff'));
    assert.equal(overview.result.structuredContent.unprocessed,0);
    const denied=await call('tools/call',{name:'cluster_list_sessions'},4,auth).then(response=>response.json());
    assert.equal(denied.result.isError,true);
    console.log('Streamable HTTP passed: initialize, concurrent clients, notification, authorization and disallowed origins.');
} finally {
    headless.kill();
    await stopped;
    await rm(folder,{recursive:true,force:true});
}
