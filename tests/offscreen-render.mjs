import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { access, mkdtemp, readFile, rm,writeFile,mkdir,copyFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { promisify } from 'node:util';
import { inflateSync } from 'node:zlib';

const run = promisify(execFile);
const executable = resolve(process.argv[2] || 'build/staged/FatFishBox.exe');
const folder = await mkdtemp(join(tmpdir(), 'fatfish-render-'));
const env = { ...process.env, FATFISHBOX_DATA_DIR: folder, FATFISHBOX_LEGACY_STATE_DIR: join(folder, 'legacy') };
delete env.DSH_WORKERS_CONFIG;

try {
    for (const [page, width, height, dpi] of [
        ['settings', 740, 520, 96],
        ['settings', 925, 650, 120],
        ['sessions', 1100, 720, 96],
        ['pools', 1100, 720, 96],
        ['queues', 740, 520, 96],
        ['onboarding', 740, 520, 96],
        ['onboarding', 925, 650, 120],
        ['sessions', 740, 520, 96],
        ['sessions', 925, 650, 120],
    ]) {
        const output = join(folder, `${page}-${dpi}.png`);
        await run(executable, ['--render-offscreen', page, output, ...[width, height, dpi].map(String)],
            { env, windowsHide: true, timeout: 15000 });
        const png = await readFile(output);
        assert.deepEqual([...png.subarray(0, 8)], [137, 80, 78, 71, 13, 10, 26, 10]);
        assert.equal(png.readUInt32BE(16), width);
        assert.equal(png.readUInt32BE(20), height);
        const imageData = [];
        for (let offset = 8; offset + 12 <= png.length;) {
            const length = png.readUInt32BE(offset);
            if (png.toString('ascii', offset + 4, offset + 8) === 'IDAT')
                imageData.push(png.subarray(offset + 8, offset + 8 + length));
            offset += 12 + length;
        }
        const pixels = inflateSync(Buffer.concat(imageData));
        assert(pixels.length >= width * height);
        assert(new Set(pixels).size > 16, 'render should contain more than a flat background');
    }
    const statePath=join(folder,'state.json'),teamPath=join(folder,'teams.json');
    const state=JSON.parse(await readFile(statePath,'utf8'));
    const team={settings:{revision:0,totalConcurrency:8,developerConcurrency:4,reviewerConcurrency:2,leadPool:'',developerPool:'',reviewerPool:'',integratorPool:''},projects:{},requests:{}};
    const queueId=state.legacyQueueId,projectId='queue-render-fixture';
    team.projects[projectId]={projectId,queueId,title:'队列团队',workspace:folder,baseSha:'1'.repeat(40),status:'running',charterVersion:1,updated:Date.now(),sequence:0,
        runtimeErrors:{fixture:{operation:'observe_session',scope:'fixture-session',message:'DSH 暂时不可用，正在自动重试',lastFailureAt:Date.now(),retryAt:Date.now()+2000}},
        domains:{feature:{domainId:'feature',name:'功能实现',mission:'按验收条件完成开发与独立审查',state:'waiting'}},jobs:{},
        decisions:{question:{decisionId:'question',state:'open',question:'公共接口如何保持兼容？',recommendation:'保留旧格式读取'}}};
    const managerId='render-manager',workerId='render-worker';
    state.sessions[managerId]={id:managerId,queueId,title:'项目经理 · 完成页面修复计划',workspace:folder,role:'lead',teamProjectId:projectId,
        status:'running',created:1,updated:2,processed:false,model:'经理模型',provider:'fixture'};
    state.sessions[workerId]={id:workerId,queueId,title:'修复列表页面布局',workspace:folder,role:'developer',teamProjectId:projectId,
        status:'running',created:1,updated:2,processed:false,model:'开发模型',provider:'fixture'};
    team.projects[projectId].jobs={lead:{jobId:'lead',domainId:'feature',role:'lead',state:'running',sessionId:managerId,created:1}};
    state.settings.accounts={fixture:{name:'Google 账号',provider:'fixture'},peer:{name:'GitHub 账号',provider:'peer'}};
    state.settings.pools={one:{id:'one',provider:'fixture',model:'hy4-flash-free',name:'Hy4 Flash Free',enabled:true,concurrency:10,effort:'high',efforts:['low','medium','high']},two:{id:'two',provider:'fixture',model:'other',name:'第二个可选模型',enabled:false,concurrency:10,effort:'',efforts:[]},three:{id:'three',provider:'peer',model:'hy4-flash-free',name:'Hy4 Flash Free',enabled:true,concurrency:10,effort:'high',efforts:['low','high']}};
    await writeFile(statePath,JSON.stringify(state));
    await writeFile(teamPath,JSON.stringify(team));
    for(const page of ['sessions','pools','queues','onboarding','onboarding-models','onboarding-queue','onboarding-start','settings']) for(const [width,height,dpi] of [[740,520,96],[1100,720,96],[925,650,120]]) {
        const preview=join(folder,`${page}-${dpi}-${width}.png`);
        await run(executable,['--render-offscreen',page,preview,...[width,height,dpi].map(String)],{env,windowsHide:true,timeout:15000});
        if(process.env.FATFISH_RENDER_EVIDENCE) {
            await mkdir(process.env.FATFISH_RENDER_EVIDENCE,{recursive:true});
            await copyFile(preview,join(process.env.FATFISH_RENDER_EVIDENCE,`${page}-${dpi}-${width}.png`));
        }
    }
    const invalid = join(folder, 'invalid.png');
    await assert.rejects(run(executable, ['--render-offscreen', 'unknown', invalid],
        { env, windowsHide: true, timeout: 15000 }), { code: 1 });
    await assert.rejects(access(invalid), { code: 'ENOENT' });
    console.log('Offscreen rendering passed for a fresh profile at 100% and 125% scale.');
} finally {
    await rm(folder, { recursive: true, force: true });
}
