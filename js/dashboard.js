import { auth, db } from "./firebase-config.js";
import {
  onAuthStateChanged,
  signOut,
} from "https://www.gstatic.com/firebasejs/10.13.0/firebase-auth.js";
import {
  ref,
  onValue,
  query,
  limitToLast,
} from "https://www.gstatic.com/firebasejs/10.13.0/firebase-database.js";
import { syncUserProfile, renderAvatar } from "./user-profile.js";

const userNameEl = document.getElementById("user-name");
const userEmailEl = document.getElementById("user-email");
const userAvatarEl = document.getElementById("user-avatar");
const logoutBtn = document.getElementById("logout-btn");
const weightValueEl = document.getElementById("weight-value");
const statusDot = document.getElementById("status-dot");
const statusText = document.getElementById("status-text");
const lastUpdateEl = document.getElementById("last-update");
const minValueEl = document.getElementById("min-value");
const maxValueEl = document.getElementById("max-value");
const avgValueEl = document.getElementById("avg-value");
const emptyNote = document.getElementById("empty-note");

let lastReceivedAt = null;
let realtimeStarted = false;

// Data mentah dari ESP32 (meterrr.ino) disimpan dalam gram — dikonversi ke kg
// untuk semua tampilan di web.
const GRAMS_PER_KG = 1000;
function gramsToKg(grams) {
  return grams / GRAMS_PER_KG;
}

onAuthStateChanged(auth, (user) => {
  if (!user) {
    window.location.href = "login.html";
    return;
  }
  userNameEl.textContent = user.displayName || "Pengguna";
  userEmailEl.textContent = user.email || "";
  renderAvatar(userAvatarEl, user);
  syncUserProfile(user).catch((err) => console.error("Gagal sinkron profil user ke Firestore:", err));

  if (!realtimeStarted) {
    realtimeStarted = true;
    initRealtime();
  }
});

logoutBtn.addEventListener("click", () => {
  signOut(auth).then(() => {
    window.location.href = "login.html";
  });
});

function setOnline(isOnline) {
  statusDot.classList.toggle("online", isOnline);
  statusDot.classList.toggle("offline", !isOnline);
  statusText.textContent = isOnline ? "Terhubung" : "Terputus";
}

function formatElapsed(ms) {
  const s = Math.floor(ms / 1000);
  if (s < 5) return "baru saja";
  if (s < 60) return `${s} detik lalu`;
  const m = Math.floor(s / 60);
  return `${m} menit lalu`;
}

function tickLastUpdate() {
  if (!lastReceivedAt) return;
  const elapsed = Date.now() - lastReceivedAt;
  lastUpdateEl.textContent = formatElapsed(elapsed);
  setOnline(elapsed < 5000);
}

let chart;
function initChart() {
  const ctx = document.getElementById("weight-chart").getContext("2d");
  chart = new Chart(ctx, {
    type: "line",
    data: {
      labels: [],
      datasets: [
        {
          label: "Berat (kg)",
          data: [],
          borderColor: "#c9375f",
          backgroundColor: "rgba(201, 55, 95, 0.18)",
          borderWidth: 2,
          fill: true,
          tension: 0.35,
          pointRadius: 0,
        },
      ],
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: { duration: 300 },
      scales: {
        x: { display: false },
        y: {
          grid: { color: "rgba(255,255,255,0.08)" },
          ticks: { color: "#d7aab9" },
        },
      },
      plugins: { legend: { display: false } },
    },
  });
}

function initRealtime() {
  initChart();

  const liveRef = ref(db, "loadcell/live");
  onValue(
    liveRef,
    (snapshot) => {
      const data = snapshot.val();
      if (!data || typeof data.weight !== "number") return;
      weightValueEl.textContent = gramsToKg(data.weight).toFixed(2);
      lastReceivedAt = Date.now();
      setOnline(true);
      tickLastUpdate();
    },
    (err) => console.error("Gagal baca loadcell/live (cek Realtime Database rules):", err)
  );

  const historyRef = query(ref(db, "loadcell/history"), limitToLast(50));
  onValue(
    historyRef,
    (snapshot) => {
      const entries = [];
      snapshot.forEach((child) => {
        const val = child.val();
        if (typeof val === "number") entries.push(gramsToKg(val));
      });

      if (entries.length === 0) {
        emptyNote.style.display = "flex";
        return;
      }
      emptyNote.style.display = "none";

      chart.data.labels = entries.map((_, i) => i + 1);
      chart.data.datasets[0].data = entries;
      chart.update();

      const min = Math.min(...entries);
      const max = Math.max(...entries);
      const avg = entries.reduce((a, b) => a + b, 0) / entries.length;
      minValueEl.textContent = min.toFixed(2);
      maxValueEl.textContent = max.toFixed(2);
      avgValueEl.textContent = avg.toFixed(2);
    },
    (err) => console.error("Gagal baca loadcell/history (cek Realtime Database rules):", err)
  );

  setInterval(tickLastUpdate, 1000);
}
