// Konfigurasi Firebase bersama — dipakai oleh semua halaman (modular SDK v10, via CDN)
import { initializeApp } from "https://www.gstatic.com/firebasejs/10.13.0/firebase-app.js";
import { getAuth } from "https://www.gstatic.com/firebasejs/10.13.0/firebase-auth.js";
import { getDatabase } from "https://www.gstatic.com/firebasejs/10.13.0/firebase-database.js";
import { getFirestore } from "https://www.gstatic.com/firebasejs/10.13.0/firebase-firestore.js";

const firebaseConfig = {
  apiKey: "AIzaSyDOn6UYH3djXgMM9ygNrW-6HgwABbvaMNA",
  authDomain: "meterr-id.firebaseapp.com",
  databaseURL: "https://meterr-id-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "meterr-id",
  storageBucket: "meterr-id.firebasestorage.app",
  messagingSenderId: "350760249202",
  appId: "1:350760249202:web:871405dbd002d36fb85feb",
  measurementId: "G-YE3XG3SV7M",
};

export const app = initializeApp(firebaseConfig);
export const auth = getAuth(app);
// Realtime Database: khusus data sensor loadcell dari ESP32 (firmware hard-coded ke RTDB).
export const db = getDatabase(app);
// Firestore: data milik aplikasi web (StreakPet, dsb) — bukan data sensor.
export const firestore = getFirestore(app);
