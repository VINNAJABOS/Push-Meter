import { firestore } from "./firebase-config.js";
import { doc, setDoc } from "https://www.gstatic.com/firebasejs/10.13.0/firebase-firestore.js";

/**
 * Simpan/perbarui data profil user (email, nama, foto Google, provider login)
 * ke Firestore users/{uid}, di-merge supaya tidak menimpa field "streak".
 * Dipanggil tiap kali user login (Google atau email/password) supaya data
 * selalu lengkap & terbaru — termasuk photoURL yang cuma tersedia dari akun
 * Google.
 */
export async function syncUserProfile(user) {
  const userDocRef = doc(firestore, "users", user.uid);
  await setDoc(
    userDocRef,
    {
      email: user.email || null,
      displayName: user.displayName || null,
      photoURL: user.photoURL || null,
      provider: user.providerData?.[0]?.providerId || "password",
      lastLoginAt: new Date().toISOString(),
    },
    { merge: true }
  );
}

/**
 * Render avatar: pakai foto profil (photoURL) kalau ada — biasanya dari akun
 * Google — atau huruf inisial nama/email sebagai fallback untuk login
 * email/password yang tidak punya foto.
 */
export function renderAvatar(el, user) {
  if (user.photoURL) {
    el.textContent = "";
    const img = document.createElement("img");
    img.src = user.photoURL;
    img.alt = "";
    img.referrerPolicy = "no-referrer";
    el.appendChild(img);
  } else {
    el.textContent = (user.displayName || user.email || "U").charAt(0).toUpperCase();
  }
}
