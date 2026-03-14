export function guessMimeFromName(name: string): string {
  const lower = String(name || "").toLowerCase();
  if (lower.endsWith(".png")) return "image/png";
  if (lower.endsWith(".jpg") || lower.endsWith(".jpeg")) return "image/jpeg";
  if (lower.endsWith(".gif")) return "image/gif";
  if (lower.endsWith(".webp")) return "image/webp";
  if (lower.endsWith(".svg")) return "image/svg+xml";
  if (lower.endsWith(".mp3")) return "audio/mpeg";
  if (lower.endsWith(".wav")) return "audio/wav";
  if (lower.endsWith(".mp4")) return "video/mp4";
  if (lower.endsWith(".webm")) return "video/webm";
  if (lower.endsWith(".mov")) return "video/quicktime";
  if (lower.endsWith(".txt") || lower.endsWith(".md")) return "text/plain";
  return "";
}

export function sanitizeUploadName(name: string): string {
  let out = String(name || "").trim();
  if (!out) return "upload.bin";
  out = out.replace(/[\\/]/g, "_");
  out = out.replace(/[^A-Za-z0-9._-]/g, "_");
  out = out.replace(/_+/g, "_");
  if (out.length > 200) out = out.slice(0, 200);
  if (out === "." || out === ".." || out === "") return "upload.bin";
  if (out.includes("..")) out = out.replace(/\.\.+/g, ".");
  return out;
}

export function isSafeSessionId(value: string): boolean {
  if (!value) return false;
  if (value.length > 200) return false;
  if (value === "." || value === "..") return false;
  if (value.includes("..")) return false;
  return /^[A-Za-z0-9._-]+$/.test(value);
}

export async function fileToBase64(file: File): Promise<string> {
  return await new Promise((resolve, reject) => {
    const fr = new FileReader();
    fr.onload = () => {
      const res = String(fr.result ?? "");
      const idx = res.indexOf(",");
      resolve(idx >= 0 ? res.slice(idx + 1) : res);
    };
    fr.onerror = () => reject(fr.error || new Error("FileReader failed"));
    fr.readAsDataURL(file);
  });
}
