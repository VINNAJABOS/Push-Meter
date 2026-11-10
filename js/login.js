import { auth } from "./firebase-config.js";
import {
  signInWithEmailAndPassword,
  GoogleAuthProvider,
  signInWithPopup,
  onAuthStateChanged,
} from "https://www.gstatic.com/firebasejs/10.13.0/firebase-auth.js";

const form = document.getElementById("login-form");
const emailInput = document.getElementById("email");
const passwordInput = document.getElementById("password");
const errorBox = document.getElementById("error-box");
const googleBtn = document.getElementById("google-login");
const submitBtn = document.getElementById("submit-btn");

onAuthStateChanged(auth, (user) => {
  if (user) window.location.href = "dashboard.html";
});

function showError(message) {
  errorBox.textContent = message;
  errorBox.classList.add("visible");
}

function clearError() {
  errorBox.textContent = "";
  errorBox.classList.remove("visible");
}

function mapAuthError(code) {
  switch (code) {
    case "auth/invalid-email":
      return "Format email tidak valid.";
    case "auth/user-not-found":
    case "auth/wrong-password":
    case "auth/invalid-credential":
      return "Email atau password salah.";
    case "auth/too-many-requests":
      return "Terlalu banyak percobaan gagal. Coba lagi nanti.";
    case "auth/popup-closed-by-user":
      return "Login Google dibatalkan.";
    case "auth/network-request-failed":
      return "Gagal terhubung ke server. Periksa koneksi internet Anda.";
    default:
      return "Terjadi kesalahan. Silakan coba lagi.";
  }
}

form.addEventListener("submit", async (e) => {
  e.preventDefault();
  clearError();
  submitBtn.disabled = true;
  submitBtn.textContent = "Memproses...";
  try {
    await signInWithEmailAndPassword(auth, emailInput.value.trim(), passwordInput.value);
    window.location.href = "dashboard.html";
  } catch (err) {
    showError(mapAuthError(err.code));
  } finally {
    submitBtn.disabled = false;
    submitBtn.textContent = "Masuk";
  }
});

googleBtn.addEventListener("click", async () => {
  clearError();
  googleBtn.disabled = true;
  const provider = new GoogleAuthProvider();
  try {
    await signInWithPopup(auth, provider);
    window.location.href = "dashboard.html";
  } catch (err) {
    if (err.code !== "auth/popup-closed-by-user") {
      showError(mapAuthError(err.code));
    }
  } finally {
    googleBtn.disabled = false;
  }
});
