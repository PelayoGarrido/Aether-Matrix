'use strict';
const pad = document.querySelector('#xy-pad');
const cursor = document.querySelector('#matrix-cursor');
const xControl = document.querySelector('#matrix-x');
const yControl = document.querySelector('#matrix-y');
const wave = document.querySelector('#wave');
const lines = document.querySelector('#connections');
const play = document.querySelector('#play-demo');
const status = document.querySelector('#demo-status');
const anchors = [[.15,.85],[.85,.85],[.5,.5],[.15,.15],[.85,.15]];
const reduced = matchMedia('(prefers-reduced-motion: reduce)');
let x = .5, y = .5, weights = [], frame = 0, animation = 0, lastFrame = 0;
let context = null, timer = null, running = false, nextNote = 0, step = 0, audioBusy = false;
function updateMatrix(nx, ny) {
  x = Math.max(0, Math.min(1, Number.isFinite(nx) ? nx : .5));
  y = Math.max(0, Math.min(1, Number.isFinite(ny) ? ny : .5));
  xControl.value = Math.round(x * 100); yControl.value = Math.round(y * 100);
  cursor.style.left = `${x * 100}%`; cursor.style.top = `${(1-y)*100}%`;
  weights = anchors.map(([ax,ay]) => 1 / ((x-ax)**2+(y-ay)**2+.025)**2);
  const total = weights.reduce((a,b) => a+b, 0); weights = weights.map(w => w/total);
  weights.forEach((w,i) => { document.querySelector(`#weight-${i}`).textContent = `${Math.round(w*100)}%`; });
  lines.setAttribute('d', anchors.map(([ax,ay]) => `M${ax*500} ${(1-ay)*330}L${x*500} ${(1-y)*330}`).join(''));
  drawWave();
}
function movePointer(event) {
  const r = pad.getBoundingClientRect();
  updateMatrix((event.clientX-r.left)/r.width, 1-(event.clientY-r.top)/r.height);
}
pad.addEventListener('pointerdown', event => {
  if (event.target.closest('button') || event.button !== 0) return;
  pad.setPointerCapture(event.pointerId); movePointer(event);
});
pad.addEventListener('pointermove', event => { if (pad.hasPointerCapture(event.pointerId)) movePointer(event); });
for (const node of document.querySelectorAll('.style-node'))
  node.addEventListener('click', () => updateMatrix(Number(node.dataset.x), Number(node.dataset.y)));
xControl.addEventListener('input', () => updateMatrix(Number(xControl.value)/100, y));
yControl.addEventListener('input', () => updateMatrix(x, Number(yControl.value)/100));
for (const name of ['density', 'tension']) {
  const input = document.querySelector(`#${name}`);
  input.addEventListener('input', () => { document.querySelector(`#${name}-value`).textContent = `${input.value}%`; drawWave(); });
}
function drawWave() {
  const tension = Number(document.querySelector('#tension').value)/100;
  let path = '';
  for (let i=0; i<=500; i+=2) {
    const envelope = Math.sin(i/500*Math.PI)**2;
    const signal = Math.sin(i*.12+frame*.018)*(12+y*15) + Math.sin(i*(.23+x*.08)-frame*.011)*(4+tension*10);
    path += `${i===0?'M':'L'}${i} ${(165+signal*envelope).toFixed(2)}`;
  }
  wave.setAttribute('d', path);
}
function animate(now) {
  if (document.hidden || reduced.matches || !padVisible) { animation = 0; return; }
  if (now-lastFrame>40) { frame++; drawWave(); lastFrame=now; }
  animation = requestAnimationFrame(animate);
}
let padVisible = true;
const observer = new IntersectionObserver(entries => {
  padVisible = entries[0].isIntersecting;
  if (padVisible && !animation && !reduced.matches) animation = requestAnimationFrame(animate);
}, {rootMargin:'80px'});
observer.observe(pad);
reduced.addEventListener('change', () => {
  if (reduced.matches && animation) { cancelAnimationFrame(animation); animation=0; }
  else if (!animation && padVisible && !document.hidden) animation=requestAnimationFrame(animate);
});
function scheduleNote(frequency, time, duration, type, volume) {
  const oscillator = context.createOscillator();
  const gain = context.createGain();
  const filter = context.createBiquadFilter();
  oscillator.type=type; oscillator.frequency.setValueAtTime(frequency,time);
  filter.type='lowpass'; filter.frequency.value=700+x*4500; filter.Q.value=.7;
  gain.gain.setValueAtTime(0,time);
  gain.gain.linearRampToValueAtTime(volume,time+.015);
  gain.gain.exponentialRampToValueAtTime(.0001,time+duration);
  oscillator.connect(filter); filter.connect(gain); gain.connect(context.destination);
  oscillator.start(time); oscillator.stop(time+duration+.02);
  oscillator.onended=()=>{ oscillator.disconnect(); filter.disconnect(); gain.disconnect(); };
}
function schedule() {
  if (!context || !running) return;
  const scale=[0,2,3,5,7,8,10], density=Number(document.querySelector('#density').value)/100;
  while (nextNote<context.currentTime+.15) {
    const degree=scale[(step+Math.floor(x*4))%7], frequency=440*2**((57+degree+(y>.6?12:0)-69)/12);
    if (step%2===0 || density>.7)
      scheduleNote(frequency,nextNote,.26+y*.2,weights[3]>.3?'sawtooth':'triangle',.055);
    if (step%4===0) {
      scheduleNote(110,nextNote,.42,'sine',.08);
      scheduleNote(220,nextNote,.4,'sine',.025);
      scheduleNote(329.63,nextNote,.4,'sine',.018);
    }
    step=(step+1)%64;nextNote+=.125;
  }
}
async function stopAudio() {
  running=false;clearInterval(timer);timer=null;
  document.body.classList.remove('playing');
  play.setAttribute('aria-pressed','false');
  document.querySelector('#play-icon').textContent='▶';
  document.querySelector('#play-label').textContent='Play web sketch';
  status.textContent='CLICK TO LISTEN';
  const previous=context;context=null;
  if (previous && previous.state!=='closed') await previous.close();
}
play.addEventListener('click', async () => {
  if (audioBusy) return;
  audioBusy=true;play.disabled=true;
  try {
    if (running) { await stopAudio(); return; }
    const AudioEngine=window.AudioContext || window.webkitAudioContext;
    if (!AudioEngine) { status.textContent='AUDIO UNAVAILABLE'; return; }
    context=new AudioEngine({latencyHint:'interactive'});await context.resume();
    if (document.hidden) { await stopAudio(); return; }
    running=true;step=0;nextNote=context.currentTime+.04;
    document.body.classList.add('playing');
    play.setAttribute('aria-pressed','true');
    document.querySelector('#play-icon').textContent='Ⅱ';
    document.querySelector('#play-label').textContent='Stop sketch';
    status.textContent='WEB SYNTH · 120 BPM';schedule();timer=setInterval(schedule,60);
  } catch { await stopAudio();status.textContent='AUDIO UNAVAILABLE'; }
  finally { audioBusy=false;play.disabled=false; }
});
document.addEventListener('visibilitychange', () => {
  if (document.hidden) { if (animation) cancelAnimationFrame(animation);animation=0;void stopAudio(); }
  else if (!animation && !reduced.matches && padVisible) animation=requestAnimationFrame(animate);
});
window.addEventListener('pagehide', () => { void stopAudio(); });
updateMatrix(.5,.5);
