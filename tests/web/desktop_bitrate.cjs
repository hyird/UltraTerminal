const assert=require('node:assert/strict');
const path=require('node:path');
const {pathToFileURL}=require('node:url');
const {chromium}=require('playwright');
async function main(){
 const root=path.resolve(__dirname,'../../web');
 const {createServer}=await import(pathToFileURL(path.join(root,'node_modules/vite/dist/node/index.js')));
 const server=await createServer({root,server:{host:'127.0.0.1',port:0}});await server.listen();
 const browser=await chromium.launch({executablePath:process.argv[2] || 'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});
 try{
  const page=await browser.newPage();
  await page.route('**/bitrate-fixture',r=>r.fulfill({contentType:'text/html',body:'<canvas></canvas><canvas class="cursor-overlay"></canvas>'}));
  await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/bitrate-fixture`);
  await page.clock.install();
  await page.evaluate(async()=>{
   const {DesktopSession}=await import('/src/sessions/desktop/desktop.session.ts');
   window.rates=[];
   window.session=new DesktopSession({id:'bitrate',writeControl:false,channels:[]},document.querySelector('canvas'),document.querySelector('.cursor-overlay'),()=>{},()=>{},()=>{},value=>window.rates.push(value));
   window.session.recordVideoBytes(125000);
  });
  await page.clock.runFor(1000);
  assert.deepEqual(await page.evaluate(()=>window.rates),[1000000]);
  await page.clock.runFor(1000);
  assert.deepEqual(await page.evaluate(()=>window.rates),[1000000,0]);
  await page.clock.runFor(5000);
  assert.equal(await page.evaluate(()=>window.rates.length),2);
  await page.evaluate(()=>window.session.recordVideoBytes(250000));
  await page.clock.runFor(1000);
  assert.deepEqual(await page.evaluate(()=>window.rates),[1000000,0,2000000]);
  await page.evaluate(()=>{window.session.recordVideoBytes(1000);window.session.stop();});
  await page.clock.runFor(5000);
  assert.equal(await page.evaluate(()=>window.rates.length),3);
  console.log('PASS: video bytes to bit/s, one-second samples, idle zero, no idle polling, resume and cancellation');
 }finally{await browser.close();await server.close();}
}
main().catch(e=>{console.error(e);process.exitCode=1;});
