import { auth, db, firestore } from "./firebase-config.js";
import { onAuthStateChanged, signOut } from "https://www.gstatic.com/firebasejs/10.13.0/firebase-auth.js";
import { ref, onValue, query, limitToLast } from "https://www.gstatic.com/firebasejs/10.13.0/firebase-database.js";
import { doc, getDoc, setDoc } from "https://www.gstatic.com/firebasejs/10.13.0/firebase-firestore.js";
import { askSatriaKlengerAI } from "./gemini-config.js";
import { syncUserProfile, renderAvatar } from "./user-profile.js";

const userAvatarEl = document.getElementById("user-avatar");
const logoutBtn = document.getElementById("logout-btn");
const chatMessagesEl = document.getElementById("chat-messages");
const chatForm = document.getElementById("chat-form");
const chatInput = document.getElementById("chat-input");
const chatSendBtn = document.getElementById("chat-send");
const chatStatusEl = document.getElementById("chat-status");
const streakImgEl = document.getElementById("streak-img");
const streakCountEl = document.getElementById("streak-count");
const streakBestEl = document.getElementById("streak-best");
const streakNoteEl = document.getElementById("streak-note");

const STREAK_IMAGES = {
  sad: "asset/streak-sad.png",
  flat: "asset/streak-flat.png",
  happy: "asset/streak-happy.png",
};

const STREAK_NOTES = {
  sad: "Streak sebelumnya putus, tapi hari ini kamu balik lagi. Ayo lanjutkan!",
  flat: "Bagus, terus jaga konsistensinya!",
  happy: "Mantap, streak-mu lagi bagus-bagusnya! 🔥",
};

let history = [];

// Konteks data realtime (berat sensor + streak) yang dikirim ke AI supaya
// jawabannya nyambung dengan data asli, bukan mengarang.
const liveContext = {};

onAuthStateChanged(auth, (user) => {
  if (!user) {
    window.location.href = "login.html";
    return;
  }
  renderAvatar(userAvatarEl, user);
  syncUserProfile(user).catch((err) => console.error("Gagal sinkron profil user ke Firestore:", err));
  updateDailyStreak(user.uid).catch((err) => {
    console.error(
      "StreakPet gagal disimpan/dibaca — kemungkinan besar Firestore rules " +
        'belum mengizinkan dokumen "users/{uid}". Cek README.md bagian setup ' +
        "Firestore. Detail error:",
      err
    );
    renderStreak(1, 1, "flat");
  });
  watchLiveWeight();
});

logoutBtn.addEventListener("click", () => {
  signOut(auth).then(() => (window.location.href = "login.html"));
});

// Data mentah dari ESP32 (meterrr.ino) disimpan dalam gram — dikonversi ke kg
// supaya konteks yang dikirim ke AI konsisten dengan satuan yang tampil di web.
const GRAMS_PER_KG = 1000;
function gramsToKg(grams) {
  return grams / GRAMS_PER_KG;
}

function watchLiveWeight() {
  onValue(
    ref(db, "loadcell/live"),
    (snapshot) => {
      const data = snapshot.val();
      if (data && typeof data.weight === "number") {
        liveContext.liveWeight = gramsToKg(data.weight);
      }
    },
    (err) => console.error("Gagal baca loadcell/live (cek RTDB rules):", err)
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
      if (entries.length === 0) return;
      liveContext.historyMin = Math.min(...entries);
      liveContext.historyMax = Math.max(...entries);
      liveContext.historyAvg = entries.reduce((a, b) => a + b, 0) / entries.length;
    },
    (err) => console.error("Gagal baca loadcell/history (cek RTDB rules):", err)
  );
}

function todayLocalString() {
  const d = new Date();
  const y = d.getFullYear();
  const m = String(d.getMonth() + 1).padStart(2, "0");
  const day = String(d.getDate()).padStart(2, "0");
  return `${y}-${m}-${day}`;
}

function daysBetween(dateStrA, dateStrB) {
  const a = new Date(dateStrA + "T00:00:00");
  const b = new Date(dateStrB + "T00:00:00");
  return Math.round((b - a) / (1000 * 60 * 60 * 24));
}

async function updateDailyStreak(uid) {
  // StreakPet disimpan di Firestore (koleksi "users", satu dokumen per uid,
  // field "streak") — bukan di Realtime Database, yang khusus dipakai untuk
  // data sensor loadcell dari ESP32.
  const userDocRef = doc(firestore, "users", uid);
  const snap = await getDoc(userDocRef);
  const today = todayLocalString();
  const prev = snap.exists() ? snap.data().streak : null;

  let count = 1;
  let best = 1;
  let mood = "flat";

  if (!prev) {
    count = 1;
    best = 1;
    mood = "flat";
  } else if (prev.lastActiveDate === today) {
    count = prev.count || 1;
    best = Math.max(prev.best || 1, count);
    mood = count >= 3 ? "happy" : "flat";
  } else {
    const gap = daysBetween(prev.lastActiveDate, today);
    if (gap === 1) {
      count = (prev.count || 0) + 1;
      mood = count >= 3 ? "happy" : "flat";
    } else {
      count = 1;
      mood = "sad";
    }
    best = Math.max(prev.best || 1, count);
  }

  await setDoc(userDocRef, { streak: { count, best, lastActiveDate: today } }, { merge: true });
  renderStreak(count, best, mood);
}

function renderStreak(count, best, mood) {
  streakImgEl.src = STREAK_IMAGES[mood];
  streakCountEl.textContent = String(count);
  streakBestEl.textContent = String(best);
  streakNoteEl.textContent = STREAK_NOTES[mood];
  liveContext.streakCount = count;
  liveContext.streakBest = best;
}

function appendMessage(role, text) {
  const wrap = document.createElement("div");
  wrap.className = `msg ${role === "user" ? "msg-user" : "msg-ai"}`;

  if (role !== "user") {
    const avatar = document.createElement("div");
    avatar.className = "msg-avatar";
    const img = document.createElement("img");
    img.src = "asset/satria-icon.png";
    img.alt = "";
    avatar.appendChild(img);
    wrap.appendChild(avatar);
  }

  const bubble = document.createElement("div");
  bubble.className = "msg-bubble";
  bubble.textContent = text;
  wrap.appendChild(bubble);

  chatMessagesEl.appendChild(wrap);
  chatMessagesEl.scrollTop = chatMessagesEl.scrollHeight;
  return bubble;
}

chatForm.addEventListener("submit", async (e) => {
  e.preventDefault();
  const text = chatInput.value.trim();
  if (!text) return;

  appendMessage("user", text);
  history.push({ role: "user", text });
  chatInput.value = "";
  chatInput.disabled = true;
  chatSendBtn.disabled = true;
  chatStatusEl.textContent = "Sedang mengetik...";

  const typingBubble = appendMessage("ai", "...");

  try {
    const reply = await askSatriaKlengerAI(history, liveContext);
    typingBubble.textContent = reply;
    history.push({ role: "model", text: reply });
    chatStatusEl.textContent = "Siap ngobrol";
  } catch (err) {
    console.error("SatriaKlengerAI gagal merespon:", err);
    // Batalkan pesan user dari history supaya tidak ikut dikirim ulang di
    // permintaan berikutnya kalau AI gagal total menjawabnya.
    history.pop();
    typingBubble.textContent =
      "Aduh, semua jalur AI lagi penuh/limit nih. Coba lagi beberapa saat ya. (Detail error ada di console browser)";
    chatStatusEl.textContent = "Gagal merespon";
  } finally {
    chatInput.disabled = false;
    chatSendBtn.disabled = false;
    chatInput.focus();
  }
});
