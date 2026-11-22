import assert from 'node:assert/strict';
import {mkdtemp,copyFile,readdir,readFile,rm} from 'node:fs/promises';
import {join,resolve} from 'node:path';
import {tmpdir} from 'node:os';
import {spawnSync} from 'node:child_process';
const directory=await mkdtemp(join(tmpdir(),'fatfish-diagnostics-'));
try {
 const exe=join(directory,'probe.exe');
 await copyFile(resolve(process.argv[2]),exe);
 for (const mode of ['recover','terminate','seh']) {
  const result=spawnSync(exe,[mode],{cwd:tmpdir(),windowsHide:true,timeout:15000});
  assert.ifError(result.error);
  assert.equal(result.status===0,mode==='recover');
 }
 const files=await readdir(join(directory,'logs'));
 const logs=await Promise.all(files.filter(f=>f.endsWith('.log')).map(f=>readFile(join(directory,'logs',f),'utf8')));
 assert.equal(logs.length,3);
 const combined=logs.join('\n');
 for (const text of ['可恢复异常 probe','process remains alive','Unknown non-standard','worker fatal probe','E0000123','diagnostics-test.cpp','tid=',' #0 ']) assert.ok(combined.includes(text),text);
 const dumps=files.filter(f=>f.endsWith('.dmp'));
 assert.equal(dumps.length,2);
 for (const dump of dumps) assert.equal((await readFile(join(directory,'logs',dump))).subarray(0,4).toString(),'MDMP');
 console.log('Diagnostics passed: recovered exceptions, worker terminate, SEH, adjacent UTF-8 logs and valid minidumps.');
} finally { await rm(directory,{recursive:true,force:true}); }
