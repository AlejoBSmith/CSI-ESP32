/* Spatial response heuristic, not a range measurement or target counter. */
const ActivityMapMath = (() => {
  const receivers=['rx1','rx2','rx3'];
  const point=p=>p&&Number.isFinite(p.x)&&Number.isFinite(p.y);
  function estimate(layout,states,minimum=1){
    const links=[];
    for(const id of receivers){
      const s=states[id],p=layout.nodes[id];
      if(!s||!point(p)||!point(layout.nodes.tx))continue;
      const usable=(s.measurement_valid??['ACTIVE','CLEAR'].includes(s.state))&&Number.isFinite(s.score)&&(s._feature_age_s??0)<1.5;
      const calibrated=s.amplitude_calibrated===true;
      const weight=usable?Math.max(0,s.score-minimum)/Math.max(.01,p.sensitivity??1):0;
      links.push({id,score:s.score,weight,usable,calibrated,position:p});
    }
    const good=links.filter(l=>l.usable&&l.calibrated),sum=good.reduce((s,l)=>s+l.weight,0);
    let center=null;
    const distinct=good.some((a,i)=>good.slice(i+1).some(b=>Math.hypot(a.position.x-b.position.x,a.position.y-b.position.y)>.01));
    if(layout.confirmed&&good.length>=2&&distinct&&sum>0){
      center={x:good.reduce((s,l)=>s+l.weight*l.position.x,0)/sum,y:good.reduce((s,l)=>s+l.weight*l.position.y,0)/sum};
    }
    return {links,center};
  }
  return {estimate};
})();
if(typeof module!=='undefined')module.exports=ActivityMapMath;
if(typeof window!=='undefined')window.ActivityMap=(()=>{
  const colors={rx1:'#68c5fd',rx2:'#f4b66e',rx3:'#a4e384',tx:'#edf3f8'};
  const defaults=()=>({width:4,height:3,confirmed:false,nodes:{tx:{x:0.3,y:1.5},rx1:{x:3.5,y:.4,sensitivity:1},rx2:{x:3.5,y:1.5,sensitivity:1},rx3:{x:3.5,y:2.6,sensitivity:1}}});
  let layout=defaults(),states={},drag=null,peak=1;
  const el=id=>document.getElementById(id);
  function valid(v){return v&&v.width>0&&v.width<=100&&v.height>0&&v.height<=100&&Object.keys(defaults().nodes).every(n=>v.nodes?.[n]&&Number.isFinite(v.nodes[n].x)&&Number.isFinite(v.nodes[n].y));}
  function controls(){el('mapwidth').value=layout.width;el('mapheight').value=layout.height;const p=layout.nodes[el('mapnode').value];el('mapx').value=p.x.toFixed(2);el('mapy').value=p.y.toFixed(2);el('mapsensitivity').value=p.sensitivity??1;}
  function save(){
    layout.confirmed=true;try{localStorage.setItem('rat-csi-layout-v1',JSON.stringify(layout));}catch(e){el('mapnote').textContent=e.message;}
    if(typeof act==='function')act({action:'metadata',setup:{map_layout:layout}});
    draw();
  }
  function frame(){const c=el('map'),w=c.clientWidth,h=340,dpr=devicePixelRatio||1;
    if(c.width!==Math.round(w*dpr)||c.height!==Math.round(h*dpr)){c.width=Math.round(w*dpr);c.height=Math.round(h*dpr)}
    const x=c.getContext('2d');x.setTransform(dpr,0,0,dpr,0,0);
    const scale=Math.min((w-70)/layout.width,(h-60)/layout.height),left=(w-scale*layout.width)/2,top=20;
    return {c,x,w,h,scale,left,top,px:p=>[left+p.x*scale,top+(layout.height-p.y)*scale]};
  }
  function draw(){
    const f=frame(),{c,x,w,h,scale,left,top,px}=f;x.clearRect(0,0,w,h);
    const result=ActivityMapMath.estimate(layout,states,Number(el('mapminimum').value)||0);
    x.font='12px system-ui';x.lineWidth=1;x.strokeStyle='#385164';x.fillStyle='#a8bdce';
    x.strokeRect(left,top,layout.width*scale,layout.height*scale);
    for(let i=0;i<=4;i++){
      let a=left+layout.width*scale*i/4,b=top+layout.height*scale*i/4;
      x.beginPath();x.moveTo(a,top);x.lineTo(a,top+layout.height*scale);x.moveTo(left,b);x.lineTo(left+layout.width*scale,b);x.stroke();
      x.fillText((layout.width*i/4).toFixed(1),a-8,top+layout.height*scale+18);
      x.fillText((layout.height*(1-i/4)).toFixed(1),left-28,b+4);
    }
    x.fillText('m',left+layout.width*scale+6,top+layout.height*scale+18);
    const tx=px(layout.nodes.tx);
    for(const l of result.links){
      const p=px(l.position);if(l.usable)peak=Math.max(peak,l.weight);
      const level=l.usable?Math.min(1,l.weight/peak):0;
      x.strokeStyle=colors[l.id];x.globalAlpha=l.usable ? .2+.8*level : .12;x.lineWidth=2+5*level;
      x.setLineDash(l.calibrated?[]:[5,5]);x.beginPath();x.moveTo(...tx);x.lineTo(...p);x.stroke();x.setLineDash([]);
      if(l.usable&&level>0){x.globalAlpha=.1+.4*level;x.fillStyle=colors[l.id];x.beginPath();x.arc(...p,12+35*level,0,Math.PI*2);x.fill();}
      x.globalAlpha=1;
    }
    for(const [id,p] of Object.entries(layout.nodes)){
      const [a,b]=px(p);x.fillStyle=colors[id];x.globalAlpha=id==='tx'||states[id]?1:.3;
      x.beginPath();x.arc(a,b,7,0,Math.PI*2);x.fill();x.globalAlpha=1;x.fillText(id.toUpperCase(),Math.min(w-35,a+10),b-9);
    }
    const center=el('mapcenter').checked?result.center:null;
    if(center){const [a,b]=px(center);x.strokeStyle='#fff';x.lineWidth=2;x.beginPath();x.arc(a,b,9,0,2*Math.PI);x.moveTo(a-13,b);x.lineTo(a+13,b);x.moveTo(a,b-13);x.lineTo(a,b+13);x.stroke();}
    const lengths=result.links.map(l=>l.id.toUpperCase()+': TX-RX '+Math.hypot(l.position.x-layout.nodes.tx.x,l.position.y-layout.nodes.tx.y).toFixed(2)+' m').join(' | ');
    let note=layout.confirmed?'Geometria guardada. ':'Posiciones de ejemplo: ajusta y guarda el montaje. ';
    note+=lengths;
    if(center){note+=' | Centro ponderado ('+center.x.toFixed(2)+', '+center.y.toFixed(2)+') m; distancia geometrica al TX '+Math.hypot(center.x-layout.nodes.tx.x,center.y-layout.nodes.tx.y).toFixed(2)+' m.';}
    else if(el('mapcenter').checked){note+=' | Sin estimacion de centro: requiere montaje guardado y al menos dos enlaces validos con referencia y actividad.';}
    el('mapnote').textContent=note;c.dataset.center=center?'visible':'none';
  }
  function init(){
    try{const saved=JSON.parse(localStorage.getItem('rat-csi-layout-v1'));if(valid(saved))layout=saved;}catch{}
    controls();el('mapnode').addEventListener('change',controls);
    el('mapapply').addEventListener('click',()=>{
      const w=Number(el('mapwidth').value),h=Number(el('mapheight').value),x=Number(el('mapx').value),y=Number(el('mapy').value),s=Number(el('mapsensitivity').value);
      if(!(w>0&&w<=100&&h>0&&h<=100&&x>=0&&x<=w&&y>=0&&y<=h&&s>0)){el('mapnote').textContent='Revisa dimensiones y coordenadas.';return;}
      layout.width=w;layout.height=h;layout.nodes[el('mapnode').value]={x,y,sensitivity:s};
      for(const p of Object.values(layout.nodes)){p.x=Math.min(w,p.x);p.y=Math.min(h,p.y);}save();
    });
    el('mapcenter').addEventListener('change',draw);el('mapminimum').addEventListener('input',draw);
    el('map').addEventListener('pointerdown',e=>{
      const f=frame(),rect=f.c.getBoundingClientRect();const mx=e.clientX-rect.left,my=e.clientY-rect.top;
      drag=Object.keys(layout.nodes).find(id=>{const [x,y]=f.px(layout.nodes[id]);return Math.hypot(x-mx,y-my)<22});
      if(drag){f.c.setPointerCapture(e.pointerId);el('mapnode').value=drag;controls();}
    });
    el('map').addEventListener('pointermove',e=>{
      if(!drag)return;const f=frame(),rect=f.c.getBoundingClientRect();
      layout.nodes[drag].x=Math.max(0,Math.min(layout.width,(e.clientX-rect.left-f.left)/f.scale));
      layout.nodes[drag].y=Math.max(0,Math.min(layout.height,layout.height-(e.clientY-rect.top-f.top)/f.scale));controls();draw();
    });
    el('map').addEventListener('pointerup',()=>{if(drag){drag=null;save();}});
    el('map').addEventListener('pointercancel',()=>{drag=null;});draw();
  }
  return {init,update:s=>{states=s;draw();}};
})();
