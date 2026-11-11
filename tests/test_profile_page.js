// Run the actual offline phone flow, including network failures and partial saves.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert/strict');
const html=fs.readFileSync('main/apps/home/phone_setup.html','utf8');
const cityCatalog=JSON.parse(fs.readFileSync('main/apps/home/cities.json','utf8'));
const script=html.match(/<script>\r?\n([\s\S]*?)<\/script>/)[1];
assert.ok(!/<(?:script|img|link)\b[^>]*(?:src|href)=["']https?:|url\(["']?https?:/i.test(html),'the captive page must not load internet resources');
const elements=new Map(),requests=[],timers=new Map();let nextTimer=1;
class Element {
 constructor(){this.value='';this.textContent='';this.style={};this.dataset={};this.options=[];this.classList={toggle(){}};this.type='password';this.files=[];}
 add(o){this.options.push(o)} replaceChildren(...o){this.options=o;this.value=''}
 appendChild(o){this.options.push(o);return o} click(){this.onclick?.()} setAttribute(k,v){this[k]=v} removeAttribute(k){delete this[k]} focus(){} setPointerCapture(){}
 get selectedOptions(){return this.options.filter(o=>o.value===this.value)}
 getContext(){return {drawImage(){},fillRect(){},putImageData(){},getImageData:(x,y,w,h)=>({data:new Uint8ClampedArray(w*h*4).fill(255)}),createImageData:(w,h)=>({data:new Uint8ClampedArray(w*h*4)})}}
 toBlob(cb,type,quality){assert.equal(type,'image/jpeg');qualityAttempts.push(quality);cb(new Blob([new Uint8Array(typeof blobBytes==='function'?blobBytes(quality):blobBytes)],{type}))}
}
const get=id=>{if(!elements.has(id))elements.set(id,new Element());return elements.get(id)};
let imageMode='ok';
class Image {constructor(){this.naturalWidth=240;this.naturalHeight=320}set src(v){this._src=v;const mode=imageMode;if(mode!=='stall')setImmediate(()=>mode==='error'?this.onerror?.():this.onload?.())}get src(){return this._src}}
let state='idle',saved=false,failShare=false,denyFinish=false,emptyScan=true,failScan=false;
let connectError='';
let avatarError='',avatarDeviceCode='ESP_FAIL';
let failCities=false,cityGate=null;let savedProfile={name:'测试',signature:'已有签名',address:'原地址',city:'自定义城市',lat:'22.3',lon:'113.6',avatar:true,share_image:true};let gallery=[{slot:0,bytes:12000}],blobBytes=20000,qualityAttempts=[];
const phoneWindow={__T:314159,scrollTo(){},addEventListener(){},isSecureContext:false,crypto:require('node:crypto').webcrypto,location:{origin:'http://192.168.4.1'}};
const phoneNavigator={};
class TestURL extends URL{static createObjectURL(){return 'blob:fixture'}static revokeObjectURL(){}}
const context=vm.createContext({document:{getElementById:get,createElement:()=>new Element()},window:phoneWindow,navigator:phoneNavigator,Option:class{constructor(text,value){this.text=text;this.value=value;this.dataset={}}},Image,TextEncoder,Uint8Array,Uint8ClampedArray,Blob,AbortController,URL:TestURL,URLSearchParams,setTimeout(fn,ms){const id=nextTimer++;timers.set(id,{fn,ms});return id},clearTimeout:id=>timers.delete(id),fetch:async(url,options)=>{
 requests.push({url,options});let result={},ok=true;
 if(url.startsWith('/api/cities')){if(cityGate)await cityGate;if(failCities)throw new Error('offline');result=cityCatalog;}
 if(url.startsWith('/api/profile')&&!options.method)result=savedProfile;
 if(url.startsWith('/api/avatar')&&options.method==='POST'&&avatarError){ok=false;result={error:avatarError,code:avatarDeviceCode};}
 if(url.startsWith('/api/setup'))result={state,saved_network:saved,saved_networks:saved?['Absent Wi-Fi','Test Wi-Fi']:[],ssid:'Test Wi-Fi'};
 if(url.startsWith('/api/connect')&&connectError){ok=false;result={error:connectError};}
 if(url.startsWith('/api/networks')){if(failScan)throw new Error('offline');result=emptyScan?[]:[{ssid:'Test Wi-Fi',secure:true}];}
 if(url.startsWith('/api/cards')){if(options.method==='POST'){const order=JSON.parse(options.body).order;gallery=order.map(slot=>gallery.find(c=>c.slot===slot));}else result={max:6,per_image_bytes:24576,storage_free:1900000,storage_reserve:65536,images:gallery.map(c=>({...c}))};}
 if(url.startsWith('/api/share')&&options.method==='POST'){if(failShare){ok=false;result={error:'store'}}else{const slot=Number(new URL('http://device'+url).searchParams.get('slot')||0);gallery=gallery.filter(c=>c.slot!==slot).concat({slot,bytes:options.body.size});}}
 if(url.startsWith('/api/finish')&&denyFinish){ok=false;result={error:'network_required'}}
 return {ok,json:async()=>result};
}});
vm.runInContext(script,context);
const flush=()=>new Promise(resolve=>setImmediate(resolve));
const posts=()=>requests.filter(r=>r.options.method==='POST');
(async()=>{
 await flush();await flush();
 assert.match(get('scanStatus').textContent,/Waiting for scan results/);
 const retry=[...timers.entries()].find(([,t])=>t.ms===1500);assert.ok(retry,'empty first result must schedule a retry');
 emptyScan=false;timers.delete(retry[0]);await retry[1].fn();assert.match(get('scanStatus').textContent,/Found 1/);
 get('network').value='Test Wi-Fi';failScan=true;await context.scan();assert.equal(get('network').value,'Test Wi-Fi','failed refresh preserves selection');
 failScan=false;await context.scan();
 await get('save').onclick();assert.equal(posts().length,0,'first boot cannot complete without Wi-Fi');
 get('network').value='Test Wi-Fi';get('password').value='short';await get('connect').onclick();assert.equal(posts().length,0);
 get('password').value='  secret with spaces  ';await get('connect').onclick();
 let req=posts().at(-1);assert.equal(req.url,'/api/connect?t=314159');assert.equal(JSON.parse(req.options.body).password,'  secret with spaces  ');
 state='failed';await context.poll();assert.equal(get('connect').disabled,false);assert.match(get('networkStatus').textContent,/Check the password/);
 await get('connect').onclick();state='connected';await context.poll();assert.equal(get('step2').hidden,false);assert.equal(get('password').value,'');
 get('name').value='新名字';
 get('file').files=[{size:1000}];await get('file').onchange();
 get('shareFile').files=[{size:1000}];await get('shareFile').onchange();
 failShare=true;await get('save').onclick();assert.equal(posts().filter(r=>r.url.startsWith('/api/finish')).length,0,'failed image must not close hotspot');assert.equal(get('save').disabled,false);
 req=posts().find(r=>r.url.startsWith('/api/profile'));let profile=JSON.parse(req.options.body);assert.equal(profile.name,'新名字');assert.equal(profile.address,'','address field removed');assert.equal(profile.city,'','city selection removed');assert.equal(profile.lat,'','device requires these keys as empty strings');assert.equal(profile.region,'');assert.equal(profile.radio_city,'');
 req=posts().find(r=>r.url.startsWith('/api/avatar'));assert.equal(req.options.body.byteLength,4976);assert.equal(req.options.body[4],3);
 // Failed avatar uploads retain the page and pending image for a later retry.
 for(const [code,message] of [['avatar_size',/length mismatch/],['avatar_recv',/transfer interrupted/],['avatar_store',/avatar is kept/]]){
  avatarError=code;const finishesBefore=posts().filter(r=>r.url.startsWith('/api/finish')).length;
  await get('save').onclick();assert.match(get('saveStatus').textContent,message);
  assert.equal(posts().filter(r=>r.url.startsWith('/api/finish')).length,finishesBefore);
  assert.equal(get('save').disabled,false);assert.equal(get('back').disabled,false);
 }
 avatarDeviceCode='ESP_ERR_NO_MEM';await get('save').onclick();assert.match(get('saveStatus').textContent,/Not enough device memory/);
 await assert.rejects(context.api('/api/avatar',new Uint8Array(4976)),error=>error.path==='/api/avatar'&&error.code==='avatar_store'&&error.deviceCode==='ESP_ERR_NO_MEM');
 avatarError='';avatarDeviceCode='ESP_FAIL';
 const writesBeforePreset=posts().length;
 for(const [id,preset] of [['portraitPresetDot',1],['portraitPresetFlat',2],['portraitPresetHybrid',3]]){get(id).checked=true;get(id).onchange();assert.equal(context.portraitPacked()[4],preset)}
 assert.equal(posts().length,writesBeforePreset,'changing presets only updates preview');
 failShare=false;denyFinish=true;await get('save').onclick();assert.match(get('saveStatus').textContent,/Connect to Wi-Fi before finishing/);
 denyFinish=false;await get('save').onclick();assert.equal(get('step3').hidden,false);assert.equal(get('doneName').textContent,'新名字');
 // A reload during verification resumes polling instead of allowing a duplicate request.
 state='connecting';saved=true;await context.load();assert.equal(get('connect').disabled,true);assert.equal(get('reuse').hidden,false);
 assert.equal(get('savedField').hidden,false);assert.equal(get('savedNetwork').disabled,true);
 state='failed';await context.poll();get('savedNetwork').value='Test Wi-Fi';
 await get('reuse').onclick();const timeBody=JSON.parse(posts().at(-1).options.body);
 assert.equal(timeBody.saved,true);assert.equal(timeBody.ssid,'Test Wi-Fi');
 assert.ok(Number.isInteger(timeBody.epoch)&&Math.abs(timeBody.epoch-Date.now()/1000)<5);
 assert.equal(timeBody.tz_minutes,-new Date().getTimezoneOffset() || 0);
 state='failed';await context.poll();get('savedNetwork').value='';connectError='saved_not_found';
 const beforeRejectedTimer=nextTimer;
 await get('reuse').onclick();assert.equal(get('reuse').disabled,false);assert.match(get('networkStatus').textContent,/No usable saved network/);
 assert.equal([...timers.entries()].some(([id,t])=>id>=beforeRejectedTimer&&t.fn===context.poll),false,'an explicit rejection must not start polling');
 connectError='';get('keepSaved').onclick();assert.equal(get('step2').hidden,false);
 assert.match(get('connectionSummary').textContent,/reconnects when finished/);
 // Both document-picker fallbacks use the same preview and binary save paths.
 get('fileFallback').files=[{size:1000}];await get('fileFallback').onchange();
 assert.equal(get('photoTools').hidden,false);assert.match(get('avatarStatus').textContent,/Selected/);
 get('shareFileFallback').files=[{size:1000}];await get('shareFileFallback').onchange();
 assert.equal(get('sharec').hidden,false);assert.match(get('shareStatus').textContent,/Selected/);
 const beforePickerPosts=posts().length;
 get('file').onclick();assert.match(get('avatarStatus').textContent,/Opening the picker/);
 get('file').oncancel();assert.match(get('avatarStatus').textContent,/Kept the previously selected/);
 get('shareFileFallback').oncancel();assert.match(get('shareStatus').textContent,/Kept the previously selected/);
 get('fileFallback').files=[{size:0}];await get('fileFallback').onchange();assert.match(get('avatarStatus').textContent,/file is empty/);
 get('fileFallback').files=[{size:21*1024*1024}];await get('fileFallback').onchange();assert.match(get('avatarStatus').textContent,/over 20 MB/);
 get('shareFileFallback').files=[{size:1000}];imageMode='error';await get('shareFileFallback').onchange();
 assert.match(get('shareStatus').textContent,/could not be read/);assert.equal(get('shareStatus').dataset.error,'true');
 imageMode='stall';get('fileFallback').files=[{size:1000}];
 const pendingImage=get('fileFallback').onchange();await get('save').onclick();
 assert.equal(posts().length,beforePickerPosts,'unreadable/loading images must not send settings or finish');
 assert.match(get('saveStatus').textContent,/still being read/);
 const imageTimeout=[...timers.values()].find(t=>t.ms===15000);assert.ok(imageTimeout);imageTimeout.fn();await pendingImage;
 assert.match(get('avatarStatus').textContent,/image timed out/);
 imageMode='ok';await get('fileFallback').onchange();
 assert.match(get('avatarStatus').textContent,/Selected/,'retry recovers after decode timeout');
 await get('save').onclick();assert.equal(get('step3').hidden,false);
 assert.equal(posts().filter(r=>r.url.startsWith('/api/avatar')).at(-1).options.body.byteLength,4976);
 assert.equal(posts().filter(r=>r.url.startsWith('/api/share')).at(-1).options.body.type,'image/jpeg');
 // Real gallery state: compressed blobs only, stable slots, order/delete and retry.
 await context.loadCards();
 get('shareFile').files=Array.from({length:6-gallery.length},()=>({size:1000}));await get('shareFile').onchange();
 assert.equal(vm.runInContext('cards.length',context),6);
 assert.equal(vm.runInContext('shareImg',context),null,'original bitmap released after compression');
 get('shareFile').files=[{size:1000}];await get('shareFile').onchange();assert.match(get('shareStatus').textContent,/Up to 6 images/);
 await context.saveCards();assert.equal(gallery.length,6);
 const original=gallery.map(c=>c.slot);context.moveCard(5,-1);await context.saveCards();assert.equal(gallery[4].slot,original[5]);
 context.removeCard(0);await context.saveCards();assert.equal(gallery.length,5);
 const replacing=gallery[0].slot;context.chooseReplacement(replacing);get('shareReplace').files=[{size:1000}];await get('shareReplace').onchange();
 const beforeRetry=posts().filter(r=>r.url.startsWith('/api/share')).length;failShare=true;await get('save').onclick();
 assert.equal(vm.runInContext('cards.find(c=>c.slot===cards[0].slot).blob.size',context),20000,'failed upload retains compressed retry data');
 failShare=false;await get('save').onclick();assert.equal(gallery.length,5);assert.equal(gallery[0].slot,replacing);
 assert.equal(posts().filter(r=>r.url.startsWith('/api/share')).length,beforeRetry+2);
 qualityAttempts=[];blobBytes=q=>q>.48?30000:24000;get('shareFile').files=[{size:1000}];await get('shareFile').onchange();
 assert.deepEqual(qualityAttempts,[.86,.78,.68,.58,.48]);assert.equal(vm.runInContext('cards.at(-1).bytes',context),24000);
 context.removeCard(5);blobBytes=30000;qualityAttempts=[];get('shareFile').files=[{size:1000}];await get('shareFile').onchange();
 assert.match(get('shareStatus').textContent,/still too large/);assert.equal(vm.runInContext('cards.length',context),5);
 assert.equal(qualityAttempts.length,7);
 // Native chooser contracts: batch inputs stay multiple; replacement is independent.
const batchTag=html.match(/<input[^>]*id="shareFile"[^>]*>/)[0];
assert.match(batchTag,/\bmultiple\b/);assert.ok(!/\baccept=|\bcapture=/.test(batchTag));
assert.match(html.match(/<input[^>]*id="shareFileFallback"[^>]*>/)[0],/\bmultiple\b/);
assert.ok(!/\bmultiple\b/.test(html.match(/<input[^>]*id="shareReplace"[^>]*>/)[0]));
// Starting batch addition after replacement selection/cancel must never overwrite it.
blobBytes=20000;await context.loadCards();context.chooseReplacement(gallery[0].slot);get('shareReplace').oncancel();
get('shareFile').onclick();get('shareFile').files=[{size:1000,type:'image/png'}];await get('shareFile').onchange();
assert.equal(vm.runInContext('cards.length',context),6,'batch adds a card instead of replacing an old one');
context.removeCard(5);get('shareFile').files=[{size:1000,type:'application/pdf'}];await get('shareFile').onchange();
assert.match(get('shareStatus').textContent,/documents and videos/);assert.equal(vm.runInContext('cards.length',context),5);
get('shareFileFallback').onclick();get('shareFileFallback').files=[{size:1000,type:'image/png'}];await get('shareFileFallback').onchange();
assert.equal(vm.runInContext('cards.length',context),6);
// A selected filename remains visible after processing; retry resets it on click.
context.removeCard(5);
get('shareFile').value='C:\\fakepath\\card.png';
get('shareFile').files=[{size:1000,type:'image/png'}];await get('shareFile').onchange();
assert.equal(get('shareFile').value,'C:\\fakepath\\card.png');
context.removeCard(5);get('shareFile').onclick();assert.equal(get('shareFile').value,'');
get('shareFile').files=[];await get('shareFile').onchange();
assert.match(get('shareStatus').textContent,/Kept the previously selected/);
// Reading and encoding are separate stages; stalled encoding can be retried.
const originalToBlob=get('sharec').toBlob;let lateEncode;
get('sharec').toBlob=cb=>{lateEncode=cb};
get('shareFile').value='C:\\fakepath\\card.png';get('shareFile').files=[{size:1000,type:'image/png'}];
const stalledEncoding=get('shareFile').onchange();await flush();await flush();
assert.match(get('shareStatus').textContent,/Compressing/);assert.equal(vm.runInContext('cardBusy',context),true);
const encodingTimeout=[...timers.values()].find(t=>t.ms===10000);assert.ok(encodingTimeout);
encodingTimeout.fn();await stalledEncoding;
assert.match(get('shareStatus').textContent,/compression timed out/);
assert.equal(vm.runInContext('cardBusy',context),false);assert.equal(vm.runInContext('shareImg',context),null);
assert.equal(get('shareFile').value,'C:\\fakepath\\card.png','timeout must not erase the selected filename');
assert.equal(vm.runInContext('cards.length',context),5);
lateEncode(new Blob([new Uint8Array(1000)],{type:'image/jpeg'}));await flush();
assert.equal(vm.runInContext('cards.length',context),5,'late encoding callback cannot add a card');
get('sharec').toBlob=originalToBlob;get('shareFile').onclick();
get('shareFile').files=[{size:1000,type:'image/png'}];await get('shareFile').onchange();
assert.equal(vm.runInContext('cards.length',context),6,'same file can be retried after encoding timeout');
context.removeCard(5);
// Reject null, unexpected MIME and thrown encoding errors without wedging the picker.
for(const encode of [cb=>cb(null),cb=>cb(new Blob(['png'],{type:'image/png'})),()=>{throw new Error('canvas unavailable')}]){
 get('sharec').toBlob=encode;await get('shareFile').onchange();
 assert.equal(get('shareStatus').dataset.error,'true');assert.equal(vm.runInContext('cardBusy',context),false);
 assert.equal(vm.runInContext('cards.length',context),5);
}
get('sharec').toBlob=originalToBlob;imageMode='stall';
const stalledReading=get('shareFile').onchange();await flush();
assert.match(get('shareStatus').textContent,/Reading 1/);
const readingTimeout=[...timers.values()].find(t=>t.ms===15000);assert.ok(readingTimeout);
readingTimeout.fn();await stalledReading;imageMode='ok';
assert.match(get('shareStatus').textContent,/image timed out/);assert.equal(vm.runInContext('cardBusy',context),false);
assert.ok(html.indexOf('id="shareStatus"')<html.indexOf('id="cardList"'),'picker feedback precedes the gallery');
console.log('Image feedback: filename preservation, distinct decode/encode stages, encoding timeout/retry/late callback, MIME/null/exception recovery: PASS');
console.log('Multiselect: unfiltered multiple file input, multiple photo alternative, separate single replacement, cancelled replacement isolation and non-image rejection: PASS');
 console.log('Card gallery: six-image limit, sequential compression, released original bitmaps, quality ladder, reject oversized, stable slots, reorder/delete and partial-save retry: PASS');
 console.log('Phone setup: Wi-Fi/images, profile keys always sent, portrait presets, card gallery and save flow: PASS');
})().catch(e=>{console.error(e);process.exitCode=1});
