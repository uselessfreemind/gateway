'use strict';
// Deliberately local simulation: no PLC writes, alarm acknowledgements, or fabricated live connectivity.
const $ = id => document.getElementById(id);
const tags={BG:['slm',0],CDA:['slm',0],SPR2:['mbar',0],SFS1:['l/min',0],SFS2:['l/min',0],SFS3:['l/min',0],STS2:['°C',0],STS3:['°C',0]};
function row(id){return `<div class="value-row"><span>${id}</span><output data-tag="${id}">0.0 ${tags[id][0]}</output></div>`;}
$('supply-values').innerHTML=['SPR2','BG','CDA'].map(row).join('');
$('liquid-values').innerHTML=['SFS1','SFS2','SFS3','STS2'].map(row).join('');
$('mfc-values').innerHTML=['BG','CDA'].map(id=>`<div class="mfc-row"><span>${id}</span><output>0.0 slm</output><output data-tag="${id}">0.0 slm</output></div>`).join('');
$('pressure-values').innerHTML=row('SPR2');
$('flow-values').innerHTML=['SFS1','SFS2','SFS3'].map(row).join('')+'<div class="value-row"><span>SFS4</span><output class="unconfigured">not configured</output></div>';
$('temperature-values').innerHTML=['STS2','STS3'].map(row).join('');
let currentPage=1;
function screen(name){$('overview').hidden=name!=='overview';$('trend-view').hidden=name!=='trend';$('alarm-panel').hidden=name!=='alarms';if(name==='trend')drawCharts();}
function tab(n){currentPage=n;for(let i=1;i<=3;i++){$('page'+i).hidden=i!==n;$('tab'+i).setAttribute('aria-selected',String(i===n));$('tab'+i).tabIndex=i===n?0:-1;}drawCharts();}
$('trend').onclick=()=>{screen('trend');tab(1);$('tab1').focus();};
$('home').onclick=$('back').onclick=()=>screen('overview');$('alarms').onclick=()=>screen('alarms');
for(let n=1;n<=3;n++){$('tab'+n).onclick=()=>tab(n);$('tab'+n).onkeydown=e=>{let next;if(e.key==='ArrowRight')next=n%3+1;if(e.key==='ArrowLeft')next=(n+1)%3+1;if(e.key==='Home')next=1;if(e.key==='End')next=3;if(next){e.preventDefault();tab(next);$('tab'+next).focus();}};}
function chart(id,series,unit,max){const canvas=$(id);if(!canvas.clientWidth)return;const ratio=window.devicePixelRatio||1,w=canvas.clientWidth,h=canvas.clientHeight;canvas.width=w*ratio;canvas.height=h*ratio;const c=canvas.getContext('2d');c.scale(ratio,ratio);const l=55,r=20,t=26,b=35;c.font='11px Arial';c.fillStyle='#486675';c.fillText(unit,10,16);for(let i=0;i<=4;i++){const y=t+(h-t-b)*i/4;c.strokeStyle='#dce6ec';c.beginPath();c.moveTo(l,y);c.lineTo(w-r,y);c.stroke();c.fillText((max*(1-i/4)).toFixed(1),8,y+4);}for(let i=0;i<=4;i++){const x=l+(w-l-r)*i/4;c.fillText(i===4?'Now':`−${(4-i)*15}s`,x-12,h-12);}series.forEach((s,j)=>{c.strokeStyle=s[1];c.lineWidth=2;c.beginPath();for(let i=0;i<=120;i++){const v=max*(.38+j*.15+.08*Math.sin(i/10+j)+.025*Math.cos(i/3));const x=l+(w-l-r)*i/120,y=t+(h-t-b)*(1-v/max);i?c.lineTo(x,y):c.moveTo(x,y);}c.stroke();});}
function drawCharts(){if($('trend-view').hidden)return;if(currentPage===2)chart('flow-chart',[['SFS1','#007b91'],['SFS2','#d18b00'],['SFS3','#8254bb']],'l/min',20);if(currentPage===3){chart('pressure-chart',[['SPR2','#007b91']],'mbar',10);chart('temperature-chart',[['STS2','#d18b00'],['STS3','#8254bb']],'°C',60);}}
function clock(){const now=new Date();$('clock').textContent=now.toLocaleString();$('updated').textContent='Viewer clock · '+now.toLocaleTimeString();}clock();setInterval(clock,1000);window.addEventListener('resize',drawCharts);tab(1);
