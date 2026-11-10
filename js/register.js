import { auth } from "./firebase-config.js";
import {
  createUserWithEmailAndPassword,
  updateProfile,
  GoogleAuthProvider,
  signInWithPopup,
  onAuthStateChanged,
} from "https://www.gstatic.com/firebasejs/10.13.0/firebase-auth.js";

const form = document.getElementById("register-form");
const nameInput = document.getElementById("name");
const emailInput = document.getElementById("email");
const passwordInput = document.getElementById("password");
const confirmInput = document.getElementById("confirm");
const errorBox = document.getElementById("error-box");
const googleBtn = document.getElementById("google-register");
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
    case "auth/email-already-in-use":
      return "Email sudah terdaftar. Silakan masuk.";
    case "auth/invalid-email":
      return "Format email tidak valid.";
    case "auth/weak-password":
      return "Password minimal 6 karakter.";
    case "auth/popup-closed-by-user":
      return "Pendaftaran Google dibatalkan.";
    case "auth/network-request-failed":
      return "Gagal terhubung ke server. Periksa koneksi internet Anda.";
    default:
      return "Terjadi kesalahan. Silakan coba lagi.";
  }
}

form.addEventListener("submit", async (e) => {
  e.preventDefault();
  clearError();

  if (passwordInput.value !== confirmInput.value) {
    showError("Konfirmasi password tidak cocok.");
    return;
  }

  submitBtn.disabled = true;
  submitBtn.textContent = "Memproses...";
  try {
    const cred = await createUserWithEmailAndPassword(
      auth,
      emailInput.value.trim(),
      passwordInput.value
    );
    if (nameInput.value.trim()) {
      await updateProfile(cred.user, { displayName: nameInput.value.trim() });
    }
    window.location.href = "dashboard.html";
  } catch (err) {
    showError(mapAuthError(err.code));
  } finally {
    submitBtn.disabled = false;
    submitBtn.textContent = "Daftar";
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
