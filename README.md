# Meterr.id — Dashboard IoT Loadcell Realtime

Website vanilla JS (HTML/CSS/JS murni, tanpa framework/build tool) untuk memantau
data berat loadcell dari ESP32 + HX711 secara realtime, dengan autentikasi
Email/Password dan Google. Proyek ini memakai **dua database Firebase sekaligus**
dengan pembagian tugas yang jelas:

- **Realtime Database** — khusus data sensor loadcell dari ESP32 (`/loadcell/live`,
  `/loadcell/history`). Firmware ESP32 hard-coded ke RTDB (lihat `meterrr.ino`),
  jadi bagian ini tidak dipindah ke Firestore.
- **Firestore** — data milik aplikasi web per user: `users/{uid}` → field `streak`
  untuk fitur StreakPet Harian.

## Struktur file

```
index.html          Landing page (+ floating chat bubble)
login.html           Halaman masuk (email/password + Google)
register.html         Halaman daftar (email/password + Google)
dashboard.html         Dashboard realtime (protected, perlu login, + floating chat bubble)
aichat.html            Halaman chat SatriaKlengerAI + StreakPet Harian (protected)
css/style.css          Tema burgundy gradient + semua styling
js/firebase-config.js   Konfigurasi & inisialisasi Firebase (dipakai semua halaman)
js/landing.js           Ubah tombol CTA & arah bubble landing jika user sudah login
js/login.js             Logika login
js/register.js          Logika daftar akun
js/dashboard.js          Listener realtime ke /loadcell/live & /loadcell/history + chart
js/gemini-config.js      Klien Gemini API multi-key dengan auto-failover
js/aichat.js             Logika chat SatriaKlengerAI + hitung StreakPet harian
js/user-profile.js       Sinkron profil user (termasuk photoURL Google) ke Firestore + render avatar
asset/                   Icon Satria Klenger AI & Streak Path (flat/happy/sad)
meterrr/meterrr.ino      Kode firmware ESP32 (referensi struktur data)
database.rules.json      Realtime Database security rules — khusus /loadcell (sudah di-deploy)
firestore.rules          Firestore security rules — khusus /users/{uid} (sudah di-deploy)
firestore.indexes.json   Index Firestore (kosong, belum ada query yang butuh index custom)
firebase.json / .firebaserc  Config Firebase CLI, mengarah ke project meterr-id
```

## Menjalankan secara lokal

File JS menggunakan ES Modules (`type="module"`), jadi **harus dibuka lewat
web server**, bukan langsung `file://`. Contoh termudah:

```bash
npx serve .
# atau jika pakai VS Code: ekstensi "Live Server"
```

Lalu buka `http://localhost:3000` (atau port yang ditampilkan).

## Setup Firebase yang perlu dilakukan di Console

1. **Authentication > Sign-in method** — aktifkan provider:
   - Email/Password
   - Google
2. **Authentication > Settings > Authorized domains** — pastikan domain hosting
   Anda (dan `localhost` untuk testing) ada di daftar, supaya popup Google
   login berfungsi.
3. **Realtime Database > Rules** — batasi akses baca `/loadcell` hanya untuk
   user yang sudah login, tutup akses tulis dari web (ESP32 memakai database
   secret/legacy token yang tetap bisa menulis walau rules menutup akses
   tulis publik). Didefinisikan di `database.rules.json` (root proyek),
   **sudah di-deploy** ke project live `meterr-id`:

```json
{
  "rules": {
    "loadcell": {
      ".read": "auth != null",
      ".write": false
    }
  }
}
```

4. **Firestore > Rules** — batasi `users/{uid}` supaya cuma pemiliknya sendiri
   yang boleh baca/tulis. Didefinisikan di `firestore.rules` (root proyek),
   **sudah di-deploy**:

```
rules_version = '2';
service cloud.firestore {
  match /databases/{database}/documents {
    match /users/{uid} {
      allow read, write: if request.auth != null && request.auth.uid == uid;
    }
  }
}
```

Dokumen `users/{uid}` field `streak` dipakai fitur StreakPet Harian (lihat di
bawah). Tanpa rules ini, StreakPet gagal dengan error `Permission denied` di
Console browser (pernah terjadi sebelum rules ini pertama kali di-deploy).

**Kalau perlu ubah rules lagi ke depannya:** edit `database.rules.json` atau
`firestore.rules` di root proyek, lalu jalankan salah satu:

```bash
firebase deploy --only database --project meterr-id
firebase deploy --only firestore:rules --project meterr-id
```

`firebase.json` dan `.firebaserc` di root proyek sudah mengarah ke project
`meterr-id` untuk keperluan ini. (Ada juga `firebase.json`/`database.rules.json`
lama di `meterrr/` dari setup sebelumnya, tidak dipakai untuk deploy — sudah
disamakan isinya dengan versi root supaya tidak ada risiko rules basi ter-deploy
ulang kalau suatu saat dijalankan dari folder itu. **Root proyek adalah sumber
kebenaran.**)

**Data akun inti** (email, UID) dikelola otomatis oleh **Firebase
Authentication** begitu user register/login — ini terpisah dari Realtime
Database maupun Firestore dan tidak perlu rules tambahan.

Selain itu, `js/user-profile.js` (dipakai `dashboard.js` & `aichat.js`) menulis
**salinan profil user ke Firestore** setiap kali auth state berubah (login/buka
halaman), supaya data user lengkap juga tersedia di database aplikasi — bukan
cuma di sistem Auth internal Firebase. Untuk user yang login via **Google**,
ini termasuk `photoURL` foto profil Google mereka (tidak tersedia untuk login
email/password biasa). Avatar di navbar dashboard & chat otomatis menampilkan
foto ini kalau ada (`renderAvatar()`), atau huruf inisial nama/email sebagai
fallback kalau tidak ada foto.

## Struktur data

**Realtime Database** (ditulis ESP32, berdasarkan `meterrr.ino`):
- `/loadcell/live` → `{ weight: number, timestamp_ms: number }`, **weight dalam
  gram** — dibaca untuk angka berat saat ini (update tiap 500ms dari ESP32).
- `/loadcell/history` → list hasil `push()` berupa angka berat (gram) — dibaca
  50 data terakhir untuk grafik riwayat, min/max/rata-rata.

Semua tampilan di web (dashboard, chart, konteks yang dikirim ke
SatriaKlengerAI) **mengonversi nilai gram ini ke kg** (`/ 1000`) sebelum
ditampilkan — lihat fungsi `gramsToKg()` di `js/dashboard.js` dan
`js/aichat.js`. Data mentah di database tetap gram (mengikuti satuan yang
dipakai firmware ESP32); konversi hanya terjadi di sisi tampilan.

**Firestore** (ditulis website — `js/user-profile.js` & `js/aichat.js`):
- `users/{uid}` → dokumen dengan field:
  - `email`, `displayName`, `photoURL`, `provider`, `lastLoginAt` — disinkron
    tiap login lewat `syncUserProfile()`. `photoURL` hanya terisi untuk user
    yang login via Google.
  - `streak: { count, best, lastActiveDate }` — data StreakPet Harian.

## ⚠️ Catatan keamanan

`meterrr/meterrr.ino` menyertakan **Database Secret (legacy token)** dalam
bentuk plain text (`DATABASE_SECRET`). Token ini setara akses admin penuh ke
Realtime Database Anda dan **melewati semua security rules**. Jangan
commit/upload file ini ke repository publik. Jika sudah pernah terekspos
(termasuk di percakapan ini), sebaiknya:

1. Generate ulang secret di Firebase Console > Project Settings > Service
   Accounts > Database secrets.
2. Update `DATABASE_SECRET` di firmware ESP32 dengan nilai baru.
3. Pertimbangkan migrasi ESP32 ke autentikasi berbasis service account/OAuth
   alih-alih legacy database secret untuk keamanan jangka panjang.

Sebaliknya, `apiKey` Firebase yang dipakai di `js/firebase-config.js` **aman
untuk terekspos publik** — itu adalah identifier proyek, bukan secret, dan
keamanan sesungguhnya diatur lewat Security Rules & Authorized domains di
atas.

### API key Gemini (SatriaKlengerAI)

API key Gemini disimpan di `js/gemini-keys.js` (**di-gitignore, tidak ikut ke
GitHub**). Setelah clone, salin `js/gemini-keys.example.js` menjadi
`js/gemini-keys.js` lalu isi key Anda. File ini di-import oleh
`js/gemini-config.js`, jadi key tetap berjalan di **client-side**, dengan auto-failover: kalau key aktif kena limit/quota (HTTP
429) atau ditolak (400/403), otomatis lanjut ke key berikutnya secara
berurutan hingga salah satu berhasil.

Model yang dipakai: `gemini-flash-latest` (alias yang selalu menunjuk ke versi
Flash terbaru yang aktif). Model versi-tanggal seperti `gemini-2.5-flash` atau
`gemini-2.5-flash-lite` sempat dicoba tapi mengembalikan HTTP 404 ("no longer
available to new users") untuk key-key ini — kalau suatu saat `gemini-flash-latest`
juga bermasalah, cek daftar model yang masih aktif untuk key Anda lewat:
`GET https://generativelanguage.googleapis.com/v1beta/models` (header
`x-goog-api-key`), lalu ganti `GEMINI_MODEL` di `js/gemini-config.js`. Kalau AI
tiba-tiba selalu membalas dengan pesan error yang sama persis, buka Console
browser (F12) — pesan error asli dari Gemini (termasuk kode status) di-log ke
sana lewat `console.error`.

Ini adalah keputusan yang sudah dikonfirmasi pemilik proyek demi
kesederhanaan (tanpa server/backend). Konsekuensinya:

- **Kelima key bisa dilihat siapa saja** yang berhasil login lalu membuka
  DevTools (tab Sources/Network) di halaman `aichat.html`, karena kode
  berjalan di browser.
- Fitur ini sengaja **dibatasi hanya untuk user yang sudah login**
  (`js/aichat.js` redirect ke `login.html` jika belum) untuk sedikit
  mengurangi eksposur ke publik — tapi tidak menghilangkan risikonya sama
  sekali terhadap user yang login.
- Kalau nanti ingin lebih aman: pindahkan pemanggilan Gemini ke **Firebase
  Cloud Function** (key disimpan di server, client hanya panggil endpoint
  function). Butuh upgrade project ke paket **Blaze** dan Firebase CLI untuk
  deploy.
- Jika key mulai disalahgunakan (kuota habis tiba-tiba tanpa sebab), rotasi
  ulang key-key tersebut di [Google AI Studio](https://aistudio.google.com/apikey).

**⚠️ Temuan penting soal kuota:** saat menguji, kelima key ternyata mengembalikan
error 429 (`RESOURCE_EXHAUSTED`) **secara bersamaan** dengan pesan yang sama:
`quotaId: GenerateRequestsPerDayPerProjectPerModel-FreeTier`. Kata **"PerProject"**
di situ mengindikasikan kuota gratis (20 request/hari/model) itu dijatah **per
project Google Cloud/AI Studio**, bukan per API key — jadi kalau kelima key ini
dibuat dari **project/akun Google yang sama**, mereka kemungkinan besar berbagi
satu jatah kuota yang sama. Failover antar key tetap berguna kalau salah satu
key dicabut/invalid secara individual, tapi **tidak menambah jatah harian**
kalau penyebabnya adalah kuota project yang habis.

Supaya failover ini benar-benar efektif menambah kapasitas, ada dua opsi:
1. Buat key dari **project Google Cloud yang berbeda-beda** (bukan cuma akun
   yang sama) untuk tiap key — masing-masing project punya jatah kuota sendiri.
2. Upgrade project ke tingkat **berbayar (pay-as-you-go)** di Google AI
   Studio/Cloud Console, yang batasnya jauh lebih tinggi dari tier gratis.

Cek di [Google AI Studio](https://aistudio.google.com/apikey) project mana yang
menaungi kelima key ini untuk memastikan.

### StreakPet Harian

Disimpan di **Firestore**, koleksi `users`, dokumen `{uid}`, field `streak` =
`{ count, best, lastActiveDate }` (lihat `js/aichat.js` fungsi
`updateDailyStreak`, pakai `doc/getDoc/setDoc` dari SDK Firestore — bukan
Realtime Database). Setiap kali user membuka `aichat.html`:

- Tanggal terakhir aktif sama dengan hari ini → tidak berubah.
- Tanggal terakhir aktif = kemarin → `count` bertambah 1 (streak lanjut).
- Selain itu (lebih dari 1 hari terlewat, atau baru pertama kali) → `count`
  reset ke 1.

Mood mascot: `sad` kalau streak baru saja putus, `happy` kalau `count >= 3`,
selain itu `flat`. Ambang batas ini bisa disesuaikan di `js/aichat.js`
(fungsi `updateDailyStreak`).
