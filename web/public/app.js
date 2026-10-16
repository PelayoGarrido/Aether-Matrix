'use strict';
const motion = window.matchMedia('(prefers-reduced-motion: reduce)');
const saveData = Boolean(navigator.connection?.saveData);
const tabs = [...document.querySelectorAll('[role="tab"]')];
const views = {
  blend: ['Find the space\nbetween styles.', 'Hip-hop, classical, pop, electronic and cinematic. Move across the display to blend their synthesis voices, or use the two side faders.', 'Close view of the real style blend display'],
  shape: ['Put your hands\non the sound.', 'Four dedicated controls for rhythmic density, harmonic tension, note articulation and acoustic space. Set a key and scale. Play MIDI notes or follow your host transport.', 'Close view of the actual metallic Density and Tension controls'],
  export: ['Take the idea\ninto your track.', 'Generate a four-bar stereo WAV master and multitrack MIDI. Drag the finished files into your DAW. Keep the audio, edit the notes and make it your own.', 'Close view of the actual WAV and MIDI export controls']
};
function selectTab(tab, focus = false) {
  const view = tab.dataset.view;
  if (!Object.hasOwn(views, view)) return;
  tabs.forEach(item => {
    const selected = item === tab;
    item.setAttribute('aria-selected', String(selected));
    item.tabIndex = selected ? 0 : -1;
  });
  document.querySelector('.detail-view').dataset.view = view;
  document.querySelector('#detail-image').alt = views[view][2];
  document.querySelector('#feature-title').textContent = views[view][0];
  document.querySelector('#feature-description').textContent = views[view][1];
  document.querySelector('#feature-panel').setAttribute('aria-labelledby', tab.id);
  if (focus) tab.focus();
}
tabs.forEach((tab, index) => {
  tab.addEventListener('click', () => selectTab(tab));
  tab.addEventListener('keydown', event => {
    let next;
    if (event.key === 'ArrowRight') next = (index + 1) % tabs.length;
    if (event.key === 'ArrowLeft') next = (index + tabs.length - 1) % tabs.length;
    if (event.key === 'Home') next = 0;
    if (event.key === 'End') next = tabs.length - 1;
    if (next !== undefined) { event.preventDefault(); selectTab(tabs[next], true); }
  });
});
const audio = document.querySelector('#sample');
const audioButton = document.querySelector('#sample-button');
const status = document.querySelector('#audio-status');
const progress = document.querySelector('#audio-progress');
audioButton.disabled = false;
document.documentElement.classList.add('enhanced');
audioButton.addEventListener('click', async () => {
  if (!audio.paused) { audio.pause(); return; }
  try { await audio.play(); }
  catch { status.textContent = 'Playback unavailable. Try the audio controls below.'; document.documentElement.classList.remove('enhanced'); }
});
function audioState() {
  audioButton.textContent = audio.paused ? 'Play four-bar phrase' : 'Pause phrase';
  audioButton.setAttribute('aria-pressed', String(!audio.paused));
  status.textContent = audio.paused ? 'Rendered in Aether Matrix.' : 'Playing the native hybrid engine render.';
}
for (const event of ['play', 'pause', 'ended']) audio.addEventListener(event, audioState);
audio.addEventListener('timeupdate', () => { progress.style.width = `${Number.isFinite(audio.duration) ? 100 * audio.currentTime / audio.duration : 0}%`; });
audio.addEventListener('error', () => { status.textContent = 'The sample could not be loaded.'; document.documentElement.classList.remove('enhanced'); });
const film = document.querySelector('#studio-video');
const filmButton = document.querySelector('#film-button');
let filmVisible = false;
let wantsFilm = !motion.matches && !saveData;
function filmState() {
  filmButton.textContent = film.paused ? 'Play background film' : 'Pause background film';
  filmButton.setAttribute('aria-pressed', String(!film.paused));
}
async function updateFilm() {
  if (!wantsFilm || !filmVisible || document.hidden) { film.pause(); filmState(); return; }
  if (!film.getAttribute('src')) film.src = film.dataset.src;
  try { await film.play(); } catch { filmState(); }
}
filmButton.addEventListener('click', () => { wantsFilm = film.paused; updateFilm(); });
for (const event of ['play', 'pause', 'error']) film.addEventListener(event, filmState);
const filmObserver = new IntersectionObserver(entries => {
  filmVisible = entries[0].isIntersecting;
  updateFilm();
}, { threshold: 0.12 });
filmObserver.observe(document.querySelector('.studio-film'));
document.addEventListener('visibilitychange', () => { if (document.hidden) audio.pause(); updateFilm(); });
motion.addEventListener('change', () => { if (motion.matches) wantsFilm = false; updateFilm(); updateScroll(); });
// Native browser animation: one entrance and scroll-controlled product perspective.
// No animation library, repeating render loop or third-party request is required.
if (!motion.matches) {
  document.querySelectorAll('.hero-copy h1 span, .hero-copy p, .hero-copy .actions').forEach((element, index) => {
    element.animate([{ opacity: 0, transform: 'translateY(32px)' }, { opacity: 1, transform: 'translateY(0)' }],
      { duration: 850, delay: index * 120, easing: 'cubic-bezier(.2,.7,.2,1)', fill: 'backwards' });
  });
}
const revealObserver = new IntersectionObserver(entries => {
  for (const entry of entries) {
    if (!entry.isIntersecting) continue;
    revealObserver.unobserve(entry.target);
    if (!motion.matches) entry.target.animate([{ opacity: 0, transform: 'translateY(35px)' }, { opacity: 1, transform: 'translateY(0)' }], { duration: 900, easing: 'cubic-bezier(.2,.7,.2,1)' });
  }
}, { threshold: 0.1 });
document.querySelectorAll('.section-intro, .feature-layout, .listen-copy, .listening-room, .download>div, .faq>div').forEach(element => revealObserver.observe(element));
let scrollFrame = 0;
const product = document.querySelector('.product-crop');
function updateScroll() {
  cancelAnimationFrame(scrollFrame);
  scrollFrame = requestAnimationFrame(() => {
    if (motion.matches || window.innerWidth <= 700) { product.style.transform = 'none'; return; }
    const rect = product.getBoundingClientRect();
    if (rect.bottom < 0 || rect.top > window.innerHeight) return;
    const phase = Math.max(0, Math.min(1, 1 - rect.top / window.innerHeight));
    product.style.transform = `rotateX(${7 * (1 - phase)}deg) translateY(${14 * (1 - phase)}px)`;
  });
}
window.addEventListener('scroll', updateScroll, { passive: true });
window.addEventListener('resize', updateScroll, { passive: true });
updateScroll();
