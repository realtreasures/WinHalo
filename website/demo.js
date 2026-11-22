const demo = document.querySelector("#demo");
const desktop = demo.querySelector(".desktop");
const island = demo.querySelector(".island");
const wake = demo.querySelector(".wake");
const compactTrack = demo.querySelector(".compact-track");
const satellite = demo.querySelector(".satellite");
const player = demo.querySelector(".player");
const playerSource = demo.querySelector(".player-source");
const toggle = demo.querySelector(".play-toggle");
const progress = demo.querySelector("progress");
const elapsed = demo.querySelector(".elapsed");
const duration = demo.querySelector(".duration");
const status = demo.querySelector("#demo-status");
const hint = demo.querySelector("#demo-hint");
const stateButtons = [...demo.querySelectorAll("[data-mode]")];

const sources = [
  { label: "A", artist: "北岸电台", playing: true, track: 0, position: 42,
    tracks: [{ title: "蓝色轨道", length: 208 }, { title: "窗外有风", length: 193 }] },
  { label: "B", artist: "午夜放映室", playing: false, track: 0, position: 13,
    tracks: [{ title: "折叠的海", length: 221 }, { title: "夜行灯", length: 181 }] }
];
const hints = {
  idle: "鼠标滑过或点击待机胶囊，唤出音乐信息。",
  compact: "点击主岛展开播放器，点击右侧圆岛切换软件。",
  expanded: "可以切歌或播放、暂停。点击桌面空白处收起。",
  notice: "提示结束后会自动恢复原来的状态。"
};
const notices = {
  volume: { icon: "♫", title: "音量已调整", copy: "系统音量 45%", delay: 2200 },
  battery: { icon: "!", title: "电量较低", copy: "剩余电量 18%", delay: 5000 },
  message: { icon: "···", title: "新消息", copy: "示例联系人 · 文件已发给你", delay: 5000 }
};
let sourceIndex = 0;
let mode = "compact";
let returnMode = mode;
let idleTimer;
let noticeTimer;
let playbackTimer;
let pointerInside = false;

function timeLabel(seconds) {
  return Math.floor(seconds / 60) + ":" + String(seconds % 60).padStart(2, "0");
}

function updateTimeline() {
  const source = sources[sourceIndex];
  progress.value = source.position;
  elapsed.textContent = timeLabel(source.position);
}

function refreshTimer() {
  clearInterval(playbackTimer);
  if (mode !== "expanded" || document.hidden || !sources[sourceIndex].playing) return;
  playbackTimer = setInterval(() => {
    const source = sources[sourceIndex];
    source.position += 1;
    if (source.position >= source.tracks[source.track].length) {
      changeTrack(1);
      return;
    }
    updateTimeline();
  }, 1000);
}

function updateMedia() {
  const source = sources[sourceIndex];
  const other = sources[1 - sourceIndex];
  const track = source.tracks[source.track];
  island.dataset.source = String(sourceIndex);
  island.dataset.playing = String(source.playing);
  demo.querySelectorAll("[data-track]").forEach(element => { element.textContent = track.title; });
  demo.querySelectorAll("[data-artist]").forEach(element => { element.textContent = source.artist; });
  satellite.querySelector(".source-glyph").textContent = other.label;
  playerSource.querySelector(".source-glyph").textContent = source.label;
  const sourceLabel = "切换到播放器 " + other.label;
  satellite.setAttribute("aria-label", sourceLabel);
  playerSource.setAttribute("aria-label", sourceLabel);
  toggle.setAttribute("aria-label", source.playing ? "暂停" : "播放");
  demo.querySelector(".play-status").textContent = source.playing ? "播放中" : "已暂停";
  progress.max = track.length;
  duration.textContent = timeLabel(track.length);
  updateTimeline();
  refreshTimer();
}

function setMode(nextMode, focusTarget) {
  clearTimeout(idleTimer);
  clearTimeout(noticeTimer);
  mode = nextMode;
  island.dataset.state = mode;
  hint.textContent = hints[mode];
  wake.inert = mode !== "idle";
  compactTrack.inert = satellite.inert = mode !== "compact";
  player.inert = mode !== "expanded";
  stateButtons.forEach(button => {
    button.setAttribute("aria-pressed", String(button.dataset.mode === mode));
  });
  refreshTimer();
  focusTarget?.focus({ preventScroll: true });
}

function scheduleIdle() {
  clearTimeout(idleTimer);
  if (mode === "compact" && !pointerInside && !island.querySelector(":focus-visible")) {
    idleTimer = setTimeout(() => setMode("idle"), 5000);
  }
}

function switchSource() {
  sourceIndex = 1 - sourceIndex;
  updateMedia();
  const source = sources[sourceIndex];
  status.textContent = "已切换到播放器 " + source.label + "，" + source.tracks[source.track].title;
}

function changeTrack(step) {
  const source = sources[sourceIndex];
  source.track = (source.track + step + source.tracks.length) % source.tracks.length;
  source.position = 0;
  updateMedia();
  status.textContent = source.tracks[source.track].title;
}

function showNotice(key) {
  const notice = notices[key];
  if (mode !== "notice") returnMode = mode;
  demo.querySelector(".notice-icon").textContent = notice.icon;
  demo.querySelector(".notice-title").textContent = notice.title;
  demo.querySelector(".notice-copy").textContent = notice.copy;
  setMode("notice");
  status.textContent = notice.title + "，" + notice.copy;
  noticeTimer = setTimeout(() => setMode(returnMode), notice.delay);
}

wake.addEventListener("click", () => setMode("compact", compactTrack));
compactTrack.addEventListener("click", () => setMode("expanded", toggle));
satellite.addEventListener("click", switchSource);
playerSource.addEventListener("click", switchSource);
player.querySelector("[data-command='previous']").addEventListener("click", () => changeTrack(-1));
player.querySelector("[data-command='next']").addEventListener("click", () => changeTrack(1));
toggle.addEventListener("click", () => {
  const source = sources[sourceIndex];
  source.playing = !source.playing;
  updateMedia();
  status.textContent = source.playing ? "示例播放已恢复" : "示例播放已暂停";
});

stateButtons.forEach(button => {
  button.addEventListener("click", () => setMode(button.dataset.mode));
});
document.querySelectorAll("[data-preview]").forEach(link => {
  link.addEventListener("click", () => {
    if (link.dataset.preview === "expanded") {
      setMode("expanded");
    } else {
      showNotice(link.dataset.preview);
    }
  });
});
island.addEventListener("pointerenter", event => {
  if (event.pointerType !== "mouse") return;
  pointerInside = true;
  clearTimeout(idleTimer);
  if (mode === "idle") {
    const focusTarget = document.activeElement === wake ? compactTrack : undefined;
    setMode("compact", focusTarget);
  }
});
island.addEventListener("pointerleave", () => {
  pointerInside = false;
  scheduleIdle();
});
island.addEventListener("focusin", () => clearTimeout(idleTimer));
island.addEventListener("focusout", () => queueMicrotask(scheduleIdle));
desktop.addEventListener("click", event => {
  if (mode === "expanded" && !island.contains(event.target)) {
    setMode("compact");
    scheduleIdle();
  }
});
demo.addEventListener("keydown", event => {
  if (event.key === "Escape" && mode === "expanded") {
    setMode("compact", compactTrack);
  }
});
document.addEventListener("visibilitychange", refreshTimer);

updateMedia();
setMode(mode);
requestAnimationFrame(() => requestAnimationFrame(() => { island.dataset.ready = ""; }));
