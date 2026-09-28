const assert=require('node:assert/strict');
const path=require('node:path');
const {pathToFileURL}=require('node:url');
const {chromium}=require('playwright');
async function main(){
 const root=path.resolve(__dirname,'../../web');
 const {createServer}=await import(pathToFileURL(path.join(root,'node_modules/vite/dist/node/index.js')));
 const server=await createServer({root,server:{host:'127.0.0.1',port:0}});await server.listen();
 const browser=await chromium.launch({executablePath:process.argv[2] || 'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});
 try {
  for(const ratio of [1,1.25,1.5,2]){
   const context=await browser.newContext({deviceScaleFactor:ratio,viewport:{width:Math.floor(2560/ratio),height:Math.floor(1280/ratio)}});
   const page=await context.newPage();
   await page.route('**/pixels-fixture',r=>r.fulfill({contentType:'text/html',body:'<style>html,body{margin:0;height:100%}</style><div class="desktop-viewport" style="width:100%;height:100%"><div class="desktop-stage"><canvas width="1920" height="1080"></canvas><canvas class="cursor-overlay" width="1920" height="1080"></canvas></div></div>'}));
   await page.goto(`http://127.0.0.1:${server.httpServer.address().port}/pixels-fixture`);
   const size=await page.evaluate(async()=>{
    await import('/src/styles/desktop.css');
    const {DesktopSession}=await import('/src/sessions/desktop/desktop.session.ts');
    const {DesktopInput}=await import('/src/sessions/desktop/input.session.ts');
    const c=document.querySelector('canvas'),v=document.querySelector('.desktop-viewport');
    window.session=new DesktopSession({id:'pixels',writeControl:true,channels:[]},c,document.querySelector('.cursor-overlay'),()=>{},()=>{});
    window.session.sizeDesktop();
    const ctx=c.getContext('2d');ctx.fillStyle='white';ctx.fillRect(0,0,1920,1080);ctx.fillStyle='black';ctx.font='20px sans-serif';ctx.fillText('原始像素 1920 × 1080 Windows',80,80);
    window.events=[];window.input=new DesktopInput(c,e=>window.events.push(e),{width:1920,height:1080});
    const b=c.getBoundingClientRect();
    return {physical:[b.width*devicePixelRatio,b.height*devicePixelRatio],native:[c.width,c.height],overflow:v.scrollWidth>v.clientWidth||v.scrollHeight>v.clientHeight,center:[b.left+b.width/2,b.top+b.height/2]};
   });
   assert.deepEqual(size.native,[1920,1080]);
   assert.ok(Math.abs(size.physical[0]-1920)<.05&&Math.abs(size.physical[1]-1080)<.05);
   assert.equal(size.overflow,false);
   await page.mouse.click(...size.center);
   assert.ok(await page.evaluate(()=>window.events.some(e=>Math.abs(e.x-960)<=1&&Math.abs(e.y-540)<=1)));
   await page.evaluate(()=>{window.input.dispose();window.session.stop();});
   console.log(JSON.stringify({dpr:ratio,...size}));await context.close();
  }
 } finally {await browser.close();await server.close();}
}
main().catch(e=>{console.error(e);process.exitCode=1;});
