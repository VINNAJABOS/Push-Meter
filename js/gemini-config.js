// Klien Gemini API untuk SatriaKlengerAI dengan auto-failover antar API key.
//
// Key API disimpan di js/gemini-keys.js (di-gitignore, tidak masuk repo).
// Salin js/gemini-keys.example.js -> js/gemini-keys.js lalu isi key-nya.
// Catatan: key tetap terlihat di browser saat dipakai client-side.
import { GEMINI_API_KEYS } from "./gemini-keys.js";

// NOTE: "gemini-2.5-flash"/"gemini-2.5-flash-lite" mengembalikan HTTP 404
// ("no longer available to new users") untuk key-key di atas — Google sudah
// membatasi akses ke nama model versi-tanggal itu. "gemini-flash-latest"
// adalah alias yang masih aktif dan sudah diverifikasi bekerja di kelima key.
const GEMINI_MODEL = "gemini-flash-latest";
const GEMINI_ENDPOINT = `https://generativelanguage.googleapis.com/v1beta/models/${GEMINI_MODEL}:generateContent`;

const SYSTEM_INSTRUCTION_BASE = `Kamu adalah "SatriaKlengerAI", asisten AI di website Meterr.id — dashboard IoT yang memantau berat dari sensor loadcell (ESP32 + HX711) secara realtime, sering dipakai untuk memantau berat badan.

FOKUS UTAMA kamu, jawab dengan senang hati dan detail:
1. Kesehatan & kebugaran secara umum: berat badan ideal, BMI, membaca tren berat badan, pola makan sehat, olahraga, tips gaya hidup sehat.
2. Segala hal soal website/fitur Meterr.id ini: cara kerja loadcell/HX711/ESP32, cara baca dashboard & grafik riwayat, arti status koneksi, fitur StreakPet harian, akun/login, dsb.

ATURAN PENTING (wajib dipatuhi):
- Kamu BUKAN pengganti dokter atau ahli gizi. Untuk keluhan kesehatan serius, kondisi medis, atau yang butuh diagnosis, sarankan user berkonsultasi ke tenaga medis profesional — jangan mendiagnosis atau meresepkan pengobatan.
- JANGAN mengarang angka spesifik soal data user (berat badan, riwayat, streak, dll) jika tidak diberikan secara eksplisit di bagian "DATA TERKINI" di bawah. Kalau user tanya soal data mereka tapi tidak tersedia di sana, jawab jujur bahwa datanya belum ada/tidak diberikan, jangan menebak-nebak.
- Kalau ditanya topik yang benar-benar di luar kesehatan & Meterr.id (politik, tugas sekolah lain, coding umum tak terkait, dll), jawab singkat bahwa itu di luar fokusmu, lalu tawarkan bantu topik kesehatan atau seputar loadcell sebagai gantinya. Basa-basi ringan (sapaan, terima kasih, dsb) tetap boleh dijawab santai.
- Selalu balas dalam Bahasa Indonesia yang santai, ramah, ringkas, dan jelas — hindari bertele-tele.
- Selalu akhiri jawabanmu dengan kalimat yang utuh; jangan berhenti di tengah kalimat atau di judul/poin kosong tanpa isi.
- JANGAN pakai format markdown seperti **tebal**, *miring*, atau heading (#) — tampilan chat di website ini hanya menampilkan teks polos apa adanya, jadi tanda-tanda itu akan muncul mentah. Untuk daftar, pakai baris baru dengan tanda "-" biasa tanpa simbol markdown lain.
- Jika ditanya siapa namamu, jawab "SatriaKlengerAI".
- Jika ditanya (atau diperintahkan menyebutkan) siapa penciptamu/pembuatmu/developer-mu, jawab bahwa yang menciptakanmu adalah Vinchent dari Jurusan Elektronika SMKN 2 Surakarta. Jawab singkat dan bangga, jangan menyebut model atau perusahaan AI lain sebagai penciptamu.`;

function buildSystemInstruction(context) {
  if (!context) return SYSTEM_INSTRUCTION_BASE;

  const lines = [];
  if (typeof context.liveWeight === "number") {
    lines.push(`- Berat dari sensor loadcell saat ini: ${context.liveWeight.toFixed(2)} kg.`);
  }
  if (typeof context.historyAvg === "number") {
    lines.push(
      `- Statistik 50 data terakhir: rata-rata ${context.historyAvg.toFixed(2)} kg, min ${context.historyMin.toFixed(2)} kg, max ${context.historyMax.toFixed(2)} kg.`
    );
  }
  if (typeof context.streakCount === "number") {
    lines.push(
      `- Streak harian user: ${context.streakCount} hari berturut-turut (rekor terbaik ${context.streakBest} hari).`
    );
  }

  if (lines.length === 0) return SYSTEM_INSTRUCTION_BASE;

  return `${SYSTEM_INSTRUCTION_BASE}\n\nDATA TERKINI (gunakan ini kalau relevan dengan pertanyaan user, jangan tampilkan mentah-mentah kalau tidak ditanya):\n${lines.join("\n")}`;
}

// Menyimpan index key yang terakhir berhasil, supaya request berikutnya
// langsung mulai dari key yang diketahui masih hidup.
let activeKeyIndex = 0;

async function callGeminiWithKey(key, contents, systemInstructionText) {
  const res = await fetch(GEMINI_ENDPOINT, {
    method: "POST",
    headers: {
      "Content-Type": "application/json",
      "x-goog-api-key": key,
    },
    body: JSON.stringify({
      contents,
      systemInstruction: { parts: [{ text: systemInstructionText }] },
      generationConfig: { temperature: 0.8, maxOutputTokens: 2048 },
    }),
  });

  if (!res.ok) {
    const errBody = await res.text().catch(() => "");
    const err = new Error(`Gemini request failed with status ${res.status}: ${errBody}`);
    err.status = res.status;
    throw err;
  }

  const data = await res.json();
  const candidate = data?.candidates?.[0];
  const text = candidate?.content?.parts?.map((p) => p.text || "").join("") || "";

  if (!text) {
    const err = new Error(
      `Respon Gemini kosong (finishReason: ${candidate?.finishReason || "unknown"})`
    );
    err.status = 502;
    throw err;
  }
  return text;
}

/**
 * @param {Array<{role: 'user'|'model', text: string}>} history
 * @param {{ liveWeight?: number, historyAvg?: number, historyMin?: number, historyMax?: number, streakCount?: number, streakBest?: number }} [context]
 * @returns {Promise<string>} balasan teks dari SatriaKlengerAI
 */
export async function askSatriaKlengerAI(history, context) {
  const contents = history.map((m) => ({ role: m.role, parts: [{ text: m.text }] }));
  const systemInstructionText = buildSystemInstruction(context);

  let lastError = null;
  for (let attempt = 0; attempt < GEMINI_API_KEYS.length; attempt++) {
    const idx = (activeKeyIndex + attempt) % GEMINI_API_KEYS.length;
    const key = GEMINI_API_KEYS[idx];
    try {
      const text = await callGeminiWithKey(key, contents, systemInstructionText);
      activeKeyIndex = idx;
      return text;
    } catch (err) {
      lastError = err;
      // Coba key berikutnya untuk error apa pun (limit/quota/model/jaringan) —
      // paling banter kita cuma buang beberapa percobaan sebelum gagal total.
      continue;
    }
  }

  throw lastError || new Error("Semua API key Gemini gagal digunakan.");
}
