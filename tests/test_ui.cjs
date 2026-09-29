const fs = require('node:fs');
const http = require('node:http');
const assert = require('node:assert/strict');
const {chromium} = require('playwright-core');
(async () => {
  fs.mkdirSync(__dirname+'/../.pio/analysis',{recursive:true});
  let browser;
  let label='LOCAL_EMPTY',remaining=null;let state = 'INVALID';let hop=200;let history=[];let polls=0;let extra={};
  const server = http.createServer((req, res) => {
    if (req.url === '/status') {
      polls++;res.setHeader('Content-Type','application/json');
      res.end(JSON.stringify({states:{rx1:{state,reason:'test',effective_fs:250,packet_loss:0,score:5,measurement_valid:state!=='INVALID',amplitude_calibrated:state!=='PROVISIONAL',config:{enter_score:4,window:400,hop,min_fs:250},_result_interval_s:hop/250,top:[2,3,4,5,6]},...extra},fused:state,label,remaining,issues:{},history,elapsed_s:1,folder:'synthetic test'}));
    } else if(req.url==='/activity_map.js'){res.setHeader('Content-Type','application/javascript');res.end(fs.readFileSync(__dirname+'/../host/activity_map.js'));} else res.end(fs.readFileSync(__dirname+'/../host/dashboard.html'));
  });
  await new Promise(resolve => server.listen(0,'127.0.0.1',resolve));
  try {
    browser=await chromium.launch({executablePath:process.env.CHROME_PATH||'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});
    const page=await browser.newPage();const errors=[];page.on('pageerror',e=>errors.push(e.message));
    await page.goto(`http://127.0.0.1:${server.address().port}/`);
    await page.waitForFunction(()=>document.getElementById('fusion').textContent==='Fusión: INVALID');
    state='ACTIVE';await page.waitForFunction(()=>document.getElementById('fusion').textContent==='Fusión: ACTIVE');
    state='PROVISIONAL';await page.waitForFunction(()=>document.getElementById('fusion').textContent==='Fusi\u00f3n: PROVISIONAL');
    assert.match(await page.locator('#nodes').textContent(),/Variacion.*5.00/);
    state='CLEAR';await page.waitForFunction(()=>document.getElementById('fusion').textContent==='Fusión: CLEAR');
    label='CALIBRATION_COUNTDOWN';remaining=20;
    await page.waitForFunction(()=>document.getElementById('calibrate').disabled);
    assert.equal(await page.locator('#cancelwait').isVisible(),true);
    assert.match(await page.locator('#phase').textContent(),/20 s/);
    label='LOCAL_BACKGROUND_CALIBRATION';remaining=null;
    await page.waitForFunction(()=>!document.getElementById('calibrate').disabled);
    assert.equal(await page.locator('#cancelwait').isVisible(),false);
    assert.match(await page.locator('#phase').textContent(),/Referencia de amplitud lista/);
    const baseline=polls;
    hop=25;history=Array.from({length:10},(_,i)=>({node:'rx1',t:i/10,score:i===5?100:1}));
    await page.waitForFunction(()=>document.getElementById('chart').dataset.points==='10');
    assert.match(await page.locator('#cadence').textContent(),/hop 25/);
    assert.match(await page.locator('#cadence').textContent(),/0.100 s/);
    await page.selectOption('#timespan','10');
    assert.equal(await page.locator('#chart').getAttribute('data-span'),'10');
    await page.waitForTimeout(650);
    assert.ok(polls-baseline>=4,'GUI should refresh faster than the old 500 ms poll');
    await page.locator('#mapcenter').check();
    assert.equal(await page.locator('#map').getAttribute('data-center'),'none');
    await page.locator('summary').filter({hasText:'Posiciones y escala'}).click();
    await page.locator('#mapapply').click();
    await page.waitForFunction(()=>document.getElementById('mapnote').textContent.includes('Geometria guardada'));
    assert.ok(await page.evaluate(()=>JSON.parse(localStorage.getItem('rat-csi-layout-v1')).confirmed));
    extra={rx2:{state:'ACTIVE',score:20,measurement_valid:true,amplitude_calibrated:true,_feature_age_s:0}};
    await page.waitForFunction(()=>document.getElementById('map').dataset.center==='visible');
    extra.rx2.measurement_valid=false;
    await page.waitForFunction(()=>document.getElementById('map').dataset.center==='none');
    await page.setViewportSize({width:390,height:844});
    await page.waitForTimeout(200);
    assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth>innerWidth),false);
    assert.deepEqual(errors,[]);
    await page.screenshot({path:'.pio/analysis/rat-ui-mobile.png',fullPage:true});
    console.log('PASS: INVALID / ACTIVE / CLEAR, all window points, hop cadence, time zoom, fast refresh, mobile layout, no JavaScript errors (synthetic UI data).');
  } finally {if(browser)await browser.close();await new Promise(resolve=>server.close(resolve));}
})().catch(e=>{console.error(e);process.exitCode=1;});
